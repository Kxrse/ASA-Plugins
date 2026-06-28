/*
TribeWarden - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

#include <API/ARK/Ark.h>
#include <json.hpp>
#include <Windows.h>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <random>
#include <cctype>
#include <algorithm>
#include <cstdio>
#include <sstream>

// =============================================================================
// MariaDB - Dynamic Load
// =============================================================================

typedef struct st_mysql     MYSQL;
typedef struct st_mysql_res MYSQL_RES;
typedef char** MYSQL_ROW;

typedef MYSQL* (__stdcall* mysql_init_t)              (MYSQL*);
typedef MYSQL* (__stdcall* mysql_real_connect_t)      (MYSQL*, const char*, const char*, const char*, const char*, unsigned int, const char*, unsigned long);
typedef void(__stdcall* mysql_close_t)             (MYSQL*);
typedef int(__stdcall* mysql_query_t)             (MYSQL*, const char*);
typedef MYSQL_RES* (__stdcall* mysql_store_result_t)  (MYSQL*);
typedef void(__stdcall* mysql_free_result_t)       (MYSQL_RES*);
typedef MYSQL_ROW(__stdcall* mysql_fetch_row_t)      (MYSQL_RES*);
typedef const char* (__stdcall* mysql_error_t)        (MYSQL*);
typedef unsigned long(__stdcall* mysql_real_escape_string_t)(MYSQL*, char*, const char*, unsigned long);
typedef int(__stdcall* mysql_options_t)           (MYSQL*, int, const void*);

#define MYSQL_OPT_CONNECT_TIMEOUT 0

static HMODULE                    g_mysql_module = nullptr;
static mysql_init_t               pmysql_init = nullptr;
static mysql_real_connect_t       pmysql_real_connect = nullptr;
static mysql_close_t              pmysql_close = nullptr;
static mysql_query_t              pmysql_query = nullptr;
static mysql_store_result_t       pmysql_store_result = nullptr;
static mysql_free_result_t        pmysql_free_result = nullptr;
static mysql_fetch_row_t          pmysql_fetch_row = nullptr;
static mysql_error_t              pmysql_error = nullptr;
static mysql_real_escape_string_t pmysql_real_escape_string = nullptr;
static mysql_options_t            pmysql_options = nullptr;
static bool                       g_mysql_loaded = false;

static bool LoadMySQLLib()
{
    if (g_mysql_loaded) return true;
    const char* candidates[] = {
        "libmariadb.dll", ".\\libmariadb.dll", "ArkApi\\Plugins\\libmariadb.dll",
        "libmysql.dll", ".\\libmysql.dll", nullptr
    };
    for (int i = 0; candidates[i]; ++i) {
        g_mysql_module = LoadLibraryA(candidates[i]);
        if (g_mysql_module) { Log::GetLog()->info("[TribeWarden] Loaded DB library: {}", candidates[i]); break; }
    }
    if (!g_mysql_module) { Log::GetLog()->error("[TribeWarden] Could not find libmariadb.dll or libmysql.dll"); return false; }
    pmysql_init = (mysql_init_t)GetProcAddress(g_mysql_module, "mysql_init");
    pmysql_real_connect = (mysql_real_connect_t)GetProcAddress(g_mysql_module, "mysql_real_connect");
    pmysql_close = (mysql_close_t)GetProcAddress(g_mysql_module, "mysql_close");
    pmysql_query = (mysql_query_t)GetProcAddress(g_mysql_module, "mysql_query");
    pmysql_store_result = (mysql_store_result_t)GetProcAddress(g_mysql_module, "mysql_store_result");
    pmysql_free_result = (mysql_free_result_t)GetProcAddress(g_mysql_module, "mysql_free_result");
    pmysql_fetch_row = (mysql_fetch_row_t)GetProcAddress(g_mysql_module, "mysql_fetch_row");
    pmysql_error = (mysql_error_t)GetProcAddress(g_mysql_module, "mysql_error");
    pmysql_real_escape_string = (mysql_real_escape_string_t)GetProcAddress(g_mysql_module, "mysql_real_escape_string");
    pmysql_options = (mysql_options_t)GetProcAddress(g_mysql_module, "mysql_options");
    if (!pmysql_init || !pmysql_real_connect || !pmysql_close || !pmysql_query || !pmysql_error || !pmysql_real_escape_string) {
        Log::GetLog()->error("[TribeWarden] Failed to resolve required DB functions"); return false;
    }
    g_mysql_loaded = true;
    return true;
}

// =============================================================================
// Configuration
// =============================================================================

static std::string  g_message_color = "0.902,0.365,0.137,1";
static std::string  g_db_host = "localhost";
static unsigned int g_db_port = 3306;
static std::string  g_db_user;
static std::string  g_db_pass;
static std::string  g_db_name;

static bool LoadConfig()
{
    const std::string path = "ArkApi/Plugins/TribeWarden/config.json";
    std::ifstream f(path);
    if (!f.is_open()) { Log::GetLog()->error("[TribeWarden] Cannot open config: {}", path); return false; }
    try {
        nlohmann::json j; f >> j;
        g_message_color = j.value("message_color", "0.902,0.365,0.137,1");
        g_db_host = j.value("DbHost", "localhost");
        g_db_port = j.value("DbPort", 3306);
        g_db_user = j.value("DbUser", "");
        g_db_pass = j.value("DbPassword", "");
        g_db_name = j.value("DbName", "");
    }
    catch (const std::exception& ex) { Log::GetLog()->error("[TribeWarden] Config parse error: {}", ex.what()); return false; }
    if (g_db_user.empty() || g_db_name.empty()) { Log::GetLog()->error("[TribeWarden] Config requires DbUser and DbName"); return false; }
    Log::GetLog()->info("[TribeWarden] Config loaded");
    return true;
}

// =============================================================================
// Database
// =============================================================================

static MYSQL* g_mysql = nullptr;
static std::mutex g_db_mutex;

static std::string EscapeUnsafe(const std::string& in)
{
    if (!g_mysql || !pmysql_real_escape_string) return in;
    std::string buf(in.size() * 2 + 1, '\0');
    unsigned long len = pmysql_real_escape_string(g_mysql, buf.data(), in.c_str(), (unsigned long)in.size());
    buf.resize(len);
    return buf;
}

static bool ExecQuery(const std::string& sql)
{
    if (!g_mysql) return false;
    if (pmysql_query(g_mysql, sql.c_str()) != 0) { Log::GetLog()->error("[TribeWarden] Query error: {}", pmysql_error(g_mysql)); return false; }
    if (MYSQL_RES* res = pmysql_store_result(g_mysql)) pmysql_free_result(res);
    return true;
}

static std::unordered_set<std::string> g_tribe_cache;
static std::mutex                      g_tribe_cache_mutex;

static bool InitDatabase()
{
    if (!LoadMySQLLib()) return false;
    g_mysql = pmysql_init(nullptr);
    if (!g_mysql) { Log::GetLog()->error("[TribeWarden] mysql_init returned null"); return false; }
    unsigned int timeout = 10;
    pmysql_options(g_mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    if (!pmysql_real_connect(g_mysql, g_db_host.c_str(), g_db_user.c_str(), g_db_pass.c_str(), g_db_name.c_str(), g_db_port, nullptr, 0)) {
        Log::GetLog()->error("[TribeWarden] DB connect failed: {}", pmysql_error(g_mysql));
        pmysql_close(g_mysql); g_mysql = nullptr; return false;
    }
    bool ok = true;
    ok &= ExecQuery(
        "CREATE TABLE IF NOT EXISTS tribes ("
        "  tribe_name VARCHAR(128) NOT NULL,"
        "  tribe_id   INT          NOT NULL DEFAULT 0,"
        "  map_name   VARCHAR(128) NOT NULL DEFAULT '',"
        "  PRIMARY KEY (tribe_name, map_name)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    ok &= ExecQuery(
        "CREATE TABLE IF NOT EXISTS survivor_tribes ("
        "  survivor_id BIGINT UNSIGNED NOT NULL,"
        "  map_name    VARCHAR(128)    NOT NULL,"
        "  tribe_name  VARCHAR(128)    NOT NULL DEFAULT '',"
        "  PRIMARY KEY (survivor_id, map_name)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    ok &= ExecQuery(
        "CREATE TABLE IF NOT EXISTS tribe_memberships ("
        "  survivor_id BIGINT UNSIGNED NOT NULL,"
        "  tribe_name  VARCHAR(128)    NOT NULL DEFAULT '',"
        "  PRIMARY KEY (survivor_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    ok &= ExecQuery(
        "CREATE TABLE IF NOT EXISTS pending_kicks ("
        "  survivor_id BIGINT UNSIGNED NOT NULL,"
        "  tribe_id    INT             NOT NULL,"
        "  map_name    VARCHAR(128)    NOT NULL,"
        "  PRIMARY KEY (survivor_id, map_name)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    if (!ok) { Log::GetLog()->error("[TribeWarden] Failed to create tables"); pmysql_close(g_mysql); g_mysql = nullptr; return false; }
    if (pmysql_fetch_row && pmysql_query(g_mysql, "SELECT tribe_name FROM tribes") == 0) {
        if (MYSQL_RES* res = pmysql_store_result(g_mysql)) {
            std::lock_guard<std::mutex> lock(g_tribe_cache_mutex);
            while (MYSQL_ROW row = pmysql_fetch_row(res)) { if (row[0]) g_tribe_cache.insert(row[0]); }
            pmysql_free_result(res);
            Log::GetLog()->info("[TribeWarden] Cache seeded with {} tribes", g_tribe_cache.size());
        }
    }
    Log::GetLog()->info("[TribeWarden] Database ready");
    return true;
}

static void CloseDatabase() { if (g_mysql) { pmysql_close(g_mysql); g_mysql = nullptr; } }

static void UpsertTribe(int tribeId, const std::string& tribeName, const std::string& mapName)
{
    if (tribeName.empty()) return;
    { std::lock_guard<std::mutex> lock(g_tribe_cache_mutex); g_tribe_cache.insert(tribeName); }
    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!g_mysql) return;
    char sql[512]{};
    std::snprintf(sql, sizeof(sql),
        "INSERT INTO tribes (tribe_name, tribe_id, map_name) VALUES ('%s', %d, '%s') ON DUPLICATE KEY UPDATE tribe_id = VALUES(tribe_id);",
        EscapeUnsafe(tribeName).c_str(), tribeId, EscapeUnsafe(mapName).c_str());
    ExecQuery(sql);
}

static void UpdateTribeNameByOldName(const std::string& oldName, const std::string& newName)
{
    if (oldName.empty() || newName.empty() || oldName == newName) return;
    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!g_mysql) return;
    const std::string safeOld = EscapeUnsafe(oldName);
    const std::string safeNew = EscapeUnsafe(newName);
    char sql[512]{};
    std::snprintf(sql, sizeof(sql), "UPDATE tribes SET tribe_name = '%s' WHERE tribe_name = '%s';", safeNew.c_str(), safeOld.c_str()); ExecQuery(sql);
    std::snprintf(sql, sizeof(sql), "UPDATE survivor_tribes SET tribe_name = '%s' WHERE tribe_name = '%s';", safeNew.c_str(), safeOld.c_str()); ExecQuery(sql);
    std::snprintf(sql, sizeof(sql), "UPDATE tribe_memberships SET tribe_name = '%s' WHERE tribe_name = '%s';", safeNew.c_str(), safeOld.c_str()); ExecQuery(sql);
}

static void UpdateTribeNameById(int tribeId, const std::string& newName)
{
    if (tribeId == 0 || newName.empty()) return;
    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!g_mysql) return;
    char sql[512]{};
    std::snprintf(sql, sizeof(sql), "UPDATE tribes SET tribe_name = '%s' WHERE tribe_id = %d;", EscapeUnsafe(newName).c_str(), tribeId);
    ExecQuery(sql);
}

static void UpsertSurvivorTribe(uint64_t survivorId, const std::string& mapName, const std::string& tribeName)
{
    if (survivorId == 0 || mapName.empty() || tribeName.empty()) return;
    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!g_mysql) return;
    char sql[512]{};
    std::snprintf(sql, sizeof(sql),
        "INSERT INTO survivor_tribes (survivor_id, map_name, tribe_name) VALUES (%llu, '%s', '%s') ON DUPLICATE KEY UPDATE tribe_name = VALUES(tribe_name);",
        (unsigned long long)survivorId, EscapeUnsafe(mapName).c_str(), EscapeUnsafe(tribeName).c_str());
    ExecQuery(sql);
}

static void UpsertTribeMembership(uint64_t survivorId, const std::string& tribeName)
{
    if (survivorId == 0 || tribeName.empty()) return;
    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!g_mysql) return;
    char sql[512]{};
    std::snprintf(sql, sizeof(sql),
        "INSERT INTO tribe_memberships (survivor_id, tribe_name) VALUES (%llu, '%s') ON DUPLICATE KEY UPDATE tribe_name = VALUES(tribe_name);",
        (unsigned long long)survivorId, EscapeUnsafe(tribeName).c_str());
    ExecQuery(sql);
}

static void ClearSurvivorTribeMembership(uint64_t survivorId)
{
    if (survivorId == 0) return;
    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!g_mysql) return;
    char sql[256]{};
    std::snprintf(sql, sizeof(sql), "DELETE FROM survivor_tribes WHERE survivor_id = %llu;", (unsigned long long)survivorId);
    ExecQuery(sql);
    std::snprintf(sql, sizeof(sql), "DELETE FROM tribe_memberships WHERE survivor_id = %llu;", (unsigned long long)survivorId);
    ExecQuery(sql);
}

static void InsertPendingKicks(uint64_t survivorId, const std::string& tribeName, const std::string& currentMap)
{
    if (survivorId == 0 || tribeName.empty()) return;
    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!g_mysql || !pmysql_fetch_row) return;
    char sql[512]{};
    std::snprintf(sql, sizeof(sql),
        "SELECT tribe_id, map_name FROM tribes WHERE tribe_name = '%s' AND map_name != '%s';",
        EscapeUnsafe(tribeName).c_str(), EscapeUnsafe(currentMap).c_str());
    if (pmysql_query(g_mysql, sql) != 0) return;
    MYSQL_RES* res = pmysql_store_result(g_mysql);
    if (!res) return;
    std::vector<std::pair<int, std::string>> rows;
    while (MYSQL_ROW row = pmysql_fetch_row(res))
        if (row[0] && row[1]) rows.push_back({ std::atoi(row[0]), row[1] });
    pmysql_free_result(res);
    for (const auto& [tribeId, mapName] : rows) {
        char ins[512]{};
        std::snprintf(ins, sizeof(ins),
            "INSERT IGNORE INTO pending_kicks (survivor_id, tribe_id, map_name) VALUES (%llu, %d, '%s');",
            (unsigned long long)survivorId, tribeId, EscapeUnsafe(mapName).c_str());
        ExecQuery(ins);
    }
    Log::GetLog()->info("[TribeWarden] PENDING_KICKS_INSERTED survivor_id={} tribe_name={} maps={}",
        survivorId, tribeName, rows.size());
}

static int GetPendingKickTribeId(uint64_t survivorId, const std::string& mapName)
{
    if (survivorId == 0 || mapName.empty()) return 0;
    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!g_mysql || !pmysql_fetch_row) return 0;
    char sql[256]{};
    std::snprintf(sql, sizeof(sql),
        "SELECT tribe_id FROM pending_kicks WHERE survivor_id = %llu AND map_name = '%s' LIMIT 1;",
        (unsigned long long)survivorId, EscapeUnsafe(mapName).c_str());
    if (pmysql_query(g_mysql, sql) != 0) return 0;
    int result = 0;
    if (MYSQL_RES* res = pmysql_store_result(g_mysql)) {
        MYSQL_ROW row = pmysql_fetch_row(res);
        if (row && row[0]) result = std::atoi(row[0]);
        pmysql_free_result(res);
    }
    return result;
}

static void DeletePendingKick(uint64_t survivorId, const std::string& mapName)
{
    if (survivorId == 0 || mapName.empty()) return;
    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!g_mysql) return;
    char sql[256]{};
    std::snprintf(sql, sizeof(sql),
        "DELETE FROM pending_kicks WHERE survivor_id = %llu AND map_name = '%s';",
        (unsigned long long)survivorId, EscapeUnsafe(mapName).c_str());
    ExecQuery(sql);
}

static std::string GetTribeNameFromDB(int tribeId, const std::string& mapName)
{
    if (tribeId == 0) return "";
    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!g_mysql || !pmysql_fetch_row) return "";
    char sql[256]{};
    std::snprintf(sql, sizeof(sql), "SELECT tribe_name FROM tribes WHERE tribe_id = %d AND map_name = '%s' LIMIT 1;", tribeId, EscapeUnsafe(mapName).c_str());
    if (pmysql_query(g_mysql, sql) != 0) return "";
    std::string result;
    if (MYSQL_RES* res = pmysql_store_result(g_mysql)) { MYSQL_ROW row = pmysql_fetch_row(res); if (row && row[0]) result = row[0]; pmysql_free_result(res); }
    return result;
}

static std::string GetSurvivorTribeNameFromOtherMap(uint64_t survivorId, const std::string& currentMap)
{
    if (survivorId == 0) return "";
    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!g_mysql || !pmysql_fetch_row) return "";
    char sql[512]{};
    std::snprintf(sql, sizeof(sql),
        "SELECT tribe_name FROM survivor_tribes WHERE survivor_id = %llu AND map_name != '%s' AND tribe_name != '' LIMIT 1;",
        (unsigned long long)survivorId, EscapeUnsafe(currentMap).c_str());
    if (pmysql_query(g_mysql, sql) != 0) return "";
    std::string result;
    if (MYSQL_RES* res = pmysql_store_result(g_mysql)) { MYSQL_ROW row = pmysql_fetch_row(res); if (row && row[0]) result = row[0]; pmysql_free_result(res); }
    return result;
}

static int GetTribeIdByName(const std::string& tribeName, const std::string& mapName)
{
    if (tribeName.empty() || mapName.empty()) return 0;
    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!g_mysql || !pmysql_fetch_row) return 0;
    char sql[512]{};
    std::snprintf(sql, sizeof(sql), "SELECT tribe_id FROM tribes WHERE tribe_name = '%s' AND map_name = '%s' LIMIT 1;",
        EscapeUnsafe(tribeName).c_str(), EscapeUnsafe(mapName).c_str());
    if (pmysql_query(g_mysql, sql) != 0) return 0;
    int result = 0;
    if (MYSQL_RES* res = pmysql_store_result(g_mysql)) { MYSQL_ROW row = pmysql_fetch_row(res); if (row && row[0]) result = std::atoi(row[0]); pmysql_free_result(res); }
    return result;
}

// =============================================================================
// Helpers
// =============================================================================

static std::string FStr(const FString& f) { const char* s = TCHAR_TO_UTF8(*f); return (s && s[0]) ? s : "unknown"; }

static std::string GetMapName()
{
    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return "unknown";
    FString map; world->GetMapName(&map); return FStr(map);
}

static std::string GetEosId(AShooterPlayerState* ps)
{
    if (!ps) return "unknown";
    FString eos; ps->GetUniqueNetIdAsString(&eos); return FStr(eos);
}

static bool InTribe(AShooterPlayerState* ps) { return ps && ps->GetTribeId() > 0; }

static std::string RandomTribeName()
{
    static thread_local std::mt19937_64 rng{ (uint64_t)GetTickCount64() ^ (uint64_t)(uintptr_t)&rng };
    std::uniform_int_distribution<int> dist(0, 9);
    std::string name; name.reserve(10);
    for (int i = 0; i < 10; ++i) name.push_back((char)('0' + dist(rng)));
    return name;
}

static std::pair<int, std::string> CreateSoloTribeNamed(AShooterPlayerState* ps, const std::string& name)
{
    if (!ps || name.empty()) return { 0, "" };
    FString fname(name.c_str()); FTribeGovernment gov{};
    try { ps->ServerRequestCreateNewTribe(fname, gov); Sleep(200); const int tribeId = ps->GetTribeId(); return tribeId > 0 ? std::make_pair(tribeId, name) : std::make_pair(0, std::string{}); }
    catch (...) { return { 0, "" }; }
}

static AShooterPlayerController* FindPC(AShooterPlayerState* ps)
{
    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world || !ps) return nullptr;
    auto& list = world->PlayerControllerListField();
    for (int i = 0; i < list.Num(); ++i) {
        APlayerController* ctrl = list[i].Get();
        if (ctrl && ctrl->PlayerStateField().Get() == ps)
            return static_cast<AShooterPlayerController*>(ctrl);
    }
    return nullptr;
}

static FLinearColor ParseMessageColor()
{
    float r = 0.902f, g = 0.365f, b = 0.137f, a = 1.0f;
    std::sscanf(g_message_color.c_str(), "%f,%f,%f,%f", &r, &g, &b, &a);
    return FLinearColor(r, g, b, a);
}

static void NotifyForcedJoin(AShooterPlayerState* ps)
{
    AShooterPlayerController* pc = FindPC(ps);
    if (!pc) return;
    FString msg(L"You have been automatically placed into a tribe.");
    pc->ClientServerNotificationSingle(&msg, ParseMessageColor(), 1.0f, 7.0f, nullptr, nullptr, 9001);
}

// =============================================================================
// Skip Governance Capture
// =============================================================================

static std::unordered_set<int> g_skip_governance_capture;
static std::mutex               g_skip_governance_mutex;

static void SkipNextGovernanceCapture(int tribeId) { std::lock_guard<std::mutex> lock(g_skip_governance_mutex); g_skip_governance_capture.insert(tribeId); }

static bool ShouldSkipGovernanceCapture(int tribeId)
{
    std::lock_guard<std::mutex> lock(g_skip_governance_mutex);
    auto it = g_skip_governance_capture.find(tribeId);
    if (it == g_skip_governance_capture.end()) return false;
    g_skip_governance_capture.erase(it); return true;
}

// =============================================================================
// Pending Kicks Poll
// =============================================================================

static ULONGLONG g_last_kick_poll = 0;
static constexpr ULONGLONG KICK_POLL_MS = 3000;

static void ProcessPendingKicks()
{
    const ULONGLONG now = GetTickCount64();
    if (now - g_last_kick_poll < KICK_POLL_MS) return;
    g_last_kick_poll = now;

    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return;

    const std::string mapName = GetMapName();
    auto& controllers = world->PlayerControllerListField();
    for (int i = 0; i < controllers.Num(); ++i) {
        AShooterPlayerController* pc = static_cast<AShooterPlayerController*>(controllers[i].Get());
        if (!pc) continue;
        AShooterPlayerState* ps = static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
        if (!ps || !InTribe(ps)) continue;
        APawn* pawn = pc->PawnField().Get();
        AShooterCharacter* ch = pawn ? static_cast<AShooterCharacter*>(pawn) : nullptr;
        if (!ch) continue;
        const uint64_t survivorId = ch->GetLinkedPlayerDataID();
        if (survivorId == 0) continue;
        const int pendingTribeId = GetPendingKickTribeId(survivorId, mapName);
        if (pendingTribeId == 0) continue;
        if (ps->GetTribeId() != pendingTribeId) { DeletePendingKick(survivorId, mapName); continue; }
        Log::GetLog()->info("[TribeWarden] PENDING_KICK_PROCESSING survivor_id={} tribe_id={} map={}", survivorId, pendingTribeId, mapName);
        ps->ServerRequestLeaveTribe();
        DeletePendingKick(survivorId, mapName);
    }
}

// =============================================================================
// Hook Originals
// =============================================================================

using HandleRespawned_t = void(*)(AShooterPlayerController*, APawn*, bool);
using NotifyLeft_t = void(*)(AShooterPlayerState*, FString&, FString&, bool);
using NotifyJoined_t = void(*)(AShooterPlayerState*, FString&, FString&, bool);
using GameModeTick_t = void(*)(AShooterGameMode*, float);
using NetUpdateTribeName_t = void(*)(APrimalCharacter*, FString&);
using ServerRequestRenameTribe_t = void(*)(AShooterPlayerState*, FString&);
using StartNewShooterPlayer_t = void(*)(AShooterGameMode*, APlayerController*, bool, bool, FPrimalPlayerCharacterConfigStruct&, UPrimalPlayerData*, bool);
using SaveTribeData_t = TSharedPtr<FWriteFileTaskInfo>(*)(AShooterGameMode*, FTribeData*, bool);
using RemovePlayerIndexFromMyTribe_t = void(*)(AShooterPlayerState*, int);

static HandleRespawned_t              Original_HandleRespawned = nullptr;
static NotifyLeft_t                   Original_NotifyLeft = nullptr;
static NotifyJoined_t                 Original_NotifyJoined = nullptr;
static GameModeTick_t                 Original_GameModeTick = nullptr;
static NetUpdateTribeName_t           Original_NetUpdateTribeName = nullptr;
static ServerRequestRenameTribe_t     Original_ServerRequestRenameTribe = nullptr;
static StartNewShooterPlayer_t        Original_StartNewShooterPlayer = nullptr;
static SaveTribeData_t                Original_SaveTribeData = nullptr;
static RemovePlayerIndexFromMyTribe_t Original_RemovePlayerIndexFromMyTribe = nullptr;

static std::unordered_set<std::string> g_pending_renames;
static std::mutex                      g_pending_renames_mutex;

// =============================================================================
// Survivor ID Cache
// =============================================================================

static std::unordered_map<std::string, uint64_t> g_survivor_cache;
static std::mutex                                 g_survivor_cache_mutex;

// =============================================================================
// Game Mode Reference
// =============================================================================

static AShooterGameMode* g_game_mode = nullptr;

// =============================================================================
// Enforcement Queue
// =============================================================================

struct EnforceState { AShooterPlayerState* ps = nullptr; ULONGLONG due = 0; int attempts = 0; };
static std::unordered_map<std::string, EnforceState> g_queue;
static std::mutex                                     g_queue_mutex;

static void Schedule(const std::string& eos, AShooterPlayerState* ps, ULONGLONG delayMs = 5000)
{
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    EnforceState& st = g_queue[eos]; st.ps = ps; st.due = GetTickCount64() + delayMs; st.attempts = 0;
    Log::GetLog()->info("[TribeWarden] Scheduled enforcement for {} (delay={}ms)", eos, delayMs);
}

// =============================================================================
// Detours
// =============================================================================

TSharedPtr<FWriteFileTaskInfo> Detour_SaveTribeData(AShooterGameMode* gm, FTribeData* tribeData, bool bCanDeferToTick)
{
    if (tribeData) {
        const int tribeId = tribeData->TribeIDField();
        const std::string tribeName = FStr(tribeData->TribeNameField());
        if (tribeId != 0 && !tribeName.empty() && tribeName != "unknown") {
            if (ShouldSkipGovernanceCapture(tribeId)) {
                Log::GetLog()->info("[TribeWarden] SAVE_SKIPPED tribe_name={} tribe_id={}", tribeName, tribeId);
            }
        }
    }
    return Original_SaveTribeData(gm, tribeData, bCanDeferToTick);
}

void Detour_StartNewShooterPlayer(AShooterGameMode* gm, APlayerController* pc, bool b1, bool b2, FPrimalPlayerCharacterConfigStruct& cfg, UPrimalPlayerData* playerData, bool b3)
{
    Original_StartNewShooterPlayer(gm, pc, b1, b2, cfg, playerData, b3);
    if (!pc) return;
    AShooterPlayerState* ps = static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get()); if (!ps) return;
    const std::string eos = GetEosId(ps); if (eos.empty() || eos == "unknown") return;
    APawn* pawn = pc->PawnField().Get(); if (!pawn) return;
    AShooterCharacter* ch = static_cast<AShooterCharacter*>(pawn); if (!ch) return;
    const uint64_t survivorId = ch->GetLinkedPlayerDataID(); if (survivorId == 0) return;
    bool isNewCharacter = false;
    { std::lock_guard<std::mutex> slock(g_survivor_cache_mutex); auto it = g_survivor_cache.find(eos); if (it == g_survivor_cache.end() || it->second != survivorId) isNewCharacter = true; g_survivor_cache[eos] = survivorId; }
    if (isNewCharacter && !InTribe(ps)) {
        const std::string mapName = GetMapName();
        const bool isTransfer = !GetSurvivorTribeNameFromOtherMap(survivorId, mapName).empty();
        Schedule(eos, ps, isTransfer ? 5000 : 20000);
    }
}

void Detour_HandleRespawned(AShooterPlayerController* pc, APawn* pawn, bool bNewPlayer)
{
    if (Original_HandleRespawned) Original_HandleRespawned(pc, pawn, bNewPlayer);
    if (!pc || !pawn) return;
    AShooterPlayerState* ps = static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get()); if (!ps) return;
    const std::string eos = GetEosId(ps); if (eos == "unknown" || eos.empty()) return;
    AShooterCharacter* ch = static_cast<AShooterCharacter*>(pawn);
    const uint64_t survivorId = ch ? ch->GetLinkedPlayerDataID() : 0;
    if (survivorId != 0) { std::lock_guard<std::mutex> slock(g_survivor_cache_mutex); g_survivor_cache[eos] = survivorId; }
    if (!InTribe(ps)) { Schedule(eos, ps, 5000); return; }
    const std::string mapName = GetMapName();
    const int tribeId = ps->GetTribeId();
    const std::string tribeName = ch ? FStr(ch->TribeNameField()) : "";
    if (tribeId != 0 && !tribeName.empty() && tribeName != "unknown")
        UpsertTribe(tribeId, tribeName, mapName);
}

void Detour_NotifyLeft(AShooterPlayerState* ps, FString& a, FString& b, bool c)
{
    std::string tribeName;
    uint64_t survivorId = 0;
    if (ps) {
        const std::string eos = GetEosId(ps);
        { std::lock_guard<std::mutex> slock(g_survivor_cache_mutex); auto it = g_survivor_cache.find(eos); if (it != g_survivor_cache.end()) survivorId = it->second; }
        tribeName = FStr(b);
    }
    if (Original_NotifyLeft) Original_NotifyLeft(ps, a, b, c);
    if (!ps) return;
    const std::string eos = GetEosId(ps);
    if (eos == "unknown" || eos.empty()) return;
    if (survivorId != 0 && !tribeName.empty() && tribeName != "unknown")
        InsertPendingKicks(survivorId, tribeName, GetMapName());
    if (survivorId != 0)
        ClearSurvivorTribeMembership(survivorId);
    Schedule(eos, ps);
}

void Detour_NotifyJoined(AShooterPlayerState* ps, FString& playerName, FString& tribeName, bool joinee)
{
    if (Original_NotifyJoined) Original_NotifyJoined(ps, playerName, tribeName, joinee);
    if (!ps) return;
    const std::string eos = GetEosId(ps); if (eos.empty() || eos == "unknown") return;
    const std::string tn = FStr(tribeName);
    const int tribeId = ps->GetTribeId();
    Log::GetLog()->info("[TribeWarden] NOTIFY_JOINED tribe_id={} tribeName={}", tribeId, tn);
    uint64_t survivorId = 0;
    { std::lock_guard<std::mutex> slock(g_survivor_cache_mutex); auto it = g_survivor_cache.find(eos); if (it != g_survivor_cache.end()) survivorId = it->second; }
    const std::string mapName = GetMapName();
    UpsertSurvivorTribe(survivorId, mapName, tn);
    UpsertTribeMembership(survivorId, tn);
}

void Detour_GameModeTick(AShooterGameMode* gm, float delta)
{
    if (Original_GameModeTick) Original_GameModeTick(gm, delta);

    g_game_mode = gm;

    ProcessPendingKicks();

    const ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    for (auto it = g_queue.begin(); it != g_queue.end(); ) {
        const std::string& eos = it->first; EnforceState& st = it->second;
        if (!st.ps) { it = g_queue.erase(it); continue; }
        if (InTribe(st.ps)) { Log::GetLog()->info("[TribeWarden] {} is now in tribe - stopping", eos); it = g_queue.erase(it); continue; }
        if (st.attempts >= 10) { Log::GetLog()->warn("[TribeWarden] {} exhausted 10 attempts - giving up", eos); it = g_queue.erase(it); continue; }
        if (now < st.due) { ++it; continue; }
        st.attempts++;
        const std::string mapName = GetMapName();
        uint64_t survivorId = 0;
        { std::lock_guard<std::mutex> slock(g_survivor_cache_mutex); auto sit = g_survivor_cache.find(eos); if (sit != g_survivor_cache.end()) survivorId = sit->second; }
        const std::string desiredName = (survivorId != 0) ? GetSurvivorTribeNameFromOtherMap(survivorId, mapName) : "";
        if (!desiredName.empty()) {
            const int existingTribeId = GetTribeIdByName(desiredName, mapName);
            if (existingTribeId != 0) {
                AShooterPlayerController* pc = FindPC(st.ps);
                if (pc) {
                    UShooterCheatManager* cm = static_cast<UShooterCheatManager*>(pc->CheatManagerField().Get());
                    if (cm) {
                        SkipNextGovernanceCapture(existingTribeId);
                        cm->ForcePlayerToJoinTribeId(static_cast<__int64>(survivorId), existingTribeId);
                        Sleep(200);
                        if (InTribe(st.ps)) {
                            Log::GetLog()->info("[TribeWarden] TRANSFER_JOIN eos={} tribe_name={} tribe_id={}", eos, desiredName, existingTribeId);
                            UpsertSurvivorTribe(survivorId, mapName, desiredName);
                            UpsertTribeMembership(survivorId, desiredName);
                            NotifyForcedJoin(st.ps);
                            it = g_queue.erase(it); continue;
                        }
                        else { ShouldSkipGovernanceCapture(existingTribeId); }
                    }
                }
                Log::GetLog()->warn("[TribeWarden] TRANSFER_JOIN_FAILED eos={} tribe_id={} - creating instead", eos, existingTribeId);
            }
            auto [tribeId, tribeName] = CreateSoloTribeNamed(st.ps, desiredName);
            const bool ok = tribeId > 0;
            Log::GetLog()->info("[TribeWarden] Attempt {}/10 for {} (transfer create) - {}", st.attempts, eos, ok ? "OK" : "FAIL");
            if (ok) {
                SkipNextGovernanceCapture(tribeId);
                UpsertTribe(tribeId, tribeName, mapName);
                UpsertSurvivorTribe(survivorId, mapName, tribeName);
                UpsertTribeMembership(survivorId, tribeName);
                NotifyForcedJoin(st.ps);
                it = g_queue.erase(it); continue;
            }
        }
        else {
            const std::string randomName = RandomTribeName();
            auto [tribeId, tribeName] = CreateSoloTribeNamed(st.ps, randomName);
            const bool ok = tribeId > 0;
            Log::GetLog()->info("[TribeWarden] Attempt {}/10 for {} - {}", st.attempts, eos, ok ? "OK" : "FAIL");
            if (ok) {
                UpsertTribe(tribeId, tribeName, mapName);
                UpsertSurvivorTribe(survivorId, mapName, tribeName);
                UpsertTribeMembership(survivorId, tribeName);
                NotifyForcedJoin(st.ps);
                it = g_queue.erase(it); continue;
            }
        }
        st.due = now + 5000; ++it;
    }
}

void Detour_ServerRequestRenameTribe(AShooterPlayerState* ps, FString& newNameFStr)
{
    if (!ps) { Original_ServerRequestRenameTribe(ps, newNameFStr); return; }
    const std::string newName = FStr(newNameFStr);
    if (newName.empty() || newName == "unknown") { Original_ServerRequestRenameTribe(ps, newNameFStr); return; }
    {
        std::lock_guard<std::mutex> lock(g_tribe_cache_mutex);
        for (const auto& name : g_tribe_cache) {
            const bool taken = name.size() == newName.size() && std::equal(name.begin(), name.end(), newName.begin(),
                [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
            if (taken) {
                AShooterPlayerController* pc = FindPC(ps);
                if (pc) { FString msg(L"That tribe name is already taken."); pc->ClientServerNotificationSingle(&msg, ParseMessageColor(), 1.0f, 7.0f, nullptr, nullptr, 9001); }
                Log::GetLog()->info("[TribeWarden] RENAME_BLOCKED tribe_id={} requested={}", ps->GetTribeId(), newName);
                return;
            }
        }
    }
    { std::lock_guard<std::mutex> lock(g_pending_renames_mutex); g_pending_renames.insert(newName); }
    Original_ServerRequestRenameTribe(ps, newNameFStr);
}

void Detour_NetUpdateTribeName(APrimalCharacter* character, FString& newNameFStr)
{
    if (!character) return;
    AController* controller = character->GetOwnerController(); if (!controller) return;
    AShooterPlayerController* pc = static_cast<AShooterPlayerController*>(controller);
    AShooterPlayerState* ps = static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get()); if (!ps) return;
    const int tribeId = ps->GetTribeId();
    if (tribeId == 0) { Original_NetUpdateTribeName(character, newNameFStr); return; }
    const std::string newName = FStr(newNameFStr);
    if (newName.empty() || newName == "unknown") { Original_NetUpdateTribeName(character, newNameFStr); return; }
    const std::string mapName = GetMapName();
    const std::string dbName = GetTribeNameFromDB(tribeId, mapName);
    Original_NetUpdateTribeName(character, newNameFStr);
    Log::GetLog()->info("[TribeWarden] NET_UPDATE_TRIBE tribe_id={} db={} new={}", tribeId, dbName, newName);
    bool inPending;
    { std::lock_guard<std::mutex> lock(g_pending_renames_mutex); inPending = (g_pending_renames.erase(newName) > 0); }
    if (!inPending) {
        if (dbName.empty() || dbName == newName) return;
        Log::GetLog()->info("[TribeWarden] TRIBE_SYNC_RESTORE tribe_id={} ingame={} db={} - syncing", tribeId, newName, dbName);
        FString fDbName(dbName.c_str());
        { std::lock_guard<std::mutex> lock(g_pending_renames_mutex); g_pending_renames.insert(dbName); }
        Original_ServerRequestRenameTribe(ps, fDbName);
        return;
    }
    { std::lock_guard<std::mutex> lock(g_tribe_cache_mutex); if (!dbName.empty()) g_tribe_cache.erase(dbName); g_tribe_cache.insert(newName); }
    Log::GetLog()->info("[TribeWarden] TRIBE_RENAMED tribe_id={} old={} new={}", tribeId, dbName, newName);
    if (!dbName.empty()) UpdateTribeNameByOldName(dbName, newName); else UpdateTribeNameById(tribeId, newName);
}

void Detour_RemovePlayerIndexFromMyTribe(AShooterPlayerState* ps, int memberIndex)
{
    std::unordered_set<unsigned int> membersBefore;
    std::string tribeName;
    int tribeId = 0;

    if (ps && g_game_mode) {
        tribeId = ps->GetTribeId();
        TArray<FTribeData>& tribes = g_game_mode->TribesDataField();
        for (int i = 0; i < tribes.Num(); ++i) {
            if (tribes[i].TribeIDField() != tribeId) continue;
            tribeName = FStr(tribes[i].TribeNameField());
            for (const auto& id : tribes[i].MembersPlayerDataIDSet_ServerField())
                membersBefore.insert(id);
            break;
        }
    }

    Original_RemovePlayerIndexFromMyTribe(ps, memberIndex);

    if (tribeName.empty() || tribeName == "unknown" || membersBefore.empty() || !g_game_mode) return;

    const std::string mapName = GetMapName();
    TArray<FTribeData>& tribes = g_game_mode->TribesDataField();
    for (int i = 0; i < tribes.Num(); ++i) {
        if (tribes[i].TribeIDField() != tribeId) continue;
        std::unordered_set<unsigned int> membersAfter;
        for (const auto& id : tribes[i].MembersPlayerDataIDSet_ServerField())
            membersAfter.insert(id);
        for (unsigned int id : membersBefore) {
            if (membersAfter.count(id)) continue;
            InsertPendingKicks(static_cast<uint64_t>(id), tribeName, mapName);
            Log::GetLog()->info("[TribeWarden] CROSS_MAP_KICK_DETECTED survivor_id={} tribe_name={} map={}", id, tribeName, mapName);
        }
        break;
    }
}

// =============================================================================
// Plugin Entry Points
// =============================================================================

extern "C" __declspec(dllexport) void Plugin_Init()
{
    Log::Get().Init("TribeWarden");
    if (!LoadConfig()) { Log::GetLog()->error("[TribeWarden] Halted - config error"); return; }
    if (!InitDatabase()) { Log::GetLog()->error("[TribeWarden] Halted - database error"); return; }
    AsaApi::GetHooks().SetHook("AShooterGameMode.SaveTribeData(FTribeData&,bool)", (LPVOID)&Detour_SaveTribeData, (LPVOID*)&Original_SaveTribeData);
    AsaApi::GetHooks().SetHook("AShooterPlayerController.HandleRespawned_Implementation(APawn*,bool)", (LPVOID)&Detour_HandleRespawned, (LPVOID*)&Original_HandleRespawned);
    AsaApi::GetHooks().SetHook("AShooterPlayerState.NotifyPlayerLeftTribe(FString&,FString&,bool)", (LPVOID)&Detour_NotifyLeft, (LPVOID*)&Original_NotifyLeft);
    AsaApi::GetHooks().SetHook("AShooterPlayerState.NotifyPlayerJoinedTribe(FString&,FString&,bool)", (LPVOID)&Detour_NotifyJoined, (LPVOID*)&Original_NotifyJoined);
    AsaApi::GetHooks().SetHook("AShooterGameMode.Tick(float)", (LPVOID)&Detour_GameModeTick, (LPVOID*)&Original_GameModeTick);
    AsaApi::GetHooks().SetHook("APrimalCharacter.NetUpdateTribeName_Implementation(FString&)", (LPVOID)&Detour_NetUpdateTribeName, (LPVOID*)&Original_NetUpdateTribeName);
    AsaApi::GetHooks().SetHook("AShooterPlayerState.ServerRequestRenameTribe_Implementation(FString&)", (LPVOID)&Detour_ServerRequestRenameTribe, (LPVOID*)&Original_ServerRequestRenameTribe);
    AsaApi::GetHooks().SetHook("AShooterGameMode.StartNewShooterPlayer(APlayerController*,bool,bool,FPrimalPlayerCharacterConfigStruct&,UPrimalPlayerData*,bool)", (LPVOID)&Detour_StartNewShooterPlayer, (LPVOID*)&Original_StartNewShooterPlayer);
    AsaApi::GetHooks().SetHook("AShooterPlayerState.ServerRequestRemovePlayerIndexFromMyTribe_Implementation(int)", (LPVOID)&Detour_RemovePlayerIndexFromMyTribe, (LPVOID*)&Original_RemovePlayerIndexFromMyTribe);
    Log::GetLog()->info("[TribeWarden] Plugin loaded");
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    AsaApi::GetHooks().DisableHook("AShooterGameMode.SaveTribeData(FTribeData&,bool)", (LPVOID)&Detour_SaveTribeData);
    AsaApi::GetHooks().DisableHook("AShooterPlayerController.HandleRespawned_Implementation(APawn*,bool)", (LPVOID)&Detour_HandleRespawned);
    AsaApi::GetHooks().DisableHook("AShooterPlayerState.NotifyPlayerLeftTribe(FString&,FString&,bool)", (LPVOID)&Detour_NotifyLeft);
    AsaApi::GetHooks().DisableHook("AShooterPlayerState.NotifyPlayerJoinedTribe(FString&,FString&,bool)", (LPVOID)&Detour_NotifyJoined);
    AsaApi::GetHooks().DisableHook("AShooterGameMode.Tick(float)", (LPVOID)&Detour_GameModeTick);
    AsaApi::GetHooks().DisableHook("APrimalCharacter.NetUpdateTribeName_Implementation(FString&)", (LPVOID)&Detour_NetUpdateTribeName);
    AsaApi::GetHooks().DisableHook("AShooterPlayerState.ServerRequestRenameTribe_Implementation(FString&)", (LPVOID)&Detour_ServerRequestRenameTribe);
    AsaApi::GetHooks().DisableHook("AShooterGameMode.StartNewShooterPlayer(APlayerController*,bool,bool,FPrimalPlayerCharacterConfigStruct&,UPrimalPlayerData*,bool)", (LPVOID)&Detour_StartNewShooterPlayer);
    AsaApi::GetHooks().DisableHook("AShooterPlayerState.ServerRequestRemovePlayerIndexFromMyTribe_Implementation(int)", (LPVOID)&Detour_RemovePlayerIndexFromMyTribe);
    { std::lock_guard<std::mutex> lock(g_queue_mutex); g_queue.clear(); }
    { std::lock_guard<std::mutex> lock(g_skip_governance_mutex); g_skip_governance_capture.clear(); }
    g_game_mode = nullptr;
    CloseDatabase();
    Log::GetLog()->info("[TribeWarden] Plugin unloaded");
}