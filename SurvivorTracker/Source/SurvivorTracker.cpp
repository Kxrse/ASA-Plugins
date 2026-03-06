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
 * Tracks player identity and tribe membership per map across a cluster.
 *
 * Table: survivors
 *   PK (eos_id, map_name) — one row per player per map.
 *   Columns: eos_id, survivor_name, survivor_id, tribe_name, tribe_id, map_name
 *
 * Hooks:
 *   PostLogin              — rebuild cache on reconnect
 *   HandleRespawned        — refresh name + tribe on spawn/respawn
 *   StartNewShooterPlayer  — resolve survivor_id; detect creation vs respawn
 *   NotifyPlayerJoinedTribe — update tribe fields
 *   NotifyPlayerLeftTribe  — null tribe fields
 *   NetUpdateTribeName     — update tribe name on rename
 *   Logout                 — final upsert, clear cache entry
 */

#include <API/ARK/Ark.h>
#include <json.hpp>
#include <Windows.h>
#include <fstream>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <cstdio>
#include <cstring>

 // =============================================================================
 // MariaDB — Dynamic Load
 // =============================================================================

typedef struct st_mysql     MYSQL;
typedef struct st_mysql_res MYSQL_RES;
typedef char** MYSQL_ROW;

typedef MYSQL* (__stdcall* mysql_init_t)             (MYSQL*);
typedef MYSQL* (__stdcall* mysql_real_connect_t)     (MYSQL*, const char*, const char*, const char*, const char*, unsigned int, const char*, unsigned long);
typedef void(__stdcall* mysql_close_t)            (MYSQL*);
typedef int(__stdcall* mysql_query_t)            (MYSQL*, const char*);
typedef MYSQL_RES* (__stdcall* mysql_store_result_t)     (MYSQL*);
typedef void(__stdcall* mysql_free_result_t)      (MYSQL_RES*);
typedef const char* (__stdcall* mysql_error_t)            (MYSQL*);
typedef unsigned long(__stdcall* mysql_real_escape_string_t)(MYSQL*, char*, const char*, unsigned long);
typedef int(__stdcall* mysql_options_t)          (MYSQL*, int, const void*);

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
static bool                       g_mysql_loaded = false;

// Searches candidate paths for libmariadb.dll and resolves all function pointers.
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
            Log::GetLog()->info("[SurvivorTracker] Loaded DB library: {}", candidates[i]);
            break;
        }
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

static std::string  g_db_host = "localhost";
static unsigned int g_db_port = 3306;
static std::string  g_db_user;
static std::string  g_db_pass;
static std::string  g_db_name;

// Reads database credentials from ArkApi/Plugins/SurvivorTracker/config.json.
static bool LoadConfig()
{
    const std::string path = "ArkApi/Plugins/SurvivorTracker/config.json";
    std::ifstream file(path);
    if (!file.is_open())
    {
        Log::GetLog()->error("[SurvivorTracker] Cannot open config: {}", path);
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

    return true;
}

// =============================================================================
// Database
// =============================================================================

static MYSQL* g_mysql = nullptr;
static std::mutex g_db_mutex;

// Caller must hold g_db_mutex.
static std::string EscapeUnsafe(const std::string& in)
{
    if (!g_mysql || !pmysql_real_escape_string) return in;
    std::string buf(in.size() * 2 + 1, '\0');
    unsigned long len = pmysql_real_escape_string(
        g_mysql, buf.data(), in.c_str(), (unsigned long)in.size());
    buf.resize(len);
    return buf;
}

// Caller must hold g_db_mutex.
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

// Connects to MariaDB and creates the survivors table if it does not exist.
static bool InitDatabase()
{
    if (!LoadMySQLLib()) return false;

    g_mysql = pmysql_init(nullptr);
    if (!g_mysql)
    {
        Log::GetLog()->error("[SurvivorTracker] mysql_init returned null");
        return false;
    }

    unsigned int timeout = 10;
    pmysql_options(g_mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

    if (!pmysql_real_connect(g_mysql,
        g_db_host.c_str(), g_db_user.c_str(), g_db_pass.c_str(),
        g_db_name.c_str(), g_db_port, nullptr, 0))
    {
        Log::GetLog()->error("[SurvivorTracker] DB connect failed: {}", pmysql_error(g_mysql));
        pmysql_close(g_mysql);
        g_mysql = nullptr;
        return false;
    }

    const std::string create_sql =
        "CREATE TABLE IF NOT EXISTS survivors ("
        "  eos_id        VARCHAR(128)     NOT NULL,"
        "  survivor_name VARCHAR(128)     NOT NULL,"
        "  survivor_id   BIGINT UNSIGNED  NOT NULL DEFAULT 0,"
        "  tribe_name    VARCHAR(128)     NULL,"
        "  tribe_id      INT              NULL,"
        "  map_name      VARCHAR(128)     NOT NULL,"
        "  PRIMARY KEY  (eos_id, map_name)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;";

    if (!ExecQuery(create_sql))
    {
        Log::GetLog()->error("[SurvivorTracker] Failed to create survivors table");
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

// Full upsert keyed on (eos_id, map_name). Pass nullptr for tribe fields to write SQL NULL.
static void UpsertSurvivor(const std::string& eosId,
    const std::string& survivorName,
    uint64_t           survivorId,
    const std::string* tribeName,
    const int* tribeId,
    const std::string& mapName)
{
    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!g_mysql) return;

    const std::string e_eos = EscapeUnsafe(eosId);
    const std::string e_name = EscapeUnsafe(survivorName);
    const std::string e_map = EscapeUnsafe(mapName);

    std::string tribeNameVal = tribeName ? ("'" + EscapeUnsafe(*tribeName) + "'") : "NULL";
    std::string tribeIdVal = tribeId ? std::to_string(*tribeId) : "NULL";

    char sql[1024]{};
    std::snprintf(sql, sizeof(sql),
        "INSERT INTO survivors "
        "  (eos_id, survivor_name, survivor_id, tribe_name, tribe_id, map_name) "
        "VALUES ('%s', '%s', %llu, %s, %s, '%s') "
        "ON DUPLICATE KEY UPDATE "
        "  survivor_name = VALUES(survivor_name),"
        "  survivor_id   = IF(VALUES(survivor_id) = 0, survivor_id, VALUES(survivor_id)),"
        "  tribe_name    = VALUES(tribe_name),"
        "  tribe_id      = VALUES(tribe_id);",
        e_eos.c_str(), e_name.c_str(),
        (unsigned long long)survivorId,
        tribeNameVal.c_str(), tribeIdVal.c_str(),
        e_map.c_str()
    );

    ExecQuery(sql);
}

// =============================================================================
// Player Cache
// =============================================================================

struct SurvivorRecord
{
    std::string eosId;
    std::string survivorName;
    uint64_t    survivorId = 0;  // 0 until StartNewShooterPlayer resolves it.
    std::string tribeName;
    int         tribeId = 0;
    bool        inTribe = false;
    std::string mapName;
};

static std::unordered_map<std::string, SurvivorRecord> g_cache;       // Keyed by eos_id.
static std::mutex                                       g_cache_mutex;

// =============================================================================
// Flush Thread
// =============================================================================

static std::thread       g_flush_thread;
static std::atomic<bool> g_flush_running{ false };

// Wakes every 60 seconds and upserts all cached records to the database.
static void FlushThreadFunc()
{
    while (g_flush_running.load())
    {
        for (int i = 0; i < 60 && g_flush_running.load(); ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));

        if (!g_flush_running.load()) break;

        std::lock_guard<std::mutex> lock(g_cache_mutex);
        for (const auto& kv : g_cache)
        {
            const SurvivorRecord& rec = kv.second;
            const std::string* tnPtr = rec.inTribe ? &rec.tribeName : nullptr;
            const int* tidPtr = rec.inTribe ? &rec.tribeId : nullptr;
            UpsertSurvivor(rec.eosId, rec.survivorName, rec.survivorId,
                tnPtr, tidPtr, rec.mapName);
        }
    }
}

// =============================================================================
// Helpers
// =============================================================================

static std::string FStr(const FString& f)
{
    const char* s = TCHAR_TO_UTF8(*f);
    return (s && s[0]) ? s : "unknown";
}

static std::string GetMapName()
{
    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return "unknown";
    FString map;
    world->GetMapName(&map);
    return FStr(map);
}

static void UpsertFromRecord(const SurvivorRecord& rec)
{
    const std::string* tnPtr = rec.inTribe ? &rec.tribeName : nullptr;
    const int* tidPtr = rec.inTribe ? &rec.tribeId : nullptr;
    UpsertSurvivor(rec.eosId, rec.survivorName, rec.survivorId,
        tnPtr, tidPtr, rec.mapName);
}

// =============================================================================
// Hook Type Aliases
// =============================================================================

using PostLogin_t = void(*)(AShooterGameMode*, APlayerController*);
using HandleRespawned_t = void(*)(AShooterPlayerController*, APawn*, bool);
using StartNewShooterPlayer_t = void(*)(AShooterGameMode*, APlayerController*, bool, bool, FPrimalPlayerCharacterConfigStruct&, UPrimalPlayerData*, bool);
using NetUpdateTribeName_t = void(*)(APrimalCharacter*, FString&);
using NotifyJoined_t = void(*)(AShooterPlayerState*, FString&, FString&, bool);
using NotifyLeft_t = void(*)(AShooterPlayerState*, FString&, FString&, bool);
using Logout_t = void(*)(AShooterGameMode*, AController*);

static PostLogin_t             Original_PostLogin = nullptr;
static HandleRespawned_t       Original_HandleRespawned = nullptr;
static StartNewShooterPlayer_t Original_StartNewShooterPlayer = nullptr;
static NetUpdateTribeName_t    Original_NetUpdateTribeName = nullptr;
static NotifyJoined_t          Original_NotifyJoined = nullptr;
static NotifyLeft_t            Original_NotifyLeft = nullptr;
static Logout_t                Original_Logout = nullptr;

// =============================================================================
// Detours
// =============================================================================

// Rebuilds the cache entry for returning players whose pawn is already possessed.
// New players have no pawn yet — HandleRespawned + StartNewShooterPlayer cover them.
void Detour_PostLogin(AShooterGameMode* gm, APlayerController* pc)
{
    Original_PostLogin(gm, pc);

    if (!pc) return;

    AShooterPlayerState* ps =
        static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
    if (!ps) return;

    FString eosRaw;
    ps->GetUniqueNetIdAsString(&eosRaw);
    const std::string eosId = FStr(eosRaw);
    if (eosId.empty() || eosId == "unknown") return;

    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        if (g_cache.count(eosId)) return;
    }

    APawn* pawn = pc->PawnField().Get();
    if (!pawn) return;

    AShooterCharacter* ch = static_cast<AShooterCharacter*>(pawn);
    if (!ch) return;

    SurvivorRecord rec;
    rec.eosId = eosId;
    rec.survivorId = ch->GetLinkedPlayerDataID();
    rec.mapName = GetMapName();

    FString nameRaw;
    ps->GetPlayerName(&nameRaw);
    rec.survivorName = FStr(nameRaw);

    rec.tribeId = ps->GetTribeId();
    std::string tn = FStr(ch->TribeNameField());
    if (!tn.empty() && tn != "unknown")
    {
        rec.tribeName = tn;
        rec.inTribe = true;
    }

    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        g_cache[eosId] = rec;
    }

    Log::GetLog()->info(
        "[SurvivorTracker] RECONNECT eos_id={} survivor_name={} "
        "survivor_id={} map={} tribe_name={} tribe_id={}",
        rec.eosId, rec.survivorName, rec.survivorId, rec.mapName,
        rec.inTribe ? rec.tribeName : "NULL",
        rec.inTribe ? rec.tribeId : 0
    );

    UpsertFromRecord(rec);
}

// Refreshes survivor_name and tribe data on every spawn and respawn.
// survivor_id is not captured here — GetLinkedPlayerDataID() is not yet populated
// at this point in the spawn lifecycle. StartNewShooterPlayer resolves it.
void Detour_HandleRespawned(AShooterPlayerController* pc,
    APawn* pawn,
    bool                      bNewPlayer)
{
    Original_HandleRespawned(pc, pawn, bNewPlayer);

    if (!pc || !pawn) return;

    AShooterPlayerState* ps =
        static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
    if (!ps) return;

    FString eosRaw;
    ps->GetUniqueNetIdAsString(&eosRaw);
    const std::string eosId = FStr(eosRaw);
    if (eosId.empty() || eosId == "unknown") return;

    FString nameRaw;
    ps->GetPlayerName(&nameRaw);
    const std::string survivorName = FStr(nameRaw);

    const std::string mapName = GetMapName();
    const int         tribeId = ps->GetTribeId();

    std::string tribeName;
    bool        inTribe = false;

    if (AShooterCharacter* ch = static_cast<AShooterCharacter*>(pawn))
    {
        std::string tn = FStr(ch->TribeNameField());
        if (!tn.empty() && tn != "unknown")
        {
            tribeName = tn;
            inTribe = true;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        auto it = g_cache.find(eosId);
        if (it != g_cache.end())
        {
            // Respawn — refresh mutable fields, preserve existing survivor_id.
            it->second.survivorName = survivorName;
            it->second.tribeName = tribeName;
            it->second.tribeId = tribeId;
            it->second.inTribe = inTribe;
            it->second.mapName = mapName;
            UpsertFromRecord(it->second);
        }
        else
        {
            // First spawn this session — create initial cache entry.
            SurvivorRecord rec;
            rec.eosId = eosId;
            rec.survivorName = survivorName;
            rec.survivorId = 0;
            rec.tribeName = tribeName;
            rec.tribeId = tribeId;
            rec.inTribe = inTribe;
            rec.mapName = mapName;
            g_cache[eosId] = rec;
            UpsertFromRecord(rec);
        }
    }
}

// Primary differentiation point between creation and respawn.
// survivor_id unchanged = respawn, skip.
// survivor_id is new    = character creation, log CHARACTER_CREATED.
// survivor_id differs and was non-zero = recreation, null tribe data, log CHARACTER_RECREATED.
void Detour_StartNewShooterPlayer(AShooterGameMode* gm,
    APlayerController* pc,
    bool                                b1,
    bool                                b2,
    FPrimalPlayerCharacterConfigStruct& cfg,
    UPrimalPlayerData* playerData,
    bool                                b3)
{
    Original_StartNewShooterPlayer(gm, pc, b1, b2, cfg, playerData, b3);

    if (!pc) return;

    AShooterPlayerState* ps =
        static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
    if (!ps) return;

    FString eosRaw;
    ps->GetUniqueNetIdAsString(&eosRaw);
    const std::string eosId = FStr(eosRaw);
    if (eosId.empty() || eosId == "unknown") return;

    APawn* pawn = pc->PawnField().Get();
    if (!pawn) return;

    AShooterCharacter* ch = static_cast<AShooterCharacter*>(pawn);
    if (!ch) return;

    const uint64_t newSurvivorId = ch->GetLinkedPlayerDataID();
    if (newSurvivorId == 0) return;

    const std::string mapName = GetMapName();

    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        auto it = g_cache.find(eosId);
        if (it == g_cache.end()) return;

        SurvivorRecord& rec = it->second;

        if (rec.survivorId == newSurvivorId) return;

        const bool isRecreation = (rec.survivorId != 0) &&
            (rec.mapName == mapName);

        if (isRecreation)
        {
            Log::GetLog()->info(
                "[SurvivorTracker] CHARACTER_RECREATED eos_id={} survivor_name={} "
                "old_survivor_id={} new_survivor_id={} map={}",
                rec.eosId, rec.survivorName, rec.survivorId, newSurvivorId, mapName
            );

            rec.survivorId = newSurvivorId;
            rec.tribeName = "";
            rec.tribeId = 0;
            rec.inTribe = false;
        }
        else
        {
            rec.survivorId = newSurvivorId;

            Log::GetLog()->info(
                "[SurvivorTracker] CHARACTER_CREATED eos_id={} survivor_name={} "
                "survivor_id={} map={}",
                rec.eosId, rec.survivorName, rec.survivorId, mapName
            );
        }

        UpsertFromRecord(rec);
    }
}

// Updates tribe_name for all online tribe members when the tribe is renamed.
// Skips when old name is empty — this indicates engine state restoration on
// respawn rather than a player-initiated rename.
void Detour_NetUpdateTribeName(APrimalCharacter* character, FString& newNameFStr)
{
    Original_NetUpdateTribeName(character, newNameFStr);

    if (!character) return;

    AController* controller = character->GetOwnerController();
    if (!controller) return;

    AShooterPlayerController* pc = static_cast<AShooterPlayerController*>(controller);
    if (!pc) return;

    AShooterPlayerState* ps =
        static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
    if (!ps) return;

    FString eosRaw;
    ps->GetUniqueNetIdAsString(&eosRaw);
    const std::string eosId = FStr(eosRaw);
    if (eosId.empty() || eosId == "unknown") return;

    const std::string newName = FStr(newNameFStr);
    if (newName.empty() || newName == "unknown") return;

    {
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

        UpsertFromRecord(rec);
    }
}

// Updates tribe fields when a player joins or creates a tribe.
void Detour_NotifyJoinedTribe(AShooterPlayerState* ps,
    FString& playerName,
    FString& tribeName,
    bool                 joinee)
{
    Original_NotifyJoined(ps, playerName, tribeName, joinee);

    if (!ps) return;

    FString eosRaw;
    ps->GetUniqueNetIdAsString(&eosRaw);
    const std::string eosId = FStr(eosRaw);

    const std::string newTribeName = FStr(tribeName);
    const int         newTribeId = ps->GetTribeId();

    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        auto it = g_cache.find(eosId);
        if (it != g_cache.end())
        {
            it->second.tribeName = newTribeName;
            it->second.tribeId = newTribeId;
            it->second.inTribe = true;

            const SurvivorRecord& rec = it->second;

            Log::GetLog()->info(
                "[SurvivorTracker] TRIBE_JOIN eos_id={} survivor_name={} "
                "survivor_id={} tribe_name={} tribe_id={} map={}",
                rec.eosId, rec.survivorName, rec.survivorId,
                rec.tribeName, rec.tribeId, rec.mapName
            );

            UpsertFromRecord(rec);
        }
    }
}

// Nulls tribe fields when a player leaves a tribe.
void Detour_NotifyLeftTribe(AShooterPlayerState* ps,
    FString& playerName,
    FString& tribeName,
    bool                 joinee)
{
    Original_NotifyLeft(ps, playerName, tribeName, joinee);

    if (!ps) return;

    FString eosRaw;
    ps->GetUniqueNetIdAsString(&eosRaw);
    const std::string eosId = FStr(eosRaw);

    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        auto it = g_cache.find(eosId);
        if (it != g_cache.end())
        {
            it->second.tribeName = "";
            it->second.tribeId = 0;
            it->second.inTribe = false;

            const SurvivorRecord& rec = it->second;

            Log::GetLog()->info(
                "[SurvivorTracker] TRIBE_LEAVE eos_id={} survivor_name={} "
                "survivor_id={} tribe_name=NULL tribe_id=NULL map={}",
                rec.eosId, rec.survivorName, rec.survivorId, rec.mapName
            );

            UpsertFromRecord(rec);
        }
    }
}

// Final upsert from cache on disconnect, then clear the cache entry.
// Original called last so player state remains valid during access.
void Detour_Logout(AShooterGameMode* gm, AController* controller)
{
    if (AShooterPlayerController* pc =
        static_cast<AShooterPlayerController*>(controller))
    {
        AShooterPlayerState* ps =
            static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());

        if (ps)
        {
            FString eosRaw;
            ps->GetUniqueNetIdAsString(&eosRaw);
            const std::string eosId = FStr(eosRaw);

            std::lock_guard<std::mutex> lock(g_cache_mutex);
            auto it = g_cache.find(eosId);
            if (it != g_cache.end())
            {
                const SurvivorRecord& rec = it->second;

                Log::GetLog()->info(
                    "[SurvivorTracker] DISCONNECT eos_id={} survivor_name={} "
                    "survivor_id={} map={} tribe_name={} tribe_id={}",
                    rec.eosId, rec.survivorName, rec.survivorId, rec.mapName,
                    rec.inTribe ? rec.tribeName : "NULL",
                    rec.inTribe ? rec.tribeId : 0
                );

                UpsertFromRecord(rec);
                g_cache.erase(it);
            }
        }
    }

    Original_Logout(gm, controller);
}

// =============================================================================
// Plugin Entry Points
// =============================================================================

extern "C" __declspec(dllexport) void Plugin_Init()
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

    g_flush_running.store(true);
    g_flush_thread = std::thread(FlushThreadFunc);

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
        "APrimalCharacter.NetUpdateTribeName_Implementation(FString&)",
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
        "AShooterGameMode.Logout(AController*)",
        (LPVOID)&Detour_Logout,
        (LPVOID*)&Original_Logout
    );

    Log::GetLog()->info("[SurvivorTracker] Plugin loaded");
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    // Stop flush thread before unhooking to avoid upserts during teardown.
    g_flush_running.store(false);
    if (g_flush_thread.joinable())
        g_flush_thread.join();

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
        "APrimalCharacter.NetUpdateTribeName_Implementation(FString&)",
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