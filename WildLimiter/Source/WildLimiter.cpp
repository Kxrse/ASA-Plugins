/*
WildLimiter - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * WildLimiter - ASA Plugin
 *
 * Hooks:
 *   APrimalDinoCharacter.BeginPlay()  queue wild dino spawns that match a rule
 *   APrimalDinoCharacter.Destroyed()  clear pending and cap state on destruction
 *
 * Timer:
 *   A 1Hz timer callback drives deferred judgment, the 10 second config reload
 *   check, and the one shot sweep that follows a rule change.
 *
 * Config:
 *   ArkApi/Plugins/WildLimiter/config.json
 *   Blocked: blueprint path substrings, matched case insensitively, despawned on spawn
 *   Caps: blueprint path substring to maximum live wild count on this map
 *   MapOverrides: per map Blocked and Caps that fully replace the defaults for that map
 *
 * Config Example:
 * {
 *   "Blocked": [ "microraptor", "titanoboa" ],
 *   "Caps": {
 *     "dodo": 40,
 *     "raptor": 25
 *   },
 *   "MapOverrides": {
 *     "theisland_wp": {
 *       "Blocked": [ "microraptor" ],
 *       "Caps": {
 *         "dodo": 60
 *       }
 *     }
 *   }
 * }
 *
 * Behavior:
 *   A matching wild spawn is queued at BeginPlay and judged on a later timer tick,
 *   at least one second after the spawn. If the dino acquired a team inside that
 *   window (admin spawn, GMSummon, taming) it is left untouched, otherwise the rule
 *   applies: a blocked match is destroyed, a capped match is destroyed only if the
 *   live count for its key already sits at the limit. Tamed dinos are never queued
 *   because the queue is gated on TamingTeamID 0.
 *
 *   Mission dinos are never touched. IsMissionDino is checked at judgment time and
 *   again in the sweep. The engine exposes no equivalent signal for boss or admin
 *   spawns, so a rule whose substring happens to match a boss blueprint will destroy
 *   that boss. Keep entries specific enough that they cannot.
 *
 *   Blueprint paths resolve once per UClass and are cached, so a repeat spawn of a
 *   known class costs one pointer hash lookup and no string work. The cache is
 *   dropped whenever the effective rules change.
 *
 *   Caps track live wild dinos per key and decrement on destruction, so counts stay
 *   accurate across hibernation despawn. When a map override is present its Blocked
 *   and Caps fully replace the defaults for that map rather than merging with them.
 *
 *   On load, and on any config reload, a one shot sweep runs over the server octree
 *   DINOPAWNS group: blocked matches are destroyed and cap counts are rebuilt from
 *   the live wild population. The octree only holds actors in streamed World
 *   Partition cells, so the sweep covers the loaded world rather than the whole map,
 *   and its scan count varies with what is loaded. Anything in an unloaded cell is
 *   judged by BeginPlay when that cell streams in, so nothing is missed, but the
 *   sweep total is not a population figure.
 *
 *   Dinos awaiting judgment are skipped by the sweep so the grace window is
 *   preserved. Lowering a cap does not destroy dinos that are already alive, it only
 *   blocks new spawns until the live count falls back under the limit.
 */

#include <API/ARK/Ark.h>

#pragma warning(disable: 4191)
#pragma comment(lib, "AsaApi.lib")

#include <json.hpp>
#include <fstream>
#include <string>
#include <vector>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
#include <ctime>
#include <sys/stat.h>

static const std::string g_config_path = "ArkApi/Plugins/WildLimiter/config.json";

static constexpr int   g_defer_seconds = 1;
static constexpr int   g_reload_interval_ticks = 10;
static constexpr float g_sweep_radius = 10000000.0f;

struct MapRule
{
    std::vector<std::string> blocked;
    std::vector<std::pair<std::string, int>> caps;
};

struct ClassRule
{
    bool blocked;
    int  cap_index;
};

struct PendingInfo
{
    std::chrono::steady_clock::time_point deadline;
    UClass* cls;
};

static std::vector<std::string> g_def_blocked;
static std::vector<std::pair<std::string, int>> g_def_caps;
static std::unordered_map<std::string, MapRule> g_overrides;

static std::vector<std::string> g_eff_blocked;
static std::vector<std::pair<std::string, int>> g_eff_caps;
static bool g_eff_ready = false;

static std::unordered_map<UClass*, ClassRule> g_class_rules;

static std::unordered_map<std::string, int> g_counts;
static std::unordered_map<APrimalDinoCharacter*, std::string> g_tracked;
static std::unordered_map<APrimalDinoCharacter*, PendingInfo> g_pending;
static std::mutex g_state_mutex;

static std::string g_map_name;

static long long g_config_last_size = 0;
static time_t    g_config_last_modified = 0;
static int       g_reload_counter = 0;
static bool      g_pending_sweep = false;

static std::string FStr(const FString& f)
{
    if (f.Len() == 0) return "";
    const char* s = TCHAR_TO_UTF8(*f);
    return (s && s[0]) ? s : "";
}

static std::string ToLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return s;
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

static void ParseRule(const nlohmann::json& src, std::vector<std::string>& blocked,
    std::vector<std::pair<std::string, int>>& caps)
{
    blocked.clear();
    caps.clear();

    if (src.contains("Blocked") && src["Blocked"].is_array())
    {
        for (const auto& e : src["Blocked"])
        {
            if (!e.is_string()) continue;
            std::string v = ToLower(e.get<std::string>());
            if (!v.empty()) blocked.push_back(std::move(v));
        }
    }

    if (src.contains("Caps") && src["Caps"].is_object())
    {
        for (auto it = src["Caps"].begin(); it != src["Caps"].end(); ++it)
        {
            if (!it.value().is_number_integer()) continue;
            std::string k = ToLower(it.key());
            if (k.empty()) continue;
            int v = it.value().get<int>();
            if (v < 0) v = 0;
            caps.emplace_back(std::move(k), v);
        }
    }
}

static bool LoadConfig()
{
    std::ifstream file(g_config_path);
    if (!file.is_open())
    {
        Log::GetLog()->error("[WildLimiter] Cannot open config: {}", g_config_path);
        return false;
    }

    std::vector<std::string> newBlocked;
    std::vector<std::pair<std::string, int>> newCaps;
    std::unordered_map<std::string, MapRule> newOverrides;

    try
    {
        nlohmann::json j;
        file >> j;

        ParseRule(j, newBlocked, newCaps);

        if (j.contains("MapOverrides") && j["MapOverrides"].is_object())
        {
            for (auto it = j["MapOverrides"].begin(); it != j["MapOverrides"].end(); ++it)
            {
                if (!it.value().is_object()) continue;
                MapRule rule;
                ParseRule(it.value(), rule.blocked, rule.caps);
                newOverrides[ToLower(it.key())] = std::move(rule);
            }
        }
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[WildLimiter] Config parse error, holding previous config: {}", ex.what());
        return false;
    }

    g_def_blocked = std::move(newBlocked);
    g_def_caps = std::move(newCaps);
    g_overrides = std::move(newOverrides);

    g_config_last_modified = GetFileModTime(g_config_path);
    g_config_last_size = GetFileSize(g_config_path);

    Log::GetLog()->info("[WildLimiter] Config loaded, {} blocked, {} caps, {} map overrides",
        g_def_blocked.size(), g_def_caps.size(), g_overrides.size());
    return true;
}

static bool CheckConfigReload()
{
    long long sz = GetFileSize(g_config_path);
    if (sz == 0) return false;
    time_t mt = GetFileModTime(g_config_path);
    if (mt == g_config_last_modified && sz == g_config_last_size) return false;
    return LoadConfig();
}

static void InvalidateEffective()
{
    g_eff_ready = false;
    g_class_rules.clear();
}

static bool EnsureEffective()
{
    if (g_eff_ready) return true;

    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return false;

    if (g_map_name.empty())
    {
        FString m;
        world->GetMapName(&m);
        g_map_name = ToLower(FStr(m));
        if (g_map_name.empty()) return false;
    }

    auto it = g_overrides.find(g_map_name);
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

    g_class_rules.clear();
    g_eff_ready = true;

    Log::GetLog()->info("[WildLimiter] Effective rules for '{}': {} blocked, {} caps",
        g_map_name, g_eff_blocked.size(), g_eff_caps.size());
    return true;
}

static ClassRule ResolveClassRule(UClass* cls)
{
    auto it = g_class_rules.find(cls);
    if (it != g_class_rules.end()) return it->second;

    ClassRule rule{ false, -1 };

    const std::string bp = ToLower(FStr(AsaApi::GetApiUtils().GetClassBlueprint(cls)));
    if (bp.empty()) return rule;

    for (const auto& b : g_eff_blocked)
    {
        if (bp.find(b) != std::string::npos)
        {
            rule.blocked = true;
            break;
        }
    }

    if (!rule.blocked)
    {
        for (size_t i = 0; i < g_eff_caps.size(); ++i)
        {
            if (bp.find(g_eff_caps[i].first) != std::string::npos)
            {
                rule.cap_index = (int)i;
                break;
            }
        }
    }

    g_class_rules[cls] = rule;
    return rule;
}

static void RunSweep()
{
    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return;

    if (g_eff_blocked.empty() && g_eff_caps.empty())
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_counts.clear();
        g_tracked.clear();
        return;
    }

    std::unordered_set<APrimalDinoCharacter*> pending_now;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        for (const auto& e : g_pending) pending_now.insert(e.first);
    }

    TArray<AActor*> actors = AsaApi::GetApiUtils().GetAllActorsInRange(
        FVector(0.0, 0.0, 0.0), g_sweep_radius, EServerOctreeGroup::DINOPAWNS);

    std::vector<APrimalDinoCharacter*> destroy;
    std::unordered_map<std::string, int> counts;
    std::unordered_map<APrimalDinoCharacter*, std::string> tracked;

    for (int i = 0; i < actors.Num(); ++i)
    {
        AActor* actor = actors[i];
        if (!actor) continue;
        if (!actor->IsA(APrimalDinoCharacter::GetPrivateStaticClass())) continue;

        APrimalDinoCharacter* dino = static_cast<APrimalDinoCharacter*>(actor);
        if (dino->TamingTeamIDField() != 0) continue;
        if (dino->IsMissionDino()) continue;
        if (pending_now.find(dino) != pending_now.end()) continue;

        UClass* cls = dino->ClassPrivateField();
        if (!cls) continue;

        const ClassRule rule = ResolveClassRule(cls);
        if (rule.blocked)
        {
            destroy.push_back(dino);
            continue;
        }
        if (rule.cap_index < 0) continue;

        const std::string& key = g_eff_caps[rule.cap_index].first;
        counts[key]++;
        tracked[dino] = key;
    }

    const size_t tracked_count = tracked.size();
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_counts = std::move(counts);
        g_tracked = std::move(tracked);
    }

    for (APrimalDinoCharacter* d : destroy)
        if (d) d->Destroy(false, false);

    Log::GetLog()->info("[WildLimiter] Sweep scanned {} dino pawns, destroyed {}, tracking {} capped",
        actors.Num(), destroy.size(), tracked_count);
}

static void ProcessPending(std::chrono::steady_clock::time_point now)
{
    std::vector<std::pair<APrimalDinoCharacter*, UClass*>> due;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        if (g_pending.empty()) return;
        for (auto it = g_pending.begin(); it != g_pending.end(); )
        {
            if (it->second.deadline <= now)
            {
                due.emplace_back(it->first, it->second.cls);
                it = g_pending.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    if (due.empty()) return;

    for (const auto& entry : due)
    {
        APrimalDinoCharacter* dino = entry.first;
        UClass* cls = entry.second;
        if (!dino || !cls) continue;
        if (dino->TamingTeamIDField() != 0) continue;
        if (dino->IsMissionDino()) continue;

        const ClassRule rule = ResolveClassRule(cls);

        bool destroy = false;
        if (rule.blocked)
        {
            destroy = true;
        }
        else if (rule.cap_index >= 0 && rule.cap_index < (int)g_eff_caps.size())
        {
            const std::pair<std::string, int>& cap = g_eff_caps[rule.cap_index];
            std::lock_guard<std::mutex> lock(g_state_mutex);
            int& n = g_counts[cap.first];
            if (n >= cap.second)
            {
                destroy = true;
            }
            else
            {
                n++;
                g_tracked[dino] = cap.first;
            }
        }

        if (destroy) dino->Destroy(false, false);
    }
}

DECLARE_HOOK(APrimalDinoCharacter_BeginPlay, void, APrimalDinoCharacter*);
DECLARE_HOOK(APrimalDinoCharacter_Destroyed, void, APrimalDinoCharacter*);

void Hook_APrimalDinoCharacter_BeginPlay(APrimalDinoCharacter* _this)
{
    APrimalDinoCharacter_BeginPlay_original(_this);

    if (!_this) return;
    if (_this->TamingTeamIDField() != 0) return;

    if (!EnsureEffective()) return;
    if (g_eff_blocked.empty() && g_eff_caps.empty()) return;

    UClass* cls = _this->ClassPrivateField();
    if (!cls) return;

    const ClassRule rule = ResolveClassRule(cls);
    if (!rule.blocked && rule.cap_index < 0) return;

    PendingInfo info;
    info.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(g_defer_seconds);
    info.cls = cls;

    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_pending[_this] = info;
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

static void OnTimer()
{
    if (++g_reload_counter >= g_reload_interval_ticks)
    {
        g_reload_counter = 0;
        if (CheckConfigReload())
        {
            InvalidateEffective();
            g_pending_sweep = true;
        }
    }

    if (!EnsureEffective()) return;

    if (g_pending_sweep)
    {
        g_pending_sweep = false;
        RunSweep();
    }

    ProcessPending(std::chrono::steady_clock::now());
}

static void PluginInit()
{
    Log::Get().Init("WildLimiter");

    if (!LoadConfig())
        Log::GetLog()->error("[WildLimiter] Running with no rules until a valid config is written");

    g_pending_sweep = true;

    AsaApi::GetHooks().SetHook(
        "APrimalDinoCharacter.BeginPlay()",
        (LPVOID)&Hook_APrimalDinoCharacter_BeginPlay,
        (LPVOID*)&APrimalDinoCharacter_BeginPlay_original);

    AsaApi::GetHooks().SetHook(
        "APrimalDinoCharacter.Destroyed()",
        (LPVOID)&Hook_APrimalDinoCharacter_Destroyed,
        (LPVOID*)&APrimalDinoCharacter_Destroyed_original);

    AsaApi::GetCommands().AddOnTimerCallback(FString(L"WildLimiter_Timer"), &OnTimer);

    Log::GetLog()->info("[WildLimiter] Plugin loaded");
}

static void PluginUnload()
{
    AsaApi::GetHooks().DisableHook(
        "APrimalDinoCharacter.BeginPlay()",
        (LPVOID)&Hook_APrimalDinoCharacter_BeginPlay);

    AsaApi::GetHooks().DisableHook(
        "APrimalDinoCharacter.Destroyed()",
        (LPVOID)&Hook_APrimalDinoCharacter_Destroyed);

    AsaApi::GetCommands().RemoveOnTimerCallback(FString(L"WildLimiter_Timer"));

    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_pending.clear();
        g_tracked.clear();
        g_counts.clear();
    }

    g_class_rules.clear();
    g_eff_ready = false;

    Log::GetLog()->info("[WildLimiter] Plugin unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { PluginInit(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->critical("[WildLimiter] Init exception: {}", e.what());
    }
    catch (...)
    {
        Log::GetLog()->critical("[WildLimiter] Init unknown exception");
    }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { PluginUnload(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->error("[WildLimiter] Unload exception: {}", e.what());
    }
    catch (...) {}
}