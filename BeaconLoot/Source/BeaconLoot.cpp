/*
BeaconLoot - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * BeaconLoot - ASA Plugin
 *
 * Replaces supply crate contents with configured loot tables.
 *
 * Hooks:
 *   APrimalStructureItemContainer_SupplyCrate.GenerateCrateItems()  replaces vanilla crate generation
 *
 * Config:
 *   ArkApi/Plugins/BeaconLoot/config.json
 *   Crates: map name to crate asset name to LootSets key. Resolves by substring match against the
 *           running map name, falling back to the "default" key. Crate asset names are matched
 *           case insensitively with any _C suffix stripped, so SupplyCrate_Level03 matches
 *           SupplyCrate_Level03_C and never matches SupplyCrate_Level03_Double_C.
 *   LootSets: named reusable tables, each holding SetsPerCrate, QualityMultiplier and Entries.
 *   SetsPerCrate: number of weighted rolls per crate. Rolls are with replacement, so one entry can
 *                 win more than once. Guaranteed entries always spawn and do not consume a roll.
 *   QualityMultiplier: scalar applied to every rolled quality in the set.
 *   Entries: ItemPath, MinQuantity, MaxQuantity, MinQuality, MaxQuality, Weight, Guaranteed,
 *            IsBlueprint, BlueprintChance. Quantity is per winning roll, not per crate.
 *   LogUnmatchedCrates: logs the asset name of any crate with no table, for discovering names
 *
 * Config Example:
 * {
 *   "LogUnmatchedCrates": false,
 *   "LootSets": {
 *     "WhiteSurface": {
 *       "SetsPerCrate": 3,
 *       "QualityMultiplier": 1.0,
 *       "Entries": [
 *         {
 *           "ItemPath": "Blueprint'/Game/PrimalEarth/CoreBlueprints/Resources/PrimalItemResource_Stone.PrimalItemResource_Stone'",
 *           "MinQuantity": 50,
 *           "MaxQuantity": 200,
 *           "MinQuality": 0.0,
 *           "MaxQuality": 0.0,
 *           "Weight": 0.0,
 *           "Guaranteed": true,
 *           "IsBlueprint": false,
 *           "BlueprintChance": 0.0
 *         },
 *         {
 *           "ItemPath": "Blueprint'/Game/PrimalEarth/CoreBlueprints/Weapons/PrimalItem_WeaponPike.PrimalItem_WeaponPike'",
 *           "MinQuantity": 1,
 *           "MaxQuantity": 1,
 *           "MinQuality": 0.5,
 *           "MaxQuality": 2.5,
 *           "Weight": 10.0,
 *           "Guaranteed": false,
 *           "IsBlueprint": false,
 *           "BlueprintChance": 15.0
 *         }
 *       ]
 *     }
 *   },
 *   "Crates": {
 *     "default": {
 *       "SupplyCrate_Level03": "WhiteSurface"
 *     }
 *   }
 * }
 *
 * Crates absent from the resolved table fall through to vanilla generation untouched. Matched
 * crates skip vanilla generation entirely and are filled from the configured table instead, so no
 * crate field is written and no item is ever removed. Both of those were shown during development
 * to make the crate self destruct, since a supply crate carries its own UI entries in the same
 * inventory array as its loot. Crate lifetime and expiry are unaffected.
 *
 * Item classes resolve lazily on first use and are cached per path, so the asset lookup happens
 * once rather than on every crate. Anything that fails to resolve, or resolves to something that
 * is not a UPrimalItem subclass, is logged and disabled. Failures are cached as well so a bad path
 * does not retry on every crate, and the whole cache is dropped on config reload so a corrected
 * path is picked up. Config reloads on a 10 second file size and last-write-time check and a
 * rejected config leaves the last good tables in place.
 */

#include <API/ARK/Ark.h>
#include <json.hpp>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <random>
#include <cctype>
#include <sys/stat.h>

#pragma comment(lib, "AsaApi")
#pragma warning(disable: 4191)

static const std::string g_config_path = "ArkApi/Plugins/BeaconLoot/config.json";

struct LootEntry
{
    std::string ItemPath;
    int MinQuantity = 1;
    int MaxQuantity = 1;
    float MinQuality = 0.0f;
    float MaxQuality = 0.0f;
    float Weight = 0.0f;
    bool Guaranteed = false;
    bool IsBlueprint = false;
    float BlueprintChance = 0.0f;
};

struct LootSet
{
    int SetsPerCrate = 0;
    float QualityMultiplier = 1.0f;
    std::vector<LootEntry> Entries;
};

using CrateTable = std::unordered_map<std::string, std::string>;

static std::unordered_map<std::string, LootSet> g_lootSets;
static std::unordered_map<std::string, CrateTable> g_crates;
static std::unordered_map<std::string, UClass*> g_classCache;
static bool g_logUnmatched = false;

static time_t g_config_last_modified = 0;
static long long g_config_last_size = 0;
static int g_timer_ticks = 0;

static std::mt19937& Rng()
{
    static std::mt19937 rng(std::random_device{}());
    return rng;
}

static std::string FStr(const FString& f)
{
    if (f.Len() == 0) return "";
    return std::string(TCHAR_TO_UTF8(*f));
}

static std::string ToLower(const std::string& in)
{
    std::string out = in;
    for (char& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

static time_t GetFileModTime(const std::string& path)
{
    struct _stat st{};
    if (_stat(path.c_str(), &st) == 0)
        return st.st_mtime;
    return 0;
}

static long long GetFileSize(const std::string& path)
{
    struct _stat st{};
    if (_stat(path.c_str(), &st) == 0)
        return st.st_size;
    return 0;
}

static std::string GetMap()
{
    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return "";
    FString name;
    world->GetMapName(&name);
    return ToLower(FStr(name));
}

static std::string ExtractAssetName(const std::string& bpPath)
{
    std::string s = bpPath;

    while (!s.empty() && (s.back() == '\'' || s.back() == '"'))
        s.pop_back();

    const size_t dot = s.find_last_of('.');
    if (dot != std::string::npos)
        s = s.substr(dot + 1);

    if (s.size() > 2 && s.compare(s.size() - 2, 2, "_C") == 0)
        s = s.substr(0, s.size() - 2);

    return ToLower(s);
}

static UClass* ResolveItemClass(const std::string& path)
{
    auto cached = g_classCache.find(path);
    if (cached != g_classCache.end())
        return cached->second;

    FString f(path.c_str());
    UClass* itemClass = UVictoryCore::BPLoadClass(f);

    if (!itemClass)
    {
        Log::GetLog()->error("[BeaconLoot] BPLoadClass failed for '{}', entry disabled", path);
        g_classCache[path] = nullptr;
        return nullptr;
    }

    UClass* itemBase = UPrimalItem::StaticClass();
    if (!itemBase || !itemClass->IsChildOf(itemBase))
    {
        Log::GetLog()->error("[BeaconLoot] '{}' is not a UPrimalItem, entry disabled", path);
        g_classCache[path] = nullptr;
        return nullptr;
    }

    g_classCache[path] = itemClass;
    return itemClass;
}

static bool LoadConfig()
{
    std::ifstream file(g_config_path);
    if (!file.is_open())
    {
        Log::GetLog()->error("[BeaconLoot] Cannot open {}", g_config_path);
        return false;
    }

    nlohmann::json cfg;
    try
    {
        file >> cfg;
    }
    catch (const std::exception& e)
    {
        Log::GetLog()->error("[BeaconLoot] Config parse failed: {}", e.what());
        return false;
    }
    file.close();

    std::unordered_map<std::string, LootSet> newSets;
    std::unordered_map<std::string, CrateTable> newCrates;
    bool newLogUnmatched = false;

    if (cfg.contains("LogUnmatchedCrates") && cfg["LogUnmatchedCrates"].is_boolean())
        newLogUnmatched = cfg["LogUnmatchedCrates"].get<bool>();

    if (!cfg.contains("LootSets") || !cfg["LootSets"].is_object())
    {
        Log::GetLog()->error("[BeaconLoot] LootSets missing or not an object");
        return false;
    }

    for (auto it = cfg["LootSets"].begin(); it != cfg["LootSets"].end(); ++it)
    {
        const std::string setName = it.key();
        const nlohmann::json& node = it.value();

        if (!node.is_object())
        {
            Log::GetLog()->error("[BeaconLoot] LootSet '{}' is not an object", setName);
            return false;
        }

        LootSet set;

        if (node.contains("SetsPerCrate") && node["SetsPerCrate"].is_number_integer())
            set.SetsPerCrate = node["SetsPerCrate"].get<int>();

        if (node.contains("QualityMultiplier") && node["QualityMultiplier"].is_number())
            set.QualityMultiplier = node["QualityMultiplier"].get<float>();

        if (!node.contains("Entries") || !node["Entries"].is_array())
        {
            Log::GetLog()->error("[BeaconLoot] LootSet '{}' has no Entries array", setName);
            return false;
        }

        for (const auto& e : node["Entries"])
        {
            if (!e.is_object() || !e.contains("ItemPath") || !e["ItemPath"].is_string())
            {
                Log::GetLog()->error("[BeaconLoot] LootSet '{}' has an entry with no ItemPath", setName);
                return false;
            }

            LootEntry entry;
            entry.ItemPath = e["ItemPath"].get<std::string>();

            if (entry.ItemPath.empty())
            {
                Log::GetLog()->error("[BeaconLoot] LootSet '{}' has an empty ItemPath", setName);
                return false;
            }

            if (e.contains("MinQuantity") && e["MinQuantity"].is_number_integer())
                entry.MinQuantity = e["MinQuantity"].get<int>();
            if (e.contains("MaxQuantity") && e["MaxQuantity"].is_number_integer())
                entry.MaxQuantity = e["MaxQuantity"].get<int>();
            if (e.contains("MinQuality") && e["MinQuality"].is_number())
                entry.MinQuality = e["MinQuality"].get<float>();
            if (e.contains("MaxQuality") && e["MaxQuality"].is_number())
                entry.MaxQuality = e["MaxQuality"].get<float>();
            if (e.contains("Weight") && e["Weight"].is_number())
                entry.Weight = e["Weight"].get<float>();
            if (e.contains("Guaranteed") && e["Guaranteed"].is_boolean())
                entry.Guaranteed = e["Guaranteed"].get<bool>();
            if (e.contains("IsBlueprint") && e["IsBlueprint"].is_boolean())
                entry.IsBlueprint = e["IsBlueprint"].get<bool>();
            if (e.contains("BlueprintChance") && e["BlueprintChance"].is_number())
                entry.BlueprintChance = e["BlueprintChance"].get<float>();

            if (entry.MinQuantity < 1) entry.MinQuantity = 1;
            if (entry.MaxQuantity < entry.MinQuantity) entry.MaxQuantity = entry.MinQuantity;
            if (entry.MinQuality < 0.0f) entry.MinQuality = 0.0f;
            if (entry.MaxQuality < entry.MinQuality) entry.MaxQuality = entry.MinQuality;
            if (entry.Weight < 0.0f) entry.Weight = 0.0f;
            if (entry.BlueprintChance < 0.0f) entry.BlueprintChance = 0.0f;
            if (entry.BlueprintChance > 100.0f) entry.BlueprintChance = 100.0f;

            set.Entries.push_back(entry);
        }

        if (set.Entries.empty())
        {
            Log::GetLog()->error("[BeaconLoot] LootSet '{}' has no entries", setName);
            return false;
        }

        newSets[setName] = set;
    }

    if (!cfg.contains("Crates") || !cfg["Crates"].is_object())
    {
        Log::GetLog()->error("[BeaconLoot] Crates missing or not an object");
        return false;
    }

    for (auto mapIt = cfg["Crates"].begin(); mapIt != cfg["Crates"].end(); ++mapIt)
    {
        if (!mapIt.value().is_object())
        {
            Log::GetLog()->error("[BeaconLoot] Crates entry '{}' is not an object", mapIt.key());
            return false;
        }

        CrateTable table;
        for (auto crateIt = mapIt.value().begin(); crateIt != mapIt.value().end(); ++crateIt)
        {
            if (!crateIt.value().is_string())
            {
                Log::GetLog()->error("[BeaconLoot] Crate '{}' does not name a LootSets key", crateIt.key());
                return false;
            }

            const std::string setName = crateIt.value().get<std::string>();
            if (newSets.find(setName) == newSets.end())
            {
                Log::GetLog()->error("[BeaconLoot] Crate '{}' references unknown LootSet '{}'", crateIt.key(), setName);
                return false;
            }

            table[ToLower(crateIt.key())] = setName;
        }

        newCrates[ToLower(mapIt.key())] = table;
    }

    g_lootSets = std::move(newSets);
    g_crates = std::move(newCrates);
    g_classCache.clear();
    g_logUnmatched = newLogUnmatched;

    g_config_last_modified = GetFileModTime(g_config_path);
    g_config_last_size = GetFileSize(g_config_path);

    Log::GetLog()->info("[BeaconLoot] Config loaded, {} loot sets, {} map tables, log unmatched {}",
        g_lootSets.size(), g_crates.size(), g_logUnmatched);

    if (g_crates.find("default") == g_crates.end())
        Log::GetLog()->warn("[BeaconLoot] No default crate table, maps with no match use vanilla loot");

    return true;
}

static void CheckConfigReload()
{
    const long long sz = GetFileSize(g_config_path);
    if (sz == 0) return;
    const time_t mt = GetFileModTime(g_config_path);
    if (mt == g_config_last_modified && sz == g_config_last_size) return;
    LoadConfig();
}

static void OnTimer()
{
    if (++g_timer_ticks < 10) return;
    g_timer_ticks = 0;
    CheckConfigReload();
}

static const CrateTable* ResolveCrateTable()
{
    const std::string map = GetMap();

    if (!map.empty())
    {
        for (const auto& kv : g_crates)
        {
            if (kv.first == "default") continue;
            if (map.find(kv.first) != std::string::npos)
                return &kv.second;
        }
    }

    auto def = g_crates.find("default");
    if (def != g_crates.end())
        return &def->second;

    return nullptr;
}

static const LootSet* ResolveLootSet(const std::string& assetName)
{
    const CrateTable* table = ResolveCrateTable();
    if (!table) return nullptr;

    auto it = table->find(assetName);
    if (it == table->end()) return nullptr;

    auto set = g_lootSets.find(it->second);
    if (set == g_lootSets.end()) return nullptr;

    return &set->second;
}

static void GiveEntry(UPrimalInventoryComponent* inv, const LootEntry& entry, float qualityMul)
{
    if (!inv) return;

    UClass* itemClass = ResolveItemClass(entry.ItemPath);
    if (!itemClass) return;

    UWorld* world = AsaApi::GetApiUtils().GetWorld();

    int quantity = entry.MinQuantity;
    if (entry.MaxQuantity > entry.MinQuantity)
    {
        std::uniform_int_distribution<int> qd(entry.MinQuantity, entry.MaxQuantity);
        quantity = qd(Rng());
    }
    if (quantity < 1) quantity = 1;

    float quality = entry.MinQuality;
    if (entry.MaxQuality > entry.MinQuality)
    {
        std::uniform_real_distribution<float> ql(entry.MinQuality, entry.MaxQuality);
        quality = ql(Rng());
    }
    quality *= qualityMul;
    if (quality < 0.0f) quality = 0.0f;

    bool isBlueprint = entry.IsBlueprint;
    if (!isBlueprint && entry.BlueprintChance > 0.0f)
    {
        std::uniform_real_distribution<float> roll(0.0f, 100.0f);
        isBlueprint = roll(Rng()) < entry.BlueprintChance;
    }

    int stack = quantity;
    UPrimalItem* cdo = static_cast<UPrimalItem*>(itemClass->GetDefaultObject(true));
    if (cdo)
    {
        const int maxStack = cdo->GetMaxItemQuantity(reinterpret_cast<UObject*>(world));
        if (maxStack > 0 && maxStack < stack) stack = maxStack;
    }
    if (stack < 1) stack = 1;

    TSubclassOf<UPrimalItem> itemSub = itemClass;
    TSubclassOf<UPrimalItem> noSkin{};

    int remaining = quantity;

    for (int guard = 0; guard < 256 && remaining > 0; ++guard)
    {
        const int give = remaining > stack ? stack : remaining;

        UPrimalItem* result = UPrimalItem::AddNewItem(
            itemSub, inv, false, true, quality, !isBlueprint, give, isBlueprint, 0.0f, true,
            noSkin, 0.0f, false, true, true, false, true, false, world);

        if (!result)
        {
            Log::GetLog()->warn("[BeaconLoot] AddNewItem returned null for '{}', {} undelivered",
                entry.ItemPath, remaining);
            return;
        }

        remaining -= give;
    }
}

static void FillCrate(UPrimalInventoryComponent* inv, const LootSet& set)
{
    std::vector<const LootEntry*> pool;
    float totalWeight = 0.0f;

    for (const LootEntry& e : set.Entries)
    {
        if (e.Guaranteed)
        {
            GiveEntry(inv, e, set.QualityMultiplier);
            continue;
        }

        if (e.Weight <= 0.0f) continue;

        pool.push_back(&e);
        totalWeight += e.Weight;
    }

    if (set.SetsPerCrate <= 0 || pool.empty() || totalWeight <= 0.0f) return;

    std::uniform_real_distribution<float> pick(0.0f, totalWeight);

    for (int i = 0; i < set.SetsPerCrate; ++i)
    {
        float target = pick(Rng());

        for (const LootEntry* e : pool)
        {
            target -= e->Weight;
            if (target <= 0.0f)
            {
                GiveEntry(inv, *e, set.QualityMultiplier);
                break;
            }
        }
    }
}

using GenerateCrateItems_t = void(*)(APrimalStructureItemContainer_SupplyCrate*);
static GenerateCrateItems_t Original_GenerateCrateItems = nullptr;

static void Detour_GenerateCrateItems(APrimalStructureItemContainer_SupplyCrate* crate)
{
    if (!crate)
    {
        Original_GenerateCrateItems(crate);
        return;
    }

    const std::string assetName = ExtractAssetName(FStr(AsaApi::GetApiUtils().GetBlueprint(crate)));

    const LootSet* set = ResolveLootSet(assetName);
    if (!set)
    {
        Original_GenerateCrateItems(crate);

        if (g_logUnmatched)
            Log::GetLog()->info("[BeaconLoot] Unmatched crate '{}'", assetName);

        return;
    }

    if (crate->bGeneratedCrateItems()()) return;

    UPrimalInventoryComponent* inv = crate->MyInventoryComponentField();
    if (!inv)
    {
        Original_GenerateCrateItems(crate);
        return;
    }

    FillCrate(inv, *set);

    crate->bGeneratedCrateItems() = true;
}

static void PluginInit()
{
    Log::Get().Init("BeaconLoot");

    if (!LoadConfig())
        Log::GetLog()->error("[BeaconLoot] Failed to load config, all crates use vanilla loot");

    AsaApi::GetHooks().SetHook(
        "APrimalStructureItemContainer_SupplyCrate.GenerateCrateItems()",
        &Detour_GenerateCrateItems,
        &Original_GenerateCrateItems);

    AsaApi::GetCommands().AddOnTimerCallback(FString(L"BeaconLoot_ConfigCheck"), &OnTimer);

    Log::GetLog()->info("[BeaconLoot] Loaded");
}

static void PluginUnload()
{
    AsaApi::GetHooks().DisableHook(
        "APrimalStructureItemContainer_SupplyCrate.GenerateCrateItems()",
        &Detour_GenerateCrateItems);

    AsaApi::GetCommands().RemoveOnTimerCallback(FString(L"BeaconLoot_ConfigCheck"));

    g_lootSets.clear();
    g_crates.clear();
    g_classCache.clear();

    Log::GetLog()->info("[BeaconLoot] Unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { PluginInit(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->critical("[BeaconLoot] Init exception: {}", e.what());
    }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { PluginUnload(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->error("[BeaconLoot] Unload exception: {}", e.what());
    }
}