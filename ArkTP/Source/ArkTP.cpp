/*
ArkTP - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * ArkTP - ASA Plugin
 *
 * Hooks: None - chat commands and timer only
 *
 * Table:
 *   arktp_homes  PK (eos_id, map_name, home_name)
 *     Columns: eos_id VARCHAR(64), map_name VARCHAR(64), home_name VARCHAR(32), x DOUBLE, y DOUBLE, z DOUBLE
 *
 * Commands:
 *   /sethome {name}  save current location, must be on an owned foundation unless the tier allows otherwise
 *   /home {name}     teleport to a saved home, delayed and revalidated at fire time
 *   /delhome {name}  delete a saved home
 *   /listhome        list saved homes on the current map
 *   /tpr {name} [n]  send a teleport request to an online survivor by name, same tribe auto teleports
 *   /tpa             accept an incoming teleport request
 *   /tpc             cancel an outgoing teleport request
 *   /tp {survivorid}   admin, teleport yourself to an online survivor by character id
 *   /tps {survivorid}  admin, teleport an online survivor to you by character id
 *
 * Config:
 *   ArkApi/Plugins/ArkTP/config.json
 *   DbHost defaults to 127.0.0.1. DbUser and DbName are required or the plugin halts.
 *   AdminEosIds: EOS IDs allowed to run /tp and /tps
 *   MessageColor: RichColor value for plugin chat output
 *   TpaTimeoutSeconds: lifetime of an outgoing teleport request
 *   CombatBlockEnabled, RaidBlockEnabled: master toggles for the Blockade gates
 *   Default: tier applied to anyone matching no configured group
 *   Groups: per Permissions group tier overrides. Every group requires a unique integer Priority.
 *           Lower Priority wins (1 is highest). Missing or duplicate Priority is a hard config
 *           error and the plugin halts.
 *
 * Config Example:
 * {
 *   "DbHost": "127.0.0.1",
 *   "DbPort": 3306,
 *   "DbUser": "User",
 *   "DbPassword": "Password",
 *   "DbName": "Database",
 *   "AdminEosIds": [ "EOS_ID" ],
 *   "MessageColor": "1.0,1.0,1.0,1.0",
 *   "TpaTimeoutSeconds": 30,
 *   "CombatBlockEnabled": true,
 *   "RaidBlockEnabled": true,
 *   "Default": {
 *     "MaxHomes": 1,
 *     "TeleportDelaySeconds": 10,
 *     "CooldownSeconds": 300,
 *     "FoundationRequired": true,
 *     "AllowCliffPlatforms": false,
 *     "AllowVacuumCompartments": false,
 *     "AllowTreePlatforms": false,
 *     "AllowNoBuildZones": false
 *   },
 *   "Groups": {
 *     "VIP": {
 *       "Priority": 1,
 *       "MaxHomes": 3,
 *       "TeleportDelaySeconds": 5,
 *       "CooldownSeconds": 120,
 *       "FoundationRequired": true,
 *       "AllowCliffPlatforms": true,
 *       "AllowVacuumCompartments": true,
 *       "AllowTreePlatforms": true,
 *       "AllowNoBuildZones": false
 *     }
 *   }
 * }
 *
 * Permissions:
 *   Optional. Resolved lazily via GetProcAddress from a loaded Permissions.dll, never statically
 *   linked. Only GetPlayerGroups is used. Among the player matched groups the lowest Priority wins.
 *   Falls back to the Default tier if Permissions.dll is absent.
 *
 * Blockade:
 *   Optional soft dependency. The combat and raid gates resolve Blockade_IsCombatBlocked and
 *   Blockade_IsRaidBlocked from a loaded Blockade.dll via GetProcAddress on every call, so
 *   Blockade loading later, or being unloaded mid session, is handled without a restart. With
 *   Blockade absent the gates no-op and the plugin runs standalone. Plugin load order is
 *   alphabetical and ArkTP loads before Blockade, so the availability probe is deferred a few
 *   timer ticks past load and logged once.
 */

#include <API/ARK/Ark.h>

#pragma warning(disable: 4191)
#pragma comment(lib, "AsaApi.lib")

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
            Log::GetLog()->info("[ArkTP] Loaded DB library: {}", candidates[i]);
            break;
        }
    }

    if (!g_mysql_module)
    {
        Log::GetLog()->error("[ArkTP] Could not find libmariadb.dll or libmysql.dll");
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
        Log::GetLog()->error("[ArkTP] Failed to resolve required DB functions");
        return false;
    }

    g_mysql_loaded = true;
    return true;
}

typedef TArray<FString>(*GetPlayerGroups_t)(const FString&);

static GetPlayerGroups_t pGetPlayerGroups = nullptr;
static bool              g_permissions_loaded = false;
static bool              g_permissions_attempted = false;

static void LoadPermissionsAPI()
{
    if (g_permissions_attempted) return;
    g_permissions_attempted = true;

    HMODULE hMod = GetModuleHandleA("Permissions");
    if (!hMod)
    {
        Log::GetLog()->warn("[ArkTP] Permissions.dll not loaded, using default tier for all players");
        return;
    }

    pGetPlayerGroups = (GetPlayerGroups_t)GetProcAddress(hMod,
        "?GetPlayerGroups@Permissions@@YA?AV?$TArray@VFString@@V?$TSizedDefaultAllocator@$0CA@@@@@AEBVFString@@@Z");

    if (!pGetPlayerGroups)
    {
        Log::GetLog()->warn("[ArkTP] Failed to resolve Permissions functions, using default tier");
        return;
    }

    g_permissions_loaded = true;
    Log::GetLog()->info("[ArkTP] Permissions API loaded");
}

typedef bool(*Blockade_IsCombatBlocked_t)(const FString&, int&);
typedef bool(*Blockade_IsRaidBlocked_t)(const FString&);

static bool ResolveBlockade(Blockade_IsCombatBlocked_t& combatFn, Blockade_IsRaidBlocked_t& raidFn)
{
    combatFn = nullptr;
    raidFn = nullptr;

    HMODULE hMod = GetModuleHandleA("Blockade");
    if (!hMod) return false;

    combatFn = (Blockade_IsCombatBlocked_t)GetProcAddress(hMod, "Blockade_IsCombatBlocked");
    raidFn = (Blockade_IsRaidBlocked_t)GetProcAddress(hMod, "Blockade_IsRaidBlocked");
    return combatFn && raidFn;
}

struct GroupTier
{
    int  priority = 0;
    int  maxHomes = 1;
    int  teleportDelay = 10;
    int  cooldownSeconds = 300;
    bool foundationRequired = true;
    bool allowCliffPlatforms = false;
    bool allowVacuumCompartments = false;
    bool allowTreePlatforms = false;
    bool allowNoBuildZones = false;
};

static const std::string g_config_path = "ArkApi/Plugins/ArkTP/config.json";

static std::mutex   g_config_mutex;
static std::string  g_db_host = "127.0.0.1";
static unsigned int g_db_port = 3306;
static std::string  g_db_user;
static std::string  g_db_pass;
static std::string  g_db_name;
static std::string  g_message_color = "1.0,1.0,1.0,1.0";
static int          g_tpa_timeout = 30;
static bool         g_combat_block_enabled = true;
static bool         g_raid_block_enabled = true;
static GroupTier    g_default_tier;
static std::unordered_map<std::string, GroupTier> g_group_tiers;
static std::unordered_set<std::string>            g_admin_eos;

static time_t    g_config_last_modified = 0;
static long long g_config_last_size = 0;
static int       g_reload_counter = 0;

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

static GroupTier ParseTier(const nlohmann::json& j, const GroupTier& base)
{
    GroupTier t = base;
    t.priority = j.value("Priority", base.priority);
    t.maxHomes = j.value("MaxHomes", base.maxHomes);
    t.teleportDelay = j.value("TeleportDelaySeconds", base.teleportDelay);
    t.cooldownSeconds = j.value("CooldownSeconds", base.cooldownSeconds);
    t.foundationRequired = j.value("FoundationRequired", base.foundationRequired);
    t.allowCliffPlatforms = j.value("AllowCliffPlatforms", base.allowCliffPlatforms);
    t.allowVacuumCompartments = j.value("AllowVacuumCompartments", base.allowVacuumCompartments);
    t.allowTreePlatforms = j.value("AllowTreePlatforms", base.allowTreePlatforms);
    t.allowNoBuildZones = j.value("AllowNoBuildZones", base.allowNoBuildZones);
    return t;
}

static bool LoadConfig()
{
    std::ifstream file(g_config_path);
    if (!file.is_open())
    {
        Log::GetLog()->error("[ArkTP] Cannot open config: {}", g_config_path);
        return false;
    }

    std::string  newHost, newUser, newPass, newName, newColor;
    unsigned int newPort = 3306;
    int          newTpa = 30;
    bool         newCombat = true, newRaid = true;
    GroupTier    newDefault;
    std::unordered_map<std::string, GroupTier> newGroups;
    std::unordered_set<std::string>            newAdmins;

    try
    {
        nlohmann::json j;
        file >> j;

        newHost = j.value("DbHost", std::string("127.0.0.1"));
        newPort = j.value("DbPort", 3306u);
        newUser = j.value("DbUser", std::string(""));
        newPass = j.value("DbPassword", std::string(""));
        newName = j.value("DbName", std::string(""));
        newColor = j.value("MessageColor", std::string("1.0,1.0,1.0,1.0"));
        newTpa = j.value("TpaTimeoutSeconds", 30);
        newCombat = j.value("CombatBlockEnabled", true);
        newRaid = j.value("RaidBlockEnabled", true);

        if (j.contains("AdminEosIds") && j["AdminEosIds"].is_array())
        {
            for (auto& e : j["AdminEosIds"])
                if (e.is_string()) newAdmins.insert(e.get<std::string>());
        }

        GroupTier hardDefault;
        if (j.contains("Default") && j["Default"].is_object())
            newDefault = ParseTier(j["Default"], hardDefault);
        else
            newDefault = hardDefault;

        std::unordered_set<int> seenPriorities;
        if (j.contains("Groups") && j["Groups"].is_object())
        {
            for (auto& [key, val] : j["Groups"].items())
            {
                if (!val.is_object()) continue;

                if (!val.contains("Priority") || !val["Priority"].is_number_integer())
                {
                    Log::GetLog()->error("[ArkTP] Group '{}' is missing an integer Priority", key);
                    return false;
                }

                int p = val["Priority"].get<int>();
                if (!seenPriorities.insert(p).second)
                {
                    Log::GetLog()->error("[ArkTP] Duplicate Priority {} on group '{}'", p, key);
                    return false;
                }

                newGroups[key] = ParseTier(val, newDefault);
            }
        }
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[ArkTP] Config parse error: {}", ex.what());
        return false;
    }

    if (newUser.empty() || newName.empty())
    {
        Log::GetLog()->error("[ArkTP] Config requires DbUser and DbName");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        g_db_host = newHost;
        g_db_port = newPort;
        g_db_user = newUser;
        g_db_pass = newPass;
        g_db_name = newName;
        g_message_color = newColor;
        g_tpa_timeout = newTpa;
        g_combat_block_enabled = newCombat;
        g_raid_block_enabled = newRaid;
        g_default_tier = newDefault;
        g_group_tiers = std::move(newGroups);
        g_admin_eos = std::move(newAdmins);
    }

    g_config_last_modified = GetFileModTime(g_config_path);
    g_config_last_size = GetFileSize(g_config_path);
    Log::GetLog()->info("[ArkTP] Config loaded, {} group tier(s)", g_group_tiers.size());
    return true;
}

static void CheckConfigReload()
{
    long long sz = GetFileSize(g_config_path);
    if (sz == 0) return;
    time_t mt = GetFileModTime(g_config_path);
    if (mt == g_config_last_modified && sz == g_config_last_size) return;
    LoadConfig();
}

static MYSQL*     g_db = nullptr;
static std::mutex g_db_mutex;
static bool       g_db_logged_down = false;

static std::string EscapeUnsafe(const std::string& in)
{
    if (!g_db || !pmysql_real_escape_string) return in;
    std::string buf(in.size() * 2 + 1, '\0');
    unsigned long len = pmysql_real_escape_string(g_db, buf.data(), in.c_str(), (unsigned long)in.size());
    buf.resize(len);
    return buf;
}

static bool ExecQuery(const std::string& sql)
{
    if (!g_db) return false;
    if (pmysql_query(g_db, sql.c_str()) != 0)
    {
        Log::GetLog()->error("[ArkTP] Query error: {}", pmysql_error(g_db));
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
        std::lock_guard<std::mutex> lock(g_config_mutex);
        host = g_db_host;
        user = g_db_user;
        pass = g_db_pass;
        name = g_db_name;
        port = g_db_port;
    }

    g_db = pmysql_init(nullptr);
    if (!g_db)
    {
        if (!g_db_logged_down) Log::GetLog()->error("[ArkTP] mysql_init failed");
        return false;
    }

    if (pmysql_options)
    {
        unsigned int timeout = 5;
        pmysql_options(g_db, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    }

    if (!pmysql_real_connect(g_db, host.c_str(), user.c_str(), pass.c_str(), name.c_str(), port, nullptr, 0))
    {
        if (!g_db_logged_down) Log::GetLog()->error("[ArkTP] DB connect failed: {}", pmysql_error(g_db));
        pmysql_close(g_db);
        g_db = nullptr;
        return false;
    }

    const char* create =
        "CREATE TABLE IF NOT EXISTS arktp_homes ("
        "eos_id VARCHAR(64) NOT NULL,"
        "map_name VARCHAR(64) NOT NULL,"
        "home_name VARCHAR(32) NOT NULL,"
        "x DOUBLE NOT NULL,"
        "y DOUBLE NOT NULL,"
        "z DOUBLE NOT NULL,"
        "PRIMARY KEY (eos_id, map_name, home_name)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";

    if (!ExecQuery(create))
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
                Log::GetLog()->info("[ArkTP] DB connection healthy");
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
            Log::GetLog()->info("[ArkTP] DB reconnected");
            g_db_logged_down = false;
        }
        else
        {
            Log::GetLog()->info("[ArkTP] Database ready");
        }
        return true;
    }

    if (!g_db_logged_down)
    {
        Log::GetLog()->error("[ArkTP] DB unavailable, retrying");
        g_db_logged_down = true;
    }
    return false;
}

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
        "INSERT INTO arktp_homes (eos_id, map_name, home_name, x, y, z) "
        "VALUES ('%s', '%s', '%s', %.2f, %.2f, %.2f) "
        "ON DUPLICATE KEY UPDATE x=VALUES(x), y=VALUES(y), z=VALUES(z)",
        eEos.c_str(), eMap.c_str(), eName.c_str(), x, y, z);

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!EnsureConnected()) return false;
    return ExecQuery(sql);
}

static bool DeleteHome(const std::string& eosId, const std::string& mapName, const std::string& homeName)
{
    const std::string eEos = EscapeUnsafe(eosId);
    const std::string eMap = EscapeUnsafe(mapName);
    const std::string eName = EscapeUnsafe(homeName);

    char sql[256];
    std::snprintf(sql, sizeof(sql),
        "DELETE FROM arktp_homes WHERE eos_id='%s' AND map_name='%s' AND home_name='%s'",
        eEos.c_str(), eMap.c_str(), eName.c_str());

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!EnsureConnected()) return false;
    return ExecQuery(sql);
}

static std::vector<HomeEntry> GetHomes(const std::string& eosId, const std::string& mapName)
{
    std::vector<HomeEntry> homes;

    const std::string eEos = EscapeUnsafe(eosId);
    const std::string eMap = EscapeUnsafe(mapName);

    char sql[256];
    std::snprintf(sql, sizeof(sql),
        "SELECT home_name, x, y, z FROM arktp_homes WHERE eos_id='%s' AND map_name='%s'",
        eEos.c_str(), eMap.c_str());

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!EnsureConnected()) return homes;
    if (pmysql_query(g_db, sql) != 0)
    {
        Log::GetLog()->error("[ArkTP] GetHomes query error: {}", pmysql_error(g_db));
        return homes;
    }

    MYSQL_RES* res = pmysql_store_result(g_db);
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
        "SELECT x, y, z FROM arktp_homes WHERE eos_id='%s' AND map_name='%s' AND home_name='%s'",
        eEos.c_str(), eMap.c_str(), eName.c_str());

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!EnsureConnected()) return false;
    if (pmysql_query(g_db, sql) != 0) return false;

    MYSQL_RES* res = pmysql_store_result(g_db);
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
        "SELECT COUNT(*) FROM arktp_homes WHERE eos_id='%s' AND map_name='%s'",
        eEos.c_str(), eMap.c_str());

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!EnsureConnected()) return 0;
    if (pmysql_query(g_db, sql) != 0) return 0;

    int count = 0;
    if (MYSQL_RES* res = pmysql_store_result(g_db))
    {
        MYSQL_ROW row = pmysql_fetch_row(res);
        if (row && row[0]) count = std::atoi(row[0]);
        pmysql_free_result(res);
    }

    return count;
}

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

static std::string ToLower(const std::string& s)
{
    std::string out = s;
    for (char& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
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
    AShooterPlayerState* ps = static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
    if (!ps) return "";
    FString eos;
    ps->GetUniqueNetIdAsString(&eos);
    return FStr(eos);
}

static void Notify(AShooterPlayerController* pc, const std::wstring& msg)
{
    if (!pc) return;
    std::string color;
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        color = g_message_color;
    }
    const std::wstring wColor = Widen(color);
    const std::wstring wFull = L"<RichColor Color=\"" + wColor + L"\">" + msg + L"</>";

    FString fSender(L"");
    FString fMsg(wFull.c_str());
    AsaApi::GetApiUtils().SendChatMessage(pc, fSender, L"{}", std::wstring_view(*fMsg));
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
    return character->RidingDinoField().Get() != nullptr;
}

static bool GetCharacterPosition(AShooterCharacter* character, double& outX, double& outY, double& outZ)
{
    if (!character) return false;
    USceneComponent* root = character->RootComponentField().Get();
    if (!root) return false;
    auto loc = root->RelativeLocationField();
    outX = loc.X;
    outY = loc.Y;
    outZ = loc.Z;
    return true;
}

static GroupTier ResolveTier(const std::string& eosId)
{
    GroupTier best;
    std::unordered_map<std::string, GroupTier> groups;
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        best = g_default_tier;
        groups = g_group_tiers;
    }

    if (groups.empty()) return best;

    if (!g_permissions_attempted) LoadPermissionsAPI();
    if (!g_permissions_loaded || !pGetPlayerGroups) return best;

    try
    {
        FString fEos(Widen(eosId).c_str());
        TArray<FString> playerGroups = pGetPlayerGroups(fEos);

        bool matched = false;
        for (int i = 0; i < playerGroups.Num(); ++i)
        {
            const std::string groupName = FStr(playerGroups[i]);
            auto it = groups.find(groupName);
            if (it != groups.end())
            {
                if (!matched || it->second.priority < best.priority)
                {
                    best = it->second;
                    matched = true;
                }
            }
        }
    }
    catch (...)
    {
        Log::GetLog()->warn("[ArkTP] GetPlayerGroups threw, using default tier");
    }

    return best;
}

static bool HasOwnedFoundationAt(double px, double py, double pz, int playerTeam,
    const GroupTier& tier, std::wstring* blockedReason = nullptr)
{
    if (playerTeam == 0) return false;

    FVector center{ px, py, pz };
    TArray<AActor*> structures = AsaApi::GetApiUtils().GetAllActorsInRange(center, 3000.0f, EServerOctreeGroup::STRUCTURES);

    for (int i = 0; i < structures.Num(); ++i)
    {
        AActor* actor = structures[i];
        if (!actor) continue;
        if (!actor->IsA(APrimalStructure::GetPrivateStaticClass())) continue;

        APrimalStructure* structure = static_cast<APrimalStructure*>(actor);
        if (structure->TargetingTeamField() != playerTeam) continue;

        USceneComponent* sRoot = structure->RootComponentField().Get();
        if (!sRoot) continue;

        auto sLoc = sRoot->RelativeLocationField();
        const double dx = px - sLoc.X;
        const double dy = py - sLoc.Y;
        const double dz = pz - sLoc.Z;
        const double hDist = std::sqrt(dx * dx + dy * dy);

        const std::string bpLower = ToLower(FStr(AsaApi::GetApiUtils().GetBlueprint(structure)));

        bool isFoundation = bpLower.find("foundation") != std::string::npos ||
            bpLower.find("floor") != std::string::npos;
        bool isCliff = bpLower.find("cliffplatform") != std::string::npos;
        bool isVacuum = bpLower.find("underwater_base") != std::string::npos;
        bool isTree = bpLower.find("treeplatform") != std::string::npos;

        double maxH = 150.0;
        double maxZ = 400.0;

        if (isCliff)
        {
            if (bpLower.find("_small") != std::string::npos) maxH = 1200.0;
            else if (bpLower.find("_medium") != std::string::npos) maxH = 1800.0;
            else maxH = 2400.0;
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
            if (isCliff && !tier.allowCliffPlatforms) *blockedReason = L"a cliff platform";
            else if (isVacuum && !tier.allowVacuumCompartments) *blockedReason = L"a vacuum compartment";
            else if (isTree && !tier.allowTreePlatforms) *blockedReason = L"a tree platform";
        }
    }

    return false;
}

static bool IsOnOwnedFoundation(AShooterPlayerController* pc, const GroupTier& tier, std::wstring* blockedReason = nullptr)
{
    AActor* charActor = pc->BaseGetPlayerCharacter();
    if (!charActor) return false;

    AShooterCharacter* character = static_cast<AShooterCharacter*>(charActor);
    double px, py, pz;
    if (!GetCharacterPosition(character, px, py, pz)) return false;

    const int playerTeam = charActor->TargetingTeamField();
    return HasOwnedFoundationAt(px, py, pz, playerTeam, tier, blockedReason);
}

static bool IsInNoBuildZone(AShooterPlayerController* pc)
{
    AActor* charActor = pc->BaseGetPlayerCharacter();
    if (!charActor) return false;

    AShooterCharacter* character = static_cast<AShooterCharacter*>(charActor);
    double px, py, pz;
    if (!GetCharacterPosition(character, px, py, pz)) return false;

    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return false;

    UE::Math::TVector<double> point{ px, py, pz };
    return AStructurePreventionZoneVolume::IsWithinAnyVolume(world, &point, false, nullptr, false, false, nullptr);
}

static std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_cooldowns;
static std::mutex g_cooldown_mutex;

static bool IsOnCooldown(const std::string& eosId, int cooldownSec)
{
    if (cooldownSec <= 0) return false;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_cooldown_mutex);
    auto it = g_cooldowns.find(eosId);
    if (it == g_cooldowns.end()) return false;
    return std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count() < cooldownSec;
}

static int GetCooldownRemaining(const std::string& eosId, int cooldownSec)
{
    if (cooldownSec <= 0) return 0;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_cooldown_mutex);
    auto it = g_cooldowns.find(eosId);
    if (it == g_cooldowns.end()) return 0;
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();
    if (elapsed >= cooldownSec) return 0;
    return (int)(cooldownSec - elapsed);
}

static void SetCooldown(const std::string& eosId)
{
    std::lock_guard<std::mutex> lock(g_cooldown_mutex);
    g_cooldowns[eosId] = std::chrono::steady_clock::now();
}

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

    const std::string searchLower = ToLower(searchName);
    auto& controllers = world->PlayerControllerListField();

    for (int i = 0; i < controllers.Num(); ++i)
    {
        AShooterPlayerController* pc = static_cast<AShooterPlayerController*>(controllers[i].Get());
        if (!pc) continue;

        AActor* ch = pc->BaseGetPlayerCharacter();
        if (!ch) continue;

        AShooterCharacter* character = static_cast<AShooterCharacter*>(ch);
        const std::string survivorName = FStr(character->PlayerNameField());
        if (survivorName.empty()) continue;
        if (ToLower(survivorName) != searchLower) continue;

        AShooterPlayerState* ps = static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
        std::string tribeName;
        if (ps && ps->IsInTribe())
            tribeName = FStr(character->TribeNameField());

        OnlinePlayer op;
        op.pc = pc;
        op.eosId = GetEosId(pc);
        op.survivorName = survivorName;
        op.tribeName = tribeName;
        results.push_back(op);
    }

    return results;
}

static bool IsCombatBlocked(const std::string& eosId, int& remaining)
{
    remaining = 0;

    bool enabled;
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        enabled = g_combat_block_enabled;
    }
    if (!enabled) return false;

    Blockade_IsCombatBlocked_t combatFn = nullptr;
    Blockade_IsRaidBlocked_t raidFn = nullptr;
    if (!ResolveBlockade(combatFn, raidFn)) return false;

    FString fEos(Widen(eosId).c_str());
    return combatFn(fEos, remaining);
}

static bool IsRaidBlocked(const std::string& eosId)
{
    bool enabled;
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        enabled = g_raid_block_enabled;
    }
    if (!enabled) return false;

    Blockade_IsCombatBlocked_t combatFn = nullptr;
    Blockade_IsRaidBlocked_t raidFn = nullptr;
    if (!ResolveBlockade(combatFn, raidFn)) return false;

    FString fEos(Widen(eosId).c_str());
    return raidFn(fEos);
}

static bool IsAdmin(const std::string& eosId)
{
    std::lock_guard<std::mutex> lock(g_config_mutex);
    return g_admin_eos.find(eosId) != g_admin_eos.end();
}

static AShooterPlayerController* FindOnlineBySurvivorId(unsigned long long survivorId)
{
    if (survivorId == 0) return nullptr;

    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return nullptr;

    auto& controllers = world->PlayerControllerListField();
    for (int i = 0; i < controllers.Num(); ++i)
    {
        AShooterPlayerController* pc = static_cast<AShooterPlayerController*>(controllers[i].Get());
        if (!pc) continue;

        AActor* ch = pc->BaseGetPlayerCharacter();
        if (!ch) continue;

        AShooterCharacter* character = static_cast<AShooterCharacter*>(ch);
        if (character->LinkedPlayerDataIDField() == survivorId) return pc;
    }

    return nullptr;
}

static bool GetPlayerWorldPosition(AShooterPlayerController* pc, double& outX, double& outY, double& outZ)
{
    AActor* ch = pc->BaseGetPlayerCharacter();
    if (!ch) return false;

    AShooterCharacter* character = static_cast<AShooterCharacter*>(ch);
    AActor* posActor = ch;
    APrimalDinoCharacter* dino = character->RidingDinoField().Get();
    if (dino) posActor = static_cast<AActor*>(dino);

    USceneComponent* root = posActor->RootComponentField().Get();
    if (!root) return false;

    auto loc = root->RelativeLocationField();
    outX = loc.X;
    outY = loc.Y;
    outZ = loc.Z;
    return true;
}

static bool ForceTeleportPlayer(AShooterPlayerController* pc, double x, double y, double z)
{
    AActor* ch = pc->BaseGetPlayerCharacter();
    if (!ch) return false;

    AShooterCharacter* character = static_cast<AShooterCharacter*>(ch);
    APrimalDinoCharacter* dino = character->RidingDinoField().Get();

    UE::Math::TVector<double> dest{ x, y, z };
    UE::Math::TRotator<double> rot{ 0.0, 0.0, 0.0 };

    if (dino)
    {
        USceneComponent* dRoot = dino->RootComponentField().Get();
        if (dRoot) rot = dRoot->RelativeRotationField();
        dino->TeleportTo(&dest, &rot, false, true);
    }
    else
    {
        USceneComponent* root = character->RootComponentField().Get();
        if (root) rot = root->RelativeRotationField();
        character->TeleportTo(&dest, &rot, false, true);
    }
    return true;
}

static bool TeleportPlayer(AShooterPlayerController* pc, double x, double y, double z)
{
    AActor* ch = pc->BaseGetPlayerCharacter();
    if (!ch) return false;

    AShooterCharacter* character = static_cast<AShooterCharacter*>(ch);
    if (character->RidingDinoField().Get() != nullptr)
    {
        Notify(pc, L"You cannot teleport while mounted.");
        return false;
    }

    UE::Math::TVector<double> dest{ x, y, z };
    UE::Math::TRotator<double> rot{ 0.0, 0.0, 0.0 };

    USceneComponent* root = character->RootComponentField().Get();
    if (root) rot = root->RelativeRotationField();

    character->TeleportTo(&dest, &rot, false, true);
    return true;
}

static bool TeleportToPlayer(AShooterPlayerController* sender, AShooterPlayerController* receiver)
{
    AActor* rch = receiver->BaseGetPlayerCharacter();
    if (!rch) return false;

    AShooterCharacter* rCharacter = static_cast<AShooterCharacter*>(rch);
    double x, y, z;
    if (!GetCharacterPosition(rCharacter, x, y, z)) return false;

    return TeleportPlayer(sender, x, y, z);
}

static void ProcessPendingTeleports()
{
    const auto now = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(g_pending_mutex);

        for (auto it = g_pending.begin(); it != g_pending.end(); )
        {
            FString fEos(Widen(it->eosId).c_str());
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

            if (now < it->dueTime)
            {
                ++it;
                continue;
            }

            const int playerTeam = ch->TargetingTeamField();
            const GroupTier tier = ResolveTier(it->eosId);

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

            if (tier.foundationRequired && !HasOwnedFoundationAt(it->targetX, it->targetY, it->targetZ, playerTeam, tier))
            {
                DeleteHome(it->eosId, it->mapName, it->homeName);
                Notify(pc, L"Home '" + Widen(it->homeName) + L"' no longer has a valid foundation. Home deleted.");
                Log::GetLog()->info("[ArkTP] HOME_INVALIDATED eos={} name={}", it->eosId, it->homeName);
                it = g_pending.erase(it);
                continue;
            }

            if (TeleportPlayer(pc, it->targetX, it->targetY, it->targetZ))
            {
                SetCooldown(it->eosId);
                Notify(pc, L"Teleported to home: " + Widen(it->homeName));
            }
            else
            {
                Notify(pc, L"Teleport failed.");
            }

            it = g_pending.erase(it);
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_tpa_mutex);
        g_tpa_requests.erase(
            std::remove_if(g_tpa_requests.begin(), g_tpa_requests.end(),
                [&now](const TPARequest& r) { return now >= r.expiry; }),
            g_tpa_requests.end());
    }
}

static int  g_blockade_probe_counter = 0;
static bool g_blockade_probed = false;

static void ProbeBlockade()
{
    if (g_blockade_probed) return;
    if (++g_blockade_probe_counter < 5) return;

    g_blockade_probed = true;

    Blockade_IsCombatBlocked_t combatFn = nullptr;
    Blockade_IsRaidBlocked_t raidFn = nullptr;
    const bool ok = ResolveBlockade(combatFn, raidFn);

    bool combat, raid;
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        combat = g_combat_block_enabled;
        raid = g_raid_block_enabled;
    }

    if (ok)
        Log::GetLog()->info("[ArkTP] Blockade API resolved, block gating active");
    else if (combat || raid)
        Log::GetLog()->warn("[ArkTP] CombatBlockEnabled/RaidBlockEnabled set but Blockade is not loaded, block gating inactive");
}

static void OnTimer()
{
    ProbeBlockade();
    ProcessPendingTeleports();

    if (++g_reload_counter >= 10)
    {
        g_reload_counter = 0;
        CheckConfigReload();
    }
}

static void Cmd_SetHome(AShooterPlayerController* pc, FString* message, int, int)
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

    if (tier.foundationRequired)
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

    double dummy1, dummy2, dummy3;
    if (GetHome(eosId, mapName, homeName, dummy1, dummy2, dummy3))
    {
        Notify(pc, L"You already have a home with this name.");
        return;
    }

    const int current = CountHomes(eosId, mapName);
    if (current >= tier.maxHomes)
    {
        Notify(pc, L"You have reached your home limit (" + std::to_wstring(tier.maxHomes) + L").");
        return;
    }

    AActor* ch = pc->BaseGetPlayerCharacter();
    if (!ch) return;

    AShooterCharacter* character = static_cast<AShooterCharacter*>(ch);
    double x, y, z;
    if (!GetCharacterPosition(character, x, y, z)) return;

    if (SaveHome(eosId, mapName, homeName, x, y, z))
    {
        Notify(pc, L"Home '" + Widen(homeName) + L"' saved.");
        Log::GetLog()->info("[ArkTP] SETHOME eos={} map={} name={} ({:.0f}, {:.0f}, {:.0f})",
            eosId, mapName, homeName, x, y, z);
    }
    else
    {
        Notify(pc, L"Failed to save home. Try again.");
    }
}

static void Cmd_Home(AShooterPlayerController* pc, FString* message, int, int)
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

    if (IsOnCooldown(eosId, tier.cooldownSeconds))
    {
        const int remaining = GetCooldownRemaining(eosId, tier.cooldownSeconds);
        Notify(pc, L"Teleport on cooldown. " + std::to_wstring(remaining) + L"s remaining.");
        return;
    }

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

        if (tier.foundationRequired && !HasOwnedFoundationAt(x, y, z, playerTeam, tier))
        {
            DeleteHome(eosId, mapName, homeName);
            Notify(pc, L"Home '" + Widen(homeName) + L"' no longer has a valid foundation. Home deleted.");
            return;
        }

        if (TeleportPlayer(pc, x, y, z))
        {
            SetCooldown(eosId);
            Notify(pc, L"Teleported to home: " + Widen(homeName));
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
    pt.dueTime = std::chrono::steady_clock::now() + std::chrono::seconds(tier.teleportDelay);

    {
        std::lock_guard<std::mutex> lock(g_pending_mutex);
        g_pending.push_back(pt);
    }

    Notify(pc, L"Teleporting in " + std::to_wstring(tier.teleportDelay) + L"s...");
    Log::GetLog()->info("[ArkTP] HOME_QUEUED eos={} name={} delay={}s", eosId, homeName, tier.teleportDelay);
}

static void Cmd_HomeDel(AShooterPlayerController* pc, FString* message, int, int)
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
        Notify(pc, L"Home '" + Widen(homeName) + L"' deleted.");
        Log::GetLog()->info("[ArkTP] HOMEDEL eos={} map={} name={}", eosId, mapName, homeName);
    }
    else
    {
        Notify(pc, L"Failed to delete home.");
    }
}

static void Cmd_Homes(AShooterPlayerController* pc, FString*, int, int)
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
        list += Widen(homes[i].name);
    }

    Notify(pc, list);
}

static void Cmd_TPR(AShooterPlayerController* pc, FString* message, int, int)
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

    std::string searchName = args;
    int selection = 0;

    auto matches = FindOnlineByName(searchName);
    matches.erase(
        std::remove_if(matches.begin(), matches.end(),
            [&senderEos](const OnlinePlayer& p) { return p.eosId == senderEos; }),
        matches.end());

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
        Notify(pc, Widen(searchName) + L" is not currently online.");
        return;
    }

    if (matches.size() > 1 && selection == 0)
    {
        Notify(pc, L"Multiple survivors found:");
        Notify(pc, L"Use /tpr " + Widen(searchName) + L" <number>");

        for (size_t i = 0; i < matches.size(); ++i)
        {
            const auto& m = matches[i];
            std::wstring wTribe = m.tribeName.empty() ? L"no tribe" : Widen(m.tribeName);
            Notify(pc, std::to_wstring(i + 1) + L" - " + Widen(m.survivorName) + L" (" + wTribe + L")");
        }
        return;
    }

    const OnlinePlayer* target = nullptr;
    if (matches.size() == 1)
        target = &matches[0];
    else if (selection >= 1 && selection <= (int)matches.size())
        target = &matches[selection - 1];
    else
    {
        Notify(pc, L"Invalid selection number.");
        return;
    }

    AActor* sch = pc->BaseGetPlayerCharacter();
    std::string senderName;
    if (sch)
        senderName = FStr(static_cast<AShooterCharacter*>(sch)->PlayerNameField());
    if (senderName.empty()) senderName = "Unknown";

    {
        std::lock_guard<std::mutex> lock(g_tpa_mutex);
        for (const auto& r : g_tpa_requests)
        {
            if (r.receiverEos == target->eosId)
            {
                Notify(pc, Widen(target->survivorName) + L" already has a pending teleport request.");
                return;
            }
        }
    }

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
                Notify(pc, L"Teleported to tribe member " + Widen(target->survivorName) + L".");
                Notify(target->pc, Widen(senderName) + L" teleported to you.");
                Log::GetLog()->info("[ArkTP] TPR_TRIBE sender={} receiver={}", senderEos, target->eosId);
            }
            else
            {
                Notify(pc, L"Teleport failed.");
            }
            return;
        }
    }

    int timeout;
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        timeout = g_tpa_timeout;
    }

    TPARequest req;
    req.senderEos = senderEos;
    req.receiverEos = target->eosId;
    req.senderName = senderName;
    req.expiry = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);

    {
        std::lock_guard<std::mutex> lock(g_tpa_mutex);
        g_tpa_requests.push_back(req);
    }

    Notify(pc, L"Teleport request sent to " + Widen(target->survivorName) + L". Expires in " +
        std::to_wstring(timeout) + L"s.");
    Notify(target->pc, Widen(senderName) + L" wants to teleport to you. Type /tpa to accept.");
    Log::GetLog()->info("[ArkTP] TPR sender={} receiver={}", senderEos, target->eosId);
}

static void Cmd_TPA(AShooterPlayerController* pc, FString*, int, int)
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

    FString fSenderEos(Widen(found.senderEos).c_str());
    AShooterPlayerController* senderPc = AsaApi::GetApiUtils().FindPlayerFromEOSID(fSenderEos);
    if (!senderPc)
    {
        Notify(pc, L"The requesting player is no longer online.");
        return;
    }

    AActor* senderCh = senderPc->BaseGetPlayerCharacter();
    if (!senderCh)
    {
        Notify(pc, Widen(found.senderName) + L" has died. Teleport cancelled.");
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

    if (TeleportToPlayer(senderPc, pc))
    {
        Notify(pc, Widen(found.senderName) + L" has been teleported to you.");

        std::string receiverName;
        AActor* rch = pc->BaseGetPlayerCharacter();
        if (rch)
            receiverName = FStr(static_cast<AShooterCharacter*>(rch)->PlayerNameField());
        Notify(senderPc, L"Teleported to " + Widen(receiverName) + L".");

        Log::GetLog()->info("[ArkTP] TPA_ACCEPTED sender={} receiver={}", found.senderEos, receiverEos);
    }
    else
    {
        Notify(pc, L"Teleport failed.");
        Notify(senderPc, L"Teleport failed.");
    }
}

static void Cmd_TPC(AShooterPlayerController* pc, FString*, int, int)
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

    FString fReceiverEos(Widen(receiverEos).c_str());
    AShooterPlayerController* receiverPc = AsaApi::GetApiUtils().FindPlayerFromEOSID(fReceiverEos);
    if (receiverPc)
        Notify(receiverPc, L"The teleport request has been cancelled.");

    Log::GetLog()->info("[ArkTP] TPC sender={}", senderEos);
}

static void Cmd_TP(AShooterPlayerController* pc, FString* message, int, int)
{
    const std::string callerEos = GetEosId(pc);
    if (callerEos.empty()) return;
    if (!IsAdmin(callerEos)) return;

    const std::string msg = FStr(*message);
    const auto space = msg.find(' ');
    if (space == std::string::npos || space + 1 >= msg.size())
    {
        Notify(pc, L"Usage: /tp <survivorid>");
        return;
    }

    const unsigned long long survivorId = std::strtoull(msg.substr(space + 1).c_str(), nullptr, 10);
    if (survivorId == 0)
    {
        Notify(pc, L"Invalid survivor id.");
        return;
    }

    AShooterPlayerController* target = FindOnlineBySurvivorId(survivorId);
    if (!target)
    {
        Notify(pc, L"No online survivor with that id.");
        return;
    }
    if (target == pc)
    {
        Notify(pc, L"You are already there.");
        return;
    }

    double x, y, z;
    if (!GetPlayerWorldPosition(target, x, y, z))
    {
        Notify(pc, L"Could not resolve that survivor's location.");
        return;
    }

    if (ForceTeleportPlayer(pc, x, y, z))
    {
        Notify(pc, L"Teleported to survivor " + std::to_wstring(survivorId) + L".");
        Log::GetLog()->info("[ArkTP] ADMIN_TP caller={} target_id={}", callerEos, survivorId);
    }
    else
    {
        Notify(pc, L"Teleport failed.");
    }
}

static void Cmd_TPS(AShooterPlayerController* pc, FString* message, int, int)
{
    const std::string callerEos = GetEosId(pc);
    if (callerEos.empty()) return;
    if (!IsAdmin(callerEos)) return;

    const std::string msg = FStr(*message);
    const auto space = msg.find(' ');
    if (space == std::string::npos || space + 1 >= msg.size())
    {
        Notify(pc, L"Usage: /tps <survivorid>");
        return;
    }

    const unsigned long long survivorId = std::strtoull(msg.substr(space + 1).c_str(), nullptr, 10);
    if (survivorId == 0)
    {
        Notify(pc, L"Invalid survivor id.");
        return;
    }

    AShooterPlayerController* target = FindOnlineBySurvivorId(survivorId);
    if (!target)
    {
        Notify(pc, L"No online survivor with that id.");
        return;
    }
    if (target == pc)
    {
        Notify(pc, L"You cannot teleport yourself to yourself.");
        return;
    }

    double x, y, z;
    if (!GetPlayerWorldPosition(pc, x, y, z))
    {
        Notify(pc, L"Could not resolve your location.");
        return;
    }

    if (ForceTeleportPlayer(target, x, y, z))
    {
        Notify(pc, L"Survivor " + std::to_wstring(survivorId) + L" teleported to you.");
        Notify(target, L"You have been teleported by an admin.");
        Log::GetLog()->info("[ArkTP] ADMIN_TPS caller={} target_id={}", callerEos, survivorId);
    }
    else
    {
        Notify(pc, L"Teleport failed.");
    }
}

static void PluginInit()
{
    Log::Get().Init("ArkTP");

    if (!LoadConfig())
    {
        Log::GetLog()->error("[ArkTP] Halted: config error");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_db_mutex);
        if (!EnsureConnected())
        {
            Log::GetLog()->error("[ArkTP] Halted: database error");
            return;
        }
    }

    AsaApi::GetCommands().AddChatCommand(FString(L"/sethome"), &Cmd_SetHome);
    AsaApi::GetCommands().AddChatCommand(FString(L"/home"), &Cmd_Home);
    AsaApi::GetCommands().AddChatCommand(FString(L"/delhome"), &Cmd_HomeDel);
    AsaApi::GetCommands().AddChatCommand(FString(L"/listhome"), &Cmd_Homes);
    AsaApi::GetCommands().AddChatCommand(FString(L"/tpr"), &Cmd_TPR);
    AsaApi::GetCommands().AddChatCommand(FString(L"/tpa"), &Cmd_TPA);
    AsaApi::GetCommands().AddChatCommand(FString(L"/tpc"), &Cmd_TPC);
    AsaApi::GetCommands().AddChatCommand(FString(L"/tp"), &Cmd_TP);
    AsaApi::GetCommands().AddChatCommand(FString(L"/tps"), &Cmd_TPS);

    AsaApi::GetCommands().AddOnTimerCallback(FString(L"ArkTP_Timer"), &OnTimer);

    Log::GetLog()->info("[ArkTP] Plugin loaded");
}

static void PluginUnload()
{
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/sethome"));
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/home"));
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/delhome"));
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/listhome"));
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/tpr"));
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/tpa"));
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/tpc"));
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/tp"));
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/tps"));

    AsaApi::GetCommands().RemoveOnTimerCallback(FString(L"ArkTP_Timer"));

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
    {
        std::lock_guard<std::mutex> lock(g_db_mutex);
        if (g_db)
        {
            pmysql_close(g_db);
            g_db = nullptr;
        }
    }

    Log::GetLog()->info("[ArkTP] Plugin unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { PluginInit(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->critical("[ArkTP] Init exception: {}", e.what());
    }
    catch (...)
    {
        Log::GetLog()->critical("[ArkTP] Init unknown exception");
    }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { PluginUnload(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->error("[ArkTP] Unload exception: {}", e.what());
    }
    catch (...) {}
}