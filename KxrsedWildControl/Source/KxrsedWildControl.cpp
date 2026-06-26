/*
KxrsedWildControl - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * KxrsedWildControl - ASA Plugin
 *
 * Hooks:
 *   APrimalDinoCharacter.BeginPlay() - queue wild dino spawns that match a rule
 *   APrimalDinoCharacter.Destroyed() - clear pending and cap state on destruction
 *   AShooterGameMode.Tick(float) - config hot-reload (10s), blocked sweep on change, pending processing
 *
 * Config:
 *   blocked - blueprint path substrings (lowercased) despawned on spawn
 *   caps - blueprint substring to max live wild count for this map
 *   map_overrides - per map blocked and caps that fully replace the defaults
 *
 * Despawns or caps wild dino spawns by blueprint path substring, matched case
 * insensitively. A matching spawn is queued at BeginPlay and judged a fixed 0.1s
 * later: if it acquired a team in that window (admin spawn, GMSummon) it is left
 * untouched, otherwise the rule applies (blocked despawn, or cap count and despawn
 * over the limit). Tamed dinos are never queued (gated on TamingTeamID 0). Caps
 * track live wild dinos per key in process and decrement on destruction, so counts
 * stay accurate across hibernation despawn. When a map override is present its
 * blocked and caps fully replace the defaults for that map. On a config reload that
 * changes the effective rules, a one shot sweep destroys existing live wild dinos
 * that match the blocked list.
 */

#include <API/ARK/Ark.h>
#include <json.hpp>
#include <fstream>
#include <string>
#include <vector>
#include <utility>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
#include <ctime>
#include <sys/stat.h>

#pragma comment(lib, "AsaApi.lib")
#pragma warning(disable: 4191)

static const std::string g_config_path = "ArkApi/Plugins/KxrsedWildControl/config.json";

static constexpr double g_defer_seconds = 0.1;

struct MapRule
{
    std::vector<std::string> blocked;
    std::vector<std::pair<std::string, int>> caps;
};

struct PendingInfo
{
    std::chrono::steady_clock::time_point deadline;
    std::string bp;
};

static std::vector<std::string> g_def_blocked;
static std::vector<std::pair<std::string, int>> g_def_caps;
static std::unordered_map<std::string, MapRule> g_overrides;

static std::vector<std::string> g_eff_blocked;
static std::vector<std::pair<std::string, int>> g_eff_caps;
static bool g_eff_ready = false;
static std::string g_eff_map;

static std::unordered_map<std::string, int> g_counts;
static std::unordered_map<APrimalDinoCharacter*, std::string> g_tracked;
static std::unordered_map<APrimalDinoCharacter*, PendingInfo> g_pending;
static std::mutex g_state_mutex;

static std::string g_map_name;

static long long g_config_last_size = 0;
static time_t g_config_last_modified = 0;
static std::chrono::steady_clock::time_point g_last_config_check;

static std::string FStr(const FString& f)
{
    const char* s = TCHAR_TO_UTF8(*f);
    return (s && s[0]) ? s : "";
}

static std::string ToLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static const std::string& MapName()
{
    if (g_map_name.empty())
    {
        UWorld* world = AsaApi::GetApiUtils().GetWorld();
        if (world)
        {
            FString m;
            world->GetMapName(&m);
            g_map_name = FStr(m);
        }
    }
    return g_map_name;
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

static void ParseBlocked(const nlohmann::json& src, std::vector<std::string>& out)
{
    out.clear();
    if (src.contains("blocked") && src["blocked"].is_array())
        for (auto& e : src["blocked"])
            if (e.is_string()) out.push_back(ToLower(e.get<std::string>()));
}

static void ParseCaps(const nlohmann::json& src, std::vector<std::pair<std::string, int>>& out)
{
    out.clear();
    if (src.contains("caps") && src["caps"].is_object())
        for (auto it = src["caps"].begin(); it != src["caps"].end(); ++it)
            if (it.value().is_number_integer())
                out.emplace_back(ToLower(it.key()), it.value().get<int>());
}

static bool LoadConfig()
{
    std::ifstream file(g_config_path);
    if (!file.is_open())
    {
        Log::GetLog()->error("[KxrsedWildControl] Cannot open config: {}", g_config_path);
        return false;
    }

    try
    {
        nlohmann::json j;
        file >> j;

        ParseBlocked(j, g_def_blocked);
        ParseCaps(j, g_def_caps);

        std::unordered_map<std::string, MapRule> newOverrides;
        if (j.contains("map_overrides") && j["map_overrides"].is_object())
        {
            for (auto it = j["map_overrides"].begin(); it != j["map_overrides"].end(); ++it)
            {
                if (!it.value().is_object()) continue;
                MapRule rule;
                ParseBlocked(it.value(), rule.blocked);
                ParseCaps(it.value(), rule.caps);
                newOverrides[ToLower(it.key())] = std::move(rule);
            }
        }
        g_overrides = std::move(newOverrides);

        g_eff_ready = false;
        g_config_last_modified = GetFileModTime(g_config_path);
        g_config_last_size = GetFileSize(g_config_path);
        Log::GetLog()->info("[KxrsedWildControl] Config loaded, {} blocked, {} caps, {} overrides",
            g_def_blocked.size(), g_def_caps.size(), g_overrides.size());
        return true;
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[KxrsedWildControl] Config parse error: {}", ex.what());
        return false;
    }
}

static bool CheckConfigReload()
{
    long long sz = GetFileSize(g_config_path);
    if (sz == 0) return false;
    time_t mt = GetFileModTime(g_config_path);
    if (mt == g_config_last_modified && sz == g_config_last_size) return false;
    return LoadConfig();
}

static void EnsureEffective()
{
    std::string map = ToLower(MapName());
    if (g_eff_ready && map == g_eff_map) return;

    auto it = g_overrides.find(map);
    if (it != g_overrides.end())
    {
        g_eff_blocked = it->second.blocked;
        g_eff_caps = it->second.caps;
    }
    else
    {
        g_eff_blocked = g_def_blocked;
        g_eff_caps = g_def_caps;
    }

    g_eff_map = map;
    g_eff_ready = true;
    Log::GetLog()->info("[KxrsedWildControl] Effective rules for '{}': {} blocked, {} caps",
        map, g_eff_blocked.size(), g_eff_caps.size());
}

static bool MatchesAnyRule(const std::string& bp)
{
    for (const auto& b : g_eff_blocked)
        if (bp.find(b) != std::string::npos) return true;
    for (const auto& c : g_eff_caps)
        if (bp.find(c.first) != std::string::npos) return true;
    return false;
}

static void SweepBlocked()
{
    if (g_eff_blocked.empty()) return;

    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return;
    ULevel* level = world->PersistentLevelField();
    if (!level) return;

    auto actors = level->ActorsField();
    std::vector<APrimalDinoCharacter*> targets;

    for (int i = 0; i < actors.Num(); ++i)
    {
        AActor* actor = actors[i];
        if (!actor) continue;
        if (!actor->IsA(APrimalDinoCharacter::StaticClass())) continue;

        APrimalDinoCharacter* dino = static_cast<APrimalDinoCharacter*>(actor);
        if (dino->TamingTeamIDField() != 0) continue;

        std::string bp = ToLower(FStr(AsaApi::GetApiUtils().GetBlueprint(static_cast<AActor*>(dino))));
        if (bp.empty()) continue;

        for (const auto& b : g_eff_blocked)
        {
            if (bp.find(b) != std::string::npos)
            {
                targets.push_back(dino);
                break;
            }
        }
    }

    for (APrimalDinoCharacter* d : targets)
        if (d) d->Destroy(false, false);

    if (!targets.empty())
        Log::GetLog()->info("[KxrsedWildControl] Sweep removed {} blocked wild dinos", targets.size());
}

static void ProcessPending(std::chrono::steady_clock::time_point now)
{
    std::vector<std::pair<APrimalDinoCharacter*, std::string>> due;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        if (g_pending.empty()) return;
        for (auto it = g_pending.begin(); it != g_pending.end(); )
        {
            if (it->second.deadline <= now)
            {
                due.emplace_back(it->first, it->second.bp);
                it = g_pending.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    if (due.empty()) return;
    EnsureEffective();

    for (auto& entry : due)
    {
        APrimalDinoCharacter* dino = entry.first;
        const std::string& bp = entry.second;
        if (!dino) continue;
        if (dino->TamingTeamIDField() != 0) continue;

        bool handled = false;
        for (const auto& b : g_eff_blocked)
        {
            if (bp.find(b) != std::string::npos)
            {
                dino->Destroy(false, false);
                handled = true;
                break;
            }
        }
        if (handled) continue;

        bool overcap = false;
        for (const auto& c : g_eff_caps)
        {
            if (bp.find(c.first) == std::string::npos) continue;
            {
                std::lock_guard<std::mutex> lock(g_state_mutex);
                int& n = g_counts[c.first];
                if (n >= c.second) overcap = true;
                else
                {
                    n++;
                    g_tracked[dino] = c.first;
                }
            }
            break;
        }

        if (overcap) dino->Destroy(false, false);
    }
}

DECLARE_HOOK(APrimalDinoCharacter_BeginPlay, void, APrimalDinoCharacter*);
DECLARE_HOOK(APrimalDinoCharacter_Destroyed, void, APrimalDinoCharacter*);

void Hook_APrimalDinoCharacter_BeginPlay(APrimalDinoCharacter* _this)
{
    APrimalDinoCharacter_BeginPlay_original(_this);

    if (!_this) return;
    if (_this->TamingTeamIDField() != 0) return;

    EnsureEffective();
    if (g_eff_blocked.empty() && g_eff_caps.empty()) return;

    std::string bp = ToLower(FStr(AsaApi::GetApiUtils().GetBlueprint(static_cast<AActor*>(_this))));
    if (bp.empty()) return;
    if (!MatchesAnyRule(bp)) return;

    PendingInfo info;
    info.deadline = std::chrono::steady_clock::now()
        + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(g_defer_seconds));
    info.bp = std::move(bp);

    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_pending[_this] = std::move(info);
}

void Hook_APrimalDinoCharacter_Destroyed(APrimalDinoCharacter* _this)
{
    if (_this)
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_pending.erase(_this);
        auto it = g_tracked.find(_this);
        if (it != g_tracked.end())
        {
            auto cit = g_counts.find(it->second);
            if (cit != g_counts.end() && cit->second > 0) cit->second--;
            g_tracked.erase(it);
        }
    }

    APrimalDinoCharacter_Destroyed_original(_this);
}

using Tick_t = void(*)(AShooterGameMode*, float);
static Tick_t Original_Tick = nullptr;

static void Detour_Tick(AShooterGameMode* gm, float dt)
{
    Original_Tick(gm, dt);

    auto now = std::chrono::steady_clock::now();
    auto sinceCheck = std::chrono::duration_cast<std::chrono::seconds>(now - g_last_config_check).count();
    if (sinceCheck >= 10)
    {
        g_last_config_check = now;
        if (CheckConfigReload())
        {
            EnsureEffective();
            SweepBlocked();
        }
    }

    ProcessPending(now);
}

static void PluginInit()
{
    Log::Get().Init("KxrsedWildControl");

    if (!LoadConfig())
        Log::GetLog()->error("[KxrsedWildControl] Failed to load config");

    g_last_config_check = std::chrono::steady_clock::now();

    AsaApi::GetHooks().SetHook(
        "APrimalDinoCharacter.BeginPlay()",
        (LPVOID)&Hook_APrimalDinoCharacter_BeginPlay,
        (LPVOID*)&APrimalDinoCharacter_BeginPlay_original);

    AsaApi::GetHooks().SetHook(
        "APrimalDinoCharacter.Destroyed()",
        (LPVOID)&Hook_APrimalDinoCharacter_Destroyed,
        (LPVOID*)&APrimalDinoCharacter_Destroyed_original);

    AsaApi::GetHooks().SetHook(
        "AShooterGameMode.Tick(float)",
        (LPVOID)&Detour_Tick,
        (LPVOID*)&Original_Tick);

    Log::GetLog()->info("[KxrsedWildControl] Loaded");
}

static void PluginUnload()
{
    AsaApi::GetHooks().DisableHook(
        "APrimalDinoCharacter.BeginPlay()",
        (LPVOID)&Hook_APrimalDinoCharacter_BeginPlay);

    AsaApi::GetHooks().DisableHook(
        "APrimalDinoCharacter.Destroyed()",
        (LPVOID)&Hook_APrimalDinoCharacter_Destroyed);

    AsaApi::GetHooks().DisableHook(
        "AShooterGameMode.Tick(float)",
        (LPVOID)&Detour_Tick);

    Log::GetLog()->info("[KxrsedWildControl] Unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { PluginInit(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->critical("[KxrsedWildControl] Init exception: {}", e.what());
    }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { PluginUnload(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->error("[KxrsedWildControl] Unload exception: {}", e.what());
    }
}