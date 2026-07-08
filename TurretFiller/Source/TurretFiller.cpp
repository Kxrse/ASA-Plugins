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
 * Hook categories: Chat, Structures (DoFire), Inventory
 *
 * Commands:
 *   /fill               — distributes ammo from player inventory into nearby turrets
 *   /fillrange {meters} — sets personal fill radius (persisted in MariaDB per EOS ID)
 *
 * Hooks:
 *   APrimalStructureTurret.DoFire(int) — tracks last fire time for combat cooldown
 *   AShooterGameMode.Tick              — hot-reloads config every 10 seconds
 *
 * Table:
 *   turret_filler_ranges — PK (eos_id)
 *   Columns: eos_id VARCHAR(64), fill_range FLOAT
 *
 * Permissions:
 *   Dynamically loads Permissions.dll at runtime (lazy, first use).
 *   Per-group tiers control: fill_enabled, max_fill_range, command_cooldown, combat_cooldown.
 *   Best tier selected by highest max_fill_range among matched groups.
 *   Falls back to "default" tier if Permissions.dll is absent or player has no matching group.
 */

#include <API/ARK/Ark.h>
#include <json.hpp>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <sys/stat.h>

// =============================================================================
// Constants
// =============================================================================

static constexpr double UE_UNITS_PER_METER = 100.0;

// =============================================================================
// MariaDB — Dynamic Load
// =============================================================================

typedef struct st_mysql     MYSQL;
typedef struct st_mysql_res MYSQL_RES;
typedef char** MYSQL_ROW;

typedef MYSQL* (__stdcall* mysql_init_t)             (MYSQL*);
typedef MYSQL* (__stdcall* mysql_real_connect_t)     (MYSQL*, const char*, const char*, const char*, const char*, unsigned int, const char*, unsigned long);
typedef void   (__stdcall* mysql_close_t)            (MYSQL*);
typedef int    (__stdcall* mysql_query_t)             (MYSQL*, const char*);
typedef MYSQL_RES* (__stdcall* mysql_store_result_t) (MYSQL*);
typedef void   (__stdcall* mysql_free_result_t)      (MYSQL_RES*);
typedef const char* (__stdcall* mysql_error_t)       (MYSQL*);
typedef unsigned long (__stdcall* mysql_real_escape_string_t)(MYSQL*, char*, const char*, unsigned long);
typedef int    (__stdcall* mysql_options_t)           (MYSQL*, int, const void*);
typedef MYSQL_ROW (__stdcall* mysql_fetch_row_t)     (MYSQL_RES*);

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

#pragma warning(push)
#pragma warning(disable: 4191)

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
            Log::GetLog()->info("[TurretFiller] Loaded DB library: {}", candidates[i]);
            break;
        }
    }

    if (!g_mysql_module)
    {
        Log::GetLog()->error("[TurretFiller] Could not find libmariadb.dll or libmysql.dll");
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
        Log::GetLog()->error("[TurretFiller] Failed to resolve required DB functions");
        return false;
    }

    g_mysql_loaded = true;
    return true;
}

#pragma warning(pop)

// =============================================================================
// Permissions API (dynamically loaded at runtime)
// =============================================================================

typedef TArray<FString>(*GetPlayerGroups_t)(const FString&);

static GetPlayerGroups_t pGetPlayerGroups  = nullptr;
static bool              g_permissions_loaded    = false;
static bool              g_permissions_attempted = false;

static void LoadPermissionsAPI()
{
    if (g_permissions_attempted) return;
    g_permissions_attempted = true;

    HMODULE hMod = GetModuleHandleA("Permissions");
    if (!hMod)
    {
        Log::GetLog()->warn("[TurretFiller] Permissions.dll not found, using default tier for all players");
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
    bool  fillEnabled    = true;
    float maxFillRange   = 100.0f;
    float minFillRange   = 10.0f;
    float defaultRange   = 50.0f;
    int   commandCooldown = 30;
    int   combatCooldown  = 300;
};

struct AmmoMapping
{
    std::string turretBpSubstring;
    std::string ammoName;
    std::string ammoBpPath;
    UClass*     cachedClass = nullptr;
};

static const std::string g_config_path = "ArkApi/Plugins/TurretFiller/config.json";

static GroupTier g_default_tier;
static std::unordered_map<std::string, GroupTier> g_group_tiers;
static std::vector<AmmoMapping> g_ammo_map;

static std::string  g_db_host = "localhost";
static unsigned int g_db_port = 3306;
static std::string  g_db_user;
static std::string  g_db_pass;
static std::string  g_db_name;

static time_t    g_config_mtime = 0;
static uintmax_t g_config_size  = 0;

static time_t GetFileMTime(const std::string& path)
{
    struct _stat st {};
    if (_stat(path.c_str(), &st) == 0) return st.st_mtime;
    return 0;
}

static uintmax_t GetFileSize(const std::string& path)
{
    struct _stat st {};
    if (_stat(path.c_str(), &st) == 0) return static_cast<uintmax_t>(st.st_size);
    return 0;
}

static GroupTier ParseTier(const nlohmann::json& j, const GroupTier& fallback)
{
    GroupTier t;
    t.fillEnabled     = j.value("fill_enabled", fallback.fillEnabled);
    t.maxFillRange    = j.value("max_fill_range", fallback.maxFillRange);
    t.minFillRange    = j.value("min_fill_range", fallback.minFillRange);
    t.defaultRange    = j.value("default_fill_range", fallback.defaultRange);
    t.commandCooldown = j.value("command_cooldown", fallback.commandCooldown);
    t.combatCooldown  = j.value("combat_cooldown", fallback.combatCooldown);
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

    try
    {
        nlohmann::json j;
        file >> j;

        if (j.contains("Database") && j["Database"].is_object())
        {
            auto& db  = j["Database"];
            g_db_host = db.value("Host", "localhost");
            g_db_port = db.value("Port", 3306);
            g_db_user = db.value("User", "");
            g_db_pass = db.value("Password", "");
            g_db_name = db.value("Name", "");
        }

        // Default tier
        GroupTier hardDefault;
        if (j.contains("default") && j["default"].is_object())
            g_default_tier = ParseTier(j["default"], hardDefault);
        else
            g_default_tier = hardDefault;

        // Group tiers
        g_group_tiers.clear();
        if (j.contains("groups") && j["groups"].is_object())
        {
            for (auto& [key, val] : j["groups"].items())
            {
                if (!val.is_object()) continue;
                g_group_tiers[key] = ParseTier(val, g_default_tier);
            }
        }

        // Ammo mappings
        std::vector<AmmoMapping> newMap;
        if (j.contains("TurretAmmoMap") && j["TurretAmmoMap"].is_object())
        {
            for (auto& [key, val] : j["TurretAmmoMap"].items())
            {
                if (!val.is_object()) continue;
                AmmoMapping m;
                m.turretBpSubstring = key;
                m.ammoName   = val.value("AmmoName", "");
                m.ammoBpPath = val.value("AmmoBP", "");
                if (!m.ammoName.empty() && !m.ammoBpPath.empty())
                    newMap.push_back(std::move(m));
            }
        }

        g_ammo_map = std::move(newMap);
        g_config_mtime = GetFileMTime(g_config_path);
        g_config_size  = GetFileSize(g_config_path);
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[TurretFiller] Config parse error: {}", ex.what());
        return false;
    }

    Log::GetLog()->info("[TurretFiller] Config loaded: {} group tier(s), {} ammo mappings",
        g_group_tiers.size(), g_ammo_map.size());
    return true;
}

// =============================================================================
// Resolve Tier — picks best match from player's permission groups
// =============================================================================

static std::string FStr(const FString& f)
{
    const char* s = TCHAR_TO_UTF8(*f);
    return (s && s[0]) ? s : "";
}

static GroupTier ResolveTier(const std::string& eosId)
{
    GroupTier best = g_default_tier;

    if (g_group_tiers.empty()) return best;

    if (!g_permissions_attempted) LoadPermissionsAPI();
    if (!g_permissions_loaded || !pGetPlayerGroups) return best;

    try
    {
        const std::wstring wEos(eosId.begin(), eosId.end());
        FString fEos(wEos.c_str());
        TArray<FString> groups = pGetPlayerGroups(fEos);

        bool matched = false;
        for (int i = 0; i < groups.Num(); ++i)
        {
            const std::string groupName = FStr(groups[i]);
            auto it = g_group_tiers.find(groupName);
            if (it != g_group_tiers.end())
            {
                if (!matched || it->second.maxFillRange > best.maxFillRange)
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

static bool InitDatabase()
{
    if (!LoadMySQLLib()) return false;

    g_mysql = pmysql_init(nullptr);
    if (!g_mysql)
    {
        Log::GetLog()->error("[TurretFiller] mysql_init returned null");
        return false;
    }

    unsigned int timeout = 10;
    if (pmysql_options) pmysql_options(g_mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

    if (!pmysql_real_connect(g_mysql,
        g_db_host.c_str(), g_db_user.c_str(), g_db_pass.c_str(),
        g_db_name.c_str(), g_db_port, nullptr, 0))
    {
        Log::GetLog()->error("[TurretFiller] DB connect failed: {}", pmysql_error(g_mysql));
        pmysql_close(g_mysql);
        g_mysql = nullptr;
        return false;
    }

    const std::string create =
        "CREATE TABLE IF NOT EXISTS turret_filler_ranges ("
        "  eos_id     VARCHAR(64)  NOT NULL,"
        "  fill_range FLOAT        NOT NULL DEFAULT 50,"
        "  PRIMARY KEY (eos_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!ExecQuery(create))
    {
        Log::GetLog()->error("[TurretFiller] Failed to create table");
        return false;
    }

    Log::GetLog()->info("[TurretFiller] Database ready");
    return true;
}

static void CloseDatabase()
{
    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (g_mysql) { pmysql_close(g_mysql); g_mysql = nullptr; }
}

// =============================================================================
// DB Operations — Fill Range
// =============================================================================

static float DBLoadFillRange(const std::string& eosId)
{
    const std::string eEos = EscapeUnsafe(eosId);

    char sql[256];
    std::snprintf(sql, sizeof(sql),
        "SELECT fill_range FROM turret_filler_ranges WHERE eos_id='%s'",
        eEos.c_str());

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!g_mysql) return -1.0f;

    if (pmysql_query(g_mysql, sql) != 0) return -1.0f;

    MYSQL_RES* res = pmysql_store_result(g_mysql);
    if (!res) return -1.0f;

    float result = -1.0f;
    MYSQL_ROW row = pmysql_fetch_row(res);
    if (row && row[0])
        result = static_cast<float>(std::atof(row[0]));

    pmysql_free_result(res);
    return result;
}

static void DBSaveFillRange(const std::string& eosId, float meters)
{
    const std::string eEos = EscapeUnsafe(eosId);

    char sql[256];
    std::snprintf(sql, sizeof(sql),
        "INSERT INTO turret_filler_ranges (eos_id, fill_range) VALUES ('%s', %.1f) "
        "ON DUPLICATE KEY UPDATE fill_range = %.1f",
        eEos.c_str(), meters, meters);

    std::lock_guard<std::mutex> lock(g_db_mutex);
    ExecQuery(sql);
}

// =============================================================================
// Helpers
// =============================================================================

static std::string GetEosId(AShooterPlayerController* pc)
{
    if (!pc) return "";
    AShooterPlayerState* ps = static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
    if (!ps) return "";
    FString raw;
    ps->GetUniqueNetIdAsString(&raw);
    std::string eos = FStr(raw);
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

// =============================================================================
// Per-player State (in-memory cache, backed by DB)
// =============================================================================

static std::unordered_map<std::string, float> g_fill_ranges;
static std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_cooldowns;

// Returns the player's fill range clamped to their tier limits.
static float GetFillRange(const std::string& eosId, const GroupTier& tier)
{
    auto it = g_fill_ranges.find(eosId);
    if (it != g_fill_ranges.end())
        return std::clamp(it->second, tier.minFillRange, tier.maxFillRange);

    float dbVal = DBLoadFillRange(eosId);
    if (dbVal > 0.0f)
    {
        g_fill_ranges[eosId] = dbVal;
        return std::clamp(dbVal, tier.minFillRange, tier.maxFillRange);
    }

    return tier.defaultRange;
}

// =============================================================================
// Combat Tracking
// =============================================================================

static std::mutex g_fire_mutex;
static std::unordered_map<void*, std::chrono::steady_clock::time_point> g_fire_times;

static bool IsCombatBlocked(void* turretPtr, int combatCooldown)
{
    std::lock_guard<std::mutex> lock(g_fire_mutex);
    auto it = g_fire_times.find(turretPtr);
    if (it == g_fire_times.end()) return false;
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - it->second).count();
    return elapsed < combatCooldown;
}

// =============================================================================
// Ammo Class Cache
// =============================================================================

static UClass* GetAmmoClass(AmmoMapping& mapping)
{
    if (mapping.cachedClass) return mapping.cachedClass;
    if (mapping.ammoBpPath.empty()) return nullptr;

    const std::wstring wPath(mapping.ammoBpPath.begin(), mapping.ammoBpPath.end());
    FString fPath(wPath.c_str());
    UClass* cls = UVictoryCore::BPLoadClass(fPath);
    if (cls)
    {
        mapping.cachedClass = cls;
    }
    else
    {
        Log::GetLog()->warn("[TurretFiller] BPLoadClass failed for '{}'", mapping.ammoBpPath);
    }
    return cls;
}

static int FindAmmoMapping(const std::string& turretBp)
{
    for (int i = 0; i < static_cast<int>(g_ammo_map.size()); ++i)
    {
        if (turretBp.find(g_ammo_map[i].turretBpSubstring) != std::string::npos)
            return i;
    }
    return -1;
}

// =============================================================================
// Hook Type Aliases
// =============================================================================

using Tick_t   = void(*)(AShooterGameMode*, float);
using DoFire_t = void(*)(APrimalStructureTurret*, int);

static Tick_t   Original_Tick   = nullptr;
static DoFire_t Original_DoFire = nullptr;

// =============================================================================
// DoFire Hook — Combat Tracking
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
// Remove All Ammo Stacks By Name — uses inventory RemoveItem(FItemNetID)
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
// World Position Helper — works when mounted or on foot
// =============================================================================

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
// /fill Command
// =============================================================================

static void Cmd_Fill(AShooterPlayerController* pc, FString*, int, int)
{
    if (!pc) return;

    const std::string eosId = GetEosId(pc);
    if (eosId.empty()) return;

    const GroupTier tier = ResolveTier(eosId);

    // Access check
    if (!tier.fillEnabled)
    {
        Notify(pc, L"You do not have permission to use /fill.", L"1.0,0.2,0.2,1.0");
        return;
    }

    // Cooldown check
    auto now = std::chrono::steady_clock::now();
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

    int playerTeam = character->TargetingTeamField();

    float radiusMeters = GetFillRange(eosId, tier);
    double radiusUE = static_cast<double>(radiusMeters) * UE_UNITS_PER_METER;

    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return;

    TArray<AActor*> allTurrets;
    UGameplayStatics::GetAllActorsOfClass(world,
        APrimalStructureTurret::StaticClass(), &allTurrets);

    struct TurretEntry { UPrimalInventoryComponent* inv; };
    struct AmmoGroup
    {
        std::string ammoName;
        int         configIdx = -1;
        std::vector<TurretEntry> turrets;
    };

    std::unordered_map<std::string, AmmoGroup> ammoGroups;
    int totalInRange  = 0;
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
        int mapIdx = FindAmmoMapping(bp);
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

        const std::string& ammoName = g_ammo_map[mapIdx].ammoName;
        auto& group = ammoGroups[ammoName];
        if (group.configIdx < 0)
        {
            group.ammoName  = ammoName;
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

    int totalFilled      = 0;
    int totalTransferred = 0;

    for (auto& [ammoName, group] : ammoGroups)
    {
        UClass* ammoClass = GetAmmoClass(g_ammo_map[group.configIdx]);
        if (!ammoClass) continue;

        int playerPool = CountAmmoInInventory(playerInv, ammoName);
        if (playerPool <= 0) continue;

        int numTurrets = static_cast<int>(group.turrets.size());
        int remaining = playerPool;
        int distributed = 0;

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
                distributed += added;
                remaining   -= added;
                share       -= added;
                filledAny = true;
            }

            if (filledAny) ++totalFilled;
        }

        totalTransferred += distributed;
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

    float meters = tier.defaultRange;
    size_t space = raw.find(' ');
    if (space != std::string::npos)
    {
        try { meters = std::stof(raw.substr(space + 1)); }
        catch (...) { /* use default */ }
    }
    else
    {
        float current = GetFillRange(eosId, tier);
        Notify(pc, L"Fill range: " + std::to_wstring(static_cast<int>(current)) + L"m (min: "
            + std::to_wstring(static_cast<int>(tier.minFillRange)) + L"m, max: "
            + std::to_wstring(static_cast<int>(tier.maxFillRange)) + L"m).");
        return;
    }

    meters = std::clamp(meters, tier.minFillRange, tier.maxFillRange);
    g_fill_ranges[eosId] = meters;
    DBSaveFillRange(eosId, meters);

    Notify(pc, L"Fill range set to " + std::to_wstring(static_cast<int>(meters)) + L"m.");
}

// =============================================================================
// Tick Hook — Config Hot-Reload
// =============================================================================

static float g_config_check_accumulator = 0.0f;

static void Detour_Tick(AShooterGameMode* gm, float delta)
{
    Original_Tick(gm, delta);

    g_config_check_accumulator += delta;
    if (g_config_check_accumulator >= 10.0f)
    {
        g_config_check_accumulator = 0.0f;

        const uintmax_t newSize  = GetFileSize(g_config_path);
        const time_t    newMtime = GetFileMTime(g_config_path);

        if (newSize == 0 || newMtime == 0) return;

        if (newSize != g_config_size || newMtime != g_config_mtime)
        {
            Log::GetLog()->info("[TurretFiller] Config change detected, reloading...");
            for (auto& m : g_ammo_map) m.cachedClass = nullptr;
            LoadConfig();
        }
    }
}

// =============================================================================
// Plugin Entry Points
// =============================================================================

static void PluginInit()
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

    AsaApi::GetHooks().SetHook(
        "AShooterGameMode.Tick(float)",
        (LPVOID)&Detour_Tick,
        (LPVOID*)&Original_Tick
    );

    Log::GetLog()->info("[TurretFiller] Plugin loaded");
}

static void PluginUnload()
{
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/fill"));
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/fillrange"));

    AsaApi::GetHooks().DisableHook(
        "APrimalStructureTurret.DoFire(int)",
        (LPVOID)&Detour_DoFire
    );

    AsaApi::GetHooks().DisableHook(
        "AShooterGameMode.Tick(float)",
        (LPVOID)&Detour_Tick
    );

    CloseDatabase();

    Log::GetLog()->info("[TurretFiller] Plugin unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { PluginInit(); }
    catch (const std::exception& ex)
    {
        Log::GetLog()->critical("[TurretFiller] Init exception: {}", ex.what());
    }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { PluginUnload(); }
    catch (const std::exception& ex)
    {
        Log::GetLog()->critical("[TurretFiller] Unload exception: {}", ex.what());
    }
}