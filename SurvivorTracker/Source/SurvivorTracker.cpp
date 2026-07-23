/*
SurvivorTracker - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * SurvivorTracker - ASA Plugin
 *
 * Tracks survivor identity and tribe membership per map across a cluster.
 *
 * Table: survivortracker_survivors
 *   PK (eos_id, map_name)  one current-identity row per player per map.
 *   Columns: eos_id, survivor_name, survivor_id, tribe_name, tribe_id, map_name
 *
 * Hooks:
 *   AShooterGameMode.PostLogin                                    seed cache on connect if character present
 *   AShooterPlayerController.HandleRespawned_Implementation       refresh name and tribe on spawn or respawn
 *   AShooterGameMode.StartNewShooterPlayer                        resolve survivor_id, capture creation name, detect recreation
 *   APrimalCharacter.NetUpdateTribeName                           update tribe name on tribe rename
 *   AShooterPlayerState.NotifyPlayerJoinedTribe                   set tribe fields on join
 *   AShooterPlayerState.NotifyPlayerLeftTribe                     clear tribe fields on leave
 *   AShooterCharacter.RenamePlayer                                capture survivor name on admin rename
 *   AShooterGameMode.Logout                                       mark record for final flush on disconnect
 *
 * Config:
 *   ArkApi/Plugins/SurvivorTracker/config.json
 *   DbHost, DbPort, DbUser, DbPassword, DbName
 *
 * Write strategy:
 *   Detours mutate an in-memory cache only, never the database. A background writer
 *   flushes every 10 seconds: it pings the connection, reconnects if the link dropped,
 *   then upserts every cached record. survivor_id is preserved against a zero write so a
 *   pre-resolution refresh cannot wipe a good id. Records for logged-out players are kept
 *   until their final state is written, then erased. Config rescanned every 10 seconds
 *   (size and last-write-time). No database work runs on the game thread.
 */

#include <API/ARK/Ark.h>

#pragma warning(disable: 4191)
#pragma comment(lib, "AsaApi.lib")

#include <json.hpp>
#include <Windows.h>
#include <fstream>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <unordered_map>
#include <vector>
#include <cstdio>

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
        Log::GetLog()->error("[SurvivorTracker] Could not find libmariadb.dll or libmysql.dll");
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

    if (!pmysql_init || !pmysql_real_connect || !pmysql_close ||
        !pmysql_query || !pmysql_error || !pmysql_real_escape_string)
    {
        Log::GetLog()->error("[SurvivorTracker] Failed to resolve required DB functions");
        return false;
    }

    g_mysql_loaded = true;
    return true;
}

// =============================================================================
// Configuration
// =============================================================================

static const std::string       g_config_path = "ArkApi/Plugins/SurvivorTracker/config.json";
static std::string             g_db_host = "127.0.0.1";
static unsigned int            g_db_port = 3306;
static std::string             g_db_user;
static std::string             g_db_pass;
static std::string             g_db_name;
static std::uintmax_t                   g_cfg_size = 0;
static std::filesystem::file_time_type  g_cfg_mtime{};

static bool LoadConfig()
{
    std::ifstream file(g_config_path);
    if (!file.is_open())
    {
        Log::GetLog()->error("[SurvivorTracker] Cannot open config: {}", g_config_path);
        return false;
    }

    try
    {
        nlohmann::json j;
        file >> j;
        g_db_host = j.value("DbHost", "127.0.0.1");
        g_db_port = j.value("DbPort", 3306);
        g_db_user = j.value("DbUser", "");
        g_db_pass = j.value("DbPassword", "");
        g_db_name = j.value("DbName", "");
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[SurvivorTracker] Config parse error: {}", ex.what());
        return false;
    }

    if (g_db_user.empty() || g_db_name.empty())
    {
        Log::GetLog()->error("[SurvivorTracker] Config requires DbUser and DbName");
        return false;
    }

    try
    {
        g_cfg_size = std::filesystem::file_size(g_config_path);
        g_cfg_mtime = std::filesystem::last_write_time(g_config_path);
    }
    catch (...) {}

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

    std::ifstream file(g_config_path);
    if (!file.is_open()) return;

    try
    {
        nlohmann::json j;
        file >> j;
        g_db_host = j.value("DbHost", "127.0.0.1");
        g_db_port = j.value("DbPort", 3306);
        g_db_user = j.value("DbUser", "");
        g_db_pass = j.value("DbPassword", "");
        g_db_name = j.value("DbName", "");
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[SurvivorTracker] Config reload parse error: {}", ex.what());
        return;
    }

    g_cfg_size = size;
    g_cfg_mtime = mtime;

    Log::GetLog()->info("[SurvivorTracker] Config reloaded");
}

// =============================================================================
// Player Cache
// =============================================================================

struct SurvivorRecord
{
    std::string eosId;
    std::string survivorName;
    uint64_t    survivorId = 0;
    std::string tribeName;
    int         tribeId = 0;
    bool        inTribe = false;
    std::string mapName;
    bool        pendingRemoval = false;
};

static std::unordered_map<std::string, SurvivorRecord> g_cache;
static std::mutex                                       g_cache_mutex;

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
        Log::GetLog()->error("[SurvivorTracker] Query error: {}", pmysql_error(g_mysql));
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
        Log::GetLog()->error("[SurvivorTracker] mysql_init failed");
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
            Log::GetLog()->error("[SurvivorTracker] DB connect failed: {}", pmysql_error(g_mysql));
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
                Log::GetLog()->info("[SurvivorTracker] DB connection healthy");
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
            Log::GetLog()->info("[SurvivorTracker] DB reconnected");
            g_db_logged_down = false;
        }
        return true;
    }

    if (!g_db_logged_down)
    {
        Log::GetLog()->error("[SurvivorTracker] DB connection lost, retaining cache until reconnect");
        g_db_logged_down = true;
    }
    return false;
}

static bool InitDatabase()
{
    if (!LoadMySQLLib()) return false;
    if (!EstablishConnection()) return false;

    const std::string create_sql =
        "CREATE TABLE IF NOT EXISTS survivortracker_survivors ("
        "  eos_id        VARCHAR(64)      NOT NULL,"
        "  survivor_name VARCHAR(128)     NOT NULL DEFAULT '',"
        "  survivor_id   BIGINT UNSIGNED  NOT NULL DEFAULT 0,"
        "  tribe_name    VARCHAR(128)     NOT NULL DEFAULT '',"
        "  tribe_id      INT              NOT NULL DEFAULT 0,"
        "  map_name      VARCHAR(64)      NOT NULL,"
        "  PRIMARY KEY (eos_id, map_name)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";

    if (!ExecQuery(create_sql))
    {
        Log::GetLog()->error("[SurvivorTracker] Failed to create survivors table");
        pmysql_close(g_mysql);
        g_mysql = nullptr;
        return false;
    }

    Log::GetLog()->info("[SurvivorTracker] Database ready");
    return true;
}

static void CloseDatabase()
{
    if (g_mysql)
    {
        pmysql_close(g_mysql);
        g_mysql = nullptr;
    }
}

static void UpsertSurvivor(const SurvivorRecord& rec)
{
    if (!g_mysql) return;

    const std::string e_eos = EscapeUnsafe(rec.eosId);
    const std::string e_name = EscapeUnsafe(rec.survivorName);
    const std::string e_tribe = EscapeUnsafe(rec.tribeName);
    const std::string e_map = EscapeUnsafe(rec.mapName);

    char sql[1024]{};
    std::snprintf(sql, sizeof(sql),
        "INSERT INTO survivortracker_survivors "
        "(eos_id, survivor_name, survivor_id, tribe_name, tribe_id, map_name) "
        "VALUES ('%s', '%s', %llu, '%s', %d, '%s') "
        "ON DUPLICATE KEY UPDATE "
        "  survivor_name = VALUES(survivor_name),"
        "  survivor_id   = IF(VALUES(survivor_id) = 0, survivor_id, VALUES(survivor_id)),"
        "  tribe_name    = VALUES(tribe_name),"
        "  tribe_id      = VALUES(tribe_id)",
        e_eos.c_str(),
        e_name.c_str(),
        (unsigned long long)rec.survivorId,
        e_tribe.c_str(),
        rec.tribeId,
        e_map.c_str());

    ExecQuery(sql);
}

// =============================================================================
// Helpers
// =============================================================================

static std::string FStr(const FString& f)
{
    if (f.IsEmpty()) return "unknown";
    std::string out(TCHAR_TO_UTF8(*f));
    return out.empty() ? "unknown" : out;
}

static std::string GetMapName()
{
    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return "unknown";
    FString map;
    world->GetMapName(&map);
    return FStr(map);
}

static std::string GetEosFromState(AShooterPlayerState* ps)
{
    if (!ps) return "";
    FString eosRaw;
    ps->GetUniqueNetIdAsString(&eosRaw);
    const std::string eosId = FStr(eosRaw);
    return (eosId == "unknown") ? "" : eosId;
}

static bool BuildRecord(AShooterPlayerController* spc, SurvivorRecord& out)
{
    if (!spc) return false;

    AShooterPlayerState* ps =
        static_cast<AShooterPlayerState*>(spc->PlayerStateField().Get());
    const std::string eosId = GetEosFromState(ps);
    if (eosId.empty()) return false;

    AShooterCharacter* ch = spc->BaseGetPlayerCharacter();
    if (!ch) return false;

    out.eosId = eosId;
    out.survivorId = ch->LinkedPlayerDataIDField();
    out.survivorName = FStr(ch->PlayerNameField());
    out.mapName = GetMapName();
    out.tribeId = ps->GetTribeId();

    const std::string tn = FStr(ch->TribeNameField());
    if (!tn.empty() && tn != "unknown")
    {
        out.tribeName = tn;
        out.inTribe = true;
    }

    return true;
}

// =============================================================================
// Hook Type Aliases
// =============================================================================

using PostLogin_t = void(*)(AShooterGameMode*, APlayerController*);
using HandleRespawned_t = void(*)(AShooterPlayerController*, APawn*, bool);
using StartNewShooterPlayer_t = void(*)(AShooterGameMode*, APlayerController*, bool, bool,
    FPrimalPlayerCharacterConfigStruct&, UPrimalPlayerData*, bool);
using NetUpdateTribeName_t = void(*)(APrimalCharacter*, const FString*);
using NotifyJoined_t = void(*)(AShooterPlayerState*, const FString&, const FString&, bool);
using NotifyLeft_t = void(*)(AShooterPlayerState*, const FString&, const FString&, bool);
using RenamePlayer_t = void(*)(AShooterCharacter*, const FString*);
using Logout_t = void(*)(AShooterGameMode*, AController*);

static PostLogin_t             Original_PostLogin = nullptr;
static HandleRespawned_t       Original_HandleRespawned = nullptr;
static StartNewShooterPlayer_t Original_StartNewShooterPlayer = nullptr;
static NetUpdateTribeName_t    Original_NetUpdateTribeName = nullptr;
static NotifyJoined_t          Original_NotifyJoined = nullptr;
static NotifyLeft_t            Original_NotifyLeft = nullptr;
static RenamePlayer_t          Original_RenamePlayer = nullptr;
static Logout_t                Original_Logout = nullptr;

// =============================================================================
// Detours
// =============================================================================

void Detour_PostLogin(AShooterGameMode* gm, APlayerController* pc)
{
    Original_PostLogin(gm, pc);

    if (!pc || !pc->IsA(AShooterPlayerController::StaticClass())) return;
    AShooterPlayerController* spc = static_cast<AShooterPlayerController*>(pc);

    AShooterPlayerState* ps =
        static_cast<AShooterPlayerState*>(spc->PlayerStateField().Get());
    const std::string eosId = GetEosFromState(ps);
    if (eosId.empty()) return;

    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        auto it = g_cache.find(eosId);
        if (it != g_cache.end())
        {
            it->second.pendingRemoval = false;
            return;
        }
    }

    SurvivorRecord rec;
    if (!BuildRecord(spc, rec)) return;

    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        g_cache[eosId] = rec;
    }

    Log::GetLog()->info(
        "[SurvivorTracker] RECONNECT eos_id={} survivor_name={} survivor_id={} map={} "
        "tribe_name={} tribe_id={}",
        rec.eosId, rec.survivorName, rec.survivorId, rec.mapName,
        rec.inTribe ? rec.tribeName : "NULL",
        rec.inTribe ? rec.tribeId : 0
    );
}

void Detour_HandleRespawned(AShooterPlayerController* pc, APawn* pawn, bool bNewPlayer)
{
    Original_HandleRespawned(pc, pawn, bNewPlayer);

    if (!pc || !pawn) return;

    AShooterPlayerState* ps =
        static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
    const std::string eosId = GetEosFromState(ps);
    if (eosId.empty()) return;

    AShooterCharacter* ch = static_cast<AShooterCharacter*>(pawn);
    if (!ch) return;

    const std::string survivorName = FStr(ch->PlayerNameField());
    const std::string mapName = GetMapName();
    const int         tribeId = ps->GetTribeId();

    std::string tribeName;
    bool        inTribe = false;
    const std::string tn = FStr(ch->TribeNameField());
    if (!tn.empty() && tn != "unknown")
    {
        tribeName = tn;
        inTribe = true;
    }

    std::lock_guard<std::mutex> lock(g_cache_mutex);
    auto it = g_cache.find(eosId);
    if (it != g_cache.end())
    {
        it->second.survivorName = survivorName;
        it->second.tribeName = tribeName;
        it->second.tribeId = tribeId;
        it->second.inTribe = inTribe;
        it->second.mapName = mapName;
        it->second.pendingRemoval = false;
    }
    else
    {
        SurvivorRecord rec;
        rec.eosId = eosId;
        rec.survivorName = survivorName;
        rec.survivorId = 0;
        rec.tribeName = tribeName;
        rec.tribeId = tribeId;
        rec.inTribe = inTribe;
        rec.mapName = mapName;
        g_cache[eosId] = rec;
    }
}

void Detour_StartNewShooterPlayer(AShooterGameMode* gm,
    APlayerController* pc,
    bool                                b1,
    bool                                b2,
    FPrimalPlayerCharacterConfigStruct& cfg,
    UPrimalPlayerData* playerData,
    bool                                b3)
{
    Original_StartNewShooterPlayer(gm, pc, b1, b2, cfg, playerData, b3);

    if (!pc || !pc->IsA(AShooterPlayerController::StaticClass())) return;
    AShooterPlayerController* spc = static_cast<AShooterPlayerController*>(pc);

    AShooterPlayerState* ps =
        static_cast<AShooterPlayerState*>(spc->PlayerStateField().Get());
    const std::string eosId = GetEosFromState(ps);
    if (eosId.empty()) return;

    AShooterCharacter* ch = spc->BaseGetPlayerCharacter();
    if (!ch) return;

    const uint64_t newSurvivorId = ch->LinkedPlayerDataIDField();
    if (newSurvivorId == 0) return;

    const std::string survivorName = FStr(ch->PlayerNameField());
    const std::string mapName = GetMapName();

    std::lock_guard<std::mutex> lock(g_cache_mutex);
    auto it = g_cache.find(eosId);
    if (it == g_cache.end()) return;

    SurvivorRecord& rec = it->second;
    if (rec.survivorId == newSurvivorId)
    {
        rec.survivorName = survivorName;
        return;
    }

    const bool isRecreation = (rec.survivorId != 0) && (rec.mapName == mapName);

    if (isRecreation)
    {
        Log::GetLog()->info(
            "[SurvivorTracker] CHARACTER_RECREATED eos_id={} survivor_name={} "
            "old_survivor_id={} new_survivor_id={} map={}",
            rec.eosId, survivorName, rec.survivorId, newSurvivorId, mapName
        );

        rec.survivorId = newSurvivorId;
        rec.survivorName = survivorName;
        rec.tribeName = "";
        rec.tribeId = 0;
        rec.inTribe = false;
    }
    else
    {
        rec.survivorId = newSurvivorId;
        rec.survivorName = survivorName;

        Log::GetLog()->info(
            "[SurvivorTracker] CHARACTER_CREATED eos_id={} survivor_name={} survivor_id={} map={}",
            rec.eosId, rec.survivorName, rec.survivorId, mapName
        );
    }
}

void Detour_NetUpdateTribeName(APrimalCharacter* character, const FString* newTribeName)
{
    Original_NetUpdateTribeName(character, newTribeName);

    if (!character) return;

    APlayerController* controller = character->GetOwnerController();
    if (!controller || !controller->IsA(AShooterPlayerController::StaticClass())) return;

    AShooterPlayerController* spc = static_cast<AShooterPlayerController*>(controller);
    AShooterPlayerState* ps =
        static_cast<AShooterPlayerState*>(spc->PlayerStateField().Get());
    const std::string eosId = GetEosFromState(ps);
    if (eosId.empty()) return;

    const std::string newName = newTribeName ? FStr(*newTribeName) : "";
    if (newName.empty() || newName == "unknown") return;

    std::lock_guard<std::mutex> lock(g_cache_mutex);
    auto it = g_cache.find(eosId);
    if (it == g_cache.end()) return;

    SurvivorRecord& rec = it->second;
    if (rec.inTribe && rec.tribeName == newName) return;

    const std::string oldName = rec.tribeName;
    rec.tribeName = newName;
    rec.inTribe = true;

    if (!oldName.empty())
    {
        Log::GetLog()->info(
            "[SurvivorTracker] TRIBE_RENAMED eos_id={} survivor_name={} "
            "old_tribe_name={} new_tribe_name={} tribe_id={} map={}",
            rec.eosId, rec.survivorName, oldName, rec.tribeName, rec.tribeId, rec.mapName
        );
    }
}

void Detour_NotifyJoinedTribe(AShooterPlayerState* ps,
    const FString& playerName,
    const FString& tribeName,
    bool           joinee)
{
    Original_NotifyJoined(ps, playerName, tribeName, joinee);

    const std::string eosId = GetEosFromState(ps);
    if (eosId.empty()) return;

    const std::string newTribeName = FStr(tribeName);
    const int         newTribeId = ps->GetTribeId();

    std::lock_guard<std::mutex> lock(g_cache_mutex);
    auto it = g_cache.find(eosId);
    if (it == g_cache.end()) return;

    it->second.tribeName = newTribeName;
    it->second.tribeId = newTribeId;
    it->second.inTribe = true;

    const SurvivorRecord& rec = it->second;
    Log::GetLog()->info(
        "[SurvivorTracker] TRIBE_JOIN eos_id={} survivor_name={} survivor_id={} "
        "tribe_name={} tribe_id={} map={}",
        rec.eosId, rec.survivorName, rec.survivorId,
        rec.tribeName, rec.tribeId, rec.mapName
    );
}

void Detour_NotifyLeftTribe(AShooterPlayerState* ps,
    const FString& playerName,
    const FString& tribeName,
    bool           joinee)
{
    Original_NotifyLeft(ps, playerName, tribeName, joinee);

    const std::string eosId = GetEosFromState(ps);
    if (eosId.empty()) return;

    std::lock_guard<std::mutex> lock(g_cache_mutex);
    auto it = g_cache.find(eosId);
    if (it == g_cache.end()) return;

    it->second.tribeName = "";
    it->second.tribeId = 0;
    it->second.inTribe = false;

    const SurvivorRecord& rec = it->second;
    Log::GetLog()->info(
        "[SurvivorTracker] TRIBE_LEAVE eos_id={} survivor_name={} survivor_id={} "
        "tribe_name=NULL tribe_id=NULL map={}",
        rec.eosId, rec.survivorName, rec.survivorId, rec.mapName
    );
}

void Detour_RenamePlayer(AShooterCharacter* character, const FString* newName)
{
    Original_RenamePlayer(character, newName);

    if (!character) return;

    std::string newSurvivorName = FStr(character->PlayerNameField());
    if ((newSurvivorName.empty() || newSurvivorName == "unknown") && newName)
        newSurvivorName = FStr(*newName);
    if (newSurvivorName.empty() || newSurvivorName == "unknown") return;

    APlayerController* controller = character->GetOwnerController();
    if (!controller || !controller->IsA(AShooterPlayerController::StaticClass())) return;

    AShooterPlayerController* spc = static_cast<AShooterPlayerController*>(controller);
    AShooterPlayerState* ps =
        static_cast<AShooterPlayerState*>(spc->PlayerStateField().Get());
    const std::string eosId = GetEosFromState(ps);
    if (eosId.empty()) return;

    const uint64_t survivorId = character->LinkedPlayerDataIDField();

    std::lock_guard<std::mutex> lock(g_cache_mutex);
    auto it = g_cache.find(eosId);
    if (it != g_cache.end())
    {
        if (it->second.survivorName == newSurvivorName) return;

        const std::string oldName = it->second.survivorName;
        it->second.survivorName = newSurvivorName;
        if (it->second.survivorId == 0 && survivorId != 0)
            it->second.survivorId = survivorId;

        Log::GetLog()->info(
            "[SurvivorTracker] NAME_CHANGED eos_id={} old_name={} new_name={} map={}",
            eosId, oldName, newSurvivorName, it->second.mapName
        );
    }
    else
    {
        SurvivorRecord rec;
        if (!BuildRecord(spc, rec)) return;
        rec.survivorName = newSurvivorName;
        g_cache[eosId] = rec;

        Log::GetLog()->info(
            "[SurvivorTracker] NAME_CHANGED eos_id={} old_name={} new_name={} map={}",
            eosId, "NULL", newSurvivorName, rec.mapName
        );
    }
}

void Detour_Logout(AShooterGameMode* gm, AController* controller)
{
    if (controller && controller->IsA(AShooterPlayerController::StaticClass()))
    {
        AShooterPlayerController* spc = static_cast<AShooterPlayerController*>(controller);
        AShooterPlayerState* ps =
            static_cast<AShooterPlayerState*>(spc->PlayerStateField().Get());
        const std::string eosId = GetEosFromState(ps);
        if (!eosId.empty())
        {
            std::lock_guard<std::mutex> lock(g_cache_mutex);
            auto it = g_cache.find(eosId);
            if (it != g_cache.end())
            {
                it->second.pendingRemoval = true;

                const SurvivorRecord& rec = it->second;
                Log::GetLog()->info(
                    "[SurvivorTracker] DISCONNECT eos_id={} survivor_name={} survivor_id={} "
                    "map={} tribe_name={} tribe_id={}",
                    rec.eosId, rec.survivorName, rec.survivorId, rec.mapName,
                    rec.inTribe ? rec.tribeName : "NULL",
                    rec.inTribe ? rec.tribeId : 0
                );
            }
        }
    }

    Original_Logout(gm, controller);
}

// =============================================================================
// Writer Thread
// =============================================================================

static std::thread       g_worker_thread;
static std::atomic<bool> g_worker_running{ false };

static void FlushCache()
{
    std::vector<SurvivorRecord> toWrite;

    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        if (g_cache.empty()) return;
        toWrite.reserve(g_cache.size());
        for (const auto& kv : g_cache)
            toWrite.push_back(kv.second);
    }

    {
        std::lock_guard<std::mutex> dbLock(g_db_mutex);
        if (!EnsureConnected()) return;
        for (const auto& rec : toWrite)
            UpsertSurvivor(rec);
    }

    std::lock_guard<std::mutex> lock(g_cache_mutex);
    for (const auto& rec : toWrite)
    {
        if (!rec.pendingRemoval) continue;
        auto it = g_cache.find(rec.eosId);
        if (it != g_cache.end() && it->second.pendingRemoval)
            g_cache.erase(it);
    }
}

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
            FlushCache();
            ReloadConfigIfChanged();
        }
    }

    FlushCache();
}

// =============================================================================
// Seeding
// =============================================================================

static void SeedAllOnlinePlayers()
{
    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return;

    auto& controllers = world->PlayerControllerListField();
    for (int i = 0; i < controllers.Num(); ++i)
    {
        APlayerController* pc = controllers[i].Get();
        if (!pc || !pc->IsA(AShooterPlayerController::StaticClass())) continue;

        AShooterPlayerController* spc = static_cast<AShooterPlayerController*>(pc);

        SurvivorRecord rec;
        if (!BuildRecord(spc, rec)) continue;

        std::lock_guard<std::mutex> lock(g_cache_mutex);
        auto it = g_cache.find(rec.eosId);
        if (it == g_cache.end())
            g_cache[rec.eosId] = rec;
        else
            it->second.pendingRemoval = false;
    }
}

// =============================================================================
// Plugin Entry Points
// =============================================================================

static void InitImpl()
{
    Log::Get().Init("SurvivorTracker");

    if (!LoadConfig())
    {
        Log::GetLog()->error("[SurvivorTracker] Halted - config error");
        return;
    }

    if (!InitDatabase())
    {
        Log::GetLog()->error("[SurvivorTracker] Halted - database error");
        return;
    }

    AsaApi::GetHooks().SetHook(
        "AShooterGameMode.PostLogin(APlayerController*)",
        (LPVOID)&Detour_PostLogin,
        (LPVOID*)&Original_PostLogin
    );

    AsaApi::GetHooks().SetHook(
        "AShooterPlayerController.HandleRespawned_Implementation(APawn*,bool)",
        (LPVOID)&Detour_HandleRespawned,
        (LPVOID*)&Original_HandleRespawned
    );

    AsaApi::GetHooks().SetHook(
        "AShooterGameMode.StartNewShooterPlayer(APlayerController*,bool,bool,FPrimalPlayerCharacterConfigStruct&,UPrimalPlayerData*,bool)",
        (LPVOID)&Detour_StartNewShooterPlayer,
        (LPVOID*)&Original_StartNewShooterPlayer
    );

    AsaApi::GetHooks().SetHook(
        "APrimalCharacter.NetUpdateTribeName(FString&)",
        (LPVOID)&Detour_NetUpdateTribeName,
        (LPVOID*)&Original_NetUpdateTribeName
    );

    AsaApi::GetHooks().SetHook(
        "AShooterPlayerState.NotifyPlayerJoinedTribe(FString&,FString&,bool)",
        (LPVOID)&Detour_NotifyJoinedTribe,
        (LPVOID*)&Original_NotifyJoined
    );

    AsaApi::GetHooks().SetHook(
        "AShooterPlayerState.NotifyPlayerLeftTribe(FString&,FString&,bool)",
        (LPVOID)&Detour_NotifyLeftTribe,
        (LPVOID*)&Original_NotifyLeft
    );

    AsaApi::GetHooks().SetHook(
        "AShooterCharacter.RenamePlayer(FString&)",
        (LPVOID)&Detour_RenamePlayer,
        (LPVOID*)&Original_RenamePlayer
    );

    AsaApi::GetHooks().SetHook(
        "AShooterGameMode.Logout(AController*)",
        (LPVOID)&Detour_Logout,
        (LPVOID*)&Original_Logout
    );

    g_worker_running.store(true);
    g_worker_thread = std::thread(WorkerLoop);

    SeedAllOnlinePlayers();

    Log::GetLog()->info("[SurvivorTracker] Plugin loaded");
}

static void UnloadImpl()
{
    g_worker_running.store(false);
    if (g_worker_thread.joinable())
        g_worker_thread.join();

    AsaApi::GetHooks().DisableHook(
        "AShooterGameMode.PostLogin(APlayerController*)",
        (LPVOID)&Detour_PostLogin
    );

    AsaApi::GetHooks().DisableHook(
        "AShooterPlayerController.HandleRespawned_Implementation(APawn*,bool)",
        (LPVOID)&Detour_HandleRespawned
    );

    AsaApi::GetHooks().DisableHook(
        "AShooterGameMode.StartNewShooterPlayer(APlayerController*,bool,bool,FPrimalPlayerCharacterConfigStruct&,UPrimalPlayerData*,bool)",
        (LPVOID)&Detour_StartNewShooterPlayer
    );

    AsaApi::GetHooks().DisableHook(
        "APrimalCharacter.NetUpdateTribeName(FString&)",
        (LPVOID)&Detour_NetUpdateTribeName
    );

    AsaApi::GetHooks().DisableHook(
        "AShooterPlayerState.NotifyPlayerJoinedTribe(FString&,FString&,bool)",
        (LPVOID)&Detour_NotifyJoinedTribe
    );

    AsaApi::GetHooks().DisableHook(
        "AShooterPlayerState.NotifyPlayerLeftTribe(FString&,FString&,bool)",
        (LPVOID)&Detour_NotifyLeftTribe
    );

    AsaApi::GetHooks().DisableHook(
        "AShooterCharacter.RenamePlayer(FString&)",
        (LPVOID)&Detour_RenamePlayer
    );

    AsaApi::GetHooks().DisableHook(
        "AShooterGameMode.Logout(AController*)",
        (LPVOID)&Detour_Logout
    );

    CloseDatabase();

    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        g_cache.clear();
    }

    Log::GetLog()->info("[SurvivorTracker] Plugin unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { InitImpl(); }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[SurvivorTracker] Plugin_Init exception: {}", ex.what());
    }
    catch (...)
    {
        Log::GetLog()->error("[SurvivorTracker] Plugin_Init unknown exception");
    }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { UnloadImpl(); }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[SurvivorTracker] Plugin_Unload exception: {}", ex.what());
    }
    catch (...)
    {
        Log::GetLog()->error("[SurvivorTracker] Plugin_Unload unknown exception");
    }
}