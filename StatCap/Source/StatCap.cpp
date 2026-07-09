/*
StatCap - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * StatCap - ASA Plugin
 *
 * Hooks:
 *   AShooterPlayerController.ServerRequestLevelUp_Implementation(UPrimalCharacterStatusComponent*,EPrimalCharacterStatusValue::Type)
 *   APrimalDinoCharacter.OnUncryo(AShooterPlayerController*)
 *
 * Config:
 *   config.json
 *     PlayerCaps                 - stat name to max applied points on the survivor, omit a stat to leave it uncapped
 *     DinoCaps                   - stat name to max applied tamed points on a dino, omit a stat to leave it uncapped
 *     DinoOverrides              - dino class name to per stat caps, each stat overrides DinoCaps, -1 uncaps that stat
 *     DeniedMessage              - chat text sent when a cap blocks a point, supports {stat}, {cap} and {s}
 *     DestroyOverCapDinoOnUncryo - when true, a dino released from a cryopod with any stat over its cap is destroyed
 *     OverCapDestroyMessage      - chat text sent when a dino is destroyed for exceeding a cap, supports {stat}, {cap} and {s}
 *     MessageColor               - RGBA string used for both messages
 *     Hot-reloaded every 10s.
 *
 * The {s} placeholder expands to an empty string when the cap is 1 and to the letter s otherwise.
 * Dino class names are matched exactly, so each variant needs its own DinoOverrides entry.
 * A level up request that does not target the caller's own survivor or a tame on the caller's team is
 * passed straight to the original and answered with silence, matching vanilla rejection behaviour.
 *
 * Caps how many level up points may be applied to any single stat, separately for survivors and for
 * tamed dinos, with optional per species caps, and optionally destroys tamed dinos that exceed a cap
 * when they leave a cryopod. Only manually spent points are counted. Wild levels, mutations, imprint
 * bonuses and taming bonuses are neither counted nor affected.
 */

#include <API/ARK/Ark.h>
#include <json.hpp>
#include <Windows.h>
#include <fstream>
#include <string>
#include <mutex>
#include <vector>
#include <array>
#include <unordered_map>
#include <filesystem>
#include <algorithm>

#pragma warning(disable: 4191)
#pragma comment(lib, "AsaApi")

namespace fs = std::filesystem;

static const std::string g_config_path = "ArkApi/Plugins/StatCap/config.json";

static constexpr int STAT_COUNT = 12;
static constexpr int CAP_NONE = -1;
static constexpr int CAP_UNSET = -2;

static const char* const STAT_NAMES[STAT_COUNT] = {
    "Health",
    "Stamina",
    "Torpidity",
    "Oxygen",
    "Food",
    "Water",
    "Temperature",
    "Weight",
    "MeleeDamageMultiplier",
    "SpeedMultiplier",
    "TemperatureFortitude",
    "CraftingSpeedMultiplier"
};

static const wchar_t* const STAT_NAMES_W[STAT_COUNT] = {
    L"Health",
    L"Stamina",
    L"Torpidity",
    L"Oxygen",
    L"Food",
    L"Water",
    L"Temperature",
    L"Weight",
    L"MeleeDamageMultiplier",
    L"SpeedMultiplier",
    L"TemperatureFortitude",
    L"CraftingSpeedMultiplier"
};

using CapArray = std::array<int, STAT_COUNT>;

static std::mutex g_config_mutex;
static int g_player_caps[STAT_COUNT] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static int g_dino_caps[STAT_COUNT] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static std::unordered_map<std::string, CapArray> g_dino_overrides;
static std::wstring g_denied_message = L"You cannot put more than {cap} point{s} into {stat}.";
static std::wstring g_over_cap_message = L"This dino has more than {cap} point{s} in {stat} and has been destroyed.";
static std::wstring g_message_color = L"1.0,0.2,0.2,1.0";
static bool g_destroy_over_cap = false;

static std::mutex g_destroy_mutex;
static std::vector<APrimalDinoCharacter*> g_destroy_queue;

static uintmax_t g_last_size = 0;
static fs::file_time_type g_last_write;
static bool g_config_seen = false;
static int g_reload_counter = 0;

using ServerRequestLevelUp_t = void(*)(AShooterPlayerController*, UPrimalCharacterStatusComponent*, EPrimalCharacterStatusValue::Type);
static ServerRequestLevelUp_t Original_ServerRequestLevelUp = nullptr;

using OnUncryo_t = void(*)(APrimalDinoCharacter*, AShooterPlayerController*);
static OnUncryo_t Original_OnUncryo = nullptr;

static std::string FStr(const FString& f)
{
    const char* s = TCHAR_TO_UTF8(*f);
    return (s && s[0]) ? s : "";
}

static std::wstring Widen(const std::string& s)
{
    if (s.empty()) return std::wstring();

    const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return std::wstring();

    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

static void ReplaceAll(std::wstring& subject, const std::wstring& from, const std::wstring& to)
{
    if (from.empty()) return;

    size_t pos = 0;
    while ((pos = subject.find(from, pos)) != std::wstring::npos)
    {
        subject.replace(pos, from.length(), to);
        pos += to.length();
    }
}

static std::wstring BuildMessage(std::wstring subject, int statIndex, int cap)
{
    ReplaceAll(subject, L"{stat}", STAT_NAMES_W[statIndex]);
    ReplaceAll(subject, L"{cap}", std::to_wstring(cap));
    ReplaceAll(subject, L"{s}", cap == 1 ? L"" : L"s");
    return subject;
}

static std::string GetDinoClassName(AActor* actor)
{
    if (!actor) return "";

    UClass* cls = actor->ClassField();
    if (!cls) return "";

    return FStr(UVictoryCore::GetClassFName(cls).ToString());
}

static AShooterCharacter* GetRequesterCharacter(AShooterPlayerController* pc)
{
    if (!pc) return nullptr;

    AActor* baseChar = pc->BaseGetPlayerCharacter();
    if (!baseChar) return nullptr;

    return static_cast<AShooterCharacter*>(baseChar);
}

static int ResolveStatIndex(const std::string& name)
{
    for (int i = 0; i < STAT_COUNT; ++i)
    {
        if (name == STAT_NAMES[i]) return i;
    }
    return -1;
}

static void ParseCaps(const nlohmann::json& j, const char* key, int* out)
{
    for (int i = 0; i < STAT_COUNT; ++i) out[i] = CAP_NONE;

    if (!j.contains(key) || !j[key].is_object()) return;

    for (auto it = j[key].begin(); it != j[key].end(); ++it)
    {
        if (!it.value().is_number_integer()) continue;

        const int idx = ResolveStatIndex(it.key());
        if (idx < 0)
        {
            Log::GetLog()->warn("[StatCap] Unknown stat name in {}: {}", key, it.key());
            continue;
        }

        const int value = it.value().get<int>();
        if (value < 0) continue;

        out[idx] = value;
    }
}

static void ParseDinoOverrides(const nlohmann::json& j, std::unordered_map<std::string, CapArray>& out)
{
    out.clear();

    if (!j.contains("DinoOverrides") || !j["DinoOverrides"].is_object()) return;

    for (auto species = j["DinoOverrides"].begin(); species != j["DinoOverrides"].end(); ++species)
    {
        if (!species.value().is_object())
        {
            Log::GetLog()->warn("[StatCap] DinoOverrides entry is not an object: {}", species.key());
            continue;
        }

        CapArray caps;
        caps.fill(CAP_UNSET);

        for (auto stat = species.value().begin(); stat != species.value().end(); ++stat)
        {
            if (!stat.value().is_number_integer()) continue;

            const int idx = ResolveStatIndex(stat.key());
            if (idx < 0)
            {
                Log::GetLog()->warn("[StatCap] Unknown stat name in DinoOverrides {}: {}",
                    species.key(), stat.key());
                continue;
            }

            const int value = stat.value().get<int>();
            if (value < CAP_NONE) continue;

            caps[idx] = value;
        }

        out[species.key()] = caps;
    }
}

static void ResolveDinoCaps(const std::string& className, int* out)
{
    std::lock_guard<std::mutex> lock(g_config_mutex);
    std::copy(g_dino_caps, g_dino_caps + STAT_COUNT, out);

    if (className.empty()) return;

    const auto it = g_dino_overrides.find(className);
    if (it == g_dino_overrides.end()) return;

    for (int i = 0; i < STAT_COUNT; ++i)
    {
        if (it->second[i] != CAP_UNSET) out[i] = it->second[i];
    }
}

static void ApplyConfig(const nlohmann::json& j)
{
    int playerCaps[STAT_COUNT];
    int dinoCaps[STAT_COUNT];

    ParseCaps(j, "PlayerCaps", playerCaps);
    ParseCaps(j, "DinoCaps", dinoCaps);

    std::unordered_map<std::string, CapArray> overrides;
    ParseDinoOverrides(j, overrides);

    std::wstring denied = L"You cannot put more than {cap} point{s} into {stat}.";
    if (j.contains("DeniedMessage") && j["DeniedMessage"].is_string())
        denied = Widen(j["DeniedMessage"].get<std::string>());

    std::wstring overCap = L"This dino has more than {cap} point{s} in {stat} and has been destroyed.";
    if (j.contains("OverCapDestroyMessage") && j["OverCapDestroyMessage"].is_string())
        overCap = Widen(j["OverCapDestroyMessage"].get<std::string>());

    std::wstring color = L"1.0,0.2,0.2,1.0";
    if (j.contains("MessageColor") && j["MessageColor"].is_string())
        color = Widen(j["MessageColor"].get<std::string>());

    bool destroy = false;
    if (j.contains("DestroyOverCapDinoOnUncryo") && j["DestroyOverCapDinoOnUncryo"].is_boolean())
        destroy = j["DestroyOverCapDinoOnUncryo"].get<bool>();

    std::lock_guard<std::mutex> lock(g_config_mutex);
    std::copy(playerCaps, playerCaps + STAT_COUNT, g_player_caps);
    std::copy(dinoCaps, dinoCaps + STAT_COUNT, g_dino_caps);
    g_dino_overrides = std::move(overrides);
    g_denied_message = std::move(denied);
    g_over_cap_message = std::move(overCap);
    g_message_color = std::move(color);
    g_destroy_over_cap = destroy;
}

static bool LoadConfig()
{
    std::ifstream file(g_config_path);
    if (!file.is_open())
    {
        Log::GetLog()->error("[StatCap] Cannot open config: {}", g_config_path);
        return false;
    }

    try
    {
        nlohmann::json j;
        file >> j;
        ApplyConfig(j);
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[StatCap] Config parse error: {}", ex.what());
        return false;
    }

    try
    {
        if (fs::exists(g_config_path))
        {
            g_last_size = fs::file_size(g_config_path);
            g_last_write = fs::last_write_time(g_config_path);
            g_config_seen = true;
        }
    }
    catch (...) {}

    return true;
}

static void ReloadCheck()
{
    try
    {
        if (!fs::exists(g_config_path)) return;

        const uintmax_t sz = fs::file_size(g_config_path);
        if (sz == 0) return;

        const fs::file_time_type wt = fs::last_write_time(g_config_path);
        if (g_config_seen && sz == g_last_size && wt == g_last_write) return;

        std::ifstream file(g_config_path);
        if (!file.is_open()) return;

        nlohmann::json j;
        file >> j;
        ApplyConfig(j);

        g_last_size = sz;
        g_last_write = wt;
        g_config_seen = true;

        Log::GetLog()->info("[StatCap] Config reloaded");
    }
    catch (...)
    {
        return;
    }
}

static void Notify(AShooterPlayerController* pc, const std::wstring& msg, const std::wstring& color)
{
    if (!pc) return;

    const std::wstring rich = L"<RichColor Color=\"" + color + L"\">" + msg + L"</>";
    FString fSender(L"");
    FString fMsg(rich.c_str());
    AsaApi::GetApiUtils().SendChatMessage(pc, fSender, L"{}", std::wstring_view(*fMsg));
}

static void QueueDestroy(APrimalDinoCharacter* dino)
{
    if (!dino) return;

    std::lock_guard<std::mutex> lock(g_destroy_mutex);
    for (APrimalDinoCharacter* queued : g_destroy_queue)
    {
        if (queued == dino) return;
    }
    g_destroy_queue.push_back(dino);
}

static void FlushDestroyQueue()
{
    std::vector<APrimalDinoCharacter*> local;
    {
        std::lock_guard<std::mutex> lock(g_destroy_mutex);
        if (g_destroy_queue.empty()) return;
        local.swap(g_destroy_queue);
    }

    for (APrimalDinoCharacter* dino : local)
    {
        if (!dino) continue;
        if (!UVictoryCore::BPIsValidLowLevelFast(dino)) continue;
        if (dino->bActorIsBeingDestroyed()()) continue;

        try { dino->Destroy(false, false); }
        catch (...) {}
    }
}

static int FindOverCapStat(APrimalDinoCharacter* dino, const std::string& className,
    int* outPoints, int* outCap)
{
    if (!dino) return -1;

    UPrimalCharacterStatusComponent* statusComp = dino->MyCharacterStatusComponentField();
    if (!statusComp) return -1;

    unsigned __int8* points = statusComp->NumberOfLevelUpPointsAppliedTamedField()();
    if (!points) return -1;

    int caps[STAT_COUNT];
    ResolveDinoCaps(className, caps);

    for (int i = 0; i < STAT_COUNT; ++i)
    {
        if (caps[i] < 0) continue;

        const int applied = static_cast<int>(points[i]);
        if (applied <= caps[i]) continue;

        *outPoints = applied;
        *outCap = caps[i];
        return i;
    }

    return -1;
}

static void Detour_ServerRequestLevelUp(AShooterPlayerController* pc,
    UPrimalCharacterStatusComponent* forStatusComp,
    EPrimalCharacterStatusValue::Type valueType)
{
    if (!pc || !forStatusComp)
    {
        Original_ServerRequestLevelUp(pc, forStatusComp, valueType);
        return;
    }

    const int idx = static_cast<int>(valueType);
    if (idx < 0 || idx >= STAT_COUNT)
    {
        Original_ServerRequestLevelUp(pc, forStatusComp, valueType);
        return;
    }

    AActor* owner = forStatusComp->OwnerPrivateField();
    if (!owner)
    {
        Original_ServerRequestLevelUp(pc, forStatusComp, valueType);
        return;
    }

    AShooterCharacter* requester = GetRequesterCharacter(pc);
    if (!requester)
    {
        Original_ServerRequestLevelUp(pc, forStatusComp, valueType);
        return;
    }

    const bool isDino = owner->IsA(APrimalDinoCharacter::StaticClass());

    if (isDino)
    {
        APrimalDinoCharacter* dino = static_cast<APrimalDinoCharacter*>(owner);
        if (dino->TargetingTeamField() != requester->TargetingTeamField())
        {
            Original_ServerRequestLevelUp(pc, forStatusComp, valueType);
            return;
        }
    }
    else if (owner != static_cast<AActor*>(requester))
    {
        Original_ServerRequestLevelUp(pc, forStatusComp, valueType);
        return;
    }

    int cap = CAP_NONE;
    if (isDino)
    {
        int caps[STAT_COUNT];
        ResolveDinoCaps(GetDinoClassName(owner), caps);
        cap = caps[idx];
    }
    else
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        cap = g_player_caps[idx];
    }

    if (cap < 0)
    {
        Original_ServerRequestLevelUp(pc, forStatusComp, valueType);
        return;
    }

    unsigned __int8* points = isDino
        ? forStatusComp->NumberOfLevelUpPointsAppliedTamedField()()
        : forStatusComp->NumberOfLevelUpPointsAppliedField()();

    if (!points)
    {
        Original_ServerRequestLevelUp(pc, forStatusComp, valueType);
        return;
    }

    if (static_cast<int>(points[idx]) < cap)
    {
        Original_ServerRequestLevelUp(pc, forStatusComp, valueType);
        return;
    }

    std::wstring message;
    std::wstring color;
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        message = g_denied_message;
        color = g_message_color;
    }

    Notify(pc, BuildMessage(message, idx, cap), color);
}

static void Detour_OnUncryo(APrimalDinoCharacter* dino, AShooterPlayerController* pc)
{
    Original_OnUncryo(dino, pc);

    if (!dino) return;

    bool enabled;
    std::wstring message;
    std::wstring color;
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        enabled = g_destroy_over_cap;
        message = g_over_cap_message;
        color = g_message_color;
    }

    if (!enabled) return;

    const std::string className = GetDinoClassName(dino);

    int applied = 0;
    int cap = 0;
    const int idx = FindOverCapStat(dino, className, &applied, &cap);
    if (idx < 0) return;

    Log::GetLog()->info("[StatCap] OVER_CAP_DESTROY class={} stat={} applied={} cap={}",
        className, STAT_NAMES[idx], applied, cap);

    Notify(pc, BuildMessage(message, idx, cap), color);

    QueueDestroy(dino);
}

static void TimerCallback()
{
    FlushDestroyQueue();

    ++g_reload_counter;
    if (g_reload_counter < 10) return;

    g_reload_counter = 0;
    ReloadCheck();
}

static void PluginInit()
{
    Log::Get().Init("StatCap");

    LoadConfig();

    AsaApi::GetHooks().SetHook(
        "AShooterPlayerController.ServerRequestLevelUp_Implementation(UPrimalCharacterStatusComponent*,EPrimalCharacterStatusValue::Type)",
        (LPVOID)&Detour_ServerRequestLevelUp,
        (LPVOID*)&Original_ServerRequestLevelUp
    );

    AsaApi::GetHooks().SetHook(
        "APrimalDinoCharacter.OnUncryo(AShooterPlayerController*)",
        (LPVOID)&Detour_OnUncryo,
        (LPVOID*)&Original_OnUncryo
    );

    AsaApi::GetCommands().AddOnTimerCallback(FString(L"StatCap_Timer"), &TimerCallback);

    Log::GetLog()->info("[StatCap] Plugin loaded");
}

static void PluginUnload()
{
    AsaApi::GetCommands().RemoveOnTimerCallback(FString(L"StatCap_Timer"));

    AsaApi::GetHooks().DisableHook(
        "APrimalDinoCharacter.OnUncryo(AShooterPlayerController*)",
        (LPVOID)&Detour_OnUncryo
    );

    AsaApi::GetHooks().DisableHook(
        "AShooterPlayerController.ServerRequestLevelUp_Implementation(UPrimalCharacterStatusComponent*,EPrimalCharacterStatusValue::Type)",
        (LPVOID)&Detour_ServerRequestLevelUp
    );

    Log::GetLog()->info("[StatCap] Plugin unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { PluginInit(); }
    catch (const std::exception& ex)
    {
        Log::GetLog()->critical("[StatCap] Init exception: {}", ex.what());
    }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { PluginUnload(); }
    catch (const std::exception& ex)
    {
        Log::GetLog()->critical("[StatCap] Unload exception: {}", ex.what());
    }
}