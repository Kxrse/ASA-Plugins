/*
EnhancedTeleporting - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * EnhancedTeleporting - ASA Plugin
 *
 * Hooks: None - chat commands and timers only
 *
 * Table:
 *   home_teleports - PK (eos_id, map_name, home_name)
 *     Columns: x (DOUBLE), y (DOUBLE), z (DOUBLE)
 *
 * Chat commands:
 *   /sethome {name}  - save current location (must be on owned foundation)
 *   /home {name}     - teleport to saved home (delayed, validates destination)
 *   /delhome {name}  - delete a saved home
 *   /listhome        - list all saved homes on current map
 *   /tpr {name}      - send teleport request to online player by survivor name
 *   /tpa             - accept incoming teleport request
 *   /tpc             - cancel outgoing teleport request
 *
 * Permissions integration:
 *   Dynamically loads Ark:SA Permissions V1.1 at runtime.
 *   Per-group config for max_homes, teleport_delay, cooldown, foundation_required.
 *   Same-tribe /tpr auto-teleports without needing /tpa.
 *   Falls back to "default" tier if player matches no configured group.
 *
 * Block integration:
 *   combat_block_enabled and raid_block_enabled config toggles (both default true).
 *   When enabled, block state is read from the player_blocks table owned by KxrsedBlocking.
 *   If player_blocks is unreachable at startup, the relevant block is treated as disabled until restart.
 */

#include <API/ARK/Ark.h>
#include <json.hpp>
#include <Windows.h>
#include <fstream>
#include <string>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <chrono>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <ctime>
#include <cstdlib>

 // =============================================================================
 // MariaDB - Dynamic Load
 // =============================================================================

typedef struct st_mysql     MYSQL;
typedef struct st_mysql_res MYSQL_RES;
typedef char** MYSQL_ROW;

typedef MYSQL* (__stdcall* mysql_init_t)             (MYSQL*);
typedef MYSQL* (__stdcall* mysql_real_connect_t)     (MYSQL*, const char*, const char*, const char*, const char*, unsigned int, const char*, unsigned long);
typedef void(__stdcall* mysql_close_t)            (MYSQL*);
typedef int(__stdcall* mysql_query_t)             (MYSQL*, const char*);
typedef MYSQL_RES* (__stdcall* mysql_store_result_t) (MYSQL*);
typedef void(__stdcall* mysql_free_result_t)      (MYSQL_RES*);
typedef const char* (__stdcall* mysql_error_t)       (MYSQL*);
typedef unsigned long(__stdcall* mysql_real_escape_string_t)(MYSQL*, char*, const char*, unsigned long);
typedef int(__stdcall* mysql_options_t)          (MYSQL*, int, const void*);
typedef MYSQL_ROW(__stdcall* mysql_fetch_row_t)      (MYSQL_RES*);

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
            Log::GetLog()->info("[EnhancedTeleporting] Loaded DB library: {}", candidates[i]);
            break;
        }
    }

    if (!g_mysql_module)
    {
        Log::GetLog()->error("[EnhancedTeleporting] Could not find libmariadb.dll or libmysql.dll");
        return false;
    }

    pmysql_init = (mysql_init_t)GetProcAddress(g_mysql_module, "mysql_init");
    pmysql_real_connect = (mysql_real_connect_t)GetProcAddress(g_mysql_module, "mysql_real_connect");
    pmysql_close = (mysql_close_t)GetProcAddress(g_mysql_module, "mysql_close");
    pmysql_query = (mysql_query_t)GetProcAddress(g_mysql_module, "mysql_query");
    pmysql_store_result = (mysql_store_result_t)GetProcAddress(g_mysql_module, "mysql_store_result");
    pmysql_free_result = (mysql_free_result_t)GetProcAddress(g_mysql_module, "mysql_free_result");
    pmysql_error = (mysql_error_t)GetProcAddress(g_mysql_module, "mysql_error");
    pmysql_real_escape_string = (mysql_real_escape_string_t)GetProcAddress(g_mysql_module, "mysql_real_escape_string");
    pmysql_options = (mysql_options_t)GetProcAddress(g_mysql_module, "mysql_options");
    pmysql_fetch_row = (mysql_fetch_row_t)GetProcAddress(g_mysql_module, "mysql_fetch_row");

    if (!pmysql_init || !pmysql_real_connect || !pmysql_close ||
        !pmysql_query || !pmysql_error || !pmysql_real_escape_string || !pmysql_fetch_row)
    {
        Log::GetLog()->error("[EnhancedTeleporting] Failed to resolve required DB functions");
        return false;
    }

    g_mysql_loaded = true;
    return true;
}

// =============================================================================
// Permissions API (dynamically loaded at runtime)
// =============================================================================

typedef TArray<FString>(*GetPlayerGroups_t)(const FString&);
typedef bool(*IsPlayerInGroup_t)(const FString&, const FString&);

static GetPlayerGroups_t  pGetPlayerGroups = nullptr;
static IsPlayerInGroup_t  pIsPlayerInGroup = nullptr;
static bool               g_permissions_loaded = false;
static bool               g_permissions_attempted = false;

static void LoadPermissionsAPI()
{
    if (g_permissions_attempted) return;
    g_permissions_attempted = true;

    HMODULE hMod = GetModuleHandleA("Permissions");
    if (!hMod)
    {
        Log::GetLog()->warn("[EnhancedTeleporting] Permissions.dll not found, using default tier for all players");
        return;
    }

    pGetPlayerGroups = (GetPlayerGroups_t)GetProcAddress(hMod,
        "?GetPlayerGroups@Permissions@@YA?AV?$TArray@VFString@@V?$TSizedDefaultAllocator@$0CA@@@@@AEBVFString@@@Z");

    pIsPlayerInGroup = (IsPlayerInGroup_t)GetProcAddress(hMod,
        "?IsPlayerInGroup@Permissions@@YA_NAEBVFString@@0@Z");

    if (!pGetPlayerGroups || !pIsPlayerInGroup)
    {
        Log::GetLog()->warn("[EnhancedTeleporting] Failed to resolve Permissions functions, using default tier");
        return;
    }

    g_permissions_loaded = true;
    Log::GetLog()->info("[EnhancedTeleporting] Permissions API loaded");
}

// =============================================================================
// Configuration
// =============================================================================

struct GroupTier
{
    int    maxHomes = 1;
    int    teleportDelay = 10;
    int    cooldownSeconds = 300;
    bool   foundationRequired = true;
    bool   allowCliffPlatforms = false;
    bool   allowVacuumCompartments = false;
    bool   allowTreePlatforms = false;
    bool   allowNoBuildZones = false;
};

static std::string  g_db_host = "localhost";
static unsigned int g_db_port = 3306;
static std::string  g_db_user;
static std::string  g_db_pass;
static std::string  g_db_name;

static std::string  g_block_db_host = "127.0.0.1";
static unsigned int g_block_db_port = 3306;
static std::string  g_block_db_user;
static std::string  g_block_db_pass;
static std::string  g_block_db_name;
static std::string  g_message_color = "1.0,1.0,1.0,1.0";
static int          g_tpa_timeout = 30;
static bool         g_combat_block_enabled = true;
static bool         g_raid_block_enabled = true;
static bool         g_player_blocks_available = false;

static GroupTier                                g_default_tier;
static std::unordered_map<std::string, GroupTier> g_group_tiers;

static bool LoadConfig()
{
    const std::string path = "ArkApi/Plugins/EnhancedTeleporting/config.json";
    std::ifstream file(path);
    if (!file.is_open())
    {
        Log::GetLog()->error("[EnhancedTeleporting] Cannot open config: {}", path);
        return false;
    }

    try
    {
        nlohmann::json j;
        file >> j;

        g_db_host = j.value("DbHost", "localhost");
        g_db_port = j.value("DbPort", 3306);
        g_db_user = j.value("DbUser", "");
        g_db_pass = j.value("DbPassword", "");
        g_db_name = j.value("DbName", "");

        g_block_db_host = j.value("BlockDbHost", "127.0.0.1");
        g_block_db_port = j.value("BlockDbPort", 3306);
        g_block_db_user = j.value("BlockDbUser", "");
        g_block_db_pass = j.value("BlockDbPassword", "");
        g_block_db_name = j.value("BlockDbName", "");

        g_message_color = j.value("message_color", "1.0,1.0,1.0,1.0");
        g_tpa_timeout = j.value("tpa_timeout_seconds", 30);
        g_combat_block_enabled = j.value("combat_block_enabled", true);
        g_raid_block_enabled = j.value("raid_block_enabled", true);

        if (j.contains("default") && j["default"].is_object())
        {
            auto& d = j["default"];
            g_default_tier.maxHomes = d.value("max_homes", 1);
            g_default_tier.teleportDelay = d.value("teleport_delay_seconds", 10);
            g_default_tier.cooldownSeconds = d.value("cooldown_seconds", 300);
            g_default_tier.foundationRequired = d.value("foundation_required", true);
            g_default_tier.allowCliffPlatforms = d.value("allow_cliff_platforms", false);
            g_default_tier.allowVacuumCompartments = d.value("allow_vacuum_compartments", false);
            g_default_tier.allowTreePlatforms = d.value("allow_tree_platforms", false);
            g_default_tier.allowNoBuildZones = d.value("allow_no_build_zones", false);
        }

        g_group_tiers.clear();
        if (j.contains("groups") && j["groups"].is_object())
        {
            for (auto& [key, val] : j["groups"].items())
            {
                GroupTier t;
                t.maxHomes = val.value("max_homes", g_default_tier.maxHomes);
                t.teleportDelay = val.value("teleport_delay_seconds", g_default_tier.teleportDelay);
                t.cooldownSeconds = val.value("cooldown_seconds", g_default_tier.cooldownSeconds);
                t.foundationRequired = val.value("foundation_required", g_default_tier.foundationRequired);
                t.allowCliffPlatforms = val.value("allow_cliff_platforms", g_default_tier.allowCliffPlatforms);
                t.allowVacuumCompartments = val.value("allow_vacuum_compartments", g_default_tier.allowVacuumCompartments);
                t.allowTreePlatforms = val.value("allow_tree_platforms", g_default_tier.allowTreePlatforms);
                t.allowNoBuildZones = val.value("allow_no_build_zones", g_default_tier.allowNoBuildZones);
                g_group_tiers[key] = t;
            }
        }

        Log::GetLog()->info("[EnhancedTeleporting] Config loaded: {} group tier(s), tpa_timeout={}s",
            g_group_tiers.size(), g_tpa_timeout);
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[EnhancedTeleporting] Config parse error: {}", ex.what());
        return false;
    }

    if (g_db_user.empty() || g_db_name.empty())
    {
        Log::GetLog()->error("[EnhancedTeleporting] Config requires DbUser and DbName");
        return false;
    }

    return true;
}

static int g_reload_counter = 0;

static void ReloadConfigTick()
{
    if (++g_reload_counter < 10) return;
    g_reload_counter = 0;

    const std::string path = "ArkApi/Plugins/EnhancedTeleporting/config.json";
    std::ifstream file(path);
    if (!file.is_open()) return;

    try
    {
        nlohmann::json j;
        file >> j;

        g_message_color = j.value("message_color", g_message_color);
        g_tpa_timeout = j.value("tpa_timeout_seconds", g_tpa_timeout);
        g_combat_block_enabled = j.value("combat_block_enabled", g_combat_block_enabled);
        g_raid_block_enabled = j.value("raid_block_enabled", g_raid_block_enabled);

        if (j.contains("default") && j["default"].is_object())
        {
            auto& d = j["default"];
            g_default_tier.maxHomes = d.value("max_homes", 1);
            g_default_tier.teleportDelay = d.value("teleport_delay_seconds", 10);
            g_default_tier.cooldownSeconds = d.value("cooldown_seconds", 300);
            g_default_tier.foundationRequired = d.value("foundation_required", true);
            g_default_tier.allowCliffPlatforms = d.value("allow_cliff_platforms", false);
            g_default_tier.allowVacuumCompartments = d.value("allow_vacuum_compartments", false);
            g_default_tier.allowTreePlatforms = d.value("allow_tree_platforms", false);
            g_default_tier.allowNoBuildZones = d.value("allow_no_build_zones", false);
        }

        g_group_tiers.clear();
        if (j.contains("groups") && j["groups"].is_object())
        {
            for (auto& [key, val] : j["groups"].items())
            {
                GroupTier t;
                t.maxHomes = val.value("max_homes", g_default_tier.maxHomes);
                t.teleportDelay = val.value("teleport_delay_seconds", g_default_tier.teleportDelay);
                t.cooldownSeconds = val.value("cooldown_seconds", g_default_tier.cooldownSeconds);
                t.foundationRequired = val.value("foundation_required", g_default_tier.foundationRequired);
                t.allowCliffPlatforms = val.value("allow_cliff_platforms", g_default_tier.allowCliffPlatforms);
                t.allowVacuumCompartments = val.value("allow_vacuum_compartments", g_default_tier.allowVacuumCompartments);
                t.allowTreePlatforms = val.value("allow_tree_platforms", g_default_tier.allowTreePlatforms);
                t.allowNoBuildZones = val.value("allow_no_build_zones", g_default_tier.allowNoBuildZones);
                g_group_tiers[key] = t;
            }
        }
    }
    catch (...) {}
}

// =============================================================================
// Database
// =============================================================================

static MYSQL* g_mysql = nullptr;
static std::mutex g_db_mutex;

static MYSQL* g_block_mysql = nullptr;
static std::mutex g_block_mutex;

static std::string EscapeUnsafe(const std::string& in)
{
    if (!g_mysql || !pmysql_real_escape_string) return in;
    std::string buf(in.size() * 2 + 1, '\0');
    unsigned long len = pmysql_real_escape_string(
        g_mysql, buf.data(), in.c_str(), (unsigned long)in.size());
    buf.resize(len);
    return buf;
}

static std::string EscapeBlock(const std::string& in)
{
    if (!g_block_mysql || !pmysql_real_escape_string) return in;
    std::string buf(in.size() * 2 + 1, '\0');
    unsigned long len = pmysql_real_escape_string(
        g_block_mysql, buf.data(), in.c_str(), (unsigned long)in.size());
    buf.resize(len);
    return buf;
}

static bool ExecQuery(const std::string& sql)
{
    if (!g_mysql) return false;
    if (pmysql_query(g_mysql, sql.c_str()) != 0)
    {
        Log::GetLog()->error("[EnhancedTeleporting] Query error: {}", pmysql_error(g_mysql));
        return false;
    }
    if (MYSQL_RES* res = pmysql_store_result(g_mysql))
        pmysql_free_result(res);
    return true;
}

static bool InitDatabase()
{
    if (!LoadMySQLLib()) return false;

    g_mysql = pmysql_init(nullptr);
    if (!g_mysql)
    {
        Log::GetLog()->error("[EnhancedTeleporting] mysql_init returned null");
        return false;
    }

    unsigned int timeout = 10;
    if (pmysql_options) pmysql_options(g_mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

    if (!pmysql_real_connect(g_mysql,
        g_db_host.c_str(), g_db_user.c_str(), g_db_pass.c_str(),
        g_db_name.c_str(), g_db_port, nullptr, 0))
    {
        Log::GetLog()->error("[EnhancedTeleporting] DB connect failed: {}", pmysql_error(g_mysql));
        pmysql_close(g_mysql);
        g_mysql = nullptr;
        return false;
    }

    const std::string create =
        "CREATE TABLE IF NOT EXISTS home_teleports ("
        "  eos_id    VARCHAR(64)  NOT NULL,"
        "  map_name  VARCHAR(64)  NOT NULL,"
        "  home_name VARCHAR(32)  NOT NULL,"
        "  x         DOUBLE       NOT NULL,"
        "  y         DOUBLE       NOT NULL,"
        "  z         DOUBLE       NOT NULL,"
        "  PRIMARY KEY (eos_id, map_name, home_name)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!ExecQuery(create))
    {
        Log::GetLog()->error("[EnhancedTeleporting] Failed to create table");
        return false;
    }

    Log::GetLog()->info("[EnhancedTeleporting] Database ready");
    return true;
}

static bool InitBlockDatabase()
{
    if (!LoadMySQLLib()) return false;

    g_block_mysql = pmysql_init(nullptr);
    if (!g_block_mysql)
    {
        Log::GetLog()->error("[EnhancedTeleporting] block mysql_init returned null");
        return false;
    }

    unsigned int timeout = 10;
    if (pmysql_options) pmysql_options(g_block_mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

    if (!pmysql_real_connect(g_block_mysql,
        g_block_db_host.c_str(), g_block_db_user.c_str(), g_block_db_pass.c_str(),
        g_block_db_name.c_str(), g_block_db_port, nullptr, 0))
    {
        Log::GetLog()->error("[EnhancedTeleporting] block DB connect failed: {}", pmysql_error(g_block_mysql));
        pmysql_close(g_block_mysql);
        g_block_mysql = nullptr;
        return false;
    }

    Log::GetLog()->info("[EnhancedTeleporting] Block database connected");
    return true;
}

static void CloseDatabase()
{
    {
        std::lock_guard<std::mutex> lock(g_db_mutex);
        if (g_mysql) { pmysql_close(g_mysql); g_mysql = nullptr; }
    }
    {
        std::lock_guard<std::mutex> lock(g_block_mutex);
        if (g_block_mysql) { pmysql_close(g_block_mysql); g_block_mysql = nullptr; }
    }
}

// =============================================================================
// DB Operations
// =============================================================================

struct HomeEntry
{
    std::string name;
    double x, y, z;
};

static bool SaveHome(const std::string& eosId, const std::string& mapName,
    const std::string& homeName, double x, double y, double z)
{
    const std::string eEos = EscapeUnsafe(eosId);
    const std::string eMap = EscapeUnsafe(mapName);
    const std::string eName = EscapeUnsafe(homeName);

    char sql[512];
    std::snprintf(sql, sizeof(sql),
        "INSERT INTO home_teleports (eos_id, map_name, home_name, x, y, z) "
        "VALUES ('%s', '%s', '%s', %.2f, %.2f, %.2f) "
        "ON DUPLICATE KEY UPDATE x=VALUES(x), y=VALUES(y), z=VALUES(z)",
        eEos.c_str(), eMap.c_str(), eName.c_str(), x, y, z);

    std::lock_guard<std::mutex> lock(g_db_mutex);
    return ExecQuery(sql);
}

static bool DeleteHome(const std::string& eosId, const std::string& mapName,
    const std::string& homeName)
{
    const std::string eEos = EscapeUnsafe(eosId);
    const std::string eMap = EscapeUnsafe(mapName);
    const std::string eName = EscapeUnsafe(homeName);

    char sql[256];
    std::snprintf(sql, sizeof(sql),
        "DELETE FROM home_teleports WHERE eos_id='%s' AND map_name='%s' AND home_name='%s'",
        eEos.c_str(), eMap.c_str(), eName.c_str());

    std::lock_guard<std::mutex> lock(g_db_mutex);
    return ExecQuery(sql);
}

static std::vector<HomeEntry> GetHomes(const std::string& eosId, const std::string& mapName)
{
    std::vector<HomeEntry> homes;

    const std::string eEos = EscapeUnsafe(eosId);
    const std::string eMap = EscapeUnsafe(mapName);

    char sql[256];
    std::snprintf(sql, sizeof(sql),
        "SELECT home_name, x, y, z FROM home_teleports WHERE eos_id='%s' AND map_name='%s'",
        eEos.c_str(), eMap.c_str());

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!g_mysql) return homes;

    if (pmysql_query(g_mysql, sql) != 0)
    {
        Log::GetLog()->error("[EnhancedTeleporting] GetHomes query error: {}", pmysql_error(g_mysql));
        return homes;
    }

    MYSQL_RES* res = pmysql_store_result(g_mysql);
    if (!res) return homes;

    while (MYSQL_ROW row = pmysql_fetch_row(res))
    {
        if (row[0] && row[1] && row[2] && row[3])
        {
            HomeEntry h;
            h.name = row[0];
            h.x = std::atof(row[1]);
            h.y = std::atof(row[2]);
            h.z = std::atof(row[3]);
            homes.push_back(h);
        }
    }

    pmysql_free_result(res);
    return homes;
}

static bool GetHome(const std::string& eosId, const std::string& mapName,
    const std::string& homeName, double& outX, double& outY, double& outZ)
{
    const std::string eEos = EscapeUnsafe(eosId);
    const std::string eMap = EscapeUnsafe(mapName);
    const std::string eName = EscapeUnsafe(homeName);

    char sql[256];
    std::snprintf(sql, sizeof(sql),
        "SELECT x, y, z FROM home_teleports WHERE eos_id='%s' AND map_name='%s' AND home_name='%s'",
        eEos.c_str(), eMap.c_str(), eName.c_str());

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!g_mysql) return false;

    if (pmysql_query(g_mysql, sql) != 0) return false;

    MYSQL_RES* res = pmysql_store_result(g_mysql);
    if (!res) return false;

    bool found = false;
    MYSQL_ROW row = pmysql_fetch_row(res);
    if (row && row[0] && row[1] && row[2])
    {
        outX = std::atof(row[0]);
        outY = std::atof(row[1]);
        outZ = std::atof(row[2]);
        found = true;
    }

    pmysql_free_result(res);
    return found;
}

static int CountHomes(const std::string& eosId, const std::string& mapName)
{
    const std::string eEos = EscapeUnsafe(eosId);
    const std::string eMap = EscapeUnsafe(mapName);

    char sql[256];
    std::snprintf(sql, sizeof(sql),
        "SELECT COUNT(*) FROM home_teleports WHERE eos_id='%s' AND map_name='%s'",
        eEos.c_str(), eMap.c_str());

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!g_mysql) return 0;

    if (pmysql_query(g_mysql, sql) != 0) return 0;

    int count = 0;
    if (MYSQL_RES* res = pmysql_store_result(g_mysql))
    {
        MYSQL_ROW row = pmysql_fetch_row(res);
        if (row && row[0]) count = std::atoi(row[0]);
        pmysql_free_result(res);
    }

    return count;
}

// =============================================================================
// Helpers
// =============================================================================

static std::string FStr(const FString& f)
{
    const char* s = TCHAR_TO_UTF8(*f);
    return (s && s[0]) ? s : "";
}

static std::string GetMapName()
{
    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return "unknown";
    FString map;
    world->GetMapName(&map);
    return FStr(map);
}

static std::string GetEosId(AShooterPlayerController* pc)
{
    if (!pc) return "";
    AShooterPlayerState* ps =
        static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
    if (!ps) return "";
    FString eos;
    ps->GetUniqueNetIdAsString(&eos);
    return FStr(eos);
}

static void Notify(AShooterPlayerController* pc, const std::wstring& msg)
{
    const std::wstring wColor(g_message_color.begin(), g_message_color.end());
    const std::wstring wFull =
        L"<RichColor Color=\"" + wColor + L"\">" + msg + L"</>";

    FString fSender(L"");
    FString fMsg(wFull.c_str());
    AsaApi::GetApiUtils().SendChatMessage(pc, fSender, L"{}", std::wstring_view(*fMsg));
}

static std::string ToLower(const std::string& s)
{
    std::string out = s;
    for (char& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

static bool IsValidHomeName(const std::string& name)
{
    if (name.empty() || name.size() > 20) return false;
    for (char c : name)
    {
        if (!std::isalnum((unsigned char)c) && c != '_') return false;
    }
    return true;
}

static bool IsMounted(AShooterPlayerController* pc)
{
    AActor* ch = pc->BaseGetPlayerCharacter();
    if (!ch) return false;
    AShooterCharacter* character = static_cast<AShooterCharacter*>(ch);
    return character->RidingDinoField() != nullptr;
}

// =============================================================================
// Permission Tier Resolution
// =============================================================================

static GroupTier ResolveTier(const std::string& eosId)
{
    GroupTier best = g_default_tier;

    if (g_group_tiers.empty()) return best;

    if (!g_permissions_attempted) LoadPermissionsAPI();
    if (!g_permissions_loaded || !pGetPlayerGroups) return best;

    try
    {
        FString fEos(eosId.c_str());
        TArray<FString> groups = pGetPlayerGroups(fEos);

        bool matched = false;
        for (int i = 0; i < groups.Num(); ++i)
        {
            const std::string groupName = FStr(groups[i]);
            auto it = g_group_tiers.find(groupName);
            if (it != g_group_tiers.end())
            {
                if (!matched || it->second.maxHomes > best.maxHomes)
                {
                    best = it->second;
                    matched = true;
                }
            }
        }
    }
    catch (...)
    {
        Log::GetLog()->warn("[EnhancedTeleporting] GetPlayerGroups threw, using default tier");
    }

    return best;
}

static bool IsFoundationRequired(const GroupTier& tier)
{
    return tier.foundationRequired;
}

// =============================================================================
// Foundation Check
// =============================================================================

static bool HasOwnedFoundationAt(double px, double py, double pz, int playerTeam, const GroupTier& tier, std::wstring* blockedReason = nullptr)
{
    if (playerTeam == 0) return false;

    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return false;

    ULevel* level = world->PersistentLevelField();
    if (!level) return false;

    auto actors = level->ActorsField();

    for (int i = 0; i < actors.Num(); ++i)
    {
        AActor* actor = actors[i];
        if (!actor) continue;
        if (!actor->IsA(APrimalStructure::StaticClass())) continue;

        APrimalStructure* structure = static_cast<APrimalStructure*>(actor);
        if (structure->TargetingTeamField() != playerTeam) continue;

        USceneComponent* sRoot = structure->RootComponentField();
        if (!sRoot) continue;

        auto sLoc = sRoot->RelativeLocationField();
        const double dx = px - sLoc.X;
        const double dy = py - sLoc.Y;
        const double dz = pz - sLoc.Z;

        const double hDist = std::sqrt(dx * dx + dy * dy);

        const std::string bp = FStr(AsaApi::GetApiUtils().GetBlueprint(structure));
        std::string bpLower = ToLower(bp);

        bool isFoundation = bpLower.find("foundation") != std::string::npos ||
            bpLower.find("floor") != std::string::npos;

        bool isCliff = bpLower.find("cliffplatform") != std::string::npos;
        bool isVacuum = bpLower.find("underwater_base") != std::string::npos;
        bool isTree = bpLower.find("treeplatform") != std::string::npos;

        double maxH = 150.0;
        double maxZ = 400.0;

        if (isCliff)
        {
            if (bpLower.find("_small") != std::string::npos)
                maxH = 1200.0;
            else if (bpLower.find("_medium") != std::string::npos)
                maxH = 1800.0;
            else
                maxH = 2400.0;
            maxZ = 600.0;
        }
        else if (isTree)
        {
            maxH = 2400.0;
            maxZ = 600.0;
        }
        else if (isVacuum)
        {
            maxH = 300.0;
            maxZ = 600.0;
        }

        if (hDist > maxH) continue;
        if (dz < -50.0 || dz > maxZ) continue;

        if (isFoundation) return true;
        if (isCliff && tier.allowCliffPlatforms) return true;
        if (isVacuum && tier.allowVacuumCompartments) return true;
        if (isTree && tier.allowTreePlatforms) return true;

        if (blockedReason)
        {
            if (isCliff && !tier.allowCliffPlatforms)
                *blockedReason = L"a cliff platform";
            else if (isVacuum && !tier.allowVacuumCompartments)
                *blockedReason = L"a vacuum compartment";
            else if (isTree && !tier.allowTreePlatforms)
                *blockedReason = L"a tree platform";
        }
    }

    return false;
}

static bool IsOnOwnedFoundation(AShooterPlayerController* pc, const GroupTier& tier, std::wstring* blockedReason = nullptr)
{
    AActor* charActor = pc->BaseGetPlayerCharacter();
    if (!charActor) return false;

    USceneComponent* root = charActor->RootComponentField();
    if (!root) return false;

    auto playerLoc = root->RelativeLocationField();
    const int playerTeam = charActor->TargetingTeamField();

    return HasOwnedFoundationAt(playerLoc.X, playerLoc.Y, playerLoc.Z, playerTeam, tier, blockedReason);
}

static bool IsInNoBuildZone(AShooterPlayerController* pc)
{
    AActor* charActor = pc->BaseGetPlayerCharacter();
    if (!charActor) return false;

    USceneComponent* root = charActor->RootComponentField();
    if (!root) return false;

    auto playerLoc = root->RelativeLocationField();
    UE::Math::TVector<double> point{ playerLoc.X, playerLoc.Y, playerLoc.Z };

    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return false;

    return AStructurePreventionZoneVolume::IsWithinAnyVolume(world, &point, false, nullptr, false, false, nullptr);
}

// =============================================================================
// Cooldown Tracking (Home + TPA separate)
// =============================================================================

static std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_cooldowns;
static std::mutex g_cooldown_mutex;

static bool IsOnCooldown(const std::string& eosId, int cooldownSec,
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>& map,
    std::mutex& mtx)
{
    if (cooldownSec <= 0) return false;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mtx);
    auto it = map.find(eosId);
    if (it == map.end()) return false;
    return std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count() < cooldownSec;
}

static int GetCooldownRemaining(const std::string& eosId, int cooldownSec,
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>& map,
    std::mutex& mtx)
{
    if (cooldownSec <= 0) return 0;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mtx);
    auto it = map.find(eosId);
    if (it == map.end()) return 0;
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();
    if (elapsed >= cooldownSec) return 0;
    return (int)(cooldownSec - elapsed);
}

static void SetCooldown(const std::string& eosId,
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>& map,
    std::mutex& mtx)
{
    std::lock_guard<std::mutex> lock(mtx);
    map[eosId] = std::chrono::steady_clock::now();
}

// =============================================================================
// Pending Home Teleport Queue
// =============================================================================

struct PendingTeleport
{
    std::string eosId;
    std::string homeName;
    std::string mapName;
    double targetX, targetY, targetZ;
    std::chrono::steady_clock::time_point dueTime;
};

static std::vector<PendingTeleport> g_pending;
static std::mutex                   g_pending_mutex;

static bool HasPending(const std::string& eosId)
{
    std::lock_guard<std::mutex> lock(g_pending_mutex);
    for (const auto& p : g_pending)
        if (p.eosId == eosId) return true;
    return false;
}

// =============================================================================
// TPA Request System
// =============================================================================

struct TPARequest
{
    std::string senderEos;
    std::string receiverEos;
    std::string senderName;
    std::chrono::steady_clock::time_point expiry;
};

static std::vector<TPARequest> g_tpa_requests;
static std::mutex              g_tpa_mutex;

struct OnlinePlayer
{
    AShooterPlayerController* pc;
    std::string eosId;
    std::string survivorName;
    std::string tribeName;
};

static std::vector<OnlinePlayer> FindOnlineByName(const std::string& searchName)
{
    std::vector<OnlinePlayer> results;

    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return results;

    ULevel* level = world->PersistentLevelField();
    if (!level) return results;

    auto actors = level->ActorsField();
    const std::string searchLower = ToLower(searchName);

    for (int i = 0; i < actors.Num(); ++i)
    {
        AActor* actor = actors[i];
        if (!actor) continue;
        if (!actor->IsA(AShooterPlayerController::StaticClass())) continue;

        AShooterPlayerController* pc = static_cast<AShooterPlayerController*>(actor);

        AActor* ch = pc->BaseGetPlayerCharacter();
        if (!ch) continue;

        AShooterCharacter* character = static_cast<AShooterCharacter*>(ch);

        FString nameRaw = character->PlayerNameField();
        const std::string survivorName = FStr(nameRaw);
        if (survivorName.empty()) continue;

        if (ToLower(survivorName) != searchLower) continue;

        AShooterPlayerState* ps =
            static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());

        std::string tribeName;
        if (ps && ps->IsInTribe())
        {
            FString tribeRaw = character->TribeNameField();
            tribeName = FStr(tribeRaw);
        }

        OnlinePlayer op;
        op.pc = pc;
        op.eosId = GetEosId(pc);
        op.survivorName = survivorName;
        op.tribeName = tribeName;
        results.push_back(op);
    }

    return results;
}

// =============================================================================
// Block State (read from KxrsedBlocking player_blocks)
// =============================================================================

static long long GetActiveBlockExpiry(const std::string& eosId, const char* blockType)
{
    const std::string eEos = EscapeBlock(eosId);
    const std::string eMap = EscapeBlock(GetMapName());

    char sql[320];
    std::snprintf(sql, sizeof(sql),
        "SELECT expires_at FROM player_blocks WHERE eos_id='%s' AND map_name='%s' AND block_type='%s'",
        eEos.c_str(), eMap.c_str(), blockType);

    std::lock_guard<std::mutex> lock(g_block_mutex);
    if (!g_block_mysql) return 0;
    if (pmysql_query(g_block_mysql, sql) != 0) return 0;

    long long expiry = 0;
    if (MYSQL_RES* res = pmysql_store_result(g_block_mysql))
    {
        MYSQL_ROW row = pmysql_fetch_row(res);
        if (row && row[0]) expiry = std::atoll(row[0]);
        pmysql_free_result(res);
    }
    return expiry;
}

static bool ProbePlayerBlocksTable()
{
    std::lock_guard<std::mutex> lock(g_block_mutex);
    if (!g_block_mysql) return false;
    if (pmysql_query(g_block_mysql, "SELECT 1 FROM player_blocks LIMIT 1") != 0) return false;
    if (MYSQL_RES* res = pmysql_store_result(g_block_mysql))
        pmysql_free_result(res);
    return true;
}

static bool IsCombatBlocked(const std::string& eosId, int& remaining)
{
    if (!g_combat_block_enabled || !g_player_blocks_available) return false;
    const long long now = (long long)time(nullptr);
    const long long expiry = GetActiveBlockExpiry(eosId, "combat");
    if (expiry <= now) return false;
    remaining = (int)(expiry - now);
    return true;
}

static bool IsRaidBlocked(const std::string& eosId)
{
    if (!g_raid_block_enabled || !g_player_blocks_available) return false;
    const long long now = (long long)time(nullptr);
    return GetActiveBlockExpiry(eosId, "raid") > now;
}

// =============================================================================
// Teleport Execution
// =============================================================================

static bool TeleportPlayer(AShooterPlayerController* pc, double x, double y, double z)
{
    AActor* ch = pc->BaseGetPlayerCharacter();
    if (!ch) return false;

    AShooterCharacter* character = static_cast<AShooterCharacter*>(ch);

    if (character->RidingDinoField() != nullptr)
    {
        Notify(pc, L"You cannot teleport while mounted.");
        return false;
    }

    UE::Math::TVector<double> dest{ x, y, z };
    UE::Math::TRotator<double> rot{ 0.0, 0.0, 0.0 };

    USceneComponent* root = character->RootComponentField();
    if (root) rot = root->RelativeRotationField();

    character->TeleportTo(&dest, &rot, false, true);

    Log::GetLog()->info("[EnhancedTeleporting] Teleported player to ({:.0f}, {:.0f}, {:.0f})", x, y, z);
    return true;
}

static bool TeleportToPlayer(AShooterPlayerController* sender, AShooterPlayerController* receiver)
{
    AActor* rch = receiver->BaseGetPlayerCharacter();
    if (!rch) return false;

    USceneComponent* root = rch->RootComponentField();
    if (!root) return false;

    auto loc = root->RelativeLocationField();
    return TeleportPlayer(sender, loc.X, loc.Y, loc.Z);
}

// =============================================================================
// Timer Callback - Process Pending Teleports & TPA Cleanup
// =============================================================================

static void ProcessPendingTeleports()
{
    const auto now = std::chrono::steady_clock::now();

    // Process home teleports
    {
        std::lock_guard<std::mutex> lock(g_pending_mutex);

        for (auto it = g_pending.begin(); it != g_pending.end(); )
        {
            FString fEos(it->eosId.c_str());
            AShooterPlayerController* pc = AsaApi::GetApiUtils().FindPlayerFromEOSID(fEos);
            if (!pc)
            {
                it = g_pending.erase(it);
                continue;
            }

            AActor* ch = pc->BaseGetPlayerCharacter();
            if (!ch)
            {
                Notify(pc, L"Teleport cancelled: no character.");
                it = g_pending.erase(it);
                continue;
            }

            if (now >= it->dueTime)
            {
                const int playerTeam = ch->TargetingTeamField();
                const GroupTier tier = ResolveTier(it->eosId);

                {
                    int rem = 0;
                    if (IsCombatBlocked(it->eosId, rem))
                    {
                        Notify(pc, L"Teleport cancelled. You are in combat.");
                        it = g_pending.erase(it);
                        continue;
                    }
                    if (IsRaidBlocked(it->eosId))
                    {
                        Notify(pc, L"Teleport cancelled. You are in an active raid zone.");
                        it = g_pending.erase(it);
                        continue;
                    }
                }

                if (IsFoundationRequired(tier) && !HasOwnedFoundationAt(it->targetX, it->targetY, it->targetZ, playerTeam, tier))
                {
                    DeleteHome(it->eosId, it->mapName, it->homeName);
                    std::wstring wmsg = L"Home '" +
                        std::wstring(it->homeName.begin(), it->homeName.end()) +
                        L"' no longer has a valid foundation. Home deleted.";
                    Notify(pc, wmsg);
                    Log::GetLog()->info("[EnhancedTeleporting] HOME_INVALIDATED eos={} name={}", it->eosId, it->homeName);
                    it = g_pending.erase(it);
                    continue;
                }

                if (TeleportPlayer(pc, it->targetX, it->targetY, it->targetZ))
                {
                    SetCooldown(it->eosId, g_cooldowns, g_cooldown_mutex);
                    std::wstring wmsg = L"Teleported to home: " +
                        std::wstring(it->homeName.begin(), it->homeName.end());
                    Notify(pc, wmsg);
                }
                else
                {
                    Notify(pc, L"Teleport failed.");
                }

                it = g_pending.erase(it);
                continue;
            }

            ++it;
        }
    }

    // Expire TPA requests
    {
        std::lock_guard<std::mutex> lock(g_tpa_mutex);
        g_tpa_requests.erase(
            std::remove_if(g_tpa_requests.begin(), g_tpa_requests.end(),
                [&now](const TPARequest& r) { return now >= r.expiry; }),
            g_tpa_requests.end());
    }
}

// =============================================================================
// Command Handlers - Home
// =============================================================================

static void Cmd_SetHome(AShooterPlayerController* pc, FString* message, int /*mode*/, int /*platform*/)
{
    const std::string msg = FStr(*message);
    const auto space = msg.find(' ');
    if (space == std::string::npos || space + 1 >= msg.size())
    {
        Notify(pc, L"Usage: /sethome <name>");
        return;
    }

    const std::string homeName = ToLower(msg.substr(space + 1));
    if (!IsValidHomeName(homeName))
    {
        Notify(pc, L"Invalid home name. Use letters, numbers, underscore. Max 20 chars.");
        return;
    }

    const std::string eosId = GetEosId(pc);
    if (eosId.empty()) return;

    if (IsMounted(pc))
    {
        Notify(pc, L"You cannot set a home while mounted.");
        return;
    }

    const std::string mapName = GetMapName();
    const GroupTier tier = ResolveTier(eosId);

    if (!tier.allowNoBuildZones && IsInNoBuildZone(pc))
    {
        Notify(pc, L"You cannot set a home in or around a no build area.");
        return;
    }

    if (IsFoundationRequired(tier))
    {
        std::wstring blockedReason;
        if (!IsOnOwnedFoundation(pc, tier, &blockedReason))
        {
            if (!blockedReason.empty())
                Notify(pc, L"You are not allowed to set a home on " + blockedReason + L".");
            else
                Notify(pc, L"You must be standing on a foundation you own.");
            return;
        }
    }

    const int current = CountHomes(eosId, mapName);

    double dummy1, dummy2, dummy3;
    if (GetHome(eosId, mapName, homeName, dummy1, dummy2, dummy3))
    {
        Notify(pc, L"You already have a home with this name.");
        return;
    }

    if (current >= tier.maxHomes)
    {
        std::wstring wmsg = L"You have reached your home limit (" +
            std::to_wstring(tier.maxHomes) + L").";
        Notify(pc, wmsg);
        return;
    }

    AActor* ch = pc->BaseGetPlayerCharacter();
    if (!ch) return;

    USceneComponent* root = ch->RootComponentField();
    if (!root) return;

    auto loc = root->RelativeLocationField();

    if (SaveHome(eosId, mapName, homeName, loc.X, loc.Y, loc.Z))
    {
        std::wstring wmsg = L"Home '" +
            std::wstring(homeName.begin(), homeName.end()) + L"' saved.";
        Notify(pc, wmsg);
        Log::GetLog()->info("[EnhancedTeleporting] SETHOME eos={} map={} name={} ({:.0f}, {:.0f}, {:.0f})",
            eosId, mapName, homeName, loc.X, loc.Y, loc.Z);
    }
    else
    {
        Notify(pc, L"Failed to save home. Try again.");
    }
}

static void Cmd_Home(AShooterPlayerController* pc, FString* message, int /*mode*/, int /*platform*/)
{
    const std::string msg = FStr(*message);
    const auto space = msg.find(' ');
    if (space == std::string::npos || space + 1 >= msg.size())
    {
        Notify(pc, L"Usage: /home <name>");
        return;
    }

    const std::string homeName = ToLower(msg.substr(space + 1));
    if (!IsValidHomeName(homeName)) { Notify(pc, L"Invalid home name."); return; }

    const std::string eosId = GetEosId(pc);
    if (eosId.empty()) return;

    if (HasPending(eosId))
    {
        Notify(pc, L"You already have a teleport in progress.");
        return;
    }

    if (IsMounted(pc))
    {
        Notify(pc, L"You cannot teleport while mounted.");
        return;
    }

    const GroupTier tier = ResolveTier(eosId);

    if (IsOnCooldown(eosId, tier.cooldownSeconds, g_cooldowns, g_cooldown_mutex))
    {
        const int remaining = GetCooldownRemaining(eosId, tier.cooldownSeconds, g_cooldowns, g_cooldown_mutex);
        std::wstring wmsg = L"Teleport on cooldown. " + std::to_wstring(remaining) + L"s remaining.";
        Notify(pc, wmsg);
        return;
    }

    {
        int rem = 0;
        if (IsCombatBlocked(eosId, rem))
        {
            Notify(pc, L"You are in combat. " + std::to_wstring(rem) + L"s remaining.");
            return;
        }
        if (IsRaidBlocked(eosId))
        {
            Notify(pc, L"You are in an active raid zone.");
            return;
        }
    }

    const std::string mapName = GetMapName();
    double x, y, z;
    if (!GetHome(eosId, mapName, homeName, x, y, z))
    {
        Notify(pc, L"Home not found.");
        return;
    }

    if (tier.teleportDelay <= 0)
    {
        AActor* ch = pc->BaseGetPlayerCharacter();
        if (!ch) return;
        const int playerTeam = ch->TargetingTeamField();

        if (IsFoundationRequired(tier) && !HasOwnedFoundationAt(x, y, z, playerTeam, tier))
        {
            DeleteHome(eosId, mapName, homeName);
            std::wstring wmsg = L"Home '" +
                std::wstring(homeName.begin(), homeName.end()) +
                L"' no longer has a valid foundation. Home deleted.";
            Notify(pc, wmsg);
            return;
        }

        if (TeleportPlayer(pc, x, y, z))
        {
            SetCooldown(eosId, g_cooldowns, g_cooldown_mutex);
            std::wstring wmsg = L"Teleported to home: " +
                std::wstring(homeName.begin(), homeName.end());
            Notify(pc, wmsg);
        }
        else
        {
            Notify(pc, L"Teleport failed.");
        }
        return;
    }

    PendingTeleport pt;
    pt.eosId = eosId;
    pt.homeName = homeName;
    pt.mapName = mapName;
    pt.targetX = x;
    pt.targetY = y;
    pt.targetZ = z;
    pt.dueTime = std::chrono::steady_clock::now() +
        std::chrono::seconds(tier.teleportDelay);

    {
        std::lock_guard<std::mutex> lock(g_pending_mutex);
        g_pending.push_back(pt);
    }

    std::wstring wmsg = L"Teleporting in " + std::to_wstring(tier.teleportDelay) + L"s...";
    Notify(pc, wmsg);

    Log::GetLog()->info("[EnhancedTeleporting] HOME_QUEUED eos={} name={} delay={}s",
        eosId, homeName, tier.teleportDelay);
}

static void Cmd_HomeDel(AShooterPlayerController* pc, FString* message, int /*mode*/, int /*platform*/)
{
    const std::string msg = FStr(*message);
    const auto space = msg.find(' ');
    if (space == std::string::npos || space + 1 >= msg.size())
    {
        Notify(pc, L"Usage: /delhome <name>");
        return;
    }

    const std::string homeName = ToLower(msg.substr(space + 1));
    if (!IsValidHomeName(homeName)) { Notify(pc, L"Invalid home name."); return; }

    const std::string eosId = GetEosId(pc);
    if (eosId.empty()) return;

    const std::string mapName = GetMapName();

    if (DeleteHome(eosId, mapName, homeName))
    {
        std::wstring wmsg = L"Home '" +
            std::wstring(homeName.begin(), homeName.end()) + L"' deleted.";
        Notify(pc, wmsg);
        Log::GetLog()->info("[EnhancedTeleporting] HOMEDEL eos={} map={} name={}", eosId, mapName, homeName);
    }
    else
    {
        Notify(pc, L"Failed to delete home.");
    }
}

static void Cmd_Homes(AShooterPlayerController* pc, FString* /*message*/, int /*mode*/, int /*platform*/)
{
    const std::string eosId = GetEosId(pc);
    if (eosId.empty()) return;

    const std::string mapName = GetMapName();
    const GroupTier tier = ResolveTier(eosId);
    const auto homes = GetHomes(eosId, mapName);

    if (homes.empty())
    {
        Notify(pc, L"You have no saved homes on this map.");
        return;
    }

    std::wstring list = L"Your homes (" + std::to_wstring(homes.size()) +
        L"/" + std::to_wstring(tier.maxHomes) + L"): ";

    for (size_t i = 0; i < homes.size(); ++i)
    {
        if (i > 0) list += L", ";
        list += std::wstring(homes[i].name.begin(), homes[i].name.end());
    }

    Notify(pc, list);
}

// =============================================================================
// Command Handlers - TPA
// =============================================================================

static void Cmd_TPR(AShooterPlayerController* pc, FString* message, int /*mode*/, int /*platform*/)
{
    const std::string msg = FStr(*message);
    const auto space = msg.find(' ');
    if (space == std::string::npos || space + 1 >= msg.size())
    {
        Notify(pc, L"Usage: /tpr <survivorname> [number]");
        return;
    }

    const std::string args = msg.substr(space + 1);
    const std::string senderEos = GetEosId(pc);
    if (senderEos.empty()) return;

    if (IsMounted(pc))
    {
        Notify(pc, L"You cannot send a teleport request while mounted.");
        return;
    }

    {
        int rem = 0;
        if (IsCombatBlocked(senderEos, rem))
        {
            Notify(pc, L"You are in combat. " + std::to_wstring(rem) + L"s remaining.");
            return;
        }
        if (IsRaidBlocked(senderEos))
        {
            Notify(pc, L"You are in an active raid zone.");
            return;
        }
    }

    // Check if sender already has an outgoing request
    {
        std::lock_guard<std::mutex> lock(g_tpa_mutex);
        for (const auto& r : g_tpa_requests)
        {
            if (r.senderEos == senderEos)
            {
                Notify(pc, L"You already have a pending teleport request.");
                return;
            }
        }
    }

    // Parse name and optional number
    // Try full args as name first, only strip trailing number if no matches
    std::string searchName = args;
    int selection = 0;

    auto matches = FindOnlineByName(searchName);

    // Remove self from matches
    matches.erase(
        std::remove_if(matches.begin(), matches.end(),
            [&senderEos](const OnlinePlayer& p) { return p.eosId == senderEos; }),
        matches.end());

    // If no matches and last token is a number, retry with number as selection
    if (matches.empty())
    {
        const auto lastSpace = args.rfind(' ');
        if (lastSpace != std::string::npos)
        {
            const std::string lastPart = args.substr(lastSpace + 1);
            bool isNumber = !lastPart.empty();
            for (char c : lastPart)
            {
                if (!std::isdigit((unsigned char)c)) { isNumber = false; break; }
            }
            if (isNumber)
            {
                selection = std::atoi(lastPart.c_str());
                searchName = args.substr(0, lastSpace);

                matches = FindOnlineByName(searchName);
                matches.erase(
                    std::remove_if(matches.begin(), matches.end(),
                        [&senderEos](const OnlinePlayer& p) { return p.eosId == senderEos; }),
                    matches.end());
            }
        }
    }

    if (matches.empty())
    {
        std::wstring wmsg = std::wstring(searchName.begin(), searchName.end()) +
            L" is not currently online.";
        Notify(pc, wmsg);
        return;
    }

    if (matches.size() > 1 && selection == 0)
    {
        Notify(pc, L"Multiple survivors found:");
        std::wstring hint = L"Use /tpr " +
            std::wstring(searchName.begin(), searchName.end()) + L" <number>";
        Notify(pc, hint);

        for (size_t i = 0; i < matches.size(); ++i)
        {
            const auto& m = matches[i];
            std::wstring wName(m.survivorName.begin(), m.survivorName.end());
            std::wstring wTribe = m.tribeName.empty() ?
                L"no tribe" :
                std::wstring(m.tribeName.begin(), m.tribeName.end());

            std::wstring line = std::to_wstring(i + 1) + L" - " + wName +
                L" (" + wTribe + L")";
            Notify(pc, line);
        }
        return;
    }

    const OnlinePlayer* target = nullptr;

    if (matches.size() == 1)
    {
        target = &matches[0];
    }
    else if (selection >= 1 && selection <= (int)matches.size())
    {
        target = &matches[selection - 1];
    }
    else
    {
        Notify(pc, L"Invalid selection number.");
        return;
    }

    // Get sender's survivor name
    AActor* sch = pc->BaseGetPlayerCharacter();
    std::string senderName;
    if (sch)
    {
        AShooterCharacter* sChar = static_cast<AShooterCharacter*>(sch);
        senderName = FStr(sChar->PlayerNameField());
    }
    if (senderName.empty()) senderName = "Unknown";

    // Check if receiver already has a pending incoming request
    {
        std::lock_guard<std::mutex> lock(g_tpa_mutex);
        for (const auto& r : g_tpa_requests)
        {
            if (r.receiverEos == target->eosId)
            {
                std::wstring wTarget(target->survivorName.begin(), target->survivorName.end());
                Notify(pc, wTarget + L" already has a pending teleport request.");
                return;
            }
        }
    }

    // Same tribe: auto-teleport without /tpa
    AActor* targetCh = target->pc->BaseGetPlayerCharacter();
    if (sch && targetCh)
    {
        const int senderTeam = sch->TargetingTeamField();
        const int targetTeam = targetCh->TargetingTeamField();

        if (senderTeam != 0 && senderTeam == targetTeam)
        {
            if (IsMounted(target->pc))
            {
                Notify(pc, L"That tribe member is currently mounted.");
                return;
            }

            if (TeleportToPlayer(pc, target->pc))
            {
                std::wstring wTarget(target->survivorName.begin(), target->survivorName.end());
                Notify(pc, L"Teleported to tribe member " + wTarget + L".");

                std::wstring wSender(senderName.begin(), senderName.end());
                Notify(target->pc, wSender + L" teleported to you.");

                Log::GetLog()->info("[EnhancedTeleporting] TPR_TRIBE sender={} receiver={}", senderEos, target->eosId);
            }
            else
            {
                Notify(pc, L"Teleport failed.");
            }
            return;
        }
    }

    TPARequest req;
    req.senderEos = senderEos;
    req.receiverEos = target->eosId;
    req.senderName = senderName;
    req.expiry = std::chrono::steady_clock::now() + std::chrono::seconds(g_tpa_timeout);

    {
        std::lock_guard<std::mutex> lock(g_tpa_mutex);
        g_tpa_requests.push_back(req);
    }

    // Notify sender
    std::wstring wTarget(target->survivorName.begin(), target->survivorName.end());
    std::wstring smsg = L"Teleport request sent to " + wTarget + L". Expires in " +
        std::to_wstring(g_tpa_timeout) + L"s.";
    Notify(pc, smsg);

    // Notify receiver
    std::wstring wSender(senderName.begin(), senderName.end());
    std::wstring rmsg = wSender + L" wants to teleport to you. Type /tpa to accept.";
    Notify(target->pc, rmsg);

    Log::GetLog()->info("[EnhancedTeleporting] TPR sender={} receiver={}", senderEos, target->eosId);
}

static void Cmd_TPA(AShooterPlayerController* pc, FString* /*message*/, int /*mode*/, int /*platform*/)
{
    const std::string receiverEos = GetEosId(pc);
    if (receiverEos.empty()) return;

    TPARequest found;
    bool hasRequest = false;

    {
        std::lock_guard<std::mutex> lock(g_tpa_mutex);
        const auto now = std::chrono::steady_clock::now();

        for (auto it = g_tpa_requests.begin(); it != g_tpa_requests.end(); ++it)
        {
            if (it->receiverEos == receiverEos && now < it->expiry)
            {
                found = *it;
                hasRequest = true;
                g_tpa_requests.erase(it);
                break;
            }
        }
    }

    if (!hasRequest)
    {
        Notify(pc, L"You have no pending teleport requests.");
        return;
    }

    // Find sender online
    FString fSenderEos(found.senderEos.c_str());
    AShooterPlayerController* senderPc = AsaApi::GetApiUtils().FindPlayerFromEOSID(fSenderEos);
    if (!senderPc)
    {
        Notify(pc, L"The requesting player is no longer online.");
        return;
    }

    AActor* senderCh = senderPc->BaseGetPlayerCharacter();
    if (!senderCh)
    {
        std::wstring wSender(found.senderName.begin(), found.senderName.end());
        Notify(pc, wSender + L" has died. Teleport cancelled.");
        return;
    }

    AActor* receiverCh = pc->BaseGetPlayerCharacter();
    if (!receiverCh)
    {
        Notify(senderPc, L"The survivor you are teleporting to has died.");
        return;
    }

    if (IsMounted(senderPc))
    {
        Notify(pc, L"The requesting player is currently mounted.");
        Notify(senderPc, L"Teleport request accepted but you are mounted. Dismount first.");
        return;
    }

    if (IsMounted(pc))
    {
        Notify(pc, L"You cannot accept a teleport request while mounted.");
        return;
    }

    {
        int rem = 0;
        if (IsCombatBlocked(found.senderEos, rem))
        {
            Notify(senderPc, L"You are in combat. " + std::to_wstring(rem) + L"s remaining.");
            Notify(pc, L"The requesting player is in combat.");
            return;
        }
        if (IsRaidBlocked(found.senderEos))
        {
            Notify(senderPc, L"You are in an active raid zone.");
            Notify(pc, L"The requesting player is in an active raid zone.");
            return;
        }
    }

    if (TeleportToPlayer(senderPc, pc))
    {
        std::wstring wSender(found.senderName.begin(), found.senderName.end());
        Notify(pc, wSender + L" has been teleported to you.");

        AActor* rch = pc->BaseGetPlayerCharacter();
        std::string receiverName;
        if (rch)
        {
            AShooterCharacter* rChar = static_cast<AShooterCharacter*>(rch);
            receiverName = FStr(rChar->PlayerNameField());
        }
        std::wstring wReceiver(receiverName.begin(), receiverName.end());
        Notify(senderPc, L"Teleported to " + wReceiver + L".");

        Log::GetLog()->info("[EnhancedTeleporting] TPA_ACCEPTED sender={} receiver={}", found.senderEos, receiverEos);
    }
    else
    {
        Notify(pc, L"Teleport failed.");
        Notify(senderPc, L"Teleport failed.");
    }
}

static void Cmd_TPC(AShooterPlayerController* pc, FString* /*message*/, int /*mode*/, int /*platform*/)
{
    const std::string senderEos = GetEosId(pc);
    if (senderEos.empty()) return;

    std::string receiverEos;
    bool found = false;

    {
        std::lock_guard<std::mutex> lock(g_tpa_mutex);
        for (auto it = g_tpa_requests.begin(); it != g_tpa_requests.end(); ++it)
        {
            if (it->senderEos == senderEos)
            {
                receiverEos = it->receiverEos;
                g_tpa_requests.erase(it);
                found = true;
                break;
            }
        }
    }

    if (!found)
    {
        Notify(pc, L"You have no pending teleport request to cancel.");
        return;
    }

    Notify(pc, L"Teleport request cancelled.");

    FString fReceiverEos(receiverEos.c_str());
    AShooterPlayerController* receiverPc = AsaApi::GetApiUtils().FindPlayerFromEOSID(fReceiverEos);
    if (receiverPc)
    {
        Notify(receiverPc, L"The teleport request has been cancelled.");
    }

    Log::GetLog()->info("[EnhancedTeleporting] TPC sender={}", senderEos);
}

// =============================================================================
// Plugin Entry Points
// =============================================================================

extern "C" __declspec(dllexport) void Plugin_Init()
{
    Log::Get().Init("EnhancedTeleporting");

    if (!LoadConfig())
    {
        Log::GetLog()->error("[EnhancedTeleporting] Halted: config error");
        return;
    }

    if (!InitDatabase())
    {
        Log::GetLog()->error("[EnhancedTeleporting] Halted: database error");
        return;
    }

    if (InitBlockDatabase())
        g_player_blocks_available = ProbePlayerBlocksTable();
    if (!g_player_blocks_available && (g_combat_block_enabled || g_raid_block_enabled))
        Log::GetLog()->error("[EnhancedTeleporting] player_blocks table unreachable - combat and raid block disabled until restart");

    AsaApi::GetCommands().AddChatCommand(FString(L"/sethome"), &Cmd_SetHome);
    AsaApi::GetCommands().AddChatCommand(FString(L"/home"), &Cmd_Home);
    AsaApi::GetCommands().AddChatCommand(FString(L"/delhome"), &Cmd_HomeDel);
    AsaApi::GetCommands().AddChatCommand(FString(L"/listhome"), &Cmd_Homes);
    AsaApi::GetCommands().AddChatCommand(FString(L"/tpr"), &Cmd_TPR);
    AsaApi::GetCommands().AddChatCommand(FString(L"/tpa"), &Cmd_TPA);
    AsaApi::GetCommands().AddChatCommand(FString(L"/tpc"), &Cmd_TPC);

    AsaApi::GetCommands().AddOnTimerCallback(FString(L"EnhancedTeleporting_Timer"), &ProcessPendingTeleports);
    AsaApi::GetCommands().AddOnTimerCallback(FString(L"EnhancedTeleporting_Reload"), &ReloadConfigTick);

    Log::GetLog()->info("[EnhancedTeleporting] Plugin loaded");
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/sethome"));
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/home"));
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/delhome"));
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/listhome"));
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/tpr"));
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/tpa"));
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/tpc"));

    AsaApi::GetCommands().RemoveOnTimerCallback(FString(L"EnhancedTeleporting_Timer"));
    AsaApi::GetCommands().RemoveOnTimerCallback(FString(L"EnhancedTeleporting_Reload"));

    {
        std::lock_guard<std::mutex> lock(g_pending_mutex);
        g_pending.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_cooldown_mutex);
        g_cooldowns.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_tpa_mutex);
        g_tpa_requests.clear();
    }

    CloseDatabase();

    Log::GetLog()->info("[EnhancedTeleporting] Plugin unloaded");
}