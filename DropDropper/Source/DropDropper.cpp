/*
DropDropper - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * DropDropper - ASA Plugin
 *
 * Hooks:
 *   None. Chat commands and a 10 second config hot reload timer only.
 *
 * Config:
 *   ArkApi/Plugins/DropDropper/config.json
 *   Whitelist       array of EOS IDs allowed to use every command
 *   Drops           map name to (colour to _C class name) for /drop single crates
 *   CaveDrops       map name to (colour to _C class name) for /dropc single crates
 *   MassDrops       map name to (colour to blueprint path) for /massdrop and /dropparty
 *   CaveMassDrops   map name to (colour to blueprint path) for /massdropc and /droppartyc
 *   PartyPool       colours /dropparty draws from, filtered against the resolved MassDrops table
 *   CavePartyPool   colours /droppartyc draws from, filtered against the resolved CaveMassDrops table
 *   MassDropMax     max crates per spread command
 *   MassDropSpread  scatter radius for spread commands
 *
 * Config Example:
 * {
 *     "Whitelist": [
 *         "EOS_ID"
 *     ],
 *     "Drops": {
 *         "default": {
 *             "white": "SupplyCrate_Level03_C",
 *             "whitering": "SupplyCrate_Level03_Double_C",
 *             "green": "SupplyCrate_Level15_C",
 *             "greenring": "SupplyCrate_Level15_Double_C",
 *             "blue": "SupplyCrate_Level25_C",
 *             "bluering": "SupplyCrate_Level25_Double_C",
 *             "purple": "SupplyCrate_Level35_C",
 *             "purplering": "SupplyCrate_Level35_Double_C",
 *             "yellow": "SupplyCrate_Level45_C",
 *             "yellowring": "SupplyCrate_Level45_Double_C",
 *             "red": "SupplyCrate_Level60_C",
 *             "redring": "SupplyCrate_Level60_Double_C"
 *         }
 *     },
 *     "CaveDrops": {
 *         "default": {
 *             "blue": "SupplyCrate_Cave_QualityTier1_C",
 *             "green": "SupplyCrate_Cave_QualityTier2_C",
 *             "purple": "SupplyCrate_Cave_QualityTier3_C",
 *             "red": "SupplyCrate_Cave_QualityTier4_C"
 *         }
 *     },
 *     "MassDrops": {
 *         "default": {
 *             "white": "/Game/PrimalEarth/Structures/SupplyCrate_Level03.SupplyCrate_Level03",
 *             "whitering": "/Game/PrimalEarth/Structures/SupplyCrate_Level03_Double.SupplyCrate_Level03_Double",
 *             "green": "/Game/PrimalEarth/Structures/SupplyCrate_Level15.SupplyCrate_Level15",
 *             "greenring": "/Game/PrimalEarth/Structures/SupplyCrate_Level15_Double.SupplyCrate_Level15_Double",
 *             "blue": "/Game/PrimalEarth/Structures/SupplyCrate_Level25.SupplyCrate_Level25",
 *             "bluering": "/Game/PrimalEarth/Structures/SupplyCrate_Level25_Double.SupplyCrate_Level25_Double",
 *             "purple": "/Game/PrimalEarth/Structures/SupplyCrate_Level35.SupplyCrate_Level35",
 *             "purplering": "/Game/PrimalEarth/Structures/SupplyCrate_Level35_Double.SupplyCrate_Level35_Double",
 *             "yellow": "/Game/PrimalEarth/Structures/SupplyCrate_Level45.SupplyCrate_Level45",
 *             "yellowring": "/Game/PrimalEarth/Structures/SupplyCrate_Level45_Double.SupplyCrate_Level45_Double",
 *             "red": "/Game/PrimalEarth/Structures/SupplyCrate_Level60.SupplyCrate_Level60",
 *             "redring": "/Game/PrimalEarth/Structures/SupplyCrate_Level60_Double.SupplyCrate_Level60_Double"
 *         }
 *     },
 *     "CaveMassDrops": {
 *         "default": {
 *             "blue": "/Game/PrimalEarth/Structures/SupplyCrate_Cave_QualityTier1.SupplyCrate_Cave_QualityTier1",
 *             "green": "/Game/PrimalEarth/Structures/SupplyCrate_Cave_QualityTier2.SupplyCrate_Cave_QualityTier2",
 *             "purple": "/Game/PrimalEarth/Structures/SupplyCrate_Cave_QualityTier3.SupplyCrate_Cave_QualityTier3",
 *             "red": "/Game/PrimalEarth/Structures/SupplyCrate_Cave_QualityTier4.SupplyCrate_Cave_QualityTier4"
 *         }
 *     },
 *     "PartyPool": [
 *         "white",
 *         "green",
 *         "blue",
 *         "purple",
 *         "purplering",
 *         "yellow",
 *         "yellowring",
 *         "red",
 *         "redring"
 *     ],
 *     "CavePartyPool": [
 *         "blue",
 *         "green",
 *         "purple",
 *         "red"
 *     ],
 *     "MassDropMax": 30,
 *     "MassDropSpread": 8000.0
 * }
 *
 * Chat commands:
 *   /drop {keyword}              spawns one surface beacon on the caller
 *   /dropc {keyword}             spawns one cave beacon on the caller
 *   /massdrop {colour} {number}  spreads {number} surface beacons around the caller
 *   /massdropc {colour} {number} spreads {number} cave beacons around the caller
 *   /dropparty {number}          spreads {number} surface beacons split randomly across PartyPool
 *   /droppartyc {number}         spreads {number} cave beacons split randomly across CavePartyPool
 *
 * All four tables resolve by substring match against the running map name, falling back
 * to the "default" key. Single drop tables store _C class names used with Summon. Spread
 * tables store full blueprint paths used with SpawnActorSpread. Spawning runs a console
 * command on a temporary UShooterCheatManager swapped onto the caller's controller, with
 * bIsAdmin set only for the duration of the call and restored immediately after, so the
 * crate lands on the caller.
 */

#include <API/ARK/Ark.h>
#include <json.hpp>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <random>
#include <cctype>
#include <sys/stat.h>

#pragma comment(lib, "AsaApi")
#pragma warning(disable: 4191)

static const std::string g_config_path = "ArkApi/Plugins/DropDropper/config.json";

using ColorTable = std::unordered_map<std::string, std::string>;
using MapTables = std::unordered_map<std::string, ColorTable>;

static std::unordered_set<std::string> g_whitelist;
static MapTables g_drops;
static MapTables g_caveDrops;
static MapTables g_massDrops;
static MapTables g_caveMassDrops;
static std::vector<std::string> g_partyPool;
static std::vector<std::string> g_cavePartyPool;
static int g_massDropMax = 30;
static float g_massDropSpread = 300.0f;

static time_t g_config_last_modified = 0;
static long long g_config_last_size = 0;
static int g_timer_ticks = 0;

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

static std::wstring ToWide(const std::string& in)
{
    if (in.empty()) return std::wstring();
    const int len = MultiByteToWideChar(CP_UTF8, 0, in.c_str(), (int)in.size(), nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring out((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, in.c_str(), (int)in.size(), out.data(), len);
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

static std::string GetEos(AShooterPlayerController* pc)
{
    if (!pc) return "";
    AShooterPlayerState* ps = static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
    if (!ps) return "";
    FString raw;
    ps->GetUniqueNetIdAsString(&raw);
    return FStr(raw);
}

static std::string GetMap()
{
    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return "";
    FString m;
    world->GetMapName(&m);
    return ToLower(FStr(m));
}

static void Notify(AShooterPlayerController* pc, const std::wstring& msg)
{
    if (!pc) return;
    FString sender(L"DropDropper");
    AsaApi::GetApiUtils().SendChatMessage(pc, sender, L"{}", std::wstring_view(msg));
}

static std::vector<std::string> Tokenize(const std::string& raw)
{
    std::vector<std::string> tokens;
    std::string token;
    for (char c : raw)
    {
        if (c == ' ' || c == '\t')
        {
            if (!token.empty()) { tokens.push_back(token); token.clear(); }
        }
        else token += c;
    }
    if (!token.empty()) tokens.push_back(token);
    return tokens;
}

static bool ParseMapTables(const nlohmann::json& j, const char* key, MapTables& out)
{
    if (!j.contains(key)) return true;

    if (!j[key].is_object())
    {
        Log::GetLog()->error("[DropDropper] {} must be an object", key);
        return false;
    }

    for (auto& [mapKey, colors] : j[key].items())
    {
        if (!colors.is_object())
        {
            Log::GetLog()->error("[DropDropper] {}.{} must be an object", key, mapKey);
            return false;
        }

        ColorTable table;
        for (auto& [k, v] : colors.items())
        {
            if (!v.is_string() || v.get<std::string>().empty())
            {
                Log::GetLog()->error("[DropDropper] {}.{}.{} must be a non empty string", key, mapKey, k);
                return false;
            }
            table[ToLower(k)] = v.get<std::string>();
        }
        out[ToLower(mapKey)] = std::move(table);
    }
    return true;
}

static bool ParsePool(const nlohmann::json& j, const char* key, std::vector<std::string>& out)
{
    if (!j.contains(key)) return true;

    if (!j[key].is_array())
    {
        Log::GetLog()->error("[DropDropper] {} must be an array", key);
        return false;
    }

    for (auto& e : j[key])
    {
        if (!e.is_string() || e.get<std::string>().empty())
        {
            Log::GetLog()->error("[DropDropper] {} entries must be non empty strings", key);
            return false;
        }
        out.push_back(ToLower(e.get<std::string>()));
    }
    return true;
}

static bool LoadConfig()
{
    std::ifstream file(g_config_path);
    if (!file.is_open())
    {
        Log::GetLog()->error("[DropDropper] Cannot open config: {}", g_config_path);
        return false;
    }

    std::unordered_set<std::string> newWhitelist;
    MapTables newDrops;
    MapTables newCaveDrops;
    MapTables newMassDrops;
    MapTables newCaveMassDrops;
    std::vector<std::string> newPartyPool;
    std::vector<std::string> newCavePartyPool;
    int newMax = 30;
    float newSpread = 300.0f;

    try
    {
        nlohmann::json j;
        file >> j;

        if (j.contains("Whitelist"))
        {
            if (!j["Whitelist"].is_array())
            {
                Log::GetLog()->error("[DropDropper] Whitelist must be an array");
                return false;
            }
            for (auto& e : j["Whitelist"])
            {
                if (!e.is_string() || e.get<std::string>().empty())
                {
                    Log::GetLog()->error("[DropDropper] Whitelist entries must be non empty strings");
                    return false;
                }
                newWhitelist.insert(ToLower(e.get<std::string>()));
            }
        }

        if (!ParseMapTables(j, "Drops", newDrops)) return false;
        if (!ParseMapTables(j, "CaveDrops", newCaveDrops)) return false;
        if (!ParseMapTables(j, "MassDrops", newMassDrops)) return false;
        if (!ParseMapTables(j, "CaveMassDrops", newCaveMassDrops)) return false;
        if (!ParsePool(j, "PartyPool", newPartyPool)) return false;
        if (!ParsePool(j, "CavePartyPool", newCavePartyPool)) return false;

        newMax = j.value("MassDropMax", 30);
        newSpread = j.value("MassDropSpread", 300.0f);
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[DropDropper] Config parse error: {}", ex.what());
        return false;
    }

    if (newMax < 1)
    {
        Log::GetLog()->error("[DropDropper] MassDropMax must be 1 or more");
        return false;
    }

    if (newSpread <= 0.0f)
    {
        Log::GetLog()->error("[DropDropper] MassDropSpread must be greater than zero");
        return false;
    }

    g_whitelist = std::move(newWhitelist);
    g_drops = std::move(newDrops);
    g_caveDrops = std::move(newCaveDrops);
    g_massDrops = std::move(newMassDrops);
    g_caveMassDrops = std::move(newCaveMassDrops);
    g_partyPool = std::move(newPartyPool);
    g_cavePartyPool = std::move(newCavePartyPool);
    g_massDropMax = newMax;
    g_massDropSpread = newSpread;

    g_config_last_modified = GetFileModTime(g_config_path);
    g_config_last_size = GetFileSize(g_config_path);

    Log::GetLog()->info("[DropDropper] Config loaded, {} whitelisted, {} drop maps, {} cave drop maps, {} mass maps, {} cave mass maps, max {}, spread {}",
        g_whitelist.size(), g_drops.size(), g_caveDrops.size(), g_massDrops.size(), g_caveMassDrops.size(), g_massDropMax, g_massDropSpread);

    if (g_whitelist.empty())
        Log::GetLog()->warn("[DropDropper] Whitelist is empty, every command is denied");

    if (g_partyPool.empty())
        Log::GetLog()->warn("[DropDropper] PartyPool is empty, /dropparty is disabled");

    if (g_cavePartyPool.empty())
        Log::GetLog()->warn("[DropDropper] CavePartyPool is empty, /droppartyc is disabled");

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

static void OnTimer()
{
    if (++g_timer_ticks < 10) return;
    g_timer_ticks = 0;
    CheckConfigReload();
}

static void RunCheat(AShooterPlayerController* pc, const std::wstring& cmd)
{
    UClass* cmClass = UShooterCheatManager::StaticClass();
    if (!cmClass)
    {
        Log::GetLog()->error("[DropDropper] Failed to get UShooterCheatManager class");
        return;
    }

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
    if (!cm)
    {
        Log::GetLog()->error("[DropDropper] Failed to create cheat manager instance");
        return;
    }

    cm->MyPCField() = pc;
    cm->InitCheatManager();

    auto& cmFieldRef = pc->CheatManagerField();
    UPTRINT* cmRawPtr = reinterpret_cast<UPTRINT*>(&cmFieldRef);
    UPTRINT savedCMPtr = *cmRawPtr;
    *cmRawPtr = reinterpret_cast<UPTRINT>(cm);

    const bool wasAdmin = pc->bIsAdmin()();
    if (!wasAdmin)
        pc->bIsAdmin() = true;

    try
    {
        cm->ProcessConsoleExec(cmd.c_str(), nullptr, pc);
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[DropDropper] Console exec threw: {}", ex.what());
    }

    if (!wasAdmin)
        pc->bIsAdmin() = false;

    *cmRawPtr = savedCMPtr;
    cm->ConditionalBeginDestroy();
}

static bool Gate(AShooterPlayerController* pc, const std::string& commandWord, std::string& outEos)
{
    if (!pc) return false;

    outEos = GetEos(pc);
    if (outEos.empty()) return false;

    if (g_whitelist.find(ToLower(outEos)) == g_whitelist.end())
    {
        Notify(pc, L"You are not authorised to use this command.");
        Log::GetLog()->info("[DropDropper] Denied eos={} cmd={}", outEos, commandWord);
        return false;
    }

    if (!pc->BaseGetPlayerCharacter())
    {
        Notify(pc, L"You must be in game to drop.");
        return false;
    }

    return true;
}

static const ColorTable* ResolveMapTable(const MapTables& tables, const std::string& mapName)
{
    for (auto& [mapKey, colors] : tables)
    {
        if (mapKey == "default") continue;
        if (mapName.find(mapKey) != std::string::npos)
            return &colors;
    }

    auto def = tables.find("default");
    if (def != tables.end()) return &def->second;

    return nullptr;
}

static void HandleSingleDrop(AShooterPlayerController* pc,
    FString* message,
    const std::string& commandWord,
    const MapTables& tables)
{
    std::string eos;
    if (!Gate(pc, commandWord, eos)) return;

    const std::vector<std::string> tokens = Tokenize(message ? FStr(*message) : "");

    size_t idx = 0;
    if (!tokens.empty() && ToLower(tokens[0]) == commandWord) idx = 1;

    std::string keyword;
    if (idx < tokens.size()) keyword = ToLower(tokens[idx]);

    if (keyword.empty())
    {
        Notify(pc, L"Specify a colour. Example: /drop red");
        return;
    }

    const std::string mapName = GetMap();
    const ColorTable* table = ResolveMapTable(tables, mapName);

    if (!table)
    {
        Notify(pc, L"No drops configured for this map.");
        Log::GetLog()->info("[DropDropper] No table for map={} cmd={}", mapName, commandWord);
        return;
    }

    auto it = table->find(keyword);
    if (it == table->end())
    {
        Notify(pc, L"Unknown drop.");
        Log::GetLog()->info("[DropDropper] Unknown keyword '{}' map={} eos={} cmd={}", keyword, mapName, eos, commandWord);
        return;
    }

    const std::wstring cmd = L"Summon " + ToWide(it->second);
    RunCheat(pc, cmd);

    Log::GetLog()->info("[DropDropper] eos={} spawned {} map={} ({}) cmd={}", eos, it->second, mapName, keyword, commandWord);
}

static void SpawnSpread(AShooterPlayerController* pc, const std::string& path, int count)
{
    const std::wstring cmd =
        L"SpawnActorSpread \"Blueprint'" + ToWide(path) + L"'\" 0 0 0 " +
        std::to_wstring(count) + L" " + std::to_wstring(g_massDropSpread);
    RunCheat(pc, cmd);
}

static void SpawnGroundedSpread(AShooterPlayerController* pc, const std::string& path, int count)
{
    SpawnSpread(pc, path, count);
}

struct SpreadRequest
{
    const ColorTable* table = nullptr;
    std::string colour;
    std::string mapName;
    std::string eos;
    int count = 0;
};

static bool ResolveSpread(AShooterPlayerController* pc,
    FString* message,
    const std::string& commandWord,
    const MapTables& tables,
    bool needColour,
    const std::wstring& usage,
    SpreadRequest& out)
{
    if (!Gate(pc, commandWord, out.eos)) return false;

    const std::vector<std::string> tokens = Tokenize(message ? FStr(*message) : "");

    size_t idx = 0;
    if (!tokens.empty() && ToLower(tokens[0]) == commandWord) idx = 1;

    std::string numStr;
    if (needColour)
    {
        if (idx < tokens.size()) out.colour = ToLower(tokens[idx]);
        if (idx + 1 < tokens.size()) numStr = tokens[idx + 1];
        if (out.colour.empty() || numStr.empty())
        {
            Notify(pc, usage);
            return false;
        }
    }
    else
    {
        if (idx < tokens.size()) numStr = tokens[idx];
        if (numStr.empty())
        {
            Notify(pc, usage);
            return false;
        }
    }

    try { out.count = std::stoi(numStr); }
    catch (...) { out.count = 0; }

    if (out.count < 1)
    {
        Notify(pc, L"Number must be 1 or more.");
        return false;
    }

    if (out.count > g_massDropMax)
        out.count = g_massDropMax;

    out.mapName = GetMap();
    out.table = ResolveMapTable(tables, out.mapName);

    if (!out.table)
    {
        Notify(pc, L"No drops configured for this map.");
        Log::GetLog()->info("[DropDropper] No table for map={} cmd={}", out.mapName, commandWord);
        return false;
    }

    return true;
}

static void HandleMassDrop(AShooterPlayerController* pc,
    FString* message,
    const std::string& commandWord,
    const MapTables& tables,
    bool grounded)
{
    const std::wstring usage = L"Usage: " + ToWide(commandWord) + L" {colour} {number}";

    SpreadRequest req;
    if (!ResolveSpread(pc, message, commandWord, tables, true, usage, req)) return;

    auto it = req.table->find(req.colour);
    if (it == req.table->end())
    {
        Notify(pc, L"Unknown drop.");
        Log::GetLog()->info("[DropDropper] Unknown keyword '{}' map={} eos={} cmd={}", req.colour, req.mapName, req.eos, commandWord);
        return;
    }

    if (grounded)
        SpawnGroundedSpread(pc, it->second, req.count);
    else
        SpawnSpread(pc, it->second, req.count);

    Log::GetLog()->info("[DropDropper] eos={} spread {} x{} map={} ({}) cmd={}", req.eos, it->second, req.count, req.mapName, req.colour, commandWord);
}

static void HandleDropParty(AShooterPlayerController* pc,
    FString* message,
    const std::string& commandWord,
    const MapTables& tables,
    const std::vector<std::string>& pool,
    bool grounded)
{
    const std::wstring usage = L"Usage: " + ToWide(commandWord) + L" {number}";

    SpreadRequest req;
    if (!ResolveSpread(pc, message, commandWord, tables, false, usage, req)) return;

    std::vector<const std::string*> available;
    for (const std::string& key : pool)
    {
        if (req.table->find(key) != req.table->end())
            available.push_back(&key);
    }

    if (available.empty())
    {
        Notify(pc, L"No party drops configured for this map.");
        Log::GetLog()->info("[DropDropper] No party keys present map={} cmd={}", req.mapName, commandWord);
        return;
    }

    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<size_t> pick(0, available.size() - 1);

    std::unordered_map<std::string, int> tally;
    for (int i = 0; i < req.count; ++i)
        tally[*available[pick(rng)]]++;

    for (auto& [key, n] : tally)
    {
        if (n < 1) continue;

        const std::string& path = req.table->at(key);

        if (grounded)
            SpawnGroundedSpread(pc, path, n);
        else
            SpawnSpread(pc, path, n);

        Log::GetLog()->info("[DropDropper] eos={} party {} x{} map={} ({}) cmd={}", req.eos, path, n, req.mapName, key, commandWord);
    }

    Log::GetLog()->info("[DropDropper] eos={} party total={} map={} cmd={}", req.eos, req.count, req.mapName, commandWord);
}

static void Cmd_Drop(AShooterPlayerController* pc, FString* message, int, int)
{
    HandleSingleDrop(pc, message, "/drop", g_drops);
}

static void Cmd_DropCave(AShooterPlayerController* pc, FString* message, int, int)
{
    HandleSingleDrop(pc, message, "/dropc", g_caveDrops);
}

static void Cmd_MassDrop(AShooterPlayerController* pc, FString* message, int, int)
{
    HandleMassDrop(pc, message, "/massdrop", g_massDrops, false);
}

static void Cmd_MassDropCave(AShooterPlayerController* pc, FString* message, int, int)
{
    HandleMassDrop(pc, message, "/massdropc", g_caveMassDrops, true);
}

static void Cmd_DropParty(AShooterPlayerController* pc, FString* message, int, int)
{
    HandleDropParty(pc, message, "/dropparty", g_massDrops, g_partyPool, false);
}

static void Cmd_DropPartyCave(AShooterPlayerController* pc, FString* message, int, int)
{
    HandleDropParty(pc, message, "/droppartyc", g_caveMassDrops, g_cavePartyPool, true);
}

static void PluginInit()
{
    Log::Get().Init("DropDropper");

    if (!LoadConfig())
        Log::GetLog()->error("[DropDropper] Failed to load config, plugin will not function");

    AsaApi::GetCommands().AddChatCommand(FString(L"/drop"), &Cmd_Drop);
    AsaApi::GetCommands().AddChatCommand(FString(L"/dropc"), &Cmd_DropCave);
    AsaApi::GetCommands().AddChatCommand(FString(L"/massdrop"), &Cmd_MassDrop);
    AsaApi::GetCommands().AddChatCommand(FString(L"/massdropc"), &Cmd_MassDropCave);
    AsaApi::GetCommands().AddChatCommand(FString(L"/dropparty"), &Cmd_DropParty);
    AsaApi::GetCommands().AddChatCommand(FString(L"/droppartyc"), &Cmd_DropPartyCave);
    AsaApi::GetCommands().AddOnTimerCallback(FString(L"DropDropper_ConfigCheck"), &OnTimer);

    Log::GetLog()->info("[DropDropper] Loaded");
}

static void PluginUnload()
{
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/drop"));
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/dropc"));
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/massdrop"));
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/massdropc"));
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/dropparty"));
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/droppartyc"));
    AsaApi::GetCommands().RemoveOnTimerCallback(FString(L"DropDropper_ConfigCheck"));

    g_whitelist.clear();
    g_drops.clear();
    g_caveDrops.clear();
    g_massDrops.clear();
    g_caveMassDrops.clear();
    g_partyPool.clear();
    g_cavePartyPool.clear();

    Log::GetLog()->info("[DropDropper] Unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { PluginInit(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->critical("[DropDropper] Init exception: {}", e.what());
    }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { PluginUnload(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->error("[DropDropper] Unload exception: {}", e.what());
    }
}