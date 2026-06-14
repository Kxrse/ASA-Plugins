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
 * Hooks: none (chat commands only)
 *
 * Chat commands:
 *   /drop {keyword}            — summons a surface beacon on the caller
 *   /dropc {color}             — summons a cave beacon on the caller
 *   /massdrop {color} {number} — spreads {number} surface beacons around the caller
 *   /dropparty {number}        — spreads {number} surface beacons split randomly across yellow/purple/red and their rings
 *
 * Config:
 *   ArkApi/Plugins/DropDropper/config.json
 *   Whitelist      — array of EOS IDs allowed to use the commands
 *   Drops          — keyword to class-string map for /drop (Summon)
 *   CaveDrops      — keyword to class-string map for /dropc (Summon)
 *   MassDrops      — map to (color to full blueprint path) for /massdrop and /dropparty (SpawnActorSpread)
 *   MassDropMax    — max crates per /massdrop or /dropparty call
 *   MassDropSpread — SpawnActorSpread spread amount
 *   Hot-reloaded every 10 seconds via size + last-write-time check.
 *
 * Summon and SpawnActorSpread both run through a temporary UShooterCheatManager
 * swapped onto the caller's controller, so crates spawn at the caller.
 */

#include <API/ARK/Ark.h>
#include <json.hpp>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cctype>
#include <random>
#include <sys/stat.h>

#pragma comment(lib, "AsaApi.lib")
#pragma warning(disable: 4191)

static const std::string g_config_path = "ArkApi/Plugins/DropDropper/config.json";

static std::unordered_set<std::string> g_whitelist;
static std::unordered_map<std::string, std::string> g_drops;
static std::unordered_map<std::string, std::string> g_caveDrops;
static std::unordered_map<std::string, std::unordered_map<std::string, std::string>> g_massDrops;
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

static bool LoadConfig()
{
    std::ifstream file(g_config_path);
    if (!file.is_open())
    {
        Log::GetLog()->error("[DropDropper] Cannot open config: {}", g_config_path);
        return false;
    }

    try
    {
        nlohmann::json j;
        file >> j;

        std::unordered_set<std::string> newWhitelist;
        if (j.contains("Whitelist") && j["Whitelist"].is_array())
        {
            for (auto& e : j["Whitelist"])
            {
                if (e.is_string())
                    newWhitelist.insert(ToLower(e.get<std::string>()));
            }
        }

        std::unordered_map<std::string, std::string> newDrops;
        if (j.contains("Drops") && j["Drops"].is_object())
        {
            for (auto& [k, v] : j["Drops"].items())
            {
                if (v.is_string())
                    newDrops[ToLower(k)] = v.get<std::string>();
            }
        }

        std::unordered_map<std::string, std::string> newCaveDrops;
        if (j.contains("CaveDrops") && j["CaveDrops"].is_object())
        {
            for (auto& [k, v] : j["CaveDrops"].items())
            {
                if (v.is_string())
                    newCaveDrops[ToLower(k)] = v.get<std::string>();
            }
        }

        std::unordered_map<std::string, std::unordered_map<std::string, std::string>> newMassDrops;
        if (j.contains("MassDrops") && j["MassDrops"].is_object())
        {
            for (auto& [mapKey, colors] : j["MassDrops"].items())
            {
                if (!colors.is_object()) continue;
                std::unordered_map<std::string, std::string> colorMap;
                for (auto& [k, v] : colors.items())
                {
                    if (v.is_string())
                        colorMap[ToLower(k)] = v.get<std::string>();
                }
                newMassDrops[ToLower(mapKey)] = std::move(colorMap);
            }
        }

        int newMax = j.value("MassDropMax", 30);
        float newSpread = j.value("MassDropSpread", 300.0f);

        g_whitelist = std::move(newWhitelist);
        g_drops = std::move(newDrops);
        g_caveDrops = std::move(newCaveDrops);
        g_massDrops = std::move(newMassDrops);
        g_massDropMax = newMax > 0 ? newMax : 30;
        g_massDropSpread = newSpread > 0.0f ? newSpread : 300.0f;

        g_config_last_modified = GetFileModTime(g_config_path);
        g_config_last_size = GetFileSize(g_config_path);

        Log::GetLog()->info("[DropDropper] Config loaded — {} whitelisted, {} drops, {} cave, {} mass maps (max {}, spread {})",
            g_whitelist.size(), g_drops.size(), g_caveDrops.size(), g_massDrops.size(), g_massDropMax, g_massDropSpread);
        return true;
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[DropDropper] Config parse error: {}", ex.what());
        return false;
    }
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

    cm->ProcessConsoleExec(cmd.c_str(), nullptr, pc);

    if (!wasAdmin)
        pc->bIsAdmin() = false;

    *cmRawPtr = savedCMPtr;
    cm->ConditionalBeginDestroy();
}

static void HandleDrop(AShooterPlayerController* pc,
    FString* message,
    const std::string& commandWord,
    const std::unordered_map<std::string, std::string>& table)
{
    if (!pc) return;

    std::string eos = GetEos(pc);
    if (eos.empty()) return;

    if (g_whitelist.find(ToLower(eos)) == g_whitelist.end())
    {
        Notify(pc, L"You are not authorised to use this command.");
        Log::GetLog()->info("[DropDropper] Denied eos={} cmd={}", eos, commandWord);
        return;
    }

    AActor* ch = pc->BaseGetPlayerCharacter();
    if (!ch)
    {
        Notify(pc, L"You must be in-game to drop.");
        return;
    }

    std::string raw = message ? FStr(*message) : "";
    std::vector<std::string> tokens = Tokenize(raw);

    size_t idx = 0;
    if (!tokens.empty() && ToLower(tokens[0]) == commandWord) idx = 1;

    std::string keyword;
    if (idx < tokens.size()) keyword = ToLower(tokens[idx]);

    if (keyword.empty())
    {
        Notify(pc, L"Specify a colour. Example: /drop red");
        return;
    }

    auto it = table.find(keyword);
    if (it == table.end())
    {
        Notify(pc, L"Unknown drop.");
        Log::GetLog()->info("[DropDropper] Unknown keyword '{}' eos={}", keyword, eos);
        return;
    }

    const std::string& cls = it->second;
    std::wstring wcmd = L"Summon " + std::wstring(cls.begin(), cls.end());

    RunCheat(pc, wcmd);

    Log::GetLog()->info("[DropDropper] eos={} summoned {} ({})", eos, cls, keyword);
}

static void Cmd_Drop(AShooterPlayerController* pc, FString* message, int, int)
{
    HandleDrop(pc, message, "/drop", g_drops);
}

static void Cmd_DropCave(AShooterPlayerController* pc, FString* message, int, int)
{
    HandleDrop(pc, message, "/dropc", g_caveDrops);
}

static const std::unordered_map<std::string, std::string>* ResolveMassTable(const std::string& mapName)
{
    for (auto& [mapKey, colors] : g_massDrops)
    {
        if (mapKey == "default") continue;
        if (mapName.find(mapKey) != std::string::npos)
            return &colors;
    }

    auto def = g_massDrops.find("default");
    if (def != g_massDrops.end()) return &def->second;

    return nullptr;
}

static void Cmd_MassDrop(AShooterPlayerController* pc, FString* message, int, int)
{
    if (!pc) return;

    std::string eos = GetEos(pc);
    if (eos.empty()) return;

    if (g_whitelist.find(ToLower(eos)) == g_whitelist.end())
    {
        Notify(pc, L"You are not authorised to use this command.");
        Log::GetLog()->info("[DropDropper] Denied eos={} cmd=/massdrop", eos);
        return;
    }

    AActor* ch = pc->BaseGetPlayerCharacter();
    if (!ch)
    {
        Notify(pc, L"You must be in-game to drop.");
        return;
    }

    std::string raw = message ? FStr(*message) : "";
    std::vector<std::string> tokens = Tokenize(raw);

    size_t idx = 0;
    if (!tokens.empty() && ToLower(tokens[0]) == "/massdrop") idx = 1;

    std::string keyword;
    std::string numStr;
    if (idx < tokens.size()) keyword = ToLower(tokens[idx]);
    if (idx + 1 < tokens.size()) numStr = tokens[idx + 1];

    if (keyword.empty() || numStr.empty())
    {
        Notify(pc, L"Usage: /massdrop {colour} {number}");
        return;
    }

    std::string mapName = GetMap();
    const std::unordered_map<std::string, std::string>* table = ResolveMassTable(mapName);

    if (!table)
    {
        Notify(pc, L"No mass drops configured for this map.");
        Log::GetLog()->info("[DropDropper] No massdrop table for map={}", mapName);
        return;
    }

    auto it = table->find(keyword);
    if (it == table->end())
    {
        Notify(pc, L"Unknown drop.");
        Log::GetLog()->info("[DropDropper] Unknown massdrop keyword '{}' map={} eos={}", keyword, mapName, eos);
        return;
    }

    int count = 0;
    try { count = std::stoi(numStr); }
    catch (...) { count = 0; }

    if (count < 1)
    {
        Notify(pc, L"Number must be 1 or more.");
        return;
    }

    if (count > g_massDropMax)
        count = g_massDropMax;

    const std::string& path = it->second;
    std::wstring wPath(path.begin(), path.end());

    std::wstring wcmd =
        L"SpawnActorSpread \"Blueprint'" + wPath + L"'\" 0 0 0 " +
        std::to_wstring(count) + L" " + std::to_wstring(g_massDropSpread);

    RunCheat(pc, wcmd);

    Log::GetLog()->info("[DropDropper] eos={} massdrop {} x{} map={} ({})", eos, path, count, mapName, keyword);
}

static void Cmd_DropParty(AShooterPlayerController* pc, FString* message, int, int)
{
    if (!pc) return;

    std::string eos = GetEos(pc);
    if (eos.empty()) return;

    if (g_whitelist.find(ToLower(eos)) == g_whitelist.end())
    {
        Notify(pc, L"You are not authorised to use this command.");
        Log::GetLog()->info("[DropDropper] Denied eos={} cmd=/dropparty", eos);
        return;
    }

    AActor* ch = pc->BaseGetPlayerCharacter();
    if (!ch)
    {
        Notify(pc, L"You must be in-game to drop.");
        return;
    }

    std::string raw = message ? FStr(*message) : "";
    std::vector<std::string> tokens = Tokenize(raw);

    size_t idx = 0;
    if (!tokens.empty() && ToLower(tokens[0]) == "/dropparty") idx = 1;

    std::string numStr;
    if (idx < tokens.size()) numStr = tokens[idx];

    if (numStr.empty())
    {
        Notify(pc, L"Usage: /dropparty {number}");
        return;
    }

    std::string mapName = GetMap();
    const std::unordered_map<std::string, std::string>* table = ResolveMassTable(mapName);

    if (!table)
    {
        Notify(pc, L"No mass drops configured for this map.");
        Log::GetLog()->info("[DropDropper] No dropparty table for map={}", mapName);
        return;
    }

    int count = 0;
    try { count = std::stoi(numStr); }
    catch (...) { count = 0; }

    if (count < 1)
    {
        Notify(pc, L"Number must be 1 or more.");
        return;
    }

    if (count > g_massDropMax)
        count = g_massDropMax;

    const char* partyKeys[6] = { "yellow", "yellowring", "purple", "purplering", "red", "redring" };

    std::vector<std::string> available;
    for (const char* k : partyKeys)
    {
        if (table->find(k) != table->end())
            available.push_back(k);
    }

    if (available.empty())
    {
        Notify(pc, L"No party drops configured for this map.");
        Log::GetLog()->info("[DropDropper] No dropparty keys present map={}", mapName);
        return;
    }

    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> pick(0, (int)available.size() - 1);

    std::unordered_map<std::string, int> tally;
    for (int i = 0; i < count; ++i)
        tally[available[pick(rng)]]++;

    for (auto& [key, n] : tally)
    {
        if (n < 1) continue;

        const std::string& path = table->at(key);
        std::wstring wPath(path.begin(), path.end());

        std::wstring wcmd =
            L"SpawnActorSpread \"Blueprint'" + wPath + L"'\" 0 0 0 " +
            std::to_wstring(n) + L" " + std::to_wstring(g_massDropSpread);

        RunCheat(pc, wcmd);

        Log::GetLog()->info("[DropDropper] eos={} dropparty {} x{} map={} ({})", eos, path, n, mapName, key);
    }

    Log::GetLog()->info("[DropDropper] eos={} dropparty total={} map={}", eos, count, mapName);
}

static void PluginInit()
{
    Log::Get().Init("DropDropper");

    if (!LoadConfig())
        Log::GetLog()->error("[DropDropper] Failed to load config — plugin will not function");

    AsaApi::GetCommands().AddChatCommand(FString(L"/drop"), &Cmd_Drop);
    AsaApi::GetCommands().AddChatCommand(FString(L"/dropc"), &Cmd_DropCave);
    AsaApi::GetCommands().AddChatCommand(FString(L"/massdrop"), &Cmd_MassDrop);
    AsaApi::GetCommands().AddChatCommand(FString(L"/dropparty"), &Cmd_DropParty);
    AsaApi::GetCommands().AddOnTimerCallback(FString(L"DropDropper_ConfigCheck"), &OnTimer);

    Log::GetLog()->info("[DropDropper] Loaded");
}

static void PluginUnload()
{
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/drop"));
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/dropc"));
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/massdrop"));
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/dropparty"));
    AsaApi::GetCommands().RemoveOnTimerCallback(FString(L"DropDropper_ConfigCheck"));

    g_whitelist.clear();
    g_drops.clear();
    g_caveDrops.clear();
    g_massDrops.clear();

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