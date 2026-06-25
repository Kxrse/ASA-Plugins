/*
KxrsedBlocking - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * KxrsedBlocking - ASA Plugin
 *
 * Hooks:
 *   APrimalCharacter.TakeDamage(float,FDamageEvent&,AController*,AActor*) - PvP combat detection, gated on real health loss
 *   APrimalStructure.TakeDamage(float,FDamageEvent&,AController*,AActor*) - raid zone detection on real enemy structure damage
 *   AShooterGameMode.Tick(float) - config hot-reload (10s), combat and raid sweep (1s)
 *
 * Config:
 *   DbHost, DbPort, DbUser, DbPass, DbName - MariaDB connection (shared cluster DB)
 *   RaidBlockRadius - raid zone radius in world units
 *   ImmuneEosIds - EOS IDs exempt from both combat and raid block
 *   CombatBlockEnabled, CombatBlockSeconds, CombatBlockMessage ({seconds} token), CombatBlockClearedMessage, CombatBlockNotifyInterval
 *   RaidBlockEnabled, RaidBlockSeconds, RaidBlockMessage, RaidBlockClearedMessage, RaidBlockNotifyInterval
 *
 * Table:
 *   player_blocks - PK (eos_id, map_name, block_type)
 *   Columns: eos_id VARCHAR(64), map_name VARCHAR(128), block_type VARCHAR(16), expires_at BIGINT
 *
 * Owns combat and raid block state for the cluster, applied globally except for ImmuneEosIds. Combat
 * fires only on real victim health loss. Raid zones form on real enemy structure damage and expire
 * after RaidBlockSeconds; players inside an active zone are raid blocked. Both show an on-screen
 * marker and are written to player_blocks transition-only (upsert on enter, throttled refresh, delete
 * on exit), scoped by map, for other plugins to read on demand.
 */

#include <API/ARK/Ark.h>
#include <json.hpp>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <mutex>
#include <ctime>
#include <cstdio>
#include <cmath>
#include <sys/stat.h>

#pragma comment(lib, "AsaApi.lib")
#pragma warning(disable: 4191)

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
            Log::GetLog()->info("[KxrsedBlocking] Loaded DB library: {}", candidates[i]);
            break;
        }
    }

    if (!g_mysql_module)
    {
        Log::GetLog()->error("[KxrsedBlocking] Could not find libmariadb.dll or libmysql.dll");
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

    if (!pmysql_init || !pmysql_real_connect || !pmysql_close ||
        !pmysql_query || !pmysql_error || !pmysql_real_escape_string || !pmysql_fetch_row)
    {
        Log::GetLog()->error("[KxrsedBlocking] Failed to resolve required DB functions");
        return false;
    }

    g_mysql_loaded = true;
    return true;
}

static const std::string g_config_path = "ArkApi/Plugins/KxrsedBlocking/config.json";

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
static std::chrono::steady_clock::time_point g_last_combat_tick;

static std::string g_map_name;

static MYSQL* g_db = nullptr;
static std::mutex g_db_mutex;

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
};

struct RaidState
{
    std::chrono::steady_clock::time_point lastNotify;
    long long writtenExpiry = 0;
};

static std::vector<RaidZone> g_raid_zones;
static std::unordered_map<std::string, RaidState> g_raid_blocked_players;
static std::mutex g_raid_mutex;

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

static const std::string& MapName()
{
    if (g_map_name.empty())
    {
        UWorld* world = AsaApi::GetApiUtils().GetWorld();
        if (world)
        {
            FString m;
            world->GetMapName(&m);
            g_map_name = FStr(m);
        }
    }
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
        Log::GetLog()->error("[KxrsedBlocking] Query error: {}", pmysql_error(g_db));
        return false;
    }
    if (MYSQL_RES* res = pmysql_store_result(g_db))
        pmysql_free_result(res);
    return true;
}

static void DbWriteBlock(const std::string& eos, const char* type, long long expiresAt)
{
    const std::string eEos = EscapeUnsafe(eos);
    const std::string eMap = EscapeUnsafe(MapName());

    char sql[384];
    std::snprintf(sql, sizeof(sql),
        "INSERT INTO player_blocks (eos_id, map_name, block_type, expires_at) "
        "VALUES ('%s', '%s', '%s', %lld) "
        "ON DUPLICATE KEY UPDATE expires_at=VALUES(expires_at)",
        eEos.c_str(), eMap.c_str(), type, expiresAt);

    std::lock_guard<std::mutex> lock(g_db_mutex);
    ExecQuery(sql);
}

static void DbDeleteBlock(const std::string& eos, const char* type)
{
    const std::string eEos = EscapeUnsafe(eos);
    const std::string eMap = EscapeUnsafe(MapName());

    char sql[256];
    std::snprintf(sql, sizeof(sql),
        "DELETE FROM player_blocks WHERE eos_id='%s' AND map_name='%s' AND block_type='%s'",
        eEos.c_str(), eMap.c_str(), type);

    std::lock_guard<std::mutex> lock(g_db_mutex);
    ExecQuery(sql);
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
        Log::GetLog()->error("[KxrsedBlocking] Cannot open config: {}", g_config_path);
        return false;
    }

    try
    {
        nlohmann::json j;
        file >> j;

        g_db_host = j.value("DbHost", std::string("127.0.0.1"));
        g_db_port = j.value("DbPort", 3306u);
        g_db_user = j.value("DbUser", std::string(""));
        g_db_pass = j.value("DbPass", std::string(""));
        g_db_name = j.value("DbName", std::string(""));
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
        Log::GetLog()->info("[KxrsedBlocking] Config loaded, {} immune", g_immune_eos.size());
        return true;
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[KxrsedBlocking] Config parse error: {}", ex.what());
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
        DbWriteBlock(eos, "combat", newExpiry);

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
        DbDeleteBlock(eos, "combat");
}

static void UpdateRaidZone(double sx, double sy)
{
    std::lock_guard<std::mutex> lock(g_raid_mutex);
    auto now = std::chrono::steady_clock::now();

    for (auto& zone : g_raid_zones)
    {
        double dx = sx - zone.x;
        double dy = sy - zone.y;
        if (std::sqrt(dx * dx + dy * dy) < g_raid_block_radius)
        {
            zone.lastHit = now;
            return;
        }
    }

    g_raid_zones.push_back({ sx, sy, now });
}

static void RaidTick()
{
    auto now = std::chrono::steady_clock::now();
    long long nowUnix = (long long)time(nullptr);
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

                    USceneComponent* root = pawn->RootComponentField();
                    if (!root) continue;

                    auto loc = root->RelativeLocationField();
                    std::string eos = GetEosId(pc);
                    if (eos.empty()) continue;
                    if (IsImmune(eos)) continue;

                    bool inZone = false;
                    for (const auto& zone : g_raid_zones)
                    {
                        double dx = zone.x - loc.X;
                        double dy = zone.y - loc.Y;
                        if (std::sqrt(dx * dx + dy * dy) <= g_raid_block_radius)
                        {
                            inZone = true;
                            break;
                        }
                    }
                    if (!inZone) continue;

                    currentlyBlocked.insert(eos);
                    long long newExpiry = nowUnix + g_raid_block_seconds;

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
                        if (newExpiry - bit->second.writtenExpiry > 2)
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
        DbWriteBlock(w.first, "raid", w.second);
    for (const auto& eos : toDelete)
        DbDeleteBlock(eos, "raid");
}

static bool InitDb()
{
    if (!LoadMySQLLib()) return false;

    std::lock_guard<std::mutex> lock(g_db_mutex);

    if (g_db)
    {
        pmysql_close(g_db);
        g_db = nullptr;
    }

    g_db = pmysql_init(nullptr);
    if (!g_db)
    {
        Log::GetLog()->error("[KxrsedBlocking] mysql_init failed");
        return false;
    }

    if (!pmysql_real_connect(g_db, g_db_host.c_str(), g_db_user.c_str(), g_db_pass.c_str(),
        g_db_name.c_str(), g_db_port, nullptr, 0))
    {
        Log::GetLog()->error("[KxrsedBlocking] DB connect failed: {}", pmysql_error(g_db));
        pmysql_close(g_db);
        g_db = nullptr;
        return false;
    }

    const char* create =
        "CREATE TABLE IF NOT EXISTS player_blocks ("
        "eos_id VARCHAR(64) NOT NULL,"
        "map_name VARCHAR(128) NOT NULL,"
        "block_type VARCHAR(16) NOT NULL,"
        "expires_at BIGINT NOT NULL DEFAULT 0,"
        "PRIMARY KEY (eos_id, map_name, block_type))";

    if (pmysql_query(g_db, create))
    {
        Log::GetLog()->error("[KxrsedBlocking] Create table failed: {}", pmysql_error(g_db));
        return false;
    }

    Log::GetLog()->info("[KxrsedBlocking] DB connected");
    return true;
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
    if (instigator && instigator->IsA(AShooterPlayerController::StaticClass()))
        return false;

    if (damageCauser && damageCauser->IsA(APrimalDinoCharacter::StaticClass()))
    {
        auto* dino = static_cast<APrimalDinoCharacter*>(damageCauser);
        return dino->TamingTeamIDField() == 0;
    }

    if (damageCauser && damageCauser->IsA(APrimalStructure::StaticClass()))
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
    if (_this->IsA(APrimalDinoCharacter::StaticClass())
        && static_cast<APrimalDinoCharacter*>(_this)->TamingTeamIDField() == 0)
        return result;

    bool attackerIsPlayer = EventInstigator && EventInstigator->IsA(AShooterPlayerController::StaticClass());
    bool attackerIsDino = DamageCauser && DamageCauser->IsA(APrimalDinoCharacter::StaticClass());

    if (!attackerIsPlayer && !attackerIsDino)
    {
        if (g_raid_block_enabled)
        {
            USceneComponent* root = _this->RootComponentField();
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
        if (ctrl && ctrl->IsA(AShooterPlayerController::StaticClass()))
        {
            auto* vpc = static_cast<AShooterPlayerController*>(ctrl);
            RegisterCombat(vpc, GetEosId(vpc), g_combat_block_seconds);
        }
    }

    if (EventInstigator && EventInstigator->IsA(AShooterPlayerController::StaticClass()))
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

    USceneComponent* sRoot = _this->RootComponentField();
    double sx = 0.0, sy = 0.0;
    if (sRoot)
    {
        auto sLoc = sRoot->RelativeLocationField();
        sx = sLoc.X;
        sy = sLoc.Y;
    }

    if (after >= before) return result;
    if (structTeam == 0) return result;
    if (attackerTeam != 0 && attackerTeam == structTeam) return result;

    bool wildDino = DamageCauser && DamageCauser->IsA(APrimalDinoCharacter::StaticClass())
        && static_cast<APrimalDinoCharacter*>(DamageCauser)->TamingTeamIDField() == 0
        && !(EventInstigator && EventInstigator->IsA(AShooterPlayerController::StaticClass()));
    if (wildDino) return result;

    if (!sRoot) return result;

    UpdateRaidZone(sx, sy);
    return result;
}

using Tick_t = void(*)(AShooterGameMode*, float);
static Tick_t Original_Tick = nullptr;

static void Detour_Tick(AShooterGameMode* gm, float dt)
{
    Original_Tick(gm, dt);
    auto now = std::chrono::steady_clock::now();

    auto sinceCheck = std::chrono::duration_cast<std::chrono::seconds>(now - g_last_config_check).count();
    if (sinceCheck >= 10)
    {
        g_last_config_check = now;
        CheckConfigReload();
    }

    auto sinceCombat = std::chrono::duration_cast<std::chrono::seconds>(now - g_last_combat_tick).count();
    if (sinceCombat >= 1)
    {
        g_last_combat_tick = now;
        CombatTick();
        RaidTick();
    }
}

static void PluginInit()
{
    Log::Get().Init("KxrsedBlocking");

    if (!LoadConfig())
        Log::GetLog()->error("[KxrsedBlocking] Failed to load config");

    InitDb();

    g_last_config_check = std::chrono::steady_clock::now();
    g_last_combat_tick = std::chrono::steady_clock::now();

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

    Log::GetLog()->info("[KxrsedBlocking] Loaded");
}

static void PluginUnload()
{
    AsaApi::GetHooks().DisableHook(
        "APrimalCharacter.TakeDamage(float,FDamageEvent&,AController*,AActor*)",
        (LPVOID)&Hook_APrimalCharacter_TakeDamage);

    AsaApi::GetHooks().DisableHook(
        "APrimalStructure.TakeDamage(float,FDamageEvent&,AController*,AActor*)",
        (LPVOID)&Hook_APrimalStructure_TakeDamage);

    AsaApi::GetHooks().DisableHook(
        "AShooterGameMode.Tick(float)",
        (LPVOID)&Detour_Tick);

    if (g_db)
    {
        pmysql_close(g_db);
        g_db = nullptr;
    }

    Log::GetLog()->info("[KxrsedBlocking] Unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { PluginInit(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->critical("[KxrsedBlocking] Init exception: {}", e.what());
    }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { PluginUnload(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->error("[KxrsedBlocking] Unload exception: {}", e.what());
    }
}