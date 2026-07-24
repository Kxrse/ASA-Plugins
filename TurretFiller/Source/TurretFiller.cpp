/*
TurretFiller - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * TurretFiller - ASA Plugin
 *
 * Distributes ammo from a player inventory into nearby friendly turrets.
 *
 * Commands:
 *   /fill               distribute ammo from the player inventory into nearby turrets
 *   /fillrange {meters} set personal fill radius, persisted per EOS id
 *
 * Hooks:
 *   APrimalStructureTurret.DoFire(int)  track last fire time for the combat cooldown
 *
 * Table:
 *   turretfiller_ranges  PK (eos_id)
 *   Columns: eos_id VARCHAR(64), fill_range FLOAT
 *
 * Config:
 *   ArkApi/Plugins/TurretFiller/config.json
 *   DbHost defaults to 127.0.0.1
 *   Default: base tier applied to anyone with no matching group
 *   Groups: per Permissions group tier overrides. Every group requires a unique integer
 *           Priority. Lower Priority wins (1 is highest). Missing or duplicate Priority is
 *           a hard config error and the plugin halts.
 *   TurretAmmoMap: array of entries, each with a full TurretBP path matched exactly to ammo
 *
 * Config Example:
 * {
 *     "DbHost": "127.0.0.1",
 *     "DbPort": 3306,
 *     "DbUser": "DB_USER",
 *     "DbPassword": "DB_PASSWORD",
 *     "DbName": "DB_NAME",
 *     "Default": {
 *         "FillEnabled": true,
 *         "MaxFillRange": 100.0,
 *         "MinFillRange": 10.0,
 *         "DefaultFillRange": 50.0,
 *         "CommandCooldown": 30,
 *         "CombatCooldown": 300
 *     },
 *     "Groups": {
 *         "Admins": {
 *             "Priority": 1,
 *             "FillEnabled": true,
 *             "MaxFillRange": 250.0,
 *             "MinFillRange": 10.0,
 *             "DefaultFillRange": 100.0,
 *             "CommandCooldown": 5,
 *             "CombatCooldown": 60
 *         },
 *         "Vip": {
 *             "Priority": 2,
 *             "FillEnabled": true,
 *             "MaxFillRange": 150.0,
 *             "MinFillRange": 10.0,
 *             "DefaultFillRange": 75.0,
 *             "CommandCooldown": 15,
 *             "CombatCooldown": 180
 *         }
 *     },
 *     "TurretAmmoMap": [
 *         {
 *             "TurretBP": "Blueprint'/Game/PrimalEarth/Structures/BuildingBases/StructureTurretBaseBP.StructureTurretBaseBP'",
 *             "AmmoName": "Advanced Rifle Bullet",
 *             "AmmoBP": "Blueprint'/Game/PrimalEarth/CoreBlueprints/Weapons/PrimalItemAmmo_AdvancedRifleBullet.PrimalItemAmmo_AdvancedRifleBullet'"
 *         },
 *         {
 *             "TurretBP": "Blueprint'/Game/PrimalEarth/Structures/BuildingBases/StructureTurretBaseBP_Heavy.StructureTurretBaseBP_Heavy'",
 *             "AmmoName": "Advanced Rifle Bullet",
 *             "AmmoBP": "Blueprint'/Game/PrimalEarth/CoreBlueprints/Weapons/PrimalItemAmmo_AdvancedRifleBullet.PrimalItemAmmo_AdvancedRifleBullet'"
 *         },
 *         {
 *             "TurretBP": "Blueprint'/Game/PrimalEarth/Structures/BuildingBases/StructureTurretTek.StructureTurretTek'",
 *             "AmmoName": "Element Shard",
 *             "AmmoBP": "Blueprint'/Game/PrimalEarth/CoreBlueprints/Resources/PrimalItemResource_ElementShard.PrimalItemResource_ElementShard'"
 *         }
 *     ]
 * }
 *
 * Permissions:
 *   Optional. Resolved lazily via GetProcAddress from a loaded Permissions.dll, never
 *   statically linked. Only GetPlayerGroups is used. Among the player matched groups the
 *   lowest Priority wins. Falls back to the Default tier if Permissions.dll is absent or
 *   the player has no matching group.
 *
 * Write strategy:
 *   Ranges are preloaded into memory at init. Commands mutate the in memory cache only.
 *   A background worker flushes dirty ranges every 10 seconds, pings the connection and
 *   reconnects if the link dropped, then rescans config for changes (size and last write
 *   time). No database work runs on the game thread.
 */

#include <API/ARK/Ark.h>

#pragma warning(disable: 4191)
#pragma comment(lib, "AsaApi.lib")

#include <json.hpp>
#include <Windows.h>
#include <fstream>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <cstdio>

// =============================================================================
// Constants
// =============================================================================

static constexpr double UE_UNITS_PER_METER = 100.0;

// =============================================================================
// MariaDB - Dynamic Load
// =============================================================================

typedef struct st_mysql     MYSQL;
typedef struct st_mysql_res MYSQL_RES;
typedef char** MYSQL_ROW;

typedef MYSQL* (__stdcall* mysql_init_t)               (MYSQL*);
typedef MYSQL* (__stdcall* mysql_real_connect_t)       (MYSQL*, const char*, const char*, const char*, const char*, unsigned int, const char*, unsigned long);
typedef void(__stdcall* mysql_close_t)              (MYSQL*);
typedef int(__stdcall* mysql_query_t)              (MYSQL*, const char*);
typedef MYSQL_RES* (__stdcall* mysql_store_result_t)       (MYSQL*);
typedef void(__stdcall* mysql_free_result_t)        (MYSQL_RES*);
typedef const char* (__stdcall* mysql_error_t)              (MYSQL*);
typedef unsigned long(__stdcall* mysql_real_escape_string_t) (MYSQL*, char*, const char*, unsigned long);
typedef int(__stdcall* mysql_options_t)            (MYSQL*, int, const void*);
typedef int(__stdcall* mysql_ping_t)               (MYSQL*);
typedef MYSQL_ROW(__stdcall* mysql_fetch_row_t)     (MYSQL_RES*);

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
static mysql_ping_t               pmysql_ping = nullptr;
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
            break;
    }

    if (!g_mysql_module)
    {
        Log::GetLog()->error("[TurretFiller] Could not find libmariadb.dll or libmysql.dll");
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
    pmysql_ping = (mysql_ping_t)GetProcAddress(g_mysql_module, "mysql_ping");
    pmysql_fetch_row = (mysql_fetch_row_t)GetProcAddress(g_mysql_module, "mysql_fetch_row");

    if (!pmysql_init || !pmysql_real_connect || !pmysql_close ||
        !pmysql_query || !pmysql_error || !pmysql_real_escape_string || !pmysql_fetch_row)
    {
        Log::GetLog()->error("[TurretFiller] Failed to resolve required DB functions");
        return false;
    }

    g_mysql_loaded = true;
    return true;
}

// =============================================================================
// Helpers (forward)
// =============================================================================

static std::string FStr(const FString& f)
{
    const char* s = TCHAR_TO_UTF8(*f);
    return (s && s[0]) ? s : "";
}

// =============================================================================
// Permissions API (dynamically loaded at runtime)
// =============================================================================

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
        Log::GetLog()->warn("[TurretFiller] Permissions.dll not loaded, using default tier for all players");
        return;
    }

    pGetPlayerGroups = (GetPlayerGroups_t)GetProcAddress(hMod,
        "?GetPlayerGroups@Permissions@@YA?AV?$TArray@VFString@@V?$TSizedDefaultAllocator@$0CA@@@@@AEBVFString@@@Z");

    if (!pGetPlayerGroups)
    {
        Log::GetLog()->warn("[TurretFiller] Failed to resolve Permissions functions, using default tier");
        return;
    }

    g_permissions_loaded = true;
    Log::GetLog()->info("[TurretFiller] Permissions API loaded");
}

// =============================================================================
// Configuration
// =============================================================================

struct GroupTier
{
    int   priority = 0;
    bool  fillEnabled = true;
    float maxFillRange = 100.0f;
    float minFillRange = 10.0f;
    float defaultRange = 50.0f;
    int   commandCooldown = 30;
    int   combatCooldown = 300;
};

struct AmmoMapping
{
    std::string turretBp;
    std::string ammoName;
    std::string ammoBpPath;
    UClass*     cachedClass = nullptr;
};

static const std::string g_config_path = "ArkApi/Plugins/TurretFiller/config.json";

static std::mutex g_config_mutex;
static GroupTier                                  g_default_tier;
static std::unordered_map<std::string, GroupTier> g_group_tiers;
static std::vector<AmmoMapping>                   g_ammo_map;

static std::string  g_db_host = "127.0.0.1";
static unsigned int g_db_port = 3306;
static std::string  g_db_user;
static std::string  g_db_pass;
static std::string  g_db_name;

static std::uintmax_t                  g_cfg_size = 0;
static std::filesystem::file_time_type g_cfg_mtime{};

static GroupTier ParseTier(const nlohmann::json& j, const GroupTier& fallback)
{
    GroupTier t;
    t.priority = j.value("Priority", 0);
    t.fillEnabled = j.value("FillEnabled", fallback.fillEnabled);
    t.maxFillRange = j.value("MaxFillRange", fallback.maxFillRange);
    t.minFillRange = j.value("MinFillRange", fallback.minFillRange);
    t.defaultRange = j.value("DefaultFillRange", fallback.defaultRange);
    t.commandCooldown = j.value("CommandCooldown", fallback.commandCooldown);
    t.combatCooldown = j.value("CombatCooldown", fallback.combatCooldown);
    return t;
}

static bool LoadConfig()
{
    std::ifstream file(g_config_path);
    if (!file.is_open())
    {
        Log::GetLog()->error("[TurretFiller] Cannot open config: {}", g_config_path);
        return false;
    }

    std::string  newHost = "127.0.0.1";
    unsigned int newPort = 3306;
    std::string  newUser;
    std::string  newPass;
    std::string  newName;

    GroupTier                                  newDefault;
    std::unordered_map<std::string, GroupTier> newGroups;
    std::vector<AmmoMapping>                   newAmmoMap;

    try
    {
        nlohmann::json j;
        file >> j;

        newHost = j.value("DbHost", "127.0.0.1");
        newPort = j.value("DbPort", 3306);
        newUser = j.value("DbUser", "");
        newPass = j.value("DbPassword", "");
        newName = j.value("DbName", "");

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
                    Log::GetLog()->error("[TurretFiller] Group '{}' is missing an integer Priority", key);
                    return false;
                }

                int p = val["Priority"].get<int>();
                if (!seenPriorities.insert(p).second)
                {
                    Log::GetLog()->error("[TurretFiller] Duplicate Priority {} on group '{}'", p, key);
                    return false;
                }

                newGroups[key] = ParseTier(val, newDefault);
            }
        }

        if (j.contains("TurretAmmoMap") && j["TurretAmmoMap"].is_array())
        {
            for (auto& val : j["TurretAmmoMap"])
            {
                if (!val.is_object()) continue;
                AmmoMapping m;
                m.turretBp = val.value("TurretBP", "");
                m.ammoName = val.value("AmmoName", "");
                m.ammoBpPath = val.value("AmmoBP", "");
                if (!m.turretBp.empty() && !m.ammoName.empty() && !m.ammoBpPath.empty())
                    newAmmoMap.push_back(std::move(m));
            }
        }
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[TurretFiller] Config parse error: {}", ex.what());
        return false;
    }

    if (newUser.empty() || newName.empty())
    {
        Log::GetLog()->error("[TurretFiller] Config requires DbUser and DbName");
        return false;
    }

    g_db_host = newHost;
    g_db_port = newPort;
    g_db_user = newUser;
    g_db_pass = newPass;
    g_db_name = newName;

    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        g_default_tier = newDefault;
        g_group_tiers.swap(newGroups);
        g_ammo_map.swap(newAmmoMap);
    }

    try
    {
        g_cfg_size = std::filesystem::file_size(g_config_path);
        g_cfg_mtime = std::filesystem::last_write_time(g_config_path);
    }
    catch (...) {}

    Log::GetLog()->info("[TurretFiller] Config loaded: {} group tier(s), {} ammo mapping(s)",
        g_group_tiers.size(), g_ammo_map.size());
    return true;
}

static void ReloadConfigIfChanged()
{
    std::uintmax_t size = 0;
    std::filesystem::file_time_type mtime{};
    try
    {
        size = std::filesystem::file_size(g_config_path);
        mtime = std::filesystem::last_write_time(g_config_path);
    }
    catch (...) { return; }

    if (size == 0) return;
    if (size == g_cfg_size && mtime == g_cfg_mtime) return;

    Log::GetLog()->info("[TurretFiller] Config change detected, reloading...");
    if (!LoadConfig())
    {
        g_cfg_size = size;
        g_cfg_mtime = mtime;
        Log::GetLog()->error("[TurretFiller] Config reload rejected, keeping previous config");
    }
}

// =============================================================================
// Resolve Tier
// =============================================================================

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
        const std::wstring wEos(eosId.begin(), eosId.end());
        FString fEos(wEos.c_str());
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
        Log::GetLog()->warn("[TurretFiller] GetPlayerGroups threw, using default tier");
    }

    return best;
}

// =============================================================================
// Database
// =============================================================================

static MYSQL* g_mysql = nullptr;
static std::mutex g_db_mutex;
static bool g_db_logged_down = false;

static std::string EscapeUnsafe(const std::string& in)
{
    if (!g_mysql || !pmysql_real_escape_string) return in;
    std::string buf(in.size() * 2 + 1, '\0');
    unsigned long len = pmysql_real_escape_string(
        g_mysql, buf.data(), in.c_str(), (unsigned long)in.size());
    buf.resize(len);
    return buf;
}

static bool ExecQuery(const std::string& sql)
{
    if (!g_mysql) return false;
    if (pmysql_query(g_mysql, sql.c_str()) != 0)
    {
        Log::GetLog()->error("[TurretFiller] Query error: {}", pmysql_error(g_mysql));
        return false;
    }
    if (MYSQL_RES* res = pmysql_store_result(g_mysql))
        pmysql_free_result(res);
    return true;
}

static bool EstablishConnection()
{
    g_mysql = pmysql_init(nullptr);
    if (!g_mysql)
    {
        Log::GetLog()->error("[TurretFiller] mysql_init failed");
        return false;
    }

    if (pmysql_options)
    {
        unsigned int timeout = 10;
        pmysql_options(g_mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    }

    if (!pmysql_real_connect(g_mysql,
        g_db_host.c_str(), g_db_user.c_str(), g_db_pass.c_str(),
        g_db_name.c_str(), g_db_port, nullptr, 0))
    {
        if (!g_db_logged_down)
            Log::GetLog()->error("[TurretFiller] DB connect failed: {}", pmysql_error(g_mysql));
        pmysql_close(g_mysql);
        g_mysql = nullptr;
        return false;
    }

    return true;
}

static bool EnsureConnected()
{
    if (g_mysql)
    {
        if (!pmysql_ping) return true;
        if (pmysql_ping(g_mysql) == 0)
        {
            if (g_db_logged_down)
            {
                Log::GetLog()->info("[TurretFiller] DB connection healthy");
                g_db_logged_down = false;
            }
            return true;
        }
        pmysql_close(g_mysql);
        g_mysql = nullptr;
    }

    if (EstablishConnection())
    {
        if (g_db_logged_down)
        {
            Log::GetLog()->info("[TurretFiller] DB reconnected");
            g_db_logged_down = false;
        }
        return true;
    }

    if (!g_db_logged_down)
    {
        Log::GetLog()->error("[TurretFiller] DB connection lost, retaining ranges until reconnect");
        g_db_logged_down = true;
    }
    return false;
}

// =============================================================================
// Fill Range Cache
// =============================================================================

static std::unordered_map<std::string, float> g_fill_ranges;
static std::unordered_set<std::string>        g_ranges_dirty;
static std::mutex                             g_ranges_mutex;

static void PreloadRanges()
{
    if (!g_mysql) return;
    if (pmysql_query(g_mysql, "SELECT eos_id, fill_range FROM turretfiller_ranges") != 0) return;

    MYSQL_RES* res = pmysql_store_result(g_mysql);
    if (!res) return;

    std::lock_guard<std::mutex> lock(g_ranges_mutex);
    MYSQL_ROW row;
    while ((row = pmysql_fetch_row(res)) != nullptr)
    {
        if (row[0] && row[1])
            g_fill_ranges[row[0]] = static_cast<float>(std::atof(row[1]));
    }

    pmysql_free_result(res);
}

static bool InitDatabase()
{
    if (!LoadMySQLLib()) return false;
    if (!EstablishConnection()) return false;

    const std::string create =
        "CREATE TABLE IF NOT EXISTS turretfiller_ranges ("
        "  eos_id     VARCHAR(64)  NOT NULL,"
        "  fill_range FLOAT        NOT NULL DEFAULT 50,"
        "  PRIMARY KEY (eos_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";

    if (!ExecQuery(create))
    {
        Log::GetLog()->error("[TurretFiller] Failed to create table");
        pmysql_close(g_mysql);
        g_mysql = nullptr;
        return false;
    }

    PreloadRanges();

    Log::GetLog()->info("[TurretFiller] Database ready, {} range(s) preloaded", g_fill_ranges.size());
    return true;
}

static void CloseDatabase()
{
    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (g_mysql)
    {
        pmysql_close(g_mysql);
        g_mysql = nullptr;
    }
}

static void FlushRanges()
{
    std::vector<std::pair<std::string, float>> toWrite;
    {
        std::lock_guard<std::mutex> lock(g_ranges_mutex);
        if (g_ranges_dirty.empty()) return;
        toWrite.reserve(g_ranges_dirty.size());
        for (const auto& eos : g_ranges_dirty)
        {
            auto it = g_fill_ranges.find(eos);
            if (it != g_fill_ranges.end())
                toWrite.emplace_back(eos, it->second);
        }
        g_ranges_dirty.clear();
    }

    if (toWrite.empty()) return;

    std::lock_guard<std::mutex> dbLock(g_db_mutex);
    if (!EnsureConnected())
    {
        std::lock_guard<std::mutex> lock(g_ranges_mutex);
        for (const auto& p : toWrite)
            g_ranges_dirty.insert(p.first);
        return;
    }

    for (const auto& [eos, meters] : toWrite)
    {
        const std::string eEos = EscapeUnsafe(eos);
        char sql[256];
        std::snprintf(sql, sizeof(sql),
            "INSERT INTO turretfiller_ranges (eos_id, fill_range) VALUES ('%s', %.1f) "
            "ON DUPLICATE KEY UPDATE fill_range = VALUES(fill_range)",
            eEos.c_str(), meters);
        ExecQuery(sql);
    }
}

static float GetFillRange(const std::string& eosId, const GroupTier& tier)
{
    std::lock_guard<std::mutex> lock(g_ranges_mutex);
    auto it = g_fill_ranges.find(eosId);
    const float base = (it != g_fill_ranges.end()) ? it->second : tier.defaultRange;
    return std::clamp(base, tier.minFillRange, tier.maxFillRange);
}

static void SetFillRange(const std::string& eosId, float meters)
{
    std::lock_guard<std::mutex> lock(g_ranges_mutex);
    g_fill_ranges[eosId] = meters;
    g_ranges_dirty.insert(eosId);
}

// =============================================================================
// Combat Tracking
// =============================================================================

static std::mutex g_fire_mutex;
static std::unordered_map<void*, std::chrono::steady_clock::time_point> g_fire_times;

static bool IsCombatBlocked(void* turretPtr, int combatCooldown)
{
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_fire_mutex);

    for (auto it = g_fire_times.begin(); it != g_fire_times.end(); )
    {
        if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count() > combatCooldown)
            it = g_fire_times.erase(it);
        else
            ++it;
    }

    auto it = g_fire_times.find(turretPtr);
    if (it == g_fire_times.end()) return false;
    return std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count() < combatCooldown;
}

// =============================================================================
// Ammo Class Resolution
// =============================================================================

static UClass* GetAmmoClass(AmmoMapping& mapping)
{
    if (mapping.cachedClass) return mapping.cachedClass;
    if (mapping.ammoBpPath.empty()) return nullptr;

    const std::wstring wPath(mapping.ammoBpPath.begin(), mapping.ammoBpPath.end());
    FString fPath(wPath.c_str());
    UClass* cls = UVictoryCore::BPLoadClass(fPath);
    if (cls)
        mapping.cachedClass = cls;
    else
        Log::GetLog()->warn("[TurretFiller] BPLoadClass failed for '{}'", mapping.ammoBpPath);
    return cls;
}

static int FindAmmoMapping(const std::vector<AmmoMapping>& ammoMap, const std::string& turretBp)
{
    for (int i = 0; i < static_cast<int>(ammoMap.size()); ++i)
    {
        if (turretBp == ammoMap[i].turretBp)
            return i;
    }
    return -1;
}

// =============================================================================
// Player Helpers
// =============================================================================

static std::string GetEosId(AShooterPlayerController* pc)
{
    if (!pc) return "";
    AShooterPlayerState* ps = static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
    if (!ps) return "";
    FString raw;
    ps->GetUniqueNetIdAsString(&raw);
    const std::string eos = FStr(raw);
    return (eos.empty() || eos == "unknown") ? "" : eos;
}

static void Notify(AShooterPlayerController* pc, const std::wstring& msg,
    const std::wstring& color = L"0.2,1.0,0.2,1.0")
{
    const std::wstring rich =
        L"<RichColor Color=\"" + color + L"\">" + msg + L"</>";
    FString fSender(L"");
    FString fMsg(rich.c_str());
    AsaApi::GetApiUtils().SendChatMessage(pc, fSender, L"{}", std::wstring_view(*fMsg));
}

static bool GetWorldPosition(AShooterCharacter* character, double& outX, double& outY, double& outZ)
{
    if (!character) return false;

    AActor* positionActor = static_cast<AActor*>(character);
    auto ridingDino = character->RidingDinoField();
    if (ridingDino)
        positionActor = static_cast<AActor*>(ridingDino.Get());

    USceneComponent* root = positionActor->RootComponentField();
    if (!root) return false;

    auto loc = root->RelativeLocationField();
    outX = loc.X;
    outY = loc.Y;
    outZ = loc.Z;
    return true;
}

// =============================================================================
// Inventory Helpers
// =============================================================================

static int CountAmmoInInventory(UPrimalInventoryComponent* inv, const std::string& targetName)
{
    if (!inv) return 0;

    TArray<UPrimalItem*>& items = inv->InventoryItemsField();
    int total = 0;
    for (int i = 0; i < items.Num(); ++i)
    {
        UPrimalItem* item = items[i];
        if (!item) continue;
        if (item->bIsBlueprint()()) continue;
        if (FStr(item->DescriptiveNameBaseField()) != targetName) continue;
        total += item->GetItemQuantity();
    }
    return total;
}

static void RemoveAmmoQuantity(UPrimalInventoryComponent* inv, const std::string& targetName, int amount)
{
    if (!inv || amount <= 0) return;

    TArray<UPrimalItem*>& items = inv->InventoryItemsField();
    struct AmmoStack { UPrimalItem* item; FItemNetID id; int qty; };
    std::vector<AmmoStack> stacks;

    for (int i = 0; i < items.Num(); ++i)
    {
        UPrimalItem* item = items[i];
        if (!item) continue;
        if (item->bIsBlueprint()()) continue;
        if (FStr(item->DescriptiveNameBaseField()) != targetName) continue;
        int qty = item->GetItemQuantity();
        if (qty > 0)
            stacks.push_back({ item, item->ItemIDField(), qty });
    }

    for (auto& s : stacks)
    {
        if (amount <= 0) break;
        if (s.qty <= amount)
        {
            if (inv->RemoveItem(&s.id, false, false, true, false))
                amount -= s.qty;
        }
        else
        {
            s.item->SetQuantity(s.qty - amount, false);
            amount = 0;
        }
    }
}

// =============================================================================
// Hook Type Aliases
// =============================================================================

using DoFire_t = void(*)(APrimalStructureTurret*, int);
static DoFire_t Original_DoFire = nullptr;

// =============================================================================
// DoFire Hook - Combat Tracking
// =============================================================================

static void Detour_DoFire(APrimalStructureTurret* turret, int shotIndex)
{
    if (turret)
    {
        std::lock_guard<std::mutex> lock(g_fire_mutex);
        g_fire_times[static_cast<void*>(turret)] = std::chrono::steady_clock::now();
    }
    Original_DoFire(turret, shotIndex);
}

// =============================================================================
// /fill Command
// =============================================================================

static std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_cooldowns;

static void Cmd_Fill(AShooterPlayerController* pc, FString*, int, int)
{
    if (!pc) return;

    const std::string eosId = GetEosId(pc);
    if (eosId.empty()) return;

    const GroupTier tier = ResolveTier(eosId);

    if (!tier.fillEnabled)
    {
        Notify(pc, L"You do not have permission to use /fill.", L"1.0,0.2,0.2,1.0");
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    {
        auto it = g_cooldowns.find(eosId);
        if (it != g_cooldowns.end())
        {
            auto remaining = tier.commandCooldown -
                std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();
            if (remaining > 0)
            {
                Notify(pc, L"Fill on cooldown (" + std::to_wstring(remaining) + L"s remaining).",
                    L"1.0,0.5,0.2,1.0");
                return;
            }
        }
    }

    AActor* baseChar = pc->BaseGetPlayerCharacter();
    AShooterCharacter* character = baseChar ? static_cast<AShooterCharacter*>(baseChar) : nullptr;
    if (!character)
    {
        Notify(pc, L"No character found.", L"1.0,0.2,0.2,1.0");
        return;
    }

    UPrimalInventoryComponent* playerInv = character->MyInventoryComponentField();
    if (!playerInv)
    {
        Notify(pc, L"Cannot access inventory.", L"1.0,0.2,0.2,1.0");
        return;
    }

    double px, py, pz;
    if (!GetWorldPosition(character, px, py, pz))
    {
        Notify(pc, L"Cannot determine position.", L"1.0,0.2,0.2,1.0");
        return;
    }

    const int playerTeam = character->TargetingTeamField();

    const float radiusMeters = GetFillRange(eosId, tier);
    const double radiusUE = static_cast<double>(radiusMeters) * UE_UNITS_PER_METER;

    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return;

    std::vector<AmmoMapping> ammoMap;
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        ammoMap = g_ammo_map;
    }

    TArray<AActor*> allTurrets;
    UGameplayStatics::GetAllActorsOfClass(world,
    APrimalStructureTurret::GetPrivateStaticClass(), &allTurrets);

    struct TurretEntry { UPrimalInventoryComponent* inv; };
    struct AmmoGroup
    {
        std::string ammoName;
        int         configIdx = -1;
        std::vector<TurretEntry> turrets;
    };

    std::unordered_map<std::string, AmmoGroup> ammoGroups;
    int totalInRange = 0;
    int combatBlocked = 0;

    for (int i = 0; i < allTurrets.Num(); ++i)
    {
        APrimalStructureTurret* turret =
            static_cast<APrimalStructureTurret*>(allTurrets[i]);
        if (!turret) continue;

        USceneComponent* tRoot = turret->RootComponentField();
        if (!tRoot) continue;
        auto tLoc = tRoot->RelativeLocationField();
        double dx = px - tLoc.X, dy = py - tLoc.Y, dz = pz - tLoc.Z;
        if (std::sqrt(dx * dx + dy * dy + dz * dz) > radiusUE) continue;

        const std::string bp = FStr(AsaApi::GetApiUtils().GetBlueprint(turret));
        int mapIdx = FindAmmoMapping(ammoMap, bp);
        if (mapIdx < 0) continue;

        ++totalInRange;

        if (turret->TargetingTeamField() != playerTeam) continue;

        if (IsCombatBlocked(static_cast<void*>(turret), tier.combatCooldown))
        {
            ++combatBlocked;
            continue;
        }

        UPrimalInventoryComponent* tInv = turret->MyInventoryComponentField();
        if (!tInv) continue;

        const std::string& ammoName = ammoMap[mapIdx].ammoName;
        auto& group = ammoGroups[ammoName];
        if (group.configIdx < 0)
        {
            group.ammoName = ammoName;
            group.configIdx = mapIdx;
        }
        group.turrets.push_back({ tInv });
    }

    if (ammoGroups.empty())
    {
        std::wstring msg = L"No eligible turrets in range ("
            + std::to_wstring(static_cast<int>(radiusMeters)) + L"m).";
        if (combatBlocked > 0)
            msg += L" " + std::to_wstring(combatBlocked) + L"/"
                + std::to_wstring(totalInRange) + L" skipped (recently fired).";
        Notify(pc, msg, L"1.0,0.5,0.2,1.0");
        return;
    }

    int totalFilled = 0;
    int totalTransferred = 0;

    for (auto& [ammoName, group] : ammoGroups)
    {
        UClass* ammoClass = GetAmmoClass(ammoMap[group.configIdx]);
        if (!ammoClass) continue;

        int playerPool = CountAmmoInInventory(playerInv, ammoName);
        if (playerPool <= 0) continue;

        int numTurrets = static_cast<int>(group.turrets.size());
        int remaining = playerPool;

        for (int t = 0; t < numTurrets && remaining > 0; ++t)
        {
            int turretsLeft = numTurrets - t;
            int share = remaining / turretsLeft;
            if (share <= 0) share = remaining;

            UPrimalInventoryComponent* tInv = group.turrets[t].inv;
            bool filledAny = false;

            while (share > 0)
            {
                int before = CountAmmoInInventory(tInv, ammoName);

                UPrimalItem::AddNewItem(
                    ammoClass,
                    tInv,
                    false, false, 0.0f, false,
                    share,
                    false, 0.0f, false,
                    TSubclassOf<UPrimalItem>(),
                    0.0f, false, false, false,
                    false, true, false, world
                );

                int after = CountAmmoInInventory(tInv, ammoName);
                int added = after - before;
                if (added <= 0) break;

                RemoveAmmoQuantity(playerInv, ammoName, added);
                remaining -= added;
                share -= added;
                filledAny = true;
            }

            if (filledAny) ++totalFilled;
        }

        totalTransferred += (playerPool - remaining);
    }

    g_cooldowns[eosId] = now;

    std::wstring msg = L"Filled " + std::to_wstring(totalFilled) + L" turrets with "
        + std::to_wstring(totalTransferred) + L" ammo.";
    if (combatBlocked > 0)
        msg += L" " + std::to_wstring(combatBlocked) + L"/"
            + std::to_wstring(totalInRange) + L" skipped (recently fired).";

    Notify(pc, msg);
}

// =============================================================================
// /fillrange Command
// =============================================================================

static void Cmd_FillRange(AShooterPlayerController* pc, FString* message, int, int)
{
    if (!pc || !message) return;

    const std::string eosId = GetEosId(pc);
    if (eosId.empty()) return;

    const GroupTier tier = ResolveTier(eosId);

    if (!tier.fillEnabled)
    {
        Notify(pc, L"You do not have permission to use /fillrange.", L"1.0,0.2,0.2,1.0");
        return;
    }

    const std::string raw = FStr(*message);

    size_t space = raw.find(' ');
    if (space == std::string::npos)
    {
        float current = GetFillRange(eosId, tier);
        Notify(pc, L"Fill range: " + std::to_wstring(static_cast<int>(current)) + L"m (min: "
            + std::to_wstring(static_cast<int>(tier.minFillRange)) + L"m, max: "
            + std::to_wstring(static_cast<int>(tier.maxFillRange)) + L"m).");
        return;
    }

    float meters = tier.defaultRange;
    try { meters = std::stof(raw.substr(space + 1)); }
    catch (...) { meters = tier.defaultRange; }

    meters = std::clamp(meters, tier.minFillRange, tier.maxFillRange);
    SetFillRange(eosId, meters);

    Notify(pc, L"Fill range set to " + std::to_wstring(static_cast<int>(meters)) + L"m.");
}

// =============================================================================
// Worker Thread
// =============================================================================

static std::thread       g_worker_thread;
static std::atomic<bool> g_worker_running{ false };

static void WorkerLoop()
{
    int tick = 0;
    while (g_worker_running.load())
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!g_worker_running.load()) break;

        ++tick;
        if (tick % 10 == 0)
        {
            FlushRanges();
            ReloadConfigIfChanged();
        }
    }

    FlushRanges();
}

// =============================================================================
// Plugin Entry Points
// =============================================================================

static void InitImpl()
{
    Log::Get().Init("TurretFiller");

    if (!LoadConfig())
    {
        Log::GetLog()->error("[TurretFiller] Halted - config error");
        return;
    }

    if (!InitDatabase())
    {
        Log::GetLog()->error("[TurretFiller] Halted - database error");
        return;
    }

    AsaApi::GetCommands().AddChatCommand(FString(L"/fill"), &Cmd_Fill);
    AsaApi::GetCommands().AddChatCommand(FString(L"/fillrange"), &Cmd_FillRange);

    AsaApi::GetHooks().SetHook(
        "APrimalStructureTurret.DoFire(int)",
        (LPVOID)&Detour_DoFire,
        (LPVOID*)&Original_DoFire
    );

    g_worker_running.store(true);
    g_worker_thread = std::thread(WorkerLoop);

    Log::GetLog()->info("[TurretFiller] Plugin loaded");
}

static void UnloadImpl()
{
    g_worker_running.store(false);
    if (g_worker_thread.joinable())
        g_worker_thread.join();

    AsaApi::GetCommands().RemoveChatCommand(FString(L"/fill"));
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/fillrange"));

    AsaApi::GetHooks().DisableHook(
        "APrimalStructureTurret.DoFire(int)",
        (LPVOID)&Detour_DoFire
    );

    CloseDatabase();

    {
        std::lock_guard<std::mutex> lock(g_fire_mutex);
        g_fire_times.clear();
    }

    Log::GetLog()->info("[TurretFiller] Plugin unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { InitImpl(); }
    catch (const std::exception& ex)
    {
        Log::GetLog()->critical("[TurretFiller] Init exception: {}", ex.what());
    }
    catch (...)
    {
        Log::GetLog()->critical("[TurretFiller] Init unknown exception");
    }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { UnloadImpl(); }
    catch (const std::exception& ex)
    {
        Log::GetLog()->critical("[TurretFiller] Unload exception: {}", ex.what());
    }
    catch (...)
    {
        Log::GetLog()->critical("[TurretFiller] Unload unknown exception");
    }
}