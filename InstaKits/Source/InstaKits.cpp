/*
InstaKits - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * InstaKits - ASA Plugin
 * Gives a rank based item kit to a player on spawn. Lowest Priority wins.
 * Recurring kits deliver every spawn; one time kits deliver once per EOS per group, tracked in MariaDB.
 *
 * Hooks:
 *   AShooterPlayerController.HandleRespawned_Implementation - queue kit delivery on spawn
 *
 * Commands:
 *   /savekit  - save the player's current arrangement of their own kit items
 *   /resetkit - discard the saved arrangement and fall back to the config layout
 *
 * Config:
 *   ArkApi/Plugins/InstaKits/config.json
 *   DeliverDelay: seconds to wait after a spawn before delivering, minimum 1
 *   UnlockTekEngrams: unlock tek engrams before equipping so tek gear can be worn
 *   AllowSaveKit: enable /savekit and /resetkit, defaults to false
 *   SaveKitCooldown: seconds a player must wait between /savekit calls, minimum 1
 *   DbHost DbPort DbUser DbPass DbName: MariaDB connection, applied at load only
 *   Groups: per Permissions group kits. Every group requires a unique integer Priority.
 *           Lower Priority wins (1 is highest). Missing or duplicate Priority is a hard
 *           config error and the config is rejected. Group names match the Permissions
 *           group name exactly and are case sensitive. A player in no listed group gets nothing.
 *   Recurring: true delivers every spawn, false delivers once per EOS per group
 *   Items: BlueprintPath, Quantity, Quality, Equip, Slot, Tier, ClipAmmo
 *          Equip and Slot are independent and both are applied when both are set.
 *          Slot uses the in game hotbar labels in order 1 2 3 4 5 6 7 8 9 0, so Slot 1 is the
 *          first hotbar slot and Slot 0 is the tenth. Omit Slot or use -1 for no hotbar slot.
 *          Use Slot for tools and weapons, Equip for armor.
 *          ClipAmmo fabricates that many rounds of the weapon's default ammo type directly into
 *          the clip, so the weapon is ready without a reload. It does not draw from the player's
 *          inventory and it cannot select which ammo type is loaded. Omit it or use -1 to leave
 *          the clip untouched. It has no effect on items with no clip.
 *          Tier is one of Primitive Ramshackle Apprentice Journeyman Mastercraft Ascendant
 *
 * Config Example:
 * {
 *     "DeliverDelay": 1,
 *     "UnlockTekEngrams": true,
 *     "AllowSaveKit": true,
 *     "SaveKitCooldown": 30,
 *     "DbHost": "127.0.0.1",
 *     "DbPort": 3306,
 *     "DbUser": "User",
 *     "DbPass": "Password",
 *     "DbName": "arkcluster",
 *     "Groups": {
 *         "Admins": {
 *             "Priority": 1,
 *             "Recurring": true,
 *             "Items": [
 *                 {
 *                     "BlueprintPath": "Blueprint'/Game/PrimalEarth/CoreBlueprints/Items/Armor/Metal/PrimalItemArmor_MetalHelmet.PrimalItemArmor_MetalHelmet'",
 *                     "Quantity": 1,
 *                     "Quality": 15.0,
 *                     "Equip": true,
 *                     "Slot": -1,
 *                     "Tier": "Ascendant"
 *                 },
 *                 {
 *                     "BlueprintPath": "Blueprint'/Game/PrimalEarth/CoreBlueprints/Items/Armor/Shields/PrimalItemArmor_MetalShield.PrimalItemArmor_MetalShield'",
 *                     "Quantity": 1,
 *                     "Quality": 0.0,
 *                     "Equip": true,
 *                     "Slot": -1
 *                 },
 *                 {
 *                     "BlueprintPath": "Blueprint'/Game/PrimalEarth/CoreBlueprints/Weapons/PrimalItem_WeaponMetalPick.PrimalItem_WeaponMetalPick'",
 *                     "Quantity": 1,
 *                     "Quality": 0.0,
 *                     "Equip": false,
 *                     "Slot": 1,
 *                     "Tier": "Ascendant"
 *                 },
 *                 {
 *                     "BlueprintPath": "Blueprint'/Game/PrimalEarth/CoreBlueprints/Weapons/PrimalItem_WeaponOneShotRifle.PrimalItem_WeaponOneShotRifle'",
 *                     "Quantity": 1,
 *                     "Quality": 15.0,
 *                     "Equip": false,
 *                     "Slot": 2,
 *                     "Tier": "Ascendant",
 *                     "ClipAmmo": 1
 *                 },
 *                 {
 *                     "BlueprintPath": "Blueprint'/Game/PrimalEarth/CoreBlueprints/Items/Consumables/PrimalItemConsumable_HealSoup.PrimalItemConsumable_HealSoup'",
 *                     "Quantity": 50,
 *                     "Quality": 0.0,
 *                     "Equip": false,
 *                     "Slot": 0
 *                 },
 *                 {
 *                     "BlueprintPath": "Blueprint'/Game/PrimalEarth/CoreBlueprints/Weapons/PrimalItemAmmo_SimpleRifleBullet.PrimalItemAmmo_SimpleRifleBullet'",
 *                     "Quantity": 100,
 *                     "Quality": 0.0,
 *                     "Equip": false,
 *                     "Slot": -1
 *                 }
 *             ]
 *         },
 *         "Default": {
 *             "Priority": 2,
 *             "Recurring": false,
 *             "Items": [
 *                 {
 *                     "BlueprintPath": "Blueprint'/Game/PrimalEarth/CoreBlueprints/Weapons/PrimalItem_WeaponStoneHatchet.PrimalItem_WeaponStoneHatchet'",
 *                     "Quantity": 1,
 *                     "Quality": 0.0,
 *                     "Equip": false,
 *                     "Slot": 1,
 *                     "Tier": "Primitive"
 *                 }
 *             ]
 *         }
 *     }
 * }
 *
 * Delivery runs entirely on API::Timer, so the plugin holds no per tick work.
 * All SQL runs on a dedicated worker thread that exclusively owns the MariaDB connection.
 *
 * Saved layouts live in instakits_layouts as one JSON blob per EOS per group, mapping a
 * blueprint path to a Have flag, a Slot and an Equip flag. /savekit writes a line for every
 * item in the group's config, so an item the player was not carrying is stored as Have false
 * and is then skipped at delivery. A config item with no line at all, which happens when an
 * admin adds one after the player last saved, falls back to its config values. Only paths present in that group's config can
 * ever be written, because /savekit iterates the config and looks each entry up in the
 * inventory rather than the other way around. Slot values stored there are raw engine
 * indices 0 to 9, not the config's 1 to 9 then 0 numbering, so they are applied directly.
 * Layout loading is fail open: if the database has not answered by delivery time the kit
 * still arrives on its config slots.
 */

#include <API/ARK/Ark.h>
#include <Timer.h>
#include <json.hpp>
#include <fstream>
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <ctime>
#include <sys/stat.h>
#include <cctype>

#pragma comment(lib, "AsaApi.lib")
#pragma warning(disable: 4191)

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
            Log::GetLog()->info("[InstaKits] Loaded DB library: {}", candidates[i]);
            break;
        }
    }

    if (!g_mysql_module)
    {
        Log::GetLog()->error("[InstaKits] Could not find libmariadb.dll or libmysql.dll");
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

    if (!pmysql_init || !pmysql_real_connect || !pmysql_close || !pmysql_query ||
        !pmysql_error || !pmysql_real_escape_string || !pmysql_fetch_row ||
        !pmysql_store_result || !pmysql_free_result || !pmysql_ping)
    {
        Log::GetLog()->error("[InstaKits] Failed to resolve required DB functions");
        return false;
    }

    g_mysql_loaded = true;
    return true;
}

struct ItemEntry
{
    std::string BlueprintPath;
    int Quantity = 1;
    float Quality = 0.0f;
    bool Equip = false;
    int SlotIndex = -1;
    int TierIndex = -1;
    int ClipAmmo = -1;
};

struct Kit
{
    std::string Group;
    int Priority = 0;
    bool Recurring = true;
    std::vector<ItemEntry> Items;
};

enum class DbTaskType
{
    CheckClaim,
    RecordClaim,
    LoadLayout,
    SaveLayout,
    DeleteLayout
};

struct LayoutEntry
{
    int SlotIndex = -1;
    bool Equip = false;
    bool Have = true;
};

using KitLayout = std::unordered_map<std::string, LayoutEntry>;

struct DbTask
{
    DbTaskType Type = DbTaskType::CheckClaim;
    std::string Eos;
    std::string Group;
    std::string Payload;
    long long ClaimedAt = 0;
    int Attempts = 0;
};

static const std::string g_config_path = "ArkApi/Plugins/InstaKits/config.json";
static const int g_claim_retry_limit = 5;
static const int g_record_retry_limit = 3;
static const wchar_t* g_reload_timer_id = L"InstaKits_ConfigReload";

static int g_deliver_delay = 1;
static bool g_unlock_tek_engrams = false;
static bool g_allow_savekit = false;
static int g_savekit_cooldown = 30;
static std::unordered_map<std::string, Kit> g_kits;

static std::string g_db_host = "127.0.0.1";
static unsigned int g_db_port = 3306;
static std::string g_db_user;
static std::string g_db_pass;
static std::string g_db_name;

static MYSQL* g_db = nullptr;
static bool g_db_table_ready = false;

static std::deque<DbTask> g_task_queue;
static std::mutex g_queue_mutex;
static std::condition_variable g_queue_cv;
static std::thread g_worker_thread;
static std::atomic<bool> g_worker_running{ false };

static std::unordered_map<std::string, bool> g_claim_cache;
static std::mutex g_cache_mutex;

static std::unordered_map<std::string, KitLayout> g_layout_cache;
static std::mutex g_layout_mutex;

static std::unordered_map<std::string, long long> g_savekit_cooldowns;

static time_t g_config_last_modified = 0;
static long long g_config_last_size = 0;

static std::string FStr(const FString& f)
{
    const char* s = TCHAR_TO_UTF8(*f);
    return (s && s[0]) ? s : "";
}

static std::wstring Utf8ToWide(const std::string& in)
{
    if (in.empty()) return std::wstring();
    const int len = MultiByteToWideChar(CP_UTF8, 0, in.c_str(), (int)in.size(), nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring out((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, in.c_str(), (int)in.size(), &out[0], len);
    return out;
}

static std::string ToLower(const std::string& in)
{
    std::string out = in;
    for (char& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

static std::string CacheKey(const std::string& eos, const std::string& group)
{
    return eos + "|" + group;
}

static int TierToIndex(const std::string& tier)
{
    const std::string t = ToLower(tier);
    if (t == "primitive") return 0;
    if (t == "ramshackle") return 1;
    if (t == "apprentice") return 2;
    if (t == "journeyman") return 3;
    if (t == "mastercraft") return 4;
    if (t == "ascendant") return 5;
    return -1;
}

static int SlotToIndex(int configSlot)
{
    if (configSlot == 0) return 9;
    if (configSlot >= 1 && configSlot <= 9) return configSlot - 1;
    return -1;
}

static std::string GetEos(AShooterPlayerController* pc)
{
    if (!pc) return "";
    return FStr(AsaApi::IApiUtils::GetEOSIDFromController(pc));
}

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

static void ParseItems(const nlohmann::json& val, std::vector<ItemEntry>& out)
{
    out.clear();
    if (!val.contains("Items") || !val["Items"].is_array()) return;

    for (const auto& ij : val["Items"])
    {
        if (!ij.is_object()) continue;
        ItemEntry e;
        e.BlueprintPath = ij.value("BlueprintPath", std::string(""));
        e.Quantity = ij.value("Quantity", 1);
        e.Quality = ij.value("Quality", 0.0f);
        e.Equip = ij.value("Equip", false);
        e.SlotIndex = -1;
        if (ij.contains("Slot") && ij["Slot"].is_number_integer())
            e.SlotIndex = SlotToIndex(ij["Slot"].get<int>());
        e.TierIndex = TierToIndex(ij.value("Tier", std::string("")));
        e.ClipAmmo = -1;
        if (ij.contains("ClipAmmo") && ij["ClipAmmo"].is_number_integer())
            e.ClipAmmo = ij["ClipAmmo"].get<int>();
        if (!e.BlueprintPath.empty()) out.push_back(e);
    }
}

static bool LoadConfig(bool first_load)
{
    std::ifstream file(g_config_path);
    if (!file.is_open())
    {
        Log::GetLog()->error("[InstaKits] Cannot open config: {}", g_config_path);
        return false;
    }

    int newDelay = 1;
    bool newUnlockTek = false;
    bool newAllowSaveKit = false;
    int newSaveKitCooldown = 30;
    std::unordered_map<std::string, Kit> newKits;

    std::string newDbHost = "127.0.0.1";
    unsigned int newDbPort = 3306;
    std::string newDbUser;
    std::string newDbPass;
    std::string newDbName;

    try
    {
        nlohmann::json j;
        file >> j;

        newDelay = j.value("DeliverDelay", 1);
        if (newDelay < 1) newDelay = 1;

        newUnlockTek = j.value("UnlockTekEngrams", false);

        newAllowSaveKit = j.value("AllowSaveKit", false);
        newSaveKitCooldown = j.value("SaveKitCooldown", 30);
        if (newSaveKitCooldown < 1) newSaveKitCooldown = 1;

        newDbHost = j.value("DbHost", std::string("127.0.0.1"));
        newDbPort = j.value("DbPort", 3306u);
        newDbUser = j.value("DbUser", std::string(""));
        newDbPass = j.value("DbPass", std::string(""));
        newDbName = j.value("DbName", std::string(""));

        std::unordered_set<int> seenPriorities;
        if (j.contains("Groups") && j["Groups"].is_object())
        {
            for (auto& [key, val] : j["Groups"].items())
            {
                if (!val.is_object()) continue;
                if (key.empty()) continue;

                if (!val.contains("Priority") || !val["Priority"].is_number_integer())
                {
                    Log::GetLog()->error("[InstaKits] Group '{}' is missing an integer Priority", key);
                    return false;
                }

                const int p = val["Priority"].get<int>();
                if (!seenPriorities.insert(p).second)
                {
                    Log::GetLog()->error("[InstaKits] Duplicate Priority {} on group '{}'", p, key);
                    return false;
                }

                Kit k;
                k.Group = key;
                k.Priority = p;
                k.Recurring = val.value("Recurring", true);
                ParseItems(val, k.Items);

                newKits[key] = std::move(k);
            }
        }
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[InstaKits] Config parse error: {}", ex.what());
        return false;
    }

    g_deliver_delay = newDelay;
    g_unlock_tek_engrams = newUnlockTek;
    g_allow_savekit = newAllowSaveKit;
    g_savekit_cooldown = newSaveKitCooldown;
    g_kits = std::move(newKits);

    if (first_load)
    {
        g_db_host = newDbHost;
        g_db_port = newDbPort;
        g_db_user = newDbUser;
        g_db_pass = newDbPass;
        g_db_name = newDbName;
    }

    g_config_last_modified = GetFileModTime(g_config_path);
    g_config_last_size = GetFileSize(g_config_path);
    Log::GetLog()->info("[InstaKits] Config loaded, {} groups", g_kits.size());
    return true;
}

static void CheckConfigReload()
{
    const long long sz = GetFileSize(g_config_path);
    if (sz == 0) return;
    const time_t mt = GetFileModTime(g_config_path);
    if (mt == g_config_last_modified && sz == g_config_last_size) return;
    LoadConfig(false);
}

static std::string EscapeUnsafe(const std::string& in)
{
    if (!g_db || !pmysql_real_escape_string) return "";
    std::vector<char> buf(in.size() * 2 + 1);
    const unsigned long len = pmysql_real_escape_string(g_db, buf.data(), in.c_str(), (unsigned long)in.size());
    return std::string(buf.data(), len);
}

static bool EnsureDbConnected()
{
    if (!LoadMySQLLib()) return false;

    if (g_db)
    {
        if (pmysql_ping(g_db) == 0) return true;
        pmysql_close(g_db);
        g_db = nullptr;
        g_db_table_ready = false;
    }

    g_db = pmysql_init(nullptr);
    if (!g_db)
    {
        Log::GetLog()->error("[InstaKits] mysql_init failed");
        return false;
    }

    if (!pmysql_real_connect(g_db, g_db_host.c_str(), g_db_user.c_str(), g_db_pass.c_str(),
        g_db_name.c_str(), g_db_port, nullptr, 0))
    {
        Log::GetLog()->error("[InstaKits] DB connect failed: {}", pmysql_error(g_db));
        pmysql_close(g_db);
        g_db = nullptr;
        return false;
    }

    if (!g_db_table_ready)
    {
        const char* create =
            "CREATE TABLE IF NOT EXISTS instakits_claims ("
            "eos_id VARCHAR(64) NOT NULL,"
            "kit_group VARCHAR(128) NOT NULL,"
            "claimed_at BIGINT NOT NULL DEFAULT 0,"
            "PRIMARY KEY (eos_id, kit_group))";

        if (pmysql_query(g_db, create))
        {
            Log::GetLog()->error("[InstaKits] Create claims table failed: {}", pmysql_error(g_db));
            return false;
        }

        const char* createLayouts =
            "CREATE TABLE IF NOT EXISTS instakits_layouts ("
            "eos_id VARCHAR(64) NOT NULL,"
            "kit_group VARCHAR(128) NOT NULL,"
            "layout TEXT NOT NULL,"
            "updated_at BIGINT NOT NULL DEFAULT 0,"
            "PRIMARY KEY (eos_id, kit_group))";

        if (pmysql_query(g_db, createLayouts))
        {
            Log::GetLog()->error("[InstaKits] Create layouts table failed: {}", pmysql_error(g_db));
            return false;
        }

        g_db_table_ready = true;
    }

    Log::GetLog()->info("[InstaKits] DB connected");
    return true;
}

static void SetClaimCache(const std::string& key, bool claimed)
{
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    g_claim_cache[key] = claimed;
}

static bool TryGetClaimCache(const std::string& key, bool& outClaimed)
{
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    const auto it = g_claim_cache.find(key);
    if (it == g_claim_cache.end()) return false;
    outClaimed = it->second;
    return true;
}

static bool HasClaimCache(const std::string& key)
{
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    return g_claim_cache.find(key) != g_claim_cache.end();
}

static void SetLayoutCache(const std::string& key, const KitLayout& layout)
{
    std::lock_guard<std::mutex> lock(g_layout_mutex);
    g_layout_cache[key] = layout;
}

static bool HasLayoutCache(const std::string& key)
{
    std::lock_guard<std::mutex> lock(g_layout_mutex);
    return g_layout_cache.find(key) != g_layout_cache.end();
}

static bool TryGetLayoutCache(const std::string& key, KitLayout& out)
{
    std::lock_guard<std::mutex> lock(g_layout_mutex);
    const auto it = g_layout_cache.find(key);
    if (it == g_layout_cache.end()) return false;
    out = it->second;
    return true;
}

static void QueueDbTask(const DbTask& task)
{
    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        g_task_queue.push_back(task);
    }
    g_queue_cv.notify_one();
}

static void RunCheckClaim(const DbTask& task)
{
    const std::string safeEos = EscapeUnsafe(task.Eos);
    const std::string safeGroup = EscapeUnsafe(task.Group);
    if (safeEos.empty() || safeGroup.empty()) return;

    const std::string q =
        "SELECT 1 FROM instakits_claims WHERE eos_id='" + safeEos +
        "' AND kit_group='" + safeGroup + "' LIMIT 1";

    if (pmysql_query(g_db, q.c_str()))
    {
        Log::GetLog()->error("[InstaKits] Claim query failed: {}", pmysql_error(g_db));
        return;
    }

    MYSQL_RES* res = pmysql_store_result(g_db);
    if (!res) return;

    const MYSQL_ROW row = pmysql_fetch_row(res);
    SetClaimCache(CacheKey(task.Eos, task.Group), row != nullptr);
    pmysql_free_result(res);
}

static void RunRecordClaim(const DbTask& task)
{
    const std::string safeEos = EscapeUnsafe(task.Eos);
    const std::string safeGroup = EscapeUnsafe(task.Group);
    if (safeEos.empty() || safeGroup.empty()) return;

    const std::string q =
        "INSERT IGNORE INTO instakits_claims (eos_id, kit_group, claimed_at) VALUES ('" +
        safeEos + "', '" + safeGroup + "', " + std::to_string(task.ClaimedAt) + ")";

    if (pmysql_query(g_db, q.c_str()))
    {
        Log::GetLog()->error("[InstaKits] Record claim failed: {}", pmysql_error(g_db));

        DbTask retry = task;
        retry.Attempts += 1;
        if (retry.Attempts < g_record_retry_limit)
            QueueDbTask(retry);
        else
            Log::GetLog()->error("[InstaKits] Giving up recording claim for {} group {}", task.Eos, task.Group);
    }
}

static void RunLoadLayout(const DbTask& task)
{
    const std::string safeEos = EscapeUnsafe(task.Eos);
    const std::string safeGroup = EscapeUnsafe(task.Group);
    if (safeEos.empty() || safeGroup.empty()) return;

    const std::string q =
        "SELECT layout FROM instakits_layouts WHERE eos_id='" + safeEos +
        "' AND kit_group='" + safeGroup + "' LIMIT 1";

    if (pmysql_query(g_db, q.c_str()))
    {
        Log::GetLog()->error("[InstaKits] Layout query failed: {}", pmysql_error(g_db));
        return;
    }

    MYSQL_RES* res = pmysql_store_result(g_db);
    if (!res) return;

    KitLayout layout;
    const MYSQL_ROW row = pmysql_fetch_row(res);

    if (row && row[0])
    {
        try
        {
            const nlohmann::json j = nlohmann::json::parse(row[0]);
            if (j.is_object())
            {
                for (auto& [path, val] : j.items())
                {
                    if (!val.is_object()) continue;
                    LayoutEntry e;
                    e.SlotIndex = val.value("Slot", -1);
                    e.Equip = val.value("Equip", false);
                    e.Have = val.value("Have", true);
                    if (e.SlotIndex < -1 || e.SlotIndex > 9) e.SlotIndex = -1;
                    layout[path] = e;
                }
            }
        }
        catch (const std::exception& ex)
        {
            Log::GetLog()->error("[InstaKits] Layout parse failed for {}: {}", task.Eos, ex.what());
        }
    }

    pmysql_free_result(res);
    SetLayoutCache(CacheKey(task.Eos, task.Group), layout);
}

static void RunSaveLayout(const DbTask& task)
{
    const std::string safeEos = EscapeUnsafe(task.Eos);
    const std::string safeGroup = EscapeUnsafe(task.Group);
    const std::string safePayload = EscapeUnsafe(task.Payload);
    if (safeEos.empty() || safeGroup.empty()) return;

    const std::string q =
        "INSERT INTO instakits_layouts (eos_id, kit_group, layout, updated_at) VALUES ('" +
        safeEos + "', '" + safeGroup + "', '" + safePayload + "', " + std::to_string(task.ClaimedAt) +
        ") ON DUPLICATE KEY UPDATE layout=VALUES(layout), updated_at=VALUES(updated_at)";

    if (pmysql_query(g_db, q.c_str()))
    {
        Log::GetLog()->error("[InstaKits] Save layout failed: {}", pmysql_error(g_db));

        DbTask retry = task;
        retry.Attempts += 1;
        if (retry.Attempts < g_record_retry_limit)
            QueueDbTask(retry);
    }
}

static void RunDeleteLayout(const DbTask& task)
{
    const std::string safeEos = EscapeUnsafe(task.Eos);
    const std::string safeGroup = EscapeUnsafe(task.Group);
    if (safeEos.empty() || safeGroup.empty()) return;

    const std::string q =
        "DELETE FROM instakits_layouts WHERE eos_id='" + safeEos +
        "' AND kit_group='" + safeGroup + "'";

    if (pmysql_query(g_db, q.c_str()))
    {
        Log::GetLog()->error("[InstaKits] Delete layout failed: {}", pmysql_error(g_db));

        DbTask retry = task;
        retry.Attempts += 1;
        if (retry.Attempts < g_record_retry_limit)
            QueueDbTask(retry);
    }
}

static void DbWorkerLoop()
{
    while (g_worker_running.load())
    {
        DbTask task;
        bool have = false;

        {
            std::unique_lock<std::mutex> lock(g_queue_mutex);
            g_queue_cv.wait_for(lock, std::chrono::seconds(1), []
                {
                    return !g_task_queue.empty() || !g_worker_running.load();
                });

            if (!g_worker_running.load()) break;

            if (!g_task_queue.empty())
            {
                task = g_task_queue.front();
                g_task_queue.pop_front();
                have = true;
            }
        }

        if (!have) continue;

        if (!EnsureDbConnected())
        {
            if (task.Type == DbTaskType::RecordClaim ||
                task.Type == DbTaskType::SaveLayout ||
                task.Type == DbTaskType::DeleteLayout)
            {
                DbTask retry = task;
                retry.Attempts += 1;
                if (retry.Attempts < g_record_retry_limit)
                {
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    QueueDbTask(retry);
                }
            }
            continue;
        }

        try
        {
            switch (task.Type)
            {
            case DbTaskType::CheckClaim:   RunCheckClaim(task);   break;
            case DbTaskType::RecordClaim:  RunRecordClaim(task);  break;
            case DbTaskType::LoadLayout:   RunLoadLayout(task);   break;
            case DbTaskType::SaveLayout:   RunSaveLayout(task);   break;
            case DbTaskType::DeleteLayout: RunDeleteLayout(task); break;
            }
        }
        catch (const std::exception& ex)
        {
            Log::GetLog()->error("[InstaKits] DB worker exception: {}", ex.what());
        }
    }

    if (g_db)
    {
        pmysql_close(g_db);
        g_db = nullptr;
    }
}

typedef TArray<FString>(*GetPlayerGroups_t)(const FString&);
static GetPlayerGroups_t pGetPlayerGroups = nullptr;
static bool g_permissions_loaded = false;

static void LoadPermissionsAPI()
{
    if (g_permissions_loaded) return;

    const HMODULE hMod = GetModuleHandleA("Permissions");
    if (!hMod) return;

    pGetPlayerGroups = (GetPlayerGroups_t)GetProcAddress(hMod,
        "?GetPlayerGroups@Permissions@@YA?AV?$TArray@VFString@@V?$TSizedDefaultAllocator@$0CA@@@@@AEBVFString@@@Z");

    if (!pGetPlayerGroups)
    {
        Log::GetLog()->warn("[InstaKits] Failed to resolve Permissions functions");
        return;
    }

    g_permissions_loaded = true;
    Log::GetLog()->info("[InstaKits] Permissions API loaded");
}

static bool GetGroups(const std::string& eosId, std::vector<std::string>& out)
{
    out.clear();

    if (!g_permissions_loaded || !pGetPlayerGroups)
        return false;

    const FString fEos(Utf8ToWide(eosId).c_str());
    TArray<FString> groups = pGetPlayerGroups(fEos);

    if (groups.Num() == 0)
        return false;

    for (int i = 0; i < groups.Num(); ++i)
        out.push_back(FStr(groups[i]));
    return true;
}

static const Kit* ResolveKit(const std::vector<std::string>& groups)
{
    const Kit* best = nullptr;
    for (const auto& g : groups)
    {
        const auto it = g_kits.find(g);
        if (it == g_kits.end()) continue;
        if (!best || it->second.Priority < best->Priority)
            best = &it->second;
    }
    return best;
}

static void UnlockTekEngrams(AShooterPlayerController* pc)
{
    if (!pc) return;

    UClass* cmClass = UShooterCheatManager::StaticClass();
    if (!cmClass) return;

    FStaticConstructObjectParameters params{};
    params.Class = cmClass;
    params.Outer = pc;
    params.Name = FName();
    params.SetFlags = EObjectFlags::RF_NoFlags;
    params.InternalSetFlags = EInternalObjectFlags::None;
    params.bCopyTransientsFromClassDefaults = false;
    params.bAssumeTemplateIsArchetype = false;
    params.Template = nullptr;
    params.InstanceGraph = nullptr;
    params.ExternalPackage = nullptr;
    params.SubobjectOverrides = nullptr;

    UShooterCheatManager* cm = static_cast<UShooterCheatManager*>(
        NativeCall<UObject*, FStaticConstructObjectParameters&>(nullptr,
            "Global.StaticConstructObject_Internal(FStaticConstructObjectParameters&)", params));
    if (!cm) return;

    cm->MyPCField() = pc;
    cm->InitCheatManager();

    auto& cmFieldRef = pc->CheatManagerField();
    UPTRINT* cmRawPtr = reinterpret_cast<UPTRINT*>(&cmFieldRef);
    const UPTRINT savedCMPtr = *cmRawPtr;
    *cmRawPtr = reinterpret_cast<UPTRINT>(cm);

    const bool wasAdmin = pc->bIsAdmin()();
    if (!wasAdmin)
        pc->bIsAdmin() = true;

    cm->GiveEngramsTekOnly();

    if (!wasAdmin)
        pc->bIsAdmin() = false;

    *cmRawPtr = savedCMPtr;
    cm->ConditionalBeginDestroy();
}

static void GiveItem(AShooterPlayerController* pc, UPrimalInventoryComponent* inv, const ItemEntry& it)
{
    if (!inv) return;

    const FString fPath(Utf8ToWide(it.BlueprintPath).c_str());
    UClass* itemClass = UVictoryCore::BPLoadClass(fPath);
    if (!itemClass)
    {
        Log::GetLog()->warn("[InstaKits] Item BPLoadClass failed for '{}'", it.BlueprintPath);
        return;
    }

    TSubclassOf<UPrimalItem> itemSub = itemClass;
    TSubclassOf<UPrimalItem> noSkin{};
    UPrimalItem* added = UPrimalItem::AddNewItem(
        itemSub, inv, false, false, it.Quality, false, it.Quantity, false, 0.0f, true,
        noSkin, 0.0f, false, true, true, true, true, false, AsaApi::GetApiUtils().GetWorld());

    if (!added)
    {
        Log::GetLog()->warn("[InstaKits] AddNewItem returned null for '{}'", it.BlueprintPath);
        return;
    }

    if (it.TierIndex >= 0)
        added->ItemQualityIndexField() = (unsigned char)it.TierIndex;

    if (it.ClipAmmo >= 0)
    {
        added->WeaponClipAmmoField() = it.ClipAmmo;
        inv->UpdateNetWeaponClipAmmo(added, it.ClipAmmo);
    }

    if (it.SlotIndex >= 0)
    {
        const FItemNetID id = added->ItemIDField();
        inv->ServerAddItemToSlot(id, it.SlotIndex, true);
    }

    if (it.Equip)
    {
        FItemNetID id = added->ItemIDField();
        inv->ServerEquipItem(id, pc);
    }
}

static void GiveKit(AShooterPlayerController* pc, const Kit& kit, const KitLayout* layout)
{
    if (!pc) return;

    AShooterCharacter* ch = pc->BaseGetPlayerCharacter();
    if (!ch) return;

    UPrimalInventoryComponent* inv = ch->MyInventoryComponentField();
    if (!inv) return;

    if (g_unlock_tek_engrams)
        UnlockTekEngrams(pc);

    for (const auto& it : kit.Items)
    {
        ItemEntry effective = it;

        if (layout)
        {
            const auto found = layout->find(it.BlueprintPath);
            if (found != layout->end())
            {
                if (!found->second.Have) continue;
                effective.SlotIndex = found->second.SlotIndex;
                effective.Equip = found->second.Equip;
            }
        }

        GiveItem(pc, inv, effective);
    }
}

static void TryDeliver(std::string eos, int attempt)
{
    const FString fEos(Utf8ToWide(eos).c_str());
    AShooterPlayerController* pc = AsaApi::GetApiUtils().FindPlayerFromEOSID(fEos);
    if (!pc) return;

    std::vector<std::string> groups;
    if (!GetGroups(eos, groups)) return;

    const Kit* kit = ResolveKit(groups);
    if (!kit) return;

    const std::string key = CacheKey(eos, kit->Group);

    KitLayout layout;
    const bool haveLayout = TryGetLayoutCache(key, layout);
    const KitLayout* layoutPtr = haveLayout ? &layout : nullptr;

    if (kit->Recurring)
    {
        GiveKit(pc, *kit, layoutPtr);
        return;
    }

    bool claimed = false;
    if (!TryGetClaimCache(key, claimed))
    {
        if (attempt < g_claim_retry_limit)
            API::Timer::Get().DelayExecute(&TryDeliver, 1, eos, attempt + 1);
        else
            Log::GetLog()->warn("[InstaKits] Claim lookup timed out for {} group {}", eos, kit->Group);
        return;
    }

    if (claimed) return;

    GiveKit(pc, *kit, layoutPtr);
    SetClaimCache(key, true);

    DbTask task;
    task.Type = DbTaskType::RecordClaim;
    task.Eos = eos;
    task.Group = kit->Group;
    task.ClaimedAt = (long long)std::time(nullptr);
    QueueDbTask(task);
}

static void Reply(AShooterPlayerController* pc, const std::wstring& msg)
{
    const FString fSender(L"InstaKits");
    const FString fMsg(msg.c_str());
    AsaApi::GetApiUtils().SendChatMessage(pc, fSender, L"{}", std::wstring_view(*fMsg));
}

static const Kit* ResolveKitForPlayer(AShooterPlayerController* pc, std::string& outEos)
{
    outEos = GetEos(pc);
    if (outEos.empty()) return nullptr;

    LoadPermissionsAPI();

    std::vector<std::string> groups;
    if (!GetGroups(outEos, groups)) return nullptr;

    return ResolveKit(groups);
}

static void SaveKitCommand(AShooterPlayerController* pc, FString*, int, int)
{
    if (!pc) return;

    if (!g_allow_savekit)
    {
        Reply(pc, L"Saving kit layouts is not enabled on this server.");
        return;
    }

    std::string eos;
    const Kit* kit = ResolveKitForPlayer(pc, eos);
    if (!kit)
    {
        Reply(pc, L"You do not have a spawn kit to save.");
        return;
    }

    const long long now = (long long)std::time(nullptr);
    const auto cd = g_savekit_cooldowns.find(eos);
    if (cd != g_savekit_cooldowns.end() && now < cd->second)
    {
        Reply(pc, L"You are saving too quickly, please wait a moment.");
        return;
    }

    AShooterCharacter* ch = pc->BaseGetPlayerCharacter();
    if (!ch) return;

    UPrimalInventoryComponent* inv = ch->MyInventoryComponentField();
    if (!inv) return;

    auto& equipped = inv->EquippedItemsField();
    auto& backpack = inv->InventoryItemsField();

    KitLayout layout;
    nlohmann::json j = nlohmann::json::object();

    for (const auto& it : kit->Items)
    {
        const FString fPath(Utf8ToWide(it.BlueprintPath).c_str());
        UClass* cls = UVictoryCore::BPLoadClass(fPath);
        if (!cls) continue;

        LayoutEntry entry;
        bool found = false;

        for (int i = 0; i < equipped.Num() && !found; ++i)
        {
            UPrimalItem* item = equipped[i];
            if (!item || item->ClassField() != cls) continue;
            entry.Equip = true;
            entry.SlotIndex = -1;
            found = true;
        }

        for (int i = 0; i < backpack.Num() && !found; ++i)
        {
            UPrimalItem* item = backpack[i];
            if (!item || item->ClassField() != cls) continue;

            entry.Equip = item->bEquippedItem()();
            entry.SlotIndex = entry.Equip ? -1 : item->SlotIndexField();
            if (entry.SlotIndex < 0 || entry.SlotIndex > 9) entry.SlotIndex = -1;
            found = true;
        }

        entry.Have = found;
        if (!found)
        {
            entry.SlotIndex = -1;
            entry.Equip = false;
        }

        layout[it.BlueprintPath] = entry;
        j[it.BlueprintPath] = nlohmann::json{
            { "Have", entry.Have },
            { "Slot", entry.SlotIndex },
            { "Equip", entry.Equip } };
    }

    const std::string key = CacheKey(eos, kit->Group);
    SetLayoutCache(key, layout);

    DbTask task;
    task.Type = DbTaskType::SaveLayout;
    task.Eos = eos;
    task.Group = kit->Group;
    task.Payload = j.dump();
    task.ClaimedAt = now;
    QueueDbTask(task);

    g_savekit_cooldowns[eos] = now + g_savekit_cooldown;

    Reply(pc, L"Kit layout saved. Your gear will spawn arranged this way.");
}

static void ResetKitCommand(AShooterPlayerController* pc, FString*, int, int)
{
    if (!pc) return;

    if (!g_allow_savekit)
    {
        Reply(pc, L"Saving kit layouts is not enabled on this server.");
        return;
    }

    std::string eos;
    const Kit* kit = ResolveKitForPlayer(pc, eos);
    if (!kit)
    {
        Reply(pc, L"You do not have a spawn kit to reset.");
        return;
    }

    const long long now = (long long)std::time(nullptr);
    const auto cd = g_savekit_cooldowns.find(eos);
    if (cd != g_savekit_cooldowns.end() && now < cd->second)
    {
        Reply(pc, L"You are doing that too quickly, please wait a moment.");
        return;
    }

    const std::string key = CacheKey(eos, kit->Group);
    SetLayoutCache(key, KitLayout());

    DbTask task;
    task.Type = DbTaskType::DeleteLayout;
    task.Eos = eos;
    task.Group = kit->Group;
    QueueDbTask(task);

    g_savekit_cooldowns[eos] = now + g_savekit_cooldown;

    Reply(pc, L"Kit layout reset to the server default.");
}

using HandleRespawned_t = void(*)(AShooterPlayerController*, APawn*, bool);
static HandleRespawned_t Original_HandleRespawned = nullptr;

static void Detour_HandleRespawned(AShooterPlayerController* pc, APawn* pawn, bool bNewPlayer)
{
    Original_HandleRespawned(pc, pawn, bNewPlayer);

    if (!pc || !pawn) return;

    LoadPermissionsAPI();

    const std::string eos = GetEos(pc);
    if (eos.empty()) return;

    std::vector<std::string> groups;
    if (!GetGroups(eos, groups)) return;

    const Kit* kit = ResolveKit(groups);
    if (!kit) return;

    const std::string key = CacheKey(eos, kit->Group);

    if (g_allow_savekit && !HasLayoutCache(key))
    {
        DbTask layoutTask;
        layoutTask.Type = DbTaskType::LoadLayout;
        layoutTask.Eos = eos;
        layoutTask.Group = kit->Group;
        QueueDbTask(layoutTask);
    }

    if (!kit->Recurring && !HasClaimCache(key))
    {
        DbTask task;
        task.Type = DbTaskType::CheckClaim;
        task.Eos = eos;
        task.Group = kit->Group;
        QueueDbTask(task);
    }

    API::Timer::Get().DelayExecute(&TryDeliver, g_deliver_delay, eos, 0);
}

static void PluginInit()
{
    Log::Get().Init("InstaKits");

    if (!LoadConfig(true))
        Log::GetLog()->error("[InstaKits] Failed to load config");

    g_worker_running.store(true);
    g_worker_thread = std::thread(&DbWorkerLoop);

    AsaApi::GetHooks().SetHook(
        "AShooterPlayerController.HandleRespawned_Implementation(APawn*,bool)",
        (LPVOID)&Detour_HandleRespawned,
        &Original_HandleRespawned);

    API::Timer::Get().RecurringExecute(FString(g_reload_timer_id), &CheckConfigReload, 10, -1, false);

    AsaApi::GetCommands().AddChatCommand(FString(L"/savekit"), &SaveKitCommand);
    AsaApi::GetCommands().AddChatCommand(FString(L"/resetkit"), &ResetKitCommand);


    Log::GetLog()->info("[InstaKits] Loaded");
}

static void PluginUnload()
{
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/savekit"));
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/resetkit"));

    AsaApi::GetHooks().DisableHook(
        "AShooterPlayerController.HandleRespawned_Implementation(APawn*,bool)",
        (LPVOID)&Detour_HandleRespawned);

    API::Timer::Get().UnloadAllTimers();

    g_worker_running.store(false);
    g_queue_cv.notify_all();
    if (g_worker_thread.joinable())
        g_worker_thread.join();

    Log::GetLog()->info("[InstaKits] Unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { PluginInit(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->critical("[InstaKits] Init exception: {}", e.what());
    }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { PluginUnload(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->error("[InstaKits] Unload exception: {}", e.what());
    }
}