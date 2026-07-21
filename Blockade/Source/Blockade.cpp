/*
Blockade - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * Blockade - ASA Plugin
 *
 * Tables:
 *   blockade_combat  PK (eos_id, map_name)  active combat blocks, scoped to this map
 *     Columns: eos_id VARCHAR(64), map_name VARCHAR(128), expires_at BIGINT
 *
 *   blockade_raid    PK (eos_id, map_name)  active raid blocks, scoped to this map
 *     Columns: eos_id VARCHAR(64), map_name VARCHAR(128), expires_at BIGINT
 *
 * Hooks:
 *   APrimalCharacter.TakeDamage(float,FDamageEvent&,AController*,AActor*)  combat detection, gated on real victim health loss
 *   APrimalStructure.TakeDamage(float,FDamageEvent&,AController*,AActor*)  raid zone creation on real enemy structure damage
 *   AShooterGameMode.Tick(float)                                           config reload (10s), combat and raid sweep (1s)
 *
 * Config:
 *   ArkApi/Plugins/Blockade/config.json
 *   DbHost, DbPort, DbUser, DbPass, DbName: MariaDB connection, shared cluster DB
 *   RaidBlockRadius: raid zone radius in world units
 *   ImmuneEosIds: EOS IDs exempt from both combat and raid block
 *   CombatBlockEnabled, CombatBlockSeconds, CombatBlockMessage ({seconds} token), CombatBlockClearedMessage, CombatBlockNotifyInterval
 *   RaidBlockEnabled, RaidBlockSeconds, RaidBlockMessage, RaidBlockClearedMessage, RaidBlockNotifyInterval
 *
 * Config Example:
 * {
 *   "DbHost": "127.0.0.1",
 *   "DbPort": 3306,
 *   "DbUser": "User",
 *   "DbPass": "Password",
 *   "DbName": "Database",
 *   "RaidBlockRadius": 10000.0,
 *   "ImmuneEosIds": [ "EOS_ID" ],
 *   "CombatBlockEnabled": true,
 *   "CombatBlockSeconds": 30,
 *   "CombatBlockMessage": "Combat Blocked: {seconds}s",
 *   "CombatBlockClearedMessage": "No longer combat blocked",
 *   "CombatBlockNotifyInterval": 10,
 *   "RaidBlockEnabled": true,
 *   "RaidBlockSeconds": 300,
 *   "RaidBlockMessage": "Raid Block Active",
 *   "RaidBlockClearedMessage": "Raid Block Removed",
 *   "RaidBlockNotifyInterval": 30
 * }
 *
 * Owns combat and raid block state for the cluster, applied globally except for ImmuneEosIds.
 * Combat fires only on real victim health loss from an enemy player or tamed dino. Structure
 * sourced damage to a player instead creates a raid zone at the victim. Raid zones form on real
 * enemy structure damage, merge within RaidBlockRadius, and expire RaidBlockSeconds after their
 * last hit. Players standing in an active zone are raid blocked. Both states show an on screen
 * marker and are mirrored to the tables transition only, upsert on enter, refresh on change,
 * delete on exit, for other plugins to read on demand.
 *
 * Raid expiry is anchored to the zone, not the player. A blocked player's expires_at is the
 * latest lastHit plus RaidBlockSeconds across every zone containing them, so it holds still
 * while no further structure damage lands and survives a player leaving and re-entering. Only
 * a fresh hit on the zone pushes it forward.
 *
 * Write strategy:
 *   Hooks and sweeps never touch the DB. They stamp a dirty set keyed by EOS ID, last write wins
 *   per player per type. A worker thread drains the set every second, pings the link, and
 *   reconnects if it dropped. On reconnect failure the drained ops are merged back and retried,
 *   with entries queued since the drain taking precedence. In memory state is empty at load, so
 *   any surviving rows for this map are crash leftovers and are purged on the first successful
 *   flush before that flush is applied. Config rescanned every 10 seconds, size plus last write
 *   time, reloaded only on a confirmed stable change.
 */

#include <API/ARK/Ark.h>

#pragma warning(disable: 4191)
#pragma comment(lib, "AsaApi.lib")

#include <json.hpp>
#include <Windows.h>
#include <fstream>
#include <string>
#include <vector>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <mutex>
#include <thread>
#include <atomic>
#include <ctime>
#include <cstdio>
#include <cmath>
#include <sys/stat.h>

typedef struct st_mysql     MYSQL;
typedef struct st_mysql_res MYSQL_RES;
typedef char** MYSQL_ROW;

typedef MYSQL*        (__stdcall* mysql_init_t)               (MYSQL*);
typedef MYSQL*        (__stdcall* mysql_real_connect_t)       (MYSQL*, const char*, const char*, const char*, const char*, unsigned int, const char*, unsigned long);
typedef void          (__stdcall* mysql_close_t)              (MYSQL*);
typedef int           (__stdcall* mysql_query_t)              (MYSQL*, const char*);
typedef MYSQL_RES*    (__stdcall* mysql_store_result_t)       (MYSQL*);
typedef void          (__stdcall* mysql_free_result_t)        (MYSQL_RES*);
typedef const char*   (__stdcall* mysql_error_t)              (MYSQL*);
typedef unsigned long (__stdcall* mysql_real_escape_string_t) (MYSQL*, char*, const char*, unsigned long);
typedef int           (__stdcall* mysql_options_t)            (MYSQL*, int, const void*);
typedef MYSQL_ROW     (__stdcall* mysql_fetch_row_t)          (MYSQL_RES*);
typedef int           (__stdcall* mysql_ping_t)               (MYSQL*);

#define MYSQL_OPT_CONNECT_TIMEOUT 0

static HMODULE                    g_mysql_module = nullptr;
static mysql_init_t               pmysql_init = nullptr;
static mysql_real_connect_t       pmysql_real_connect = nullptr;
static mysql_close_t              pmysql_close = nullptr;
static mysql_query_t              pmysql_query = nullptr;
static mysql_store_result_t       pmysql_store_result = nullptr;
static mysql_free_result_t        pmysql_free_result = nullptr;
static mysql_error_t              pmysql_error = nullptr;
static mysql_real_escape_string_t pmysql_real_escape_string = nullptr;
static mysql_options_t            pmysql_options = nullptr;
static mysql_fetch_row_t          pmysql_fetch_row = nullptr;
static mysql_ping_t               pmysql_ping = nullptr;
static bool                       g_mysql_loaded = false;

static bool LoadMySQLLib()
{
    if (g_mysql_loaded) return true;

    const char* candidates[] = {
        "libmariadb.dll",
        ".\\libmariadb.dll",
        "ArkApi\\Plugins\\libmariadb.dll",
        "libmysql.dll",
        ".\\libmysql.dll",
        nullptr
    };

    for (int i = 0; candidates[i]; ++i)
    {
        g_mysql_module = LoadLibraryA(candidates[i]);
        if (g_mysql_module)
        {
            Log::GetLog()->info("[Blockade] Loaded DB library: {}", candidates[i]);
            break;
        }
    }

    if (!g_mysql_module)
    {
        Log::GetLog()->error("[Blockade] Could not find libmariadb.dll or libmysql.dll");
        return false;
    }

    pmysql_init               = (mysql_init_t)GetProcAddress(g_mysql_module, "mysql_init");
    pmysql_real_connect       = (mysql_real_connect_t)GetProcAddress(g_mysql_module, "mysql_real_connect");
    pmysql_close              = (mysql_close_t)GetProcAddress(g_mysql_module, "mysql_close");
    pmysql_query              = (mysql_query_t)GetProcAddress(g_mysql_module, "mysql_query");
    pmysql_store_result       = (mysql_store_result_t)GetProcAddress(g_mysql_module, "mysql_store_result");
    pmysql_free_result        = (mysql_free_result_t)GetProcAddress(g_mysql_module, "mysql_free_result");
    pmysql_error              = (mysql_error_t)GetProcAddress(g_mysql_module, "mysql_error");
    pmysql_real_escape_string = (mysql_real_escape_string_t)GetProcAddress(g_mysql_module, "mysql_real_escape_string");
    pmysql_options            = (mysql_options_t)GetProcAddress(g_mysql_module, "mysql_options");
    pmysql_fetch_row          = (mysql_fetch_row_t)GetProcAddress(g_mysql_module, "mysql_fetch_row");
    pmysql_ping               = (mysql_ping_t)GetProcAddress(g_mysql_module, "mysql_ping");

    if (!pmysql_init || !pmysql_real_connect || !pmysql_close ||
        !pmysql_query || !pmysql_error || !pmysql_real_escape_string || !pmysql_fetch_row)
    {
        Log::GetLog()->error("[Blockade] Failed to resolve required DB functions");
        return false;
    }

    g_mysql_loaded = true;
    return true;
}

static const std::string g_config_path = "ArkApi/Plugins/Blockade/config.json";

static std::mutex g_db_cfg_mutex;
static std::string g_db_host = "127.0.0.1";
static unsigned int g_db_port = 3306;
static std::string g_db_user;
static std::string g_db_pass;
static std::string g_db_name;

static double g_raid_block_radius = 10000.0;

static std::unordered_set<std::string> g_immune_eos;

static bool g_combat_block_enabled = true;
static int g_combat_block_seconds = 30;
static std::wstring g_combat_msg = L"Combat Blocked: {seconds}s";
static std::wstring g_combat_cleared_msg = L"No longer combat blocked";
static int g_combat_notify_interval = 10;

static bool g_raid_block_enabled = true;
static int g_raid_block_seconds = 300;
static std::wstring g_raid_msg = L"Raid Block Active";
static std::wstring g_raid_cleared_msg = L"Raid Block Removed";
static int g_raid_notify_interval = 30;

static time_t g_config_last_modified = 0;
static long long g_config_last_size = 0;
static std::chrono::steady_clock::time_point g_last_config_check;
static std::chrono::steady_clock::time_point g_last_sweep;

static std::mutex g_map_name_mutex;
static std::string g_map_name;
static std::atomic<bool> g_map_name_ready{ false };

static MYSQL* g_db = nullptr;
static std::mutex g_db_mutex;
static bool g_db_logged_down = false;
static bool g_purge_done = false;

struct PendingOp
{
    bool remove = false;
    long long expiresAt = 0;
};

static std::mutex g_pending_mutex;
static std::unordered_map<std::string, PendingOp> g_pending_combat;
static std::unordered_map<std::string, PendingOp> g_pending_raid;

struct CombatState
{
    std::chrono::steady_clock::time_point lastHit;
    int blockSeconds = 0;
    std::chrono::steady_clock::time_point lastNotify;
    long long writtenExpiry = 0;
};

static std::unordered_map<std::string, CombatState> g_combat_times;
static std::mutex g_combat_mutex;

struct RaidZone
{
    double x, y;
    std::chrono::steady_clock::time_point lastHit;
    long long lastHitUnix;
};

struct RaidState
{
    std::chrono::steady_clock::time_point lastNotify;
    long long writtenExpiry = 0;
};

static std::vector<RaidZone> g_raid_zones;
static std::unordered_map<std::string, RaidState> g_raid_blocked_players;
static std::mutex g_raid_mutex;

static std::thread g_worker_thread;
static std::atomic<bool> g_worker_running{ false };

static std::string FStr(const FString& f)
{
    const char* s = TCHAR_TO_UTF8(*f);
    return (s && s[0]) ? s : "";
}

static std::wstring Widen(const std::string& s)
{
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring out((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), len);
    return out;
}

static std::wstring FormatSeconds(const std::wstring& tmpl, int seconds)
{
    std::wstring out = tmpl;
    const std::wstring token = L"{seconds}";
    const std::wstring val = std::to_wstring(seconds);
    size_t pos = 0;
    while ((pos = out.find(token, pos)) != std::wstring::npos)
    {
        out.replace(pos, token.size(), val);
        pos += val.size();
    }
    return out;
}

static std::string GetEosId(AShooterPlayerController* pc)
{
    if (!pc) return "";
    AShooterPlayerState* ps = static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
    if (!ps) return "";
    FString eos;
    ps->GetUniqueNetIdAsString(&eos);
    return FStr(eos);
}

static bool IsImmune(const std::string& eos)
{
    if (eos.empty()) return false;
    return g_immune_eos.count(eos) > 0;
}

static void ResolveMapName()
{
    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return;

    FString m;
    world->GetMapName(&m);
    std::string name = FStr(m);
    if (name.empty()) return;

    {
        std::lock_guard<std::mutex> lock(g_map_name_mutex);
        g_map_name = name;
    }
    g_map_name_ready.store(true);
}

static std::string MapName()
{
    if (!g_map_name_ready.load()) return "";
    std::lock_guard<std::mutex> lock(g_map_name_mutex);
    return g_map_name;
}

static float GetCurrentHealth(APrimalCharacter* c)
{
    if (!c) return 0.0f;
    UPrimalCharacterStatusComponent* comp = c->MyCharacterStatusComponentField();
    if (!comp) return 0.0f;
    return comp->BPGetCurrentStatusValue(EPrimalCharacterStatusValue::Health);
}

static void OnScreen(AShooterPlayerController* pc, const std::wstring& text, const FLinearColor& color, float displayTime)
{
    if (!pc) return;
    AsaApi::GetApiUtils().SendNotification(pc, color, 1.3f, displayTime, nullptr, text.c_str());
}

static void QueueCombatWrite(const std::string& eos, long long expiresAt)
{
    if (eos.empty()) return;
    std::lock_guard<std::mutex> lock(g_pending_mutex);
    PendingOp op;
    op.remove = false;
    op.expiresAt = expiresAt;
    g_pending_combat[eos] = op;
}

static void QueueCombatDelete(const std::string& eos)
{
    if (eos.empty()) return;
    std::lock_guard<std::mutex> lock(g_pending_mutex);
    PendingOp op;
    op.remove = true;
    op.expiresAt = 0;
    g_pending_combat[eos] = op;
}

static void QueueRaidWrite(const std::string& eos, long long expiresAt)
{
    if (eos.empty()) return;
    std::lock_guard<std::mutex> lock(g_pending_mutex);
    PendingOp op;
    op.remove = false;
    op.expiresAt = expiresAt;
    g_pending_raid[eos] = op;
}

static void QueueRaidDelete(const std::string& eos)
{
    if (eos.empty()) return;
    std::lock_guard<std::mutex> lock(g_pending_mutex);
    PendingOp op;
    op.remove = true;
    op.expiresAt = 0;
    g_pending_raid[eos] = op;
}

static time_t GetFileModTime(const std::string& path)
{
    struct _stat st{};
    if (_stat(path.c_str(), &st) == 0) return st.st_mtime;
    return 0;
}

static long long GetFileSize(const std::string& path)
{
    struct _stat st{};
    if (_stat(path.c_str(), &st) == 0) return st.st_size;
    return 0;
}

static bool LoadConfig()
{
    std::ifstream file(g_config_path);
    if (!file.is_open())
    {
        Log::GetLog()->error("[Blockade] Cannot open config: {}", g_config_path);
        return false;
    }

    try
    {
        nlohmann::json j;
        file >> j;

        {
            std::lock_guard<std::mutex> lock(g_db_cfg_mutex);
            g_db_host = j.value("DbHost", std::string("127.0.0.1"));
            g_db_port = j.value("DbPort", 3306u);
            g_db_user = j.value("DbUser", std::string(""));
            g_db_pass = j.value("DbPass", std::string(""));
            g_db_name = j.value("DbName", std::string(""));
        }

        g_raid_block_radius = j.value("RaidBlockRadius", 10000.0);

        std::unordered_set<std::string> newImmune;
        if (j.contains("ImmuneEosIds") && j["ImmuneEosIds"].is_array())
        {
            for (auto& e : j["ImmuneEosIds"])
                if (e.is_string()) newImmune.insert(e.get<std::string>());
        }
        g_immune_eos = std::move(newImmune);

        g_combat_block_enabled = j.value("CombatBlockEnabled", true);
        g_combat_block_seconds = j.value("CombatBlockSeconds", 30);
        g_combat_msg = Widen(j.value("CombatBlockMessage", std::string("Combat Blocked: {seconds}s")));
        g_combat_cleared_msg = Widen(j.value("CombatBlockClearedMessage", std::string("No longer combat blocked")));
        g_combat_notify_interval = j.value("CombatBlockNotifyInterval", 10);

        g_raid_block_enabled = j.value("RaidBlockEnabled", true);
        g_raid_block_seconds = j.value("RaidBlockSeconds", 300);
        g_raid_msg = Widen(j.value("RaidBlockMessage", std::string("Raid Block Active")));
        g_raid_cleared_msg = Widen(j.value("RaidBlockClearedMessage", std::string("Raid Block Removed")));
        g_raid_notify_interval = j.value("RaidBlockNotifyInterval", 30);

        if (g_combat_notify_interval < 1) g_combat_notify_interval = 10;
        if (g_raid_notify_interval < 1) g_raid_notify_interval = 30;

        g_config_last_modified = GetFileModTime(g_config_path);
        g_config_last_size = GetFileSize(g_config_path);
        Log::GetLog()->info("[Blockade] Config loaded, {} immune", g_immune_eos.size());
        return true;
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[Blockade] Config parse error: {}", ex.what());
        return false;
    }
}

static void CheckConfigReload()
{
    long long sz = GetFileSize(g_config_path);
    if (sz == 0) return;
    time_t mt = GetFileModTime(g_config_path);
    if (mt == g_config_last_modified && sz == g_config_last_size) return;
    LoadConfig();
}

static std::string EscapeUnsafe(const std::string& in)
{
    if (!g_db || !pmysql_real_escape_string) return in;
    std::string buf(in.size() * 2 + 1, '\0');
    unsigned long len = pmysql_real_escape_string(
        g_db, buf.data(), in.c_str(), (unsigned long)in.size());
    buf.resize(len);
    return buf;
}

static bool ExecQuery(const std::string& sql)
{
    if (!g_db) return false;
    if (pmysql_query(g_db, sql.c_str()) != 0)
    {
        Log::GetLog()->error("[Blockade] Query error: {}", pmysql_error(g_db));
        return false;
    }
    if (MYSQL_RES* res = pmysql_store_result(g_db))
        pmysql_free_result(res);
    return true;
}

static bool EstablishConnection()
{
    if (!LoadMySQLLib()) return false;

    std::string host, user, pass, name;
    unsigned int port = 3306;
    {
        std::lock_guard<std::mutex> lock(g_db_cfg_mutex);
        host = g_db_host;
        user = g_db_user;
        pass = g_db_pass;
        name = g_db_name;
        port = g_db_port;
    }

    g_db = pmysql_init(nullptr);
    if (!g_db)
    {
        if (!g_db_logged_down)
            Log::GetLog()->error("[Blockade] mysql_init failed");
        return false;
    }

    if (pmysql_options)
    {
        unsigned int timeout = 5;
        pmysql_options(g_db, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    }

    if (!pmysql_real_connect(g_db, host.c_str(), user.c_str(), pass.c_str(),
        name.c_str(), port, nullptr, 0))
    {
        if (!g_db_logged_down)
            Log::GetLog()->error("[Blockade] DB connect failed: {}", pmysql_error(g_db));
        pmysql_close(g_db);
        g_db = nullptr;
        return false;
    }

    const char* createCombat =
        "CREATE TABLE IF NOT EXISTS blockade_combat ("
        "eos_id VARCHAR(64) NOT NULL,"
        "map_name VARCHAR(128) NOT NULL,"
        "expires_at BIGINT NOT NULL DEFAULT 0,"
        "PRIMARY KEY (eos_id, map_name))";

    const char* createRaid =
        "CREATE TABLE IF NOT EXISTS blockade_raid ("
        "eos_id VARCHAR(64) NOT NULL,"
        "map_name VARCHAR(128) NOT NULL,"
        "expires_at BIGINT NOT NULL DEFAULT 0,"
        "PRIMARY KEY (eos_id, map_name))";

    if (!ExecQuery(createCombat) || !ExecQuery(createRaid))
    {
        pmysql_close(g_db);
        g_db = nullptr;
        return false;
    }

    return true;
}

static bool EnsureConnected()
{
    if (g_db)
    {
        if (!pmysql_ping) return true;
        if (pmysql_ping(g_db) == 0)
        {
            if (g_db_logged_down)
            {
                Log::GetLog()->info("[Blockade] DB connection healthy");
                g_db_logged_down = false;
            }
            return true;
        }
        pmysql_close(g_db);
        g_db = nullptr;
    }

    if (EstablishConnection())
    {
        if (g_db_logged_down)
        {
            Log::GetLog()->info("[Blockade] DB reconnected");
            g_db_logged_down = false;
        }
        else
        {
            Log::GetLog()->info("[Blockade] DB connected");
        }
        return true;
    }

    if (!g_db_logged_down)
    {
        Log::GetLog()->error("[Blockade] DB unavailable, retrying");
        g_db_logged_down = true;
    }
    return false;
}

static void DbWriteCombat(const std::string& eos, const std::string& map, long long expiresAt)
{
    const std::string eEos = EscapeUnsafe(eos);
    const std::string eMap = EscapeUnsafe(map);

    char sql[768];
    std::snprintf(sql, sizeof(sql),
        "INSERT INTO blockade_combat (eos_id, map_name, expires_at) "
        "VALUES ('%s', '%s', %lld) "
        "ON DUPLICATE KEY UPDATE expires_at=IF(VALUES(expires_at) = 0, expires_at, VALUES(expires_at))",
        eEos.c_str(), eMap.c_str(), expiresAt);

    ExecQuery(sql);
}

static void DbDeleteCombat(const std::string& eos, const std::string& map)
{
    const std::string eEos = EscapeUnsafe(eos);
    const std::string eMap = EscapeUnsafe(map);

    char sql[768];
    std::snprintf(sql, sizeof(sql),
        "DELETE FROM blockade_combat WHERE eos_id='%s' AND map_name='%s'",
        eEos.c_str(), eMap.c_str());

    ExecQuery(sql);
}

static void DbWriteRaid(const std::string& eos, const std::string& map, long long expiresAt)
{
    const std::string eEos = EscapeUnsafe(eos);
    const std::string eMap = EscapeUnsafe(map);

    char sql[768];
    std::snprintf(sql, sizeof(sql),
        "INSERT INTO blockade_raid (eos_id, map_name, expires_at) "
        "VALUES ('%s', '%s', %lld) "
        "ON DUPLICATE KEY UPDATE expires_at=IF(VALUES(expires_at) = 0, expires_at, VALUES(expires_at))",
        eEos.c_str(), eMap.c_str(), expiresAt);

    ExecQuery(sql);
}

static void DbDeleteRaid(const std::string& eos, const std::string& map)
{
    const std::string eEos = EscapeUnsafe(eos);
    const std::string eMap = EscapeUnsafe(map);

    char sql[768];
    std::snprintf(sql, sizeof(sql),
        "DELETE FROM blockade_raid WHERE eos_id='%s' AND map_name='%s'",
        eEos.c_str(), eMap.c_str());

    ExecQuery(sql);
}

static void PurgeMapRows(const std::string& map)
{
    const std::string eMap = EscapeUnsafe(map);

    char sql[512];
    std::snprintf(sql, sizeof(sql),
        "DELETE FROM blockade_combat WHERE map_name='%s'", eMap.c_str());
    ExecQuery(sql);

    std::snprintf(sql, sizeof(sql),
        "DELETE FROM blockade_raid WHERE map_name='%s'", eMap.c_str());
    ExecQuery(sql);

    Log::GetLog()->info("[Blockade] Purged stale rows for map {}", map);
}

static void FlushPending()
{
    const std::string map = MapName();
    if (map.empty()) return;

    std::unordered_map<std::string, PendingOp> combat;
    std::unordered_map<std::string, PendingOp> raid;
    {
        std::lock_guard<std::mutex> lock(g_pending_mutex);
        combat.swap(g_pending_combat);
        raid.swap(g_pending_raid);
    }

    if (g_purge_done && combat.empty() && raid.empty()) return;

    std::lock_guard<std::mutex> dbLock(g_db_mutex);

    if (!EnsureConnected())
    {
        std::lock_guard<std::mutex> lock(g_pending_mutex);
        for (const auto& kv : combat) g_pending_combat.emplace(kv.first, kv.second);
        for (const auto& kv : raid) g_pending_raid.emplace(kv.first, kv.second);
        return;
    }

    if (!g_purge_done)
    {
        PurgeMapRows(map);
        g_purge_done = true;
    }

    for (const auto& kv : combat)
    {
        if (kv.second.remove) DbDeleteCombat(kv.first, map);
        else DbWriteCombat(kv.first, map, kv.second.expiresAt);
    }

    for (const auto& kv : raid)
    {
        if (kv.second.remove) DbDeleteRaid(kv.first, map);
        else DbWriteRaid(kv.first, map, kv.second.expiresAt);
    }
}

static void RegisterCombat(AShooterPlayerController* pc, const std::string& eos, int blockSeconds)
{
    if (eos.empty() || blockSeconds <= 0) return;
    if (IsImmune(eos)) return;

    auto now = std::chrono::steady_clock::now();
    long long newExpiry = (long long)time(nullptr) + blockSeconds;
    bool isNew = false;
    bool doWrite = false;
    {
        std::lock_guard<std::mutex> lock(g_combat_mutex);
        auto it = g_combat_times.find(eos);
        if (it == g_combat_times.end())
        {
            CombatState st;
            st.lastHit = now;
            st.blockSeconds = blockSeconds;
            st.lastNotify = now;
            st.writtenExpiry = newExpiry;
            g_combat_times[eos] = st;
            isNew = true;
            doWrite = true;
        }
        else
        {
            it->second.lastHit = now;
            it->second.blockSeconds = blockSeconds;
            if (newExpiry - it->second.writtenExpiry > 2)
            {
                it->second.writtenExpiry = newExpiry;
                doWrite = true;
            }
        }
    }

    if (doWrite)
        QueueCombatWrite(eos, newExpiry);

    if (isNew)
    {
        FLinearColor red{ 1.0f, 0.2f, 0.2f, 1.0f };
        OnScreen(pc, FormatSeconds(g_combat_msg, blockSeconds), red, 6.0f);
    }
}

static void CombatTick()
{
    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return;

    auto now = std::chrono::steady_clock::now();
    FLinearColor red{ 1.0f, 0.2f, 0.2f, 1.0f };
    FLinearColor green{ 0.2f, 1.0f, 0.2f, 1.0f };

    std::vector<std::string> expired;
    {
        std::lock_guard<std::mutex> lock(g_combat_mutex);
        if (g_combat_times.empty()) return;

        auto& controllers = world->PlayerControllerListField();
        for (int i = 0; i < controllers.Num(); ++i)
        {
            AShooterPlayerController* pc = static_cast<AShooterPlayerController*>(controllers[i].Get());
            if (!pc) continue;

            std::string eos = GetEosId(pc);
            if (eos.empty()) continue;

            auto it = g_combat_times.find(eos);
            if (it == g_combat_times.end()) continue;

            int elapsed = (int)std::chrono::duration_cast<std::chrono::seconds>(now - it->second.lastHit).count();
            int remaining = it->second.blockSeconds - elapsed;

            if (remaining <= 0)
            {
                OnScreen(pc, g_combat_cleared_msg, green, 5.0f);
                continue;
            }

            int sinceNotify = (int)std::chrono::duration_cast<std::chrono::seconds>(now - it->second.lastNotify).count();
            if (sinceNotify >= g_combat_notify_interval)
            {
                it->second.lastNotify = now;
                OnScreen(pc, FormatSeconds(g_combat_msg, remaining), red, 6.0f);
            }
        }

        for (auto it = g_combat_times.begin(); it != g_combat_times.end(); )
        {
            int elapsed = (int)std::chrono::duration_cast<std::chrono::seconds>(now - it->second.lastHit).count();
            if (elapsed >= it->second.blockSeconds)
            {
                expired.push_back(it->first);
                it = g_combat_times.erase(it);
            }
            else
                ++it;
        }
    }

    for (const auto& eos : expired)
        QueueCombatDelete(eos);
}

static void UpdateRaidZone(double sx, double sy)
{
    std::lock_guard<std::mutex> lock(g_raid_mutex);
    auto now = std::chrono::steady_clock::now();
    long long nowUnix = (long long)time(nullptr);

    for (auto& zone : g_raid_zones)
    {
        double dx = sx - zone.x;
        double dy = sy - zone.y;
        if (std::sqrt(dx * dx + dy * dy) < g_raid_block_radius)
        {
            zone.lastHit = now;
            zone.lastHitUnix = nowUnix;
            return;
        }
    }

    g_raid_zones.push_back({ sx, sy, now, nowUnix });
}

static void RaidTick()
{
    auto now = std::chrono::steady_clock::now();
    int zoneLife = g_raid_block_seconds;

    FLinearColor red{ 1.0f, 0.2f, 0.2f, 1.0f };
    FLinearColor green{ 0.2f, 1.0f, 0.2f, 1.0f };

    std::vector<std::pair<std::string, long long>> toWrite;
    std::vector<std::string> toDelete;

    {
        std::lock_guard<std::mutex> lock(g_raid_mutex);

        for (auto it = g_raid_zones.begin(); it != g_raid_zones.end(); )
        {
            int elapsed = (int)std::chrono::duration_cast<std::chrono::seconds>(now - it->lastHit).count();
            if (zoneLife > 0 && elapsed >= zoneLife)
                it = g_raid_zones.erase(it);
            else
                ++it;
        }

        std::unordered_set<std::string> currentlyBlocked;
        if (g_raid_block_enabled && !g_raid_zones.empty())
        {
            UWorld* world = AsaApi::GetApiUtils().GetWorld();
            if (world)
            {
                auto& controllers = world->PlayerControllerListField();
                for (int i = 0; i < controllers.Num(); ++i)
                {
                    AShooterPlayerController* pc = static_cast<AShooterPlayerController*>(controllers[i].Get());
                    if (!pc) continue;

                    APawn* pawn = pc->PawnField().Get();
                    if (!pawn) continue;

                    USceneComponent* root = pawn->RootComponentField().Get();
                    if (!root) continue;

                    auto loc = root->RelativeLocationField();
                    std::string eos = GetEosId(pc);
                    if (eos.empty()) continue;
                    if (IsImmune(eos)) continue;

                    bool inZone = false;
                    long long newExpiry = 0;
                    for (const auto& zone : g_raid_zones)
                    {
                        double dx = zone.x - loc.X;
                        double dy = zone.y - loc.Y;
                        if (std::sqrt(dx * dx + dy * dy) <= g_raid_block_radius)
                        {
                            inZone = true;
                            long long zoneExpiry = zone.lastHitUnix + g_raid_block_seconds;
                            if (zoneExpiry > newExpiry) newExpiry = zoneExpiry;
                        }
                    }
                    if (!inZone) continue;

                    currentlyBlocked.insert(eos);

                    auto bit = g_raid_blocked_players.find(eos);
                    if (bit == g_raid_blocked_players.end())
                    {
                        RaidState st;
                        st.lastNotify = now;
                        st.writtenExpiry = newExpiry;
                        g_raid_blocked_players[eos] = st;
                        OnScreen(pc, g_raid_msg, red, 6.0f);
                        toWrite.push_back({ eos, newExpiry });
                    }
                    else
                    {
                        int sinceNotify = (int)std::chrono::duration_cast<std::chrono::seconds>(now - bit->second.lastNotify).count();
                        if (sinceNotify >= g_raid_notify_interval)
                        {
                            bit->second.lastNotify = now;
                            OnScreen(pc, g_raid_msg, red, 6.0f);
                        }
                        if (newExpiry != bit->second.writtenExpiry)
                        {
                            bit->second.writtenExpiry = newExpiry;
                            toWrite.push_back({ eos, newExpiry });
                        }
                    }
                }
            }
        }

        for (auto it = g_raid_blocked_players.begin(); it != g_raid_blocked_players.end(); )
        {
            if (currentlyBlocked.count(it->first) == 0)
            {
                FString fEos(it->first.c_str());
                AShooterPlayerController* pc = AsaApi::GetApiUtils().FindPlayerFromEOSID(fEos);
                if (pc) OnScreen(pc, g_raid_cleared_msg, green, 5.0f);
                toDelete.push_back(it->first);
                it = g_raid_blocked_players.erase(it);
            }
            else
                ++it;
        }
    }

    for (const auto& w : toWrite)
        QueueRaidWrite(w.first, w.second);
    for (const auto& eos : toDelete)
        QueueRaidDelete(eos);
}

DECLARE_HOOK(APrimalCharacter_TakeDamage, float, APrimalCharacter*, float, FDamageEvent*, AController*, AActor*);
DECLARE_HOOK(APrimalStructure_TakeDamage, float, APrimalStructure*, float, FDamageEvent*, AController*, AActor*);

static int GetAttackerTeam(AController* instigator, AActor* damageCauser)
{
    if (instigator)
    {
        APawn* pawn = instigator->PawnField().Get();
        if (pawn) return pawn->TargetingTeamField();
    }
    if (damageCauser) return damageCauser->TargetingTeamField();
    return 0;
}

static bool IsAttackerWild(AController* instigator, AActor* damageCauser)
{
    if (instigator && instigator->IsA(AShooterPlayerController::GetPrivateStaticClass()))
        return false;

    if (damageCauser && damageCauser->IsA(APrimalDinoCharacter::GetPrivateStaticClass()))
    {
        auto* dino = static_cast<APrimalDinoCharacter*>(damageCauser);
        return dino->TamingTeamIDField() == 0;
    }

    if (damageCauser && damageCauser->IsA(APrimalStructure::GetPrivateStaticClass()))
        return false;

    return true;
}

float Hook_APrimalCharacter_TakeDamage(APrimalCharacter* _this, float Damage, FDamageEvent* DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
    float before = GetCurrentHealth(_this);
    float result = APrimalCharacter_TakeDamage_original(_this, Damage, DamageEvent, EventInstigator, DamageCauser);
    float after = GetCurrentHealth(_this);

    if (!_this) return result;
    if (after >= before) return result;
    if (!EventInstigator && !DamageCauser) return result;

    int victimTeam = _this->TargetingTeamField();
    int attackerTeam = GetAttackerTeam(EventInstigator, DamageCauser);

    if (victimTeam == attackerTeam || attackerTeam == 0) return result;
    if (IsAttackerWild(EventInstigator, DamageCauser)) return result;
    if (_this->IsA(APrimalDinoCharacter::GetPrivateStaticClass())
        && static_cast<APrimalDinoCharacter*>(_this)->TamingTeamIDField() == 0)
        return result;

    bool attackerIsPlayer = EventInstigator && EventInstigator->IsA(AShooterPlayerController::GetPrivateStaticClass());
    bool attackerIsDino = DamageCauser && DamageCauser->IsA(APrimalDinoCharacter::GetPrivateStaticClass());

    if (!attackerIsPlayer && !attackerIsDino)
    {
        if (g_raid_block_enabled)
        {
            USceneComponent* root = _this->RootComponentField().Get();
            if (root)
            {
                auto loc = root->RelativeLocationField();
                UpdateRaidZone(loc.X, loc.Y);
            }
        }
        return result;
    }

    if (!g_combat_block_enabled) return result;

    if (_this->IsA(AShooterCharacter::StaticClass()))
    {
        AShooterCharacter* sc = static_cast<AShooterCharacter*>(_this);
        AController* ctrl = sc->ControllerField().Get();
        if (ctrl && ctrl->IsA(AShooterPlayerController::GetPrivateStaticClass()))
        {
            auto* vpc = static_cast<AShooterPlayerController*>(ctrl);
            RegisterCombat(vpc, GetEosId(vpc), g_combat_block_seconds);
        }
    }

    if (EventInstigator && EventInstigator->IsA(AShooterPlayerController::GetPrivateStaticClass()))
    {
        auto* apc = static_cast<AShooterPlayerController*>(EventInstigator);
        RegisterCombat(apc, GetEosId(apc), g_combat_block_seconds);
    }

    return result;
}

float Hook_APrimalStructure_TakeDamage(APrimalStructure* _this, float Damage, FDamageEvent* DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
    float before = _this ? _this->HealthField() : 0.0f;
    float result = APrimalStructure_TakeDamage_original(_this, Damage, DamageEvent, EventInstigator, DamageCauser);
    float after = _this ? _this->HealthField() : 0.0f;

    if (!_this) return result;
    if (!g_raid_block_enabled) return result;
    if (!EventInstigator && !DamageCauser) return result;

    int structTeam = _this->TargetingTeamField();
    int attackerTeam = GetAttackerTeam(EventInstigator, DamageCauser);

    if (after >= before) return result;
    if (structTeam == 0) return result;
    if (attackerTeam != 0 && attackerTeam == structTeam) return result;

    bool wildDino = DamageCauser && DamageCauser->IsA(APrimalDinoCharacter::GetPrivateStaticClass())
        && static_cast<APrimalDinoCharacter*>(DamageCauser)->TamingTeamIDField() == 0
        && !(EventInstigator && EventInstigator->IsA(AShooterPlayerController::GetPrivateStaticClass()));
    if (wildDino) return result;

    USceneComponent* sRoot = _this->RootComponentField().Get();
    if (!sRoot) return result;

    auto sLoc = sRoot->RelativeLocationField();
    UpdateRaidZone(sLoc.X, sLoc.Y);
    return result;
}

using Tick_t = void(*)(AShooterGameMode*, float);
static Tick_t Original_Tick = nullptr;

static void Detour_Tick(AShooterGameMode* gm, float dt)
{
    Original_Tick(gm, dt);

    if (!g_map_name_ready.load())
        ResolveMapName();

    auto now = std::chrono::steady_clock::now();

    auto sinceCheck = std::chrono::duration_cast<std::chrono::seconds>(now - g_last_config_check).count();
    if (sinceCheck >= 10)
    {
        g_last_config_check = now;
        CheckConfigReload();
    }

    auto sinceSweep = std::chrono::duration_cast<std::chrono::seconds>(now - g_last_sweep).count();
    if (sinceSweep >= 1)
    {
        g_last_sweep = now;
        CombatTick();
        RaidTick();
    }
}

static void WorkerLoop()
{
    while (g_worker_running.load())
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!g_worker_running.load()) break;
        FlushPending();
    }

    FlushPending();
}

static void PluginInit()
{
    Log::Get().Init("Blockade");

    if (!LoadConfig())
        Log::GetLog()->error("[Blockade] Failed to load config");

    LoadMySQLLib();

    g_last_config_check = std::chrono::steady_clock::now();
    g_last_sweep = std::chrono::steady_clock::now();

    AsaApi::GetHooks().SetHook(
        "APrimalCharacter.TakeDamage(float,FDamageEvent&,AController*,AActor*)",
        (LPVOID)&Hook_APrimalCharacter_TakeDamage,
        (LPVOID*)&APrimalCharacter_TakeDamage_original);

    AsaApi::GetHooks().SetHook(
        "APrimalStructure.TakeDamage(float,FDamageEvent&,AController*,AActor*)",
        (LPVOID)&Hook_APrimalStructure_TakeDamage,
        (LPVOID*)&APrimalStructure_TakeDamage_original);

    AsaApi::GetHooks().SetHook(
        "AShooterGameMode.Tick(float)",
        (LPVOID)&Detour_Tick,
        (LPVOID*)&Original_Tick);

    g_worker_running.store(true);
    g_worker_thread = std::thread(WorkerLoop);

    Log::GetLog()->info("[Blockade] Loaded");
}

static void PluginUnload()
{
    g_worker_running.store(false);
    if (g_worker_thread.joinable())
        g_worker_thread.join();

    AsaApi::GetHooks().DisableHook(
        "APrimalCharacter.TakeDamage(float,FDamageEvent&,AController*,AActor*)",
        (LPVOID)&Hook_APrimalCharacter_TakeDamage);

    AsaApi::GetHooks().DisableHook(
        "APrimalStructure.TakeDamage(float,FDamageEvent&,AController*,AActor*)",
        (LPVOID)&Hook_APrimalStructure_TakeDamage);

    AsaApi::GetHooks().DisableHook(
        "AShooterGameMode.Tick(float)",
        (LPVOID)&Detour_Tick);

    {
        std::lock_guard<std::mutex> lock(g_db_mutex);
        if (g_db)
        {
            pmysql_close(g_db);
            g_db = nullptr;
        }
    }

    Log::GetLog()->info("[Blockade] Unloaded");
}

extern "C" __declspec(dllexport) bool Blockade_IsCombatBlocked(const FString& eosId, int& outRemainingSeconds)
{
    outRemainingSeconds = 0;
    if (!g_combat_block_enabled) return false;

    const std::string eos = FStr(eosId);
    if (eos.empty()) return false;

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_combat_mutex);

    auto it = g_combat_times.find(eos);
    if (it == g_combat_times.end()) return false;

    const int elapsed = (int)std::chrono::duration_cast<std::chrono::seconds>(now - it->second.lastHit).count();
    const int remaining = it->second.blockSeconds - elapsed;
    if (remaining <= 0) return false;

    outRemainingSeconds = remaining;
    return true;
}

extern "C" __declspec(dllexport) bool Blockade_IsRaidBlocked(const FString& eosId)
{
    if (!g_raid_block_enabled) return false;

    const std::string eos = FStr(eosId);
    if (eos.empty()) return false;

    const long long now = (long long)time(nullptr);
    std::lock_guard<std::mutex> lock(g_raid_mutex);

    auto it = g_raid_blocked_players.find(eos);
    if (it == g_raid_blocked_players.end()) return false;

    return it->second.writtenExpiry > now;
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { PluginInit(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->critical("[Blockade] Init exception: {}", e.what());
    }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { PluginUnload(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->error("[Blockade] Unload exception: {}", e.what());
    }
}