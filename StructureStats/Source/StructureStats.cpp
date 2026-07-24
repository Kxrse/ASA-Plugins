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
 * Tables:
 *   structurestats_player  PK (eos_id, survivor_id, map_name)  authoritative per-player counters
 *     Columns: eos_id, survivor_name, survivor_id, tribe_name, tribe_id, map_name,
 *              structures_placed, structures_destroyed
 *
 *   structurestats_tribe   PK (tribe_id, map_name)  derived rollup, rebuilt from player rows
 *     Columns: tribe_name, tribe_id, map_name, structures_placed, structures_destroyed
 *
 * Hooks:
 *   AShooterGameMode.PostLogin                               seed identity cache on connect
 *   AShooterGameMode.StartNewShooterPlayer                   seed survivor_id on new character
 *   AShooterPlayerController.HandleRespawned_Implementation  refresh cache on spawn
 *   AShooterGameMode.Logout                                  clear cache on disconnect
 *   AShooterPlayerState.NotifyPlayerJoinedTribe              restamp player rows to joined tribe
 *   AShooterPlayerState.NotifyPlayerLeftTribe                restamp player rows to solo (tribe 0)
 *   APrimalStructure.PlacedStructure                         increment placed
 *   APrimalStructure.Die                                     increment destroyed (own-team and blacklist filtered)
 *
 * Config:
 *   ArkApi/Plugins/StructureStats/config.json
 *   DestructionBlacklist: array of blueprint path strings excluded from destruction scoring
 *
 * Write strategy:
 *   Player placed/destroyed deltas accumulated in-memory, flushed every 5 seconds.
 *   Join stamps the player's rows with GetTribeId (fresh on join). Leave stamps tribe_id 0
 *   unconditionally, since the player state tribe fields are stale at leave time.
 *   The tribe table is a pure rollup: on each flush with changes it is rebuilt for the
 *   current map by summing player rows per tribe_id (tribe_id 0 excluded). A player's
 *   score therefore follows them between tribes and drops out entirely when solo. Per map.
 *   Config rescanned every 10 seconds (size + last-write-time), blacklist reloaded live.
 *   Each flush pings the DB and reconnects if the link dropped. If reconnect fails the
 *   drained deltas are merged back into the queue and retained until the DB returns.
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
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
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
    pmysql_ping = (mysql_ping_t)GetProcAddress(g_mysql_module, "mysql_ping");

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
// Helpers (forward)
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

// =============================================================================
// Configuration
// =============================================================================

static const std::string        g_config_path = "ArkApi/Plugins/StructureStats/config.json";
static std::mutex               g_config_mutex;
static std::string             g_db_host = "127.0.0.1";
static unsigned int            g_db_port = 3306;
static std::string             g_db_user;
static std::string             g_db_pass;
static std::string             g_db_name;
static std::unordered_set<std::string> g_destruction_blacklist;
static std::uintmax_t                   g_cfg_size = 0;
static std::filesystem::file_time_type  g_cfg_mtime{};

static bool LoadConfig()
{
    std::ifstream file(g_config_path);
    if (!file.is_open())
    {
        Log::GetLog()->error("[StructureStats] Cannot open config: {}", g_config_path);
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

        g_destruction_blacklist.clear();
        if (j.contains("DestructionBlacklist") && j["DestructionBlacklist"].is_array())
        {
            for (const auto& entry : j["DestructionBlacklist"])
                if (entry.is_string())
                    g_destruction_blacklist.insert(entry.get<std::string>());
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

    std::unordered_set<std::string> newBlacklist;
    try
    {
        nlohmann::json j;
        file >> j;
        if (j.contains("DestructionBlacklist") && j["DestructionBlacklist"].is_array())
        {
            for (const auto& entry : j["DestructionBlacklist"])
                if (entry.is_string())
                    newBlacklist.insert(entry.get<std::string>());
        }
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[StructureStats] Config reload parse error: {}", ex.what());
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        g_destruction_blacklist.swap(newBlacklist);
    }

    g_cfg_size = size;
    g_cfg_mtime = mtime;

    Log::GetLog()->info("[StructureStats] Config reloaded, blacklist entries: {}",
        g_destruction_blacklist.size());
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
        Log::GetLog()->error("[StructureStats] Query error: {}", pmysql_error(g_mysql));
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
        if (!g_db_logged_down)
            Log::GetLog()->error("[StructureStats] DB connect failed: {}", pmysql_error(g_mysql));
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
                Log::GetLog()->info("[StructureStats] DB connection healthy");
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
            Log::GetLog()->info("[StructureStats] DB reconnected");
            g_db_logged_down = false;
        }
        return true;
    }

    if (!g_db_logged_down)
    {
        Log::GetLog()->error("[StructureStats] DB connection lost, retaining deltas until reconnect");
        g_db_logged_down = true;
    }
    return false;
}

static bool InitDatabase()
{
    if (!LoadMySQLLib()) return false;
    if (!EstablishConnection()) return false;

    bool ok = true;

    ok &= ExecQuery(
        "CREATE TABLE IF NOT EXISTS structurestats_player ("
        "  eos_id               VARCHAR(64)      NOT NULL,"
        "  survivor_name        VARCHAR(128)     NOT NULL DEFAULT '',"
        "  survivor_id          BIGINT UNSIGNED  NOT NULL,"
        "  tribe_name           VARCHAR(128)     NOT NULL DEFAULT '',"
        "  tribe_id             INT              NOT NULL DEFAULT 0,"
        "  map_name             VARCHAR(64)      NOT NULL DEFAULT '',"
        "  structures_placed    BIGINT UNSIGNED  NOT NULL DEFAULT 0,"
        "  structures_destroyed BIGINT UNSIGNED  NOT NULL DEFAULT 0,"
        "  PRIMARY KEY (eos_id, survivor_id, map_name)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4"
    );

    ok &= ExecQuery(
        "CREATE TABLE IF NOT EXISTS structurestats_tribe ("
        "  tribe_name           VARCHAR(128)     NOT NULL DEFAULT '',"
        "  tribe_id             INT              NOT NULL,"
        "  map_name             VARCHAR(64)      NOT NULL DEFAULT '',"
        "  structures_placed    BIGINT UNSIGNED  NOT NULL DEFAULT 0,"
        "  structures_destroyed BIGINT UNSIGNED  NOT NULL DEFAULT 0,"
        "  PRIMARY KEY (tribe_id, map_name)"
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

static std::unordered_map<std::string, PlayerRecord> g_cache;
static std::mutex                                     g_cache_mutex;

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

    const uint64_t survivorId = ch->LinkedPlayerDataIDField();
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

static bool ResolvePlayer(AController* controller,
    std::string& outEosId, uint64_t& outSurvivorId,
    std::string& outSurvivorName,
    std::string& outTribeName, int& outTribeId, bool& outInTribe)
{
    if (!controller) return false;
    if (!controller->IsA(AShooterPlayerController::GetPrivateStaticClass())) return false;

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
// Write Queues
// =============================================================================

struct PlayerDelta
{
    std::string survivorName;
    std::string tribeName;
    int         tribeId = 0;
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

struct MembershipChange
{
    std::string eosId;
    std::string tribeName;
    int         tribeId = 0;
};

static std::unordered_map<PlayerQueueKey, PlayerDelta, PlayerQueueKeyHash> g_player_queue;
static std::vector<MembershipChange>                                       g_membership_queue;
static std::mutex                                                          g_queue_mutex;

enum class StatType { Placed, Destroyed };

static void QueuePlayerStat(const std::string& eosId, uint64_t survivorId,
    const std::string& survivorName,
    const std::string& tribeName, int tribeId,
    StatType stat)
{
    if (eosId.empty() || survivorId == 0) return;
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    PlayerDelta& d = g_player_queue[{eosId, survivorId}];
    d.survivorName = survivorName;
    d.tribeName = tribeName;
    d.tribeId = tribeId;
    switch (stat)
    {
    case StatType::Placed:    ++d.placed;    break;
    case StatType::Destroyed: ++d.destroyed; break;
    }
}

static void QueueMembershipChange(const std::string& eosId, int tribeId, const std::string& tribeName)
{
    if (eosId.empty()) return;
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    g_membership_queue.push_back({ eosId, tribeName, tribeId });
}

static void RequeueOnFailure(
    std::unordered_map<PlayerQueueKey, PlayerDelta, PlayerQueueKeyHash>& pLocal,
    std::vector<MembershipChange>& mLocal)
{
    std::lock_guard<std::mutex> lock(g_queue_mutex);

    for (auto& [key, d] : pLocal)
    {
        auto it = g_player_queue.find(key);
        if (it == g_player_queue.end())
        {
            g_player_queue[key] = d;
        }
        else
        {
            it->second.placed += d.placed;
            it->second.destroyed += d.destroyed;
        }
    }

    g_membership_queue.insert(g_membership_queue.begin(), mLocal.begin(), mLocal.end());
}

// =============================================================================
// Flush
// =============================================================================

static void RebuildTribeRollup(const std::string& safeMap)
{
    ExecQuery("START TRANSACTION");

    {
        char sql[256];
        std::snprintf(sql, sizeof(sql),
            "DELETE FROM structurestats_tribe WHERE map_name = '%s'",
            safeMap.c_str());
        ExecQuery(sql);
    }

    {
        char sql[700];
        std::snprintf(sql, sizeof(sql),
            "INSERT INTO structurestats_tribe "
            "(tribe_name, tribe_id, map_name, structures_placed, structures_destroyed) "
            "SELECT MAX(tribe_name), tribe_id, map_name, "
            "SUM(structures_placed), SUM(structures_destroyed) "
            "FROM structurestats_player "
            "WHERE map_name = '%s' AND tribe_id <> 0 "
            "GROUP BY tribe_id, map_name",
            safeMap.c_str());
        ExecQuery(sql);
    }

    ExecQuery("COMMIT");
}

static void FlushQueue()
{
    std::unordered_map<PlayerQueueKey, PlayerDelta, PlayerQueueKeyHash> pLocal;
    std::vector<MembershipChange> mLocal;

    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        pLocal.swap(g_player_queue);
        mLocal.swap(g_membership_queue);
    }

    if (pLocal.empty() && mLocal.empty()) return;

    std::lock_guard<std::mutex> dbLock(g_db_mutex);

    if (!EnsureConnected())
    {
        RequeueOnFailure(pLocal, mLocal);
        return;
    }

    const std::string mapName = GetMapName();
    const std::string safeMap = EscapeUnsafe(mapName);

    std::vector<std::string> playerStatements;

    for (const auto& [key, d] : pLocal)
    {
        if (d.placed == 0 && d.destroyed == 0) continue;

        const std::string safeEos = EscapeUnsafe(key.eosId);
        const std::string safeName = EscapeUnsafe(d.survivorName);
        const std::string safeTN = EscapeUnsafe(d.tribeName);

        char sql[2048];
        std::snprintf(sql, sizeof(sql),
            "INSERT INTO structurestats_player "
            "(eos_id, survivor_name, survivor_id, tribe_name, tribe_id, map_name, "
            "structures_placed, structures_destroyed) "
            "VALUES ('%s', '%s', %llu, '%s', %d, '%s', %lld, %lld) "
            "ON DUPLICATE KEY UPDATE "
            "  survivor_name        = VALUES(survivor_name),"
            "  tribe_name           = VALUES(tribe_name),"
            "  tribe_id             = VALUES(tribe_id),"
            "  structures_placed    = structures_placed + VALUES(structures_placed),"
            "  structures_destroyed = structures_destroyed + VALUES(structures_destroyed)",
            safeEos.c_str(),
            safeName.c_str(),
            static_cast<unsigned long long>(key.survivorId),
            safeTN.c_str(),
            d.tribeId,
            safeMap.c_str(),
            static_cast<long long>(d.placed),
            static_cast<long long>(d.destroyed));
        playerStatements.emplace_back(sql);
    }

    std::vector<std::string> membershipStatements;

    for (const auto& m : mLocal)
    {
        const std::string safeEos = EscapeUnsafe(m.eosId);
        const std::string safeTN = EscapeUnsafe(m.tribeName);

        char sql[1024];
        std::snprintf(sql, sizeof(sql),
            "UPDATE structurestats_player "
            "SET tribe_id = %d, tribe_name = '%s' "
            "WHERE eos_id = '%s' AND map_name = '%s'",
            m.tribeId, safeTN.c_str(), safeEos.c_str(), safeMap.c_str());
        membershipStatements.emplace_back(sql);
    }

    if (!g_mysql) return;

    for (const auto& s : playerStatements)
        ExecQuery(s);

    for (const auto& s : membershipStatements)
        ExecQuery(s);

    RebuildTribeRollup(safeMap);
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
        if (tick % 5 == 0)  FlushQueue();
        if (tick % 10 == 0) ReloadConfigIfChanged();
    }

    FlushQueue();
}

// =============================================================================
// Destruction Deduplication
// =============================================================================

static std::unordered_map<void*, std::chrono::steady_clock::time_point> g_die_seen;
static std::mutex                                                        g_die_seen_mutex;
static constexpr int DEDUP_WINDOW_MS = 500;

static bool IsDuplicateDie(void* ptr)
{
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_die_seen_mutex);

    for (auto it = g_die_seen.begin(); it != g_die_seen.end(); )
    {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count() > DEDUP_WINDOW_MS)
            it = g_die_seen.erase(it);
        else
            ++it;
    }

    if (g_die_seen.count(ptr)) return true;
    g_die_seen[ptr] = now;
    return false;
}

// =============================================================================
// Hook Type Aliases
// =============================================================================

using PostLogin_t = void(*)(AShooterGameMode*, APlayerController*);
using StartNewShooterPlayer_t = void(*)(AShooterGameMode*, APlayerController*, bool, bool,
    FPrimalPlayerCharacterConfigStruct&, UPrimalPlayerData*, bool);
using HandleRespawned_t = void(*)(AShooterPlayerController*, APawn*, bool);
using Logout_t = void(*)(AShooterGameMode*, AController*);
using NotifyJoined_t = void(*)(AShooterPlayerState*, const FString&, const FString&, bool);
using NotifyLeft_t = void(*)(AShooterPlayerState*, const FString&, const FString&, bool);
using PlacedStructure_t = void(*)(APrimalStructure*, AShooterPlayerController*);
using StructureDie_t = bool(*)(APrimalStructure*, float, FDamageEvent&, AController*, AActor*);

static PostLogin_t             Original_PostLogin = nullptr;
static StartNewShooterPlayer_t Original_StartNewShooterPlayer = nullptr;
static HandleRespawned_t       Original_HandleRespawned = nullptr;
static Logout_t                Original_Logout = nullptr;
static NotifyJoined_t          Original_NotifyJoined = nullptr;
static NotifyLeft_t            Original_NotifyLeft = nullptr;
static PlacedStructure_t       Original_PlacedStructure = nullptr;
static StructureDie_t          Original_StructureDie = nullptr;

// =============================================================================
// Detours - Cache Management
// =============================================================================

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

static void ApplyTribe(AShooterPlayerState* ps, bool inTribe, int tribeId, const std::string& tribeName)
{
    if (!ps) return;

    FString eosRaw;
    ps->GetUniqueNetIdAsString(&eosRaw);
    const std::string eosId = FStr(eosRaw);
    if (eosId.empty() || eosId == "unknown") return;

    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        auto it = g_cache.find(eosId);
        if (it != g_cache.end())
        {
            it->second.inTribe = inTribe;
            it->second.tribeId = tribeId;
            it->second.tribeName = tribeName;
        }
    }

    QueueMembershipChange(eosId, tribeId, tribeName);
}

void Detour_NotifyJoinedTribe(AShooterPlayerState* ps,
    const FString& playerName, const FString& tribeName, bool joinee)
{
    Original_NotifyJoined(ps, playerName, tribeName, joinee);
    if (!ps) return;
    ApplyTribe(ps, true, ps->GetTribeId(), FStr(tribeName));
}

void Detour_NotifyLeftTribe(AShooterPlayerState* ps,
    const FString& playerName, const FString& tribeName, bool joinee)
{
    Original_NotifyLeft(ps, playerName, tribeName, joinee);
    ApplyTribe(ps, false, 0, "");
}

// =============================================================================
// Detours - Structure Events
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

    QueuePlayerStat(eosId, survivorId, survivorName, tribeName, tribeId, StatType::Placed);
}

bool Detour_StructureDie(APrimalStructure* structure,
    float             damage,
    FDamageEvent& damageEvent,
    AController* killer,
    AActor* damageCauser)
{
    int         structureTeam = 0;
    std::string bp;
    if (structure)
    {
        structureTeam = structure->TargetingTeamField();
        bp = FStr(AsaApi::GetApiUtils().GetBlueprint(structure));
    }

    const bool result = Original_StructureDie(structure, damage, damageEvent, killer, damageCauser);

    if (!structure) return result;
    if (IsDuplicateDie(structure)) return result;
    if (!killer) return result;
    if (!killer->IsA(AShooterPlayerController::GetPrivateStaticClass())) return result;

    AShooterPlayerController* killerPC = static_cast<AShooterPlayerController*>(killer);
    AShooterCharacter* killerCh = killerPC->BaseGetPlayerCharacter();
    const int killerTeam = killerCh ? killerCh->TargetingTeamField() : 0;

    if (structureTeam != 0 && killerTeam == structureTeam) return result;

    if (!bp.empty())
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        if (g_destruction_blacklist.count(bp)) return result;
    }

    std::string eosId, survivorName, tribeName;
    uint64_t survivorId = 0;
    int tribeId = 0;
    bool inTribe = false;

    if (!ResolvePlayer(killer, eosId, survivorId, survivorName, tribeName, tribeId, inTribe))
        return result;

    QueuePlayerStat(eosId, survivorId, survivorName, tribeName, tribeId, StatType::Destroyed);

    return result;
}

// =============================================================================
// Plugin Entry Points
// =============================================================================

static void SeedAllOnlinePlayers()
{
    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return;

    auto& controllers = world->PlayerControllerListField();
    for (int i = 0; i < controllers.Num(); ++i)
    {
        APlayerController* pc = controllers[i].Get();
        if (pc) SeedCacheFromPC(pc);
    }
}

static void InitImpl()
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
        "AShooterGameMode.Logout(AController*)",
        (LPVOID)&Detour_Logout,
        (LPVOID*)&Original_Logout
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
        "APrimalStructure.PlacedStructure(AShooterPlayerController*)",
        (LPVOID)&Detour_PlacedStructure,
        (LPVOID*)&Original_PlacedStructure
    );

    AsaApi::GetHooks().SetHook(
        "APrimalStructure.Die(float,FDamageEvent&,AController*,AActor*)",
        (LPVOID)&Detour_StructureDie,
        (LPVOID*)&Original_StructureDie
    );

    g_worker_running.store(true);
    g_worker_thread = std::thread(WorkerLoop);

    SeedAllOnlinePlayers();

    Log::GetLog()->info("[StructureStats] Plugin loaded");
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
        "AShooterGameMode.StartNewShooterPlayer(APlayerController*,bool,bool,FPrimalPlayerCharacterConfigStruct&,UPrimalPlayerData*,bool)",
        (LPVOID)&Detour_StartNewShooterPlayer
    );

    AsaApi::GetHooks().DisableHook(
        "AShooterPlayerController.HandleRespawned_Implementation(APawn*,bool)",
        (LPVOID)&Detour_HandleRespawned
    );

    AsaApi::GetHooks().DisableHook(
        "AShooterGameMode.Logout(AController*)",
        (LPVOID)&Detour_Logout
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

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { InitImpl(); }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[StructureStats] Plugin_Init exception: {}", ex.what());
    }
    catch (...)
    {
        Log::GetLog()->error("[StructureStats] Plugin_Init unknown exception");
    }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { UnloadImpl(); }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[StructureStats] Plugin_Unload exception: {}", ex.what());
    }
    catch (...)
    {
        Log::GetLog()->error("[StructureStats] Plugin_Unload unknown exception");
    }
}