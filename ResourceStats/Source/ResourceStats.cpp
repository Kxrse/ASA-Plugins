/*
ResourceStats - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * ResourceStats - ASA Plugin
 *
 * Hook category: Inventory
 *
 * Table:
 *   gathering_stats — PK (eos_id, survivor_id, item_name)
 *   Columns: eos_id, survivor_id, item_name, total_harvested
 *
 * Hooks:
 *   AShooterGameMode.StartNewShooterPlayer(...)
 *     — seeds survivor_id cache on spawn/character creation
 *   AShooterPlayerController.HandleRespawned_Implementation(APawn*,bool)
 *     — refreshes survivor_id cache on respawn
 *   UPrimalHarvestingComponent.GiveHarvestResource(...)
 *     — snapshots inventory quantities before calling original,
 *       diffs after, queues deltas; bypasses null destInv and
 *       stack-increment blindspot of NotifyItemAdded
 *
 * Write strategy:
 *   Deltas accumulated in-memory per (eos_id, survivor_id, item_name).
 *   Background thread flushes every 5 seconds via INSERT ... ON DUPLICATE KEY UPDATE.
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
            Log::GetLog()->info("[ResourceStats] Loaded DB library: {}", candidates[i]);
            break;
        }
    }

    if (!g_mysql_module)
    {
        Log::GetLog()->error("[ResourceStats] Could not find libmariadb.dll or libmysql.dll");
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
        Log::GetLog()->error("[ResourceStats] Failed to resolve required DB functions");
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

static bool LoadConfig()
{
    const std::string path = "ArkApi/Plugins/ResourceStats/config.json";
    std::ifstream file(path);
    if (!file.is_open())
    {
        Log::GetLog()->error("[ResourceStats] Cannot open config: {}", path);
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
        Log::GetLog()->error("[ResourceStats] Config parse error: {}", ex.what());
        return false;
    }

    if (g_db_user.empty() || g_db_name.empty())
    {
        Log::GetLog()->error("[ResourceStats] Config requires DbUser and DbName");
        return false;
    }

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
        Log::GetLog()->error("[ResourceStats] Query error: {}", pmysql_error(g_mysql));
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
        Log::GetLog()->error("[ResourceStats] mysql_init failed");
        return false;
    }

    if (pmysql_options)
    {
        unsigned int timeout = 5;
        pmysql_options(g_mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    }

    if (!pmysql_real_connect(g_mysql,
        g_db_host.c_str(), g_db_user.c_str(), g_db_pass.c_str(),
        g_db_name.c_str(), g_db_port, nullptr, 0))
    {
        Log::GetLog()->error("[ResourceStats] DB connect failed: {}", pmysql_error(g_mysql));
        pmysql_close(g_mysql);
        g_mysql = nullptr;
        return false;
    }

    const bool ok = ExecQuery(
        "CREATE TABLE IF NOT EXISTS gathering_stats ("
        "  eos_id          VARCHAR(64)      NOT NULL,"
        "  survivor_id     BIGINT UNSIGNED  NOT NULL,"
        "  item_name       VARCHAR(128)     NOT NULL,"
        "  total_harvested BIGINT           NOT NULL DEFAULT 0,"
        "  PRIMARY KEY (eos_id, survivor_id, item_name)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4"
    );

    if (!ok)
    {
        Log::GetLog()->error("[ResourceStats] Failed to create gathering_stats table");
        pmysql_close(g_mysql);
        g_mysql = nullptr;
        return false;
    }

    Log::GetLog()->info("[ResourceStats] Database ready");
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
// Survivor ID Cache
// =============================================================================

struct PlayerRecord
{
    uint64_t survivorId = 0;
};

static std::unordered_map<std::string, PlayerRecord> g_cache;
static std::mutex                                     g_cache_mutex;

static uint64_t GetCachedSurvivorId(const std::string& eosId)
{
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    auto it = g_cache.find(eosId);
    return it != g_cache.end() ? it->second.survivorId : 0;
}

// =============================================================================
// Write Queue
// =============================================================================

struct QueueKey
{
    std::string eosId;
    uint64_t    survivorId;
    std::string itemName;

    bool operator==(const QueueKey& o) const
    {
        return eosId == o.eosId && survivorId == o.survivorId && itemName == o.itemName;
    }
};

struct QueueKeyHash
{
    std::size_t operator()(const QueueKey& k) const
    {
        std::size_t h = std::hash<std::string>{}(k.eosId);
        h ^= std::hash<uint64_t>{}(k.survivorId) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<std::string>{}(k.itemName) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

static std::unordered_map<QueueKey, int64_t, QueueKeyHash> g_write_queue;
static std::mutex                                           g_queue_mutex;

static void QueueDelta(const std::string& eosId, uint64_t survivorId,
    const std::string& itemName, int64_t qty)
{
    if (eosId.empty() || survivorId == 0 || itemName.empty() || qty <= 0) return;
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    g_write_queue[{eosId, survivorId, itemName}] += qty;
}

static void FlushQueue()
{
    std::unordered_map<QueueKey, int64_t, QueueKeyHash> local;

    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        local.swap(g_write_queue);
    }

    if (local.empty()) return;

    std::lock_guard<std::mutex> dbLock(g_db_mutex);
    if (!g_mysql) return;

    for (const auto& [key, delta] : local)
    {
        if (delta <= 0) continue;
        const std::string safeEos = EscapeUnsafe(key.eosId);
        const std::string safeItem = EscapeUnsafe(key.itemName);
        char sql[512];
        std::snprintf(sql, sizeof(sql),
            "INSERT INTO gathering_stats (eos_id, survivor_id, item_name, total_harvested) "
            "VALUES ('%s', %llu, '%s', %lld) "
            "ON DUPLICATE KEY UPDATE total_harvested = total_harvested + VALUES(total_harvested)",
            safeEos.c_str(),
            static_cast<unsigned long long>(key.survivorId),
            safeItem.c_str(),
            static_cast<long long>(delta));
        ExecQuery(sql);
    }
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

// Snapshots item name -> quantity for all items in an inventory component.
static std::unordered_map<std::string, int> SnapshotInventory(UPrimalInventoryComponent* inv)
{
    std::unordered_map<std::string, int> snap;
    if (!inv) return snap;

    TArray<UPrimalItem*>& items = inv->InventoryItemsField();
    for (int i = 0; i < items.Num(); ++i)
    {
        UPrimalItem* item = items[i];
        if (!item) continue;
        const std::string name = FStr(item->DescriptiveNameBaseField());
        if (name.empty()) continue;
        snap[name] += item->GetItemQuantity();
    }
    return snap;
}

// =============================================================================
// Hook Type Aliases
// =============================================================================

using StartNewShooterPlayer_t = void(*)(AShooterGameMode*, APlayerController*, bool, bool,
    FPrimalPlayerCharacterConfigStruct&, UPrimalPlayerData*, bool);
using HandleRespawned_t = void(*)(AShooterPlayerController*, APawn*, bool);
using GiveHarvestResource_t = void(*)(UPrimalHarvestingComponent*, UPrimalInventoryComponent*,
    float, TSubclassOf<UDamageType>, AActor*,
    TArray<FHarvestResourceEntry, TSizedDefaultAllocator<32>>*);

static StartNewShooterPlayer_t Original_StartNewShooterPlayer = nullptr;
static HandleRespawned_t       Original_HandleRespawned = nullptr;
static GiveHarvestResource_t   Original_GiveHarvestResource = nullptr;

// =============================================================================
// Detours
// =============================================================================

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

    const uint64_t survivorId = ch->GetLinkedPlayerDataID();
    if (survivorId == 0) return;

    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        g_cache[eosId].survivorId = survivorId;
    }

    Log::GetLog()->info("[ResourceStats] SURVIVOR_ID_SEEDED eos_id={} survivor_id={}",
        eosId, survivorId);
}

void Detour_HandleRespawned(AShooterPlayerController* pc, APawn* pawn, bool newPlayer)
{
    Original_HandleRespawned(pc, pawn, newPlayer);

    if (!pc) return;

    AShooterPlayerState* ps =
        static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
    if (!ps) return;

    FString eosRaw;
    ps->GetUniqueNetIdAsString(&eosRaw);
    const std::string eosId = FStr(eosRaw);
    if (eosId.empty() || eosId == "unknown") return;

    APawn* p = pc->PawnField().Get();
    if (!p) return;

    AShooterCharacter* ch = static_cast<AShooterCharacter*>(p);
    if (!ch) return;

    const uint64_t survivorId = ch->GetLinkedPlayerDataID();
    if (survivorId == 0) return;

    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        g_cache[eosId].survivorId = survivorId;
    }

    Log::GetLog()->info("[ResourceStats] SURVIVOR_ID_REFRESHED eos_id={} survivor_id={}",
        eosId, survivorId);
}

void Detour_GiveHarvestResource(UPrimalHarvestingComponent* comp,
    UPrimalInventoryComponent* destInv,
    float                                                     harvestMultiplier,
    TSubclassOf<UDamageType>                                  damageType,
    AActor* harvester,
    TArray<FHarvestResourceEntry, TSizedDefaultAllocator<32>>* overrideResources)
{
    // harvester is the AActor performing the harvest (the player character).
    AShooterCharacter* ch = harvester ? static_cast<AShooterCharacter*>(harvester) : nullptr;

    UPrimalInventoryComponent* inv = nullptr;
    std::string  eosId;
    uint64_t     survivorId = 0;

    if (ch)
    {
        inv = ch->MyInventoryComponentField();

        AShooterPlayerController* pc =
            static_cast<AShooterPlayerController*>(ch->GetOwnerController());
        if (pc)
        {
            AShooterPlayerState* ps =
                static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
            if (ps)
            {
                FString eosRaw;
                ps->GetUniqueNetIdAsString(&eosRaw);
                const std::string raw = FStr(eosRaw);
                if (!raw.empty() && raw != "unknown")
                    eosId = raw;
            }
        }

        if (!eosId.empty())
            survivorId = GetCachedSurvivorId(eosId);
    }

    // Snapshot before.
    std::unordered_map<std::string, int> before;
    if (inv && !eosId.empty() && survivorId != 0)
        before = SnapshotInventory(inv);

    Original_GiveHarvestResource(comp, destInv, harvestMultiplier, damageType, harvester, overrideResources);

    // Diff after and queue deltas.
    if (inv && !eosId.empty() && survivorId != 0)
    {
        TArray<UPrimalItem*>& items = inv->InventoryItemsField();
        for (int i = 0; i < items.Num(); ++i)
        {
            UPrimalItem* item = items[i];
            if (!item) continue;
            const std::string name = FStr(item->DescriptiveNameBaseField());
            if (name.empty()) continue;
            const int qtyAfter = item->GetItemQuantity();
            const int qtyBefore = [&]() -> int {
                auto it = before.find(name);
                return it != before.end() ? it->second : 0;
                }();
            const int delta = qtyAfter - qtyBefore;
            if (delta > 0)
            {
                QueueDelta(eosId, survivorId, name, static_cast<int64_t>(delta));
            }
        }
    }
}

// =============================================================================
// Plugin Entry Points
// =============================================================================

extern "C" __declspec(dllexport) void Plugin_Init()
{
    Log::Get().Init("ResourceStats");

    if (!LoadConfig())
    {
        Log::GetLog()->error("[ResourceStats] Halted - config error");
        return;
    }

    if (!InitDatabase())
    {
        Log::GetLog()->error("[ResourceStats] Halted - database error");
        return;
    }

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
        "UPrimalHarvestingComponent.GiveHarvestResource(UPrimalInventoryComponent*,float,TSubclassOf<UDamageType>,AActor*,TArray<FHarvestResourceEntry,TSizedDefaultAllocator<32>>*)",
        (LPVOID)&Detour_GiveHarvestResource,
        (LPVOID*)&Original_GiveHarvestResource
    );

    g_flush_running.store(true);
    g_flush_thread = std::thread(FlushThreadFunc);

    Log::GetLog()->info("[ResourceStats] Plugin loaded");
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    g_flush_running.store(false);
    if (g_flush_thread.joinable())
        g_flush_thread.join();

    AsaApi::GetHooks().DisableHook(
        "AShooterGameMode.StartNewShooterPlayer(APlayerController*,bool,bool,FPrimalPlayerCharacterConfigStruct&,UPrimalPlayerData*,bool)",
        (LPVOID)&Detour_StartNewShooterPlayer
    );

    AsaApi::GetHooks().DisableHook(
        "AShooterPlayerController.HandleRespawned_Implementation(APawn*,bool)",
        (LPVOID)&Detour_HandleRespawned
    );

    AsaApi::GetHooks().DisableHook(
        "UPrimalHarvestingComponent.GiveHarvestResource(UPrimalInventoryComponent*,float,TSubclassOf<UDamageType>,AActor*,TArray<FHarvestResourceEntry,TSizedDefaultAllocator<32>>*)",
        (LPVOID)&Detour_GiveHarvestResource
    );

    CloseDatabase();

    Log::GetLog()->info("[ResourceStats] Plugin unloaded");
}