/*
StructureStats - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * StructureStats - ASA Plugin
 *
 * Hook category: Structures
 *
 * Tables:
 *   building_stats_player — PK (eos_id, survivor_id)
 *     Columns: survivor_name, tribe_name, tribe_id,
 *              structures_placed, structures_demolished, structures_pickedup
 *
 *   building_stats_tribe  — PK tribe_id
 *     Columns: tribe_name,
 *              structures_placed, structures_demolished, structures_pickedup
 *
 *   destruction_player    — PK (eos_id, survivor_id)
 *     Columns: survivor_name, tribe_name, tribe_id, structures_destroyed
 *
 *   destruction_tribe     — PK tribe_id
 *     Columns: tribe_name, structures_destroyed
 *
 * Hooks:
 *   AShooterGameMode.PostLogin                          — rebuild cache on reconnect
 *   AShooterGameMode.StartNewShooterPlayer              — seed survivor_id
 *   AShooterPlayerController.HandleRespawned_Implementation — refresh cache on spawn
 *   AShooterPlayerState.NotifyPlayerJoinedTribe         — update tribe fields
 *   AShooterPlayerState.NotifyPlayerLeftTribe           — clear tribe fields
 *   APrimalCharacter.NetUpdateTribeName_Implementation  — reflect tribe renames
 *   AShooterGameMode.Logout                             — clear cache on disconnect
 *   APrimalStructure.PlacedStructure                    — increment placed
 *   APrimalStructure.Demolish                           — increment demolished
 *   APrimalStructure.PickupStructure                    — increment pickedup
 *   APrimalStructure.Die                                — increment destroyed (blacklist filtered)
 *
 * Config:
 *   ArkApi/Plugins/StructureStats/config.json
 *   DestructionBlacklist: array of UClass name strings to exclude from destruction scoring
 *
 * Write strategy:
 *   Deltas accumulated in-memory; background thread flushes every 5 seconds.
 *   Tribe name renames issued as immediate UPDATE on both tribe tables.
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
#include <unordered_set>
#include <cstdio>

 // =============================================================================
 // MariaDB — Dynamic Load
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
            Log::GetLog()->info("[StructureStats] Loaded DB library: {}", candidates[i]);
            break;
        }
    }

    if (!g_mysql_module)
    {
        Log::GetLog()->error("[StructureStats] Could not find libmariadb.dll or libmysql.dll");
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
        Log::GetLog()->error("[StructureStats] Failed to resolve required DB functions");
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
static std::unordered_set<std::string> g_destruction_blacklist;

static bool LoadConfig()
{
    const std::string path = "ArkApi/Plugins/StructureStats/config.json";
    std::ifstream file(path);
    if (!file.is_open())
    {
        Log::GetLog()->error("[StructureStats] Cannot open config: {}", path);
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

        if (j.contains("DestructionBlacklist") && j["DestructionBlacklist"].is_array())
        {
            for (const auto& entry : j["DestructionBlacklist"])
            {
                if (entry.is_string())
                    g_destruction_blacklist.insert(entry.get<std::string>());
            }
        }
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[StructureStats] Config parse error: {}", ex.what());
        return false;
    }

    if (g_db_user.empty() || g_db_name.empty())
    {
        Log::GetLog()->error("[StructureStats] Config requires DbUser and DbName");
        return false;
    }

    Log::GetLog()->info("[StructureStats] Blacklist loaded with {} entries",
        g_destruction_blacklist.size());
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
        Log::GetLog()->error("[StructureStats] Query error: {}", pmysql_error(g_mysql));
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
        Log::GetLog()->error("[StructureStats] mysql_init failed");
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
        Log::GetLog()->error("[StructureStats] DB connect failed: {}", pmysql_error(g_mysql));
        pmysql_close(g_mysql);
        g_mysql = nullptr;
        return false;
    }

    bool ok = true;

    ok &= ExecQuery(
        "CREATE TABLE IF NOT EXISTS building_stats_player ("
        "  eos_id               VARCHAR(64)      NOT NULL,"
        "  survivor_id          BIGINT UNSIGNED  NOT NULL,"
        "  survivor_name        VARCHAR(128)     NOT NULL DEFAULT '',"
        "  tribe_name           VARCHAR(128)     NULL,"
        "  tribe_id             INT              NULL,"
        "  structures_placed    BIGINT           NOT NULL DEFAULT 0,"
        "  PRIMARY KEY (eos_id, survivor_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4"
    );

    ok &= ExecQuery(
        "CREATE TABLE IF NOT EXISTS building_stats_tribe ("
        "  tribe_id             INT              NOT NULL,"
        "  tribe_name           VARCHAR(128)     NOT NULL DEFAULT '',"
        "  structures_placed    BIGINT           NOT NULL DEFAULT 0,"
        "  PRIMARY KEY (tribe_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4"
    );

    ok &= ExecQuery(
        "CREATE TABLE IF NOT EXISTS destruction_player ("
        "  eos_id               VARCHAR(64)      NOT NULL,"
        "  survivor_id          BIGINT UNSIGNED  NOT NULL,"
        "  survivor_name        VARCHAR(128)     NOT NULL DEFAULT '',"
        "  tribe_name           VARCHAR(128)     NULL,"
        "  tribe_id             INT              NULL,"
        "  structures_destroyed BIGINT           NOT NULL DEFAULT 0,"
        "  PRIMARY KEY (eos_id, survivor_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4"
    );

    ok &= ExecQuery(
        "CREATE TABLE IF NOT EXISTS destruction_tribe ("
        "  tribe_id             INT              NOT NULL,"
        "  tribe_name           VARCHAR(128)     NOT NULL DEFAULT '',"
        "  structures_destroyed BIGINT           NOT NULL DEFAULT 0,"
        "  PRIMARY KEY (tribe_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4"
    );

    if (!ok)
    {
        Log::GetLog()->error("[StructureStats] Failed to create one or more tables");
        pmysql_close(g_mysql);
        g_mysql = nullptr;
        return false;
    }

    Log::GetLog()->info("[StructureStats] Database ready");
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

// =============================================================================
// Player Cache
// =============================================================================

struct PlayerRecord
{
    uint64_t    survivorId = 0;
    std::string survivorName;
    std::string tribeName;
    int         tribeId = 0;
    bool        inTribe = false;
};

static std::unordered_map<std::string, PlayerRecord> g_cache;  // keyed by eos_id
static std::mutex                                     g_cache_mutex;

// =============================================================================
// Write Queues
// =============================================================================

struct PlayerDelta
{
    std::string survivorName;
    std::string tribeName;
    int         tribeId = 0;
    bool        inTribe = false;
    int64_t     placed = 0;
    int64_t     destroyed = 0;
};

struct TribeDelta
{
    std::string tribeName;
    int64_t     placed = 0;
    int64_t     destroyed = 0;
};

struct PlayerQueueKey
{
    std::string eosId;
    uint64_t    survivorId;
    bool operator==(const PlayerQueueKey& o) const
    {
        return eosId == o.eosId && survivorId == o.survivorId;
    }
};

struct PlayerQueueKeyHash
{
    std::size_t operator()(const PlayerQueueKey& k) const
    {
        std::size_t h = std::hash<std::string>{}(k.eosId);
        h ^= std::hash<uint64_t>{}(k.survivorId) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

static std::unordered_map<PlayerQueueKey, PlayerDelta, PlayerQueueKeyHash> g_player_queue;
static std::unordered_map<int, TribeDelta>                                  g_tribe_queue;
static std::mutex                                                            g_queue_mutex;

enum class StatType { Placed, Destroyed };

static void QueuePlayerStat(const std::string& eosId, uint64_t survivorId,
    const std::string& survivorName,
    const std::string& tribeName, int tribeId, bool inTribe,
    StatType stat)
{
    if (eosId.empty() || survivorId == 0) return;
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    PlayerDelta& d = g_player_queue[{eosId, survivorId}];
    d.survivorName = survivorName;
    d.tribeName = tribeName;
    d.tribeId = tribeId;
    d.inTribe = inTribe;
    switch (stat)
    {
    case StatType::Placed:    ++d.placed;    break;
    case StatType::Destroyed: ++d.destroyed; break;
    }
}

static void QueueTribeStat(int tribeId, const std::string& tribeName, StatType stat)
{
    if (tribeId == 0) return;
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    TribeDelta& d = g_tribe_queue[tribeId];
    d.tribeName = tribeName;
    switch (stat)
    {
    case StatType::Placed:    ++d.placed;    break;
    case StatType::Destroyed: ++d.destroyed; break;
    }
}

// =============================================================================
// Flush
// =============================================================================

static void FlushQueue()
{
    std::unordered_map<PlayerQueueKey, PlayerDelta, PlayerQueueKeyHash> pLocal;
    std::unordered_map<int, TribeDelta> tLocal;

    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        pLocal.swap(g_player_queue);
        tLocal.swap(g_tribe_queue);
    }

    if (pLocal.empty() && tLocal.empty()) return;

    std::lock_guard<std::mutex> dbLock(g_db_mutex);
    if (!g_mysql) return;

    for (const auto& [key, d] : pLocal)
    {
        const std::string safeEos = EscapeUnsafe(key.eosId);
        const std::string safeName = EscapeUnsafe(d.survivorName);
        const std::string safeTN = d.inTribe ? ("'" + EscapeUnsafe(d.tribeName) + "'") : "NULL";
        const std::string safeTID = d.inTribe ? std::to_string(d.tribeId) : "NULL";

        if (d.placed > 0)
        {
            char sql[1024];
            std::snprintf(sql, sizeof(sql),
                "INSERT INTO building_stats_player "
                "(eos_id, survivor_id, survivor_name, tribe_name, tribe_id, structures_placed) "
                "VALUES ('%s', %llu, '%s', %s, %s, %lld) "
                "ON DUPLICATE KEY UPDATE "
                "  survivor_name     = VALUES(survivor_name),"
                "  tribe_name        = VALUES(tribe_name),"
                "  tribe_id          = VALUES(tribe_id),"
                "  structures_placed = structures_placed + VALUES(structures_placed)",
                safeEos.c_str(),
                static_cast<unsigned long long>(key.survivorId),
                safeName.c_str(),
                safeTN.c_str(), safeTID.c_str(),
                static_cast<long long>(d.placed));
            ExecQuery(sql);
        }

        if (d.destroyed > 0)
        {
            char sql[1024];
            std::snprintf(sql, sizeof(sql),
                "INSERT INTO destruction_player "
                "(eos_id, survivor_id, survivor_name, tribe_name, tribe_id, structures_destroyed) "
                "VALUES ('%s', %llu, '%s', %s, %s, %lld) "
                "ON DUPLICATE KEY UPDATE "
                "  survivor_name        = VALUES(survivor_name),"
                "  tribe_name           = VALUES(tribe_name),"
                "  tribe_id             = VALUES(tribe_id),"
                "  structures_destroyed = structures_destroyed + VALUES(structures_destroyed)",
                safeEos.c_str(),
                static_cast<unsigned long long>(key.survivorId),
                safeName.c_str(),
                safeTN.c_str(), safeTID.c_str(),
                static_cast<long long>(d.destroyed));
            ExecQuery(sql);
        }
    }

    for (const auto& [tribeId, d] : tLocal)
    {
        const std::string safeTN = EscapeUnsafe(d.tribeName);

        if (d.placed > 0)
        {
            char sql[512];
            std::snprintf(sql, sizeof(sql),
                "INSERT INTO building_stats_tribe "
                "(tribe_id, tribe_name, structures_placed) "
                "VALUES (%d, '%s', %lld) "
                "ON DUPLICATE KEY UPDATE "
                "  tribe_name        = VALUES(tribe_name),"
                "  structures_placed = structures_placed + VALUES(structures_placed)",
                tribeId, safeTN.c_str(),
                static_cast<long long>(d.placed));
            ExecQuery(sql);
        }

        if (d.destroyed > 0)
        {
            char sql[512];
            std::snprintf(sql, sizeof(sql),
                "INSERT INTO destruction_tribe (tribe_id, tribe_name, structures_destroyed) "
                "VALUES (%d, '%s', %lld) "
                "ON DUPLICATE KEY UPDATE "
                "  tribe_name           = VALUES(tribe_name),"
                "  structures_destroyed = structures_destroyed + VALUES(structures_destroyed)",
                tribeId, safeTN.c_str(),
                static_cast<long long>(d.destroyed));
            ExecQuery(sql);
        }
    }
}

// Updates tribe_name in both tribe tables immediately when a tribe is renamed.
static void UpdateTribeNameInDB(int tribeId, const std::string& newName)
{
    if (tribeId == 0 || newName.empty()) return;
    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!g_mysql) return;

    const std::string safeName = EscapeUnsafe(newName);
    char sql[512];

    std::snprintf(sql, sizeof(sql),
        "UPDATE building_stats_tribe SET tribe_name = '%s' WHERE tribe_id = %d",
        safeName.c_str(), tribeId);
    ExecQuery(sql);

    std::snprintf(sql, sizeof(sql),
        "UPDATE destruction_tribe SET tribe_name = '%s' WHERE tribe_id = %d",
        safeName.c_str(), tribeId);
    ExecQuery(sql);
}

// =============================================================================
// Flush Thread
// =============================================================================

static std::thread       g_flush_thread;
static std::atomic<bool> g_flush_running{ false };

static void FlushThreadFunc()
{
    while (g_flush_running.load())
    {
        for (int i = 0; i < 5 && g_flush_running.load(); ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));

        if (!g_flush_running.load()) break;

        FlushQueue();
    }

    FlushQueue();
}

// =============================================================================
// Helpers
// =============================================================================

static std::string FStr(const FString& f)
{
    const char* s = TCHAR_TO_UTF8(*f);
    return (s && s[0]) ? s : "";
}

static void SeedCacheFromPC(APlayerController* pc)
{
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

    const uint64_t survivorId = ch->GetLinkedPlayerDataID();
    if (survivorId == 0) return;

    FString nameRaw = ch->PlayerNameField();
    const std::string survivorName = FStr(nameRaw);

    const bool inTribe = ps->IsInTribe();
    const int  tribeId = inTribe ? ps->GetTribeId() : 0;
    FString tribeNameRaw = ch->TribeNameField();
    const std::string tribeName = inTribe ? FStr(tribeNameRaw) : "";

    std::lock_guard<std::mutex> lock(g_cache_mutex);
    PlayerRecord& rec = g_cache[eosId];
    rec.survivorId = survivorId;
    rec.survivorName = survivorName.empty() ? rec.survivorName : survivorName;
    rec.inTribe = inTribe;
    rec.tribeId = tribeId;
    rec.tribeName = inTribe ? tribeName : "";
}

// Resolves player identity from a controller. Returns false if not a valid player.
static bool ResolvePlayer(AController* controller,
    std::string& outEosId, uint64_t& outSurvivorId,
    std::string& outSurvivorName,
    std::string& outTribeName, int& outTribeId, bool& outInTribe)
{
    if (!controller) return false;
    if (!controller->IsA(AShooterPlayerController::StaticClass())) return false;

    AShooterPlayerController* pc = static_cast<AShooterPlayerController*>(controller);
    AShooterPlayerState* ps =
        static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
    if (!ps) return false;

    FString eosRaw;
    ps->GetUniqueNetIdAsString(&eosRaw);
    outEosId = FStr(eosRaw);
    if (outEosId.empty() || outEosId == "unknown") return false;

    std::lock_guard<std::mutex> lock(g_cache_mutex);
    auto it = g_cache.find(outEosId);
    if (it == g_cache.end() || it->second.survivorId == 0) return false;

    const PlayerRecord& rec = it->second;
    outSurvivorId = rec.survivorId;
    outSurvivorName = rec.survivorName;
    outTribeName = rec.tribeName;
    outTribeId = rec.tribeId;
    outInTribe = rec.inTribe;
    return true;
}

// =============================================================================
// Hook Type Aliases
// =============================================================================

using PostLogin_t = void(*)(AShooterGameMode*, APlayerController*);
using StartNewShooterPlayer_t = void(*)(AShooterGameMode*, APlayerController*, bool, bool,
    FPrimalPlayerCharacterConfigStruct&, UPrimalPlayerData*, bool);
using HandleRespawned_t = void(*)(AShooterPlayerController*, APawn*, bool);
using NotifyJoined_t = void(*)(AShooterPlayerState*, FString&, FString&, bool);
using NotifyLeft_t = void(*)(AShooterPlayerState*, FString&, FString&, bool);
using NetUpdateTribeName_t = void(*)(APrimalCharacter*, FString&);
using Logout_t = void(*)(AShooterGameMode*, AController*);
using PlacedStructure_t = void(*)(APrimalStructure*, AShooterPlayerController*);
using StructureDie_t = void(*)(APrimalStructure*, float, FDamageEvent&, AController*, AActor*);

static PostLogin_t             Original_PostLogin = nullptr;
static StartNewShooterPlayer_t Original_StartNewShooterPlayer = nullptr;
static HandleRespawned_t       Original_HandleRespawned = nullptr;
static NotifyJoined_t          Original_NotifyJoined = nullptr;
static NotifyLeft_t            Original_NotifyLeft = nullptr;
static NetUpdateTribeName_t    Original_NetUpdateTribeName = nullptr;
static Logout_t                Original_Logout = nullptr;
static PlacedStructure_t       Original_PlacedStructure = nullptr;
static StructureDie_t          Original_StructureDie = nullptr;

// =============================================================================
// Detours — Cache Management
// =============================================================================

// Temporary: collects unique blueprint paths from destroyed structures.
// Remove DumpStructurePath call and these globals before release.
static std::unordered_set<std::string> g_seen_bps;
static std::mutex                      g_seen_bps_mutex;

static void DumpStructurePath(APrimalStructure* structure)
{
    const std::string bp = FStr(AsaApi::GetApiUtils().GetBlueprint(structure));
    if (bp.empty()) return;
    {
        std::lock_guard<std::mutex> lock(g_seen_bps_mutex);
        if (!g_seen_bps.insert(bp).second) return;
    }
    const std::string path = "ArkApi/Plugins/StructureStats/structure_paths.txt";
    std::ofstream out(path, std::ios::app);
    if (out.is_open())
        out << bp << "\n";
}

void Detour_PostLogin(AShooterGameMode* gm, APlayerController* pc)
{
    Original_PostLogin(gm, pc);
    SeedCacheFromPC(pc);
}

void Detour_StartNewShooterPlayer(AShooterGameMode* gm,
    APlayerController* pc,
    bool b1, bool b2,
    FPrimalPlayerCharacterConfigStruct& cfg,
    UPrimalPlayerData* playerData,
    bool b3)
{
    Original_StartNewShooterPlayer(gm, pc, b1, b2, cfg, playerData, b3);
    SeedCacheFromPC(pc);
}

void Detour_HandleRespawned(AShooterPlayerController* pc, APawn* pawn, bool newPlayer)
{
    Original_HandleRespawned(pc, pawn, newPlayer);
    SeedCacheFromPC(pc);
}

void Detour_NotifyJoinedTribe(AShooterPlayerState* ps,
    FString& playerName, FString& tribeName, bool joinee)
{
    Original_NotifyJoined(ps, playerName, tribeName, joinee);

    if (!ps) return;

    FString eosRaw;
    ps->GetUniqueNetIdAsString(&eosRaw);
    const std::string eosId = FStr(eosRaw);
    if (eosId.empty() || eosId == "unknown") return;

    const std::string tn = FStr(tribeName);
    const int tid = ps->GetTribeId();

    std::lock_guard<std::mutex> lock(g_cache_mutex);
    auto it = g_cache.find(eosId);
    if (it != g_cache.end())
    {
        it->second.tribeName = tn;
        it->second.tribeId = tid;
        it->second.inTribe = true;
    }
}

void Detour_NotifyLeftTribe(AShooterPlayerState* ps,
    FString& playerName, FString& tribeName, bool joinee)
{
    Original_NotifyLeft(ps, playerName, tribeName, joinee);

    if (!ps) return;

    FString eosRaw;
    ps->GetUniqueNetIdAsString(&eosRaw);
    const std::string eosId = FStr(eosRaw);
    if (eosId.empty() || eosId == "unknown") return;

    std::lock_guard<std::mutex> lock(g_cache_mutex);
    auto it = g_cache.find(eosId);
    if (it != g_cache.end())
    {
        it->second.tribeName = "";
        it->second.tribeId = 0;
        it->second.inTribe = false;
    }
}

void Detour_NetUpdateTribeName(APrimalCharacter* character, FString& newNameFStr)
{
    Original_NetUpdateTribeName(character, newNameFStr);

    if (!character) return;

    AController* controller = character->GetOwnerController();
    if (!controller) return;

    AShooterPlayerController* pc = static_cast<AShooterPlayerController*>(controller);
    AShooterPlayerState* ps =
        static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
    if (!ps) return;

    FString eosRaw;
    ps->GetUniqueNetIdAsString(&eosRaw);
    const std::string eosId = FStr(eosRaw);
    if (eosId.empty() || eosId == "unknown") return;

    const std::string newName = FStr(newNameFStr);
    if (newName.empty()) return;

    int tribeId = 0;
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        auto it = g_cache.find(eosId);
        if (it == g_cache.end()) return;

        const std::string oldName = it->second.tribeName;
        if (oldName == newName) return;

        it->second.tribeName = newName;
        it->second.inTribe = true;
        tribeId = it->second.tribeId;

        if (!oldName.empty())
        {
            Log::GetLog()->info("[StructureStats] TRIBE_RENAMED tribe_id={} old={} new={}",
                tribeId, oldName, newName);
        }
    }

    // Immediate rename update in both tribe tables.
    UpdateTribeNameInDB(tribeId, newName);
}

void Detour_Logout(AShooterGameMode* gm, AController* controller)
{
    if (controller)
    {
        AShooterPlayerState* ps =
            static_cast<AShooterPlayerState*>(controller->PlayerStateField().Get());
        if (ps)
        {
            FString eosRaw;
            ps->GetUniqueNetIdAsString(&eosRaw);
            const std::string eosId = FStr(eosRaw);
            if (!eosId.empty() && eosId != "unknown")
            {
                std::lock_guard<std::mutex> lock(g_cache_mutex);
                g_cache.erase(eosId);
            }
        }
    }

    Original_Logout(gm, controller);
}

// =============================================================================
// Detours — Structure Events
// =============================================================================

void Detour_PlacedStructure(APrimalStructure* structure, AShooterPlayerController* pc)
{
    Original_PlacedStructure(structure, pc);

    if (!structure || !pc) return;

    std::string eosId, survivorName, tribeName;
    uint64_t survivorId = 0;
    int tribeId = 0;
    bool inTribe = false;

    if (!ResolvePlayer(pc, eosId, survivorId, survivorName, tribeName, tribeId, inTribe))
        return;

    QueuePlayerStat(eosId, survivorId, survivorName, tribeName, tribeId, inTribe,
        StatType::Placed);

    if (inTribe)
        QueueTribeStat(tribeId, tribeName, StatType::Placed);
}

void Detour_StructureDie(APrimalStructure* structure,
    float             damage,
    FDamageEvent& damageEvent,
    AController* killer,
    AActor* damageCauser)
{
    if (structure) DumpStructurePath(structure);

    Original_StructureDie(structure, damage, damageEvent, killer, damageCauser);

    if (!structure) return;

    if (!killer) return;

    // If killer belongs to the same tribe/team as the structure, it is demolish or self-removal.
    const int structureTeam = structure->TargetingTeamField();
    const bool killerIsPC = killer->IsA(AShooterPlayerController::StaticClass());

    if (structureTeam != 0 && killerIsPC)
    {
        AShooterPlayerController* killerPC = static_cast<AShooterPlayerController*>(killer);
        AShooterPlayerState* ps = static_cast<AShooterPlayerState*>(killerPC->PlayerStateField().Get());
        if (ps)
        {
            const int killerTeam = ps->GetTribeId();
            if (killerTeam != 0 && killerTeam == structureTeam)
                return;
        }
    }

    // Blacklist check by blueprint path.
    if (!g_destruction_blacklist.empty())
    {
        const std::string bp = FStr(AsaApi::GetApiUtils().GetBlueprint(structure));
        if (!bp.empty() && g_destruction_blacklist.count(bp))
            return;
    }

    std::string eosId, survivorName, tribeName;
    uint64_t survivorId = 0;
    int tribeId = 0;
    bool inTribe = false;

    if (!ResolvePlayer(killer, eosId, survivorId, survivorName, tribeName, tribeId, inTribe))
        return;

    QueuePlayerStat(eosId, survivorId, survivorName, tribeName, tribeId, inTribe,
        StatType::Destroyed);

    if (inTribe)
        QueueTribeStat(tribeId, tribeName, StatType::Destroyed);
}

// =============================================================================
// Plugin Entry Points
// =============================================================================

extern "C" __declspec(dllexport) void Plugin_Init()
{
    Log::Get().Init("StructureStats");

    if (!LoadConfig())
    {
        Log::GetLog()->error("[StructureStats] Halted - config error");
        return;
    }

    if (!InitDatabase())
    {
        Log::GetLog()->error("[StructureStats] Halted - database error");
        return;
    }

    AsaApi::GetHooks().SetHook(
        "AShooterGameMode.PostLogin(APlayerController*)",
        (LPVOID)&Detour_PostLogin,
        (LPVOID*)&Original_PostLogin
    );

    AsaApi::GetHooks().SetHook(
        "AShooterGameMode.StartNewShooterPlayer(APlayerController*,bool,bool,FPrimalPlayerCharacterConfigStruct&,UPrimalPlayerData*,bool)",
        (LPVOID)&Detour_StartNewShooterPlayer,
        (LPVOID*)&Original_StartNewShooterPlayer
    );

    AsaApi::GetHooks().SetHook(
        "AShooterPlayerController.HandleRespawned_Implementation(APawn*,bool)",
        (LPVOID)&Detour_HandleRespawned,
        (LPVOID*)&Original_HandleRespawned
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
        "APrimalCharacter.NetUpdateTribeName_Implementation(FString&)",
        (LPVOID)&Detour_NetUpdateTribeName,
        (LPVOID*)&Original_NetUpdateTribeName
    );

    AsaApi::GetHooks().SetHook(
        "AShooterGameMode.Logout(AController*)",
        (LPVOID)&Detour_Logout,
        (LPVOID*)&Original_Logout
    );

    AsaApi::GetHooks().SetHook(
        "APrimalStructure.PlacedStructure(AShooterPlayerController*)",
        (LPVOID)&Detour_PlacedStructure,
        (LPVOID*)&Original_PlacedStructure
    );

    AsaApi::GetHooks().SetHook(
        "APrimalStructure.Die(float,FDamageEvent&,AController*,AActor*)",
        (LPVOID)&Detour_StructureDie,
        (LPVOID*)&Original_StructureDie
    );

    g_flush_running.store(true);
    g_flush_thread = std::thread(FlushThreadFunc);

    Log::GetLog()->info("[StructureStats] Plugin loaded");
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    g_flush_running.store(false);
    if (g_flush_thread.joinable())
        g_flush_thread.join();

    AsaApi::GetHooks().DisableHook(
        "AShooterGameMode.PostLogin(APlayerController*)",
        (LPVOID)&Detour_PostLogin
    );

    AsaApi::GetHooks().DisableHook(
        "AShooterGameMode.StartNewShooterPlayer(APlayerController*,bool,bool,FPrimalPlayerCharacterConfigStruct&,UPrimalPlayerData*,bool)",
        (LPVOID)&Detour_StartNewShooterPlayer
    );

    AsaApi::GetHooks().DisableHook(
        "AShooterPlayerController.HandleRespawned_Implementation(APawn*,bool)",
        (LPVOID)&Detour_HandleRespawned
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
        "APrimalCharacter.NetUpdateTribeName_Implementation(FString&)",
        (LPVOID)&Detour_NetUpdateTribeName
    );

    AsaApi::GetHooks().DisableHook(
        "AShooterGameMode.Logout(AController*)",
        (LPVOID)&Detour_Logout
    );

    AsaApi::GetHooks().DisableHook(
        "APrimalStructure.PlacedStructure(AShooterPlayerController*)",
        (LPVOID)&Detour_PlacedStructure
    );

    AsaApi::GetHooks().DisableHook(
        "APrimalStructure.Die(float,FDamageEvent&,AController*,AActor*)",
        (LPVOID)&Detour_StructureDie
    );

    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        g_cache.clear();
    }

    CloseDatabase();

    Log::GetLog()->info("[StructureStats] Plugin unloaded");
}