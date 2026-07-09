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
 *   APrimalDinoCharacter.BeginPlay()
 *
 * Config:
 *   config.json
 *     PlayerCaps                 - stat name to max applied points on the survivor, omit a stat to leave it uncapped
 *     DinoCaps                   - stat name to max applied tamed points on a dino, omit a stat to leave it uncapped
 *     TotalTamedPointsCap        - max tamed points summed across every stat on a dino, -1 or absent to leave uncapped
 *     DinoOverrides              - dino class name to per stat caps, each stat overrides DinoCaps, -1 uncaps that stat
 *                                  the reserved key TotalTamedPoints overrides TotalTamedPointsCap for that species
 *     DestroyOverCapDino         - master switch for destroying dinos that breach a cap
 *     CheckOnUncryo              - when true, a dino is checked as it leaves a cryopod, the deployer is told why
 *     CheckOnBeginPlay           - when true, a dino is checked one second after entering the world, which covers
 *                                  server start, and no player is told because none is present
 *     DeniedMessage              - chat text sent when a per stat cap blocks a point, supports {stat}, {cap} and {s}
 *     TotalDeniedMessage         - chat text sent when the total cap blocks a point, supports {cap} and {s}
 *     OverCapDestroyMessage      - chat text sent when a per stat cap destroys a dino, supports {stat}, {cap} and {s}
 *     TotalOverCapDestroyMessage - chat text sent when the total cap destroys a dino, supports {cap}, {total} and {s}
 *     MessageColor               - RGBA string used for all messages
 *     Hot-reloaded every 10s.
 *
 * The {s} placeholder expands to an empty string when the cap is 1 and to the letter s otherwise.
 * Dino class names are matched exactly, so each variant needs its own DinoOverrides entry.
 * A level up request that does not target the caller's own survivor or a tame on the caller's team is
 * passed straight to the original and answered with silence, matching vanilla rejection behaviour.
 *
 * Caps how many level up points may be applied to any single stat, separately for survivors and for
 * tamed dinos, with optional per species caps, and caps the total tamed points a dino may hold across
 * every stat combined. Dinos breaching a cap are optionally destroyed. Only manually spent points are
 * counted. Wild levels, mutations, imprint bonuses and taming bonuses are neither counted nor affected.
 */

#include <API/ARK/Ark.h>
#include <json.hpp>
#include <Windows.h>
#include <fstream>
#include <string>
#include <mutex>
#include <vector>
#include <array>
#include <chrono>
#include <unordered_map>
#include <filesystem>
#include <algorithm>

#pragma warning(disable: 4191)
#pragma comment(lib, "AsaApi")

namespace fs = std::filesystem;

static const std::string g_config_path = "ArkApi/Plugins/StatCap/config.json";
static const std::string TOTAL_OVERRIDE_KEY = "TotalTamedPoints";

static constexpr int STAT_COUNT = 12;
static constexpr int CAP_NONE = -1;
static constexpr int CAP_UNSET = -2;
static constexpr int BEGIN_PLAY_DELAY_SECONDS = 1;

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

struct OverrideEntry
{
    CapArray caps;
    int totalCap;
};

struct CapBreach
{
    bool isTotal;
    int statIndex;
    int applied;
    int cap;
};

struct PendingCheck
{
    APrimalDinoCharacter* dino;
    std::chrono::steady_clock::time_point checkAt;
};

static std::mutex g_config_mutex;
static int g_player_caps[STAT_COUNT] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static int g_dino_caps[STAT_COUNT] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static int g_total_cap = CAP_NONE;
static std::unordered_map<std::string, OverrideEntry> g_dino_overrides;
static std::wstring g_denied_message = L"You cannot put more than {cap} point{s} into {stat}.";
static std::wstring g_total_denied_message = L"This dino cannot hold more than {cap} point{s} across all stats.";
static std::wstring g_over_cap_message = L"This dino has more than {cap} point{s} in {stat} and has been destroyed.";
static std::wstring g_total_over_cap_message = L"This dino holds {total} points across all stats, above the limit of {cap}, and has been destroyed.";
static std::wstring g_message_color = L"1.0,0.2,0.2,1.0";
static bool g_destroy_over_cap = false;
static bool g_check_on_uncryo = true;
static bool g_check_on_begin_play = false;

static std::mutex g_destroy_mutex;
static std::vector<APrimalDinoCharacter*> g_destroy_queue;

static std::mutex g_pending_mutex;
static std::vector<PendingCheck> g_pending_checks;

static uintmax_t g_last_size = 0;
static fs::file_time_type g_last_write;
static bool g_config_seen = false;
static int g_reload_counter = 0;

using ServerRequestLevelUp_t = void(*)(AShooterPlayerController*, UPrimalCharacterStatusComponent*, EPrimalCharacterStatusValue::Type);
static ServerRequestLevelUp_t Original_ServerRequestLevelUp = nullptr;

using OnUncryo_t = void(*)(APrimalDinoCharacter*, AShooterPlayerController*);
static OnUncryo_t Original_OnUncryo = nullptr;

using BeginPlay_t = void(*)(APrimalDinoCharacter*);
static BeginPlay_t Original_BeginPlay = nullptr;

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

static std::wstring BuildStatMessage(std::wstring subject, int statIndex, int cap)
{
    ReplaceAll(subject, L"{stat}", STAT_NAMES_W[statIndex]);
    ReplaceAll(subject, L"{cap}", std::to_wstring(cap));
    ReplaceAll(subject, L"{s}", cap == 1 ? L"" : L"s");
    return subject;
}

static std::wstring BuildTotalMessage(std::wstring subject, int cap, int total)
{
    ReplaceAll(subject, L"{cap}", std::to_wstring(cap));
    ReplaceAll(subject, L"{total}", std::to_wstring(total));
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

static int SumTamedPoints(unsigned __int8* points)
{
    if (!points) return 0;

    int total = 0;
    for (int i = 0; i < STAT_COUNT; ++i) total += static_cast<int>(points[i]);
    return total;
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

static void ParseDinoOverrides(const nlohmann::json& j, std::unordered_map<std::string, OverrideEntry>& out)
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

        OverrideEntry entry;
        entry.caps.fill(CAP_UNSET);
        entry.totalCap = CAP_UNSET;

        for (auto stat = species.value().begin(); stat != species.value().end(); ++stat)
        {
            if (!stat.value().is_number_integer()) continue;

            const int value = stat.value().get<int>();
            if (value < CAP_NONE) continue;

            if (stat.key() == TOTAL_OVERRIDE_KEY)
            {
                entry.totalCap = value;
                continue;
            }

            const int idx = ResolveStatIndex(stat.key());
            if (idx < 0)
            {
                Log::GetLog()->warn("[StatCap] Unknown stat name in DinoOverrides {}: {}",
                    species.key(), stat.key());
                continue;
            }

            entry.caps[idx] = value;
        }

        out[species.key()] = entry;
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
        if (it->second.caps[i] != CAP_UNSET) out[i] = it->second.caps[i];
    }
}

static int ResolveTotalCap(const std::string& className)
{
    std::lock_guard<std::mutex> lock(g_config_mutex);

    if (!className.empty())
    {
        const auto it = g_dino_overrides.find(className);
        if (it != g_dino_overrides.end() && it->second.totalCap != CAP_UNSET)
            return it->second.totalCap;
    }

    return g_total_cap;
}

static void ApplyConfig(const nlohmann::json& j)
{
    int playerCaps[STAT_COUNT];
    int dinoCaps[STAT_COUNT];

    ParseCaps(j, "PlayerCaps", playerCaps);
    ParseCaps(j, "DinoCaps", dinoCaps);

    std::unordered_map<std::string, OverrideEntry> overrides;
    ParseDinoOverrides(j, overrides);

    int totalCap = CAP_NONE;
    if (j.contains("TotalTamedPointsCap") && j["TotalTamedPointsCap"].is_number_integer())
    {
        const int value = j["TotalTamedPointsCap"].get<int>();
        if (value >= CAP_NONE) totalCap = value;
    }

    std::wstring denied = L"You cannot put more than {cap} point{s} into {stat}.";
    if (j.contains("DeniedMessage") && j["DeniedMessage"].is_string())
        denied = Widen(j["DeniedMessage"].get<std::string>());

    std::wstring totalDenied = L"This dino cannot hold more than {cap} point{s} across all stats.";
    if (j.contains("TotalDeniedMessage") && j["TotalDeniedMessage"].is_string())
        totalDenied = Widen(j["TotalDeniedMessage"].get<std::string>());

    std::wstring overCap = L"This dino has more than {cap} point{s} in {stat} and has been destroyed.";
    if (j.contains("OverCapDestroyMessage") && j["OverCapDestroyMessage"].is_string())
        overCap = Widen(j["OverCapDestroyMessage"].get<std::string>());

    std::wstring totalOverCap = L"This dino holds {total} points across all stats, above the limit of {cap}, and has been destroyed.";
    if (j.contains("TotalOverCapDestroyMessage") && j["TotalOverCapDestroyMessage"].is_string())
        totalOverCap = Widen(j["TotalOverCapDestroyMessage"].get<std::string>());

    std::wstring color = L"1.0,0.2,0.2,1.0";
    if (j.contains("MessageColor") && j["MessageColor"].is_string())
        color = Widen(j["MessageColor"].get<std::string>());

    bool destroy = false;
    if (j.contains("DestroyOverCapDino") && j["DestroyOverCapDino"].is_boolean())
        destroy = j["DestroyOverCapDino"].get<bool>();

    bool checkUncryo = true;
    if (j.contains("CheckOnUncryo") && j["CheckOnUncryo"].is_boolean())
        checkUncryo = j["CheckOnUncryo"].get<bool>();

    bool checkBeginPlay = false;
    if (j.contains("CheckOnBeginPlay") && j["CheckOnBeginPlay"].is_boolean())
        checkBeginPlay = j["CheckOnBeginPlay"].get<bool>();

    std::lock_guard<std::mutex> lock(g_config_mutex);
    std::copy(playerCaps, playerCaps + STAT_COUNT, g_player_caps);
    std::copy(dinoCaps, dinoCaps + STAT_COUNT, g_dino_caps);
    g_total_cap = totalCap;
    g_dino_overrides = std::move(overrides);
    g_denied_message = std::move(denied);
    g_total_denied_message = std::move(totalDenied);
    g_over_cap_message = std::move(overCap);
    g_total_over_cap_message = std::move(totalOverCap);
    g_message_color = std::move(color);
    g_destroy_over_cap = destroy;
    g_check_on_uncryo = checkUncryo;
    g_check_on_begin_play = checkBeginPlay;
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

static void QueuePendingCheck(APrimalDinoCharacter* dino)
{
    if (!dino) return;

    PendingCheck pending;
    pending.dino = dino;
    pending.checkAt = std::chrono::steady_clock::now() + std::chrono::seconds(BEGIN_PLAY_DELAY_SECONDS);

    std::lock_guard<std::mutex> lock(g_pending_mutex);
    g_pending_checks.push_back(pending);
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

static bool FindCapBreach(APrimalDinoCharacter* dino, const std::string& className, CapBreach* out)
{
    if (!dino) return false;

    UPrimalCharacterStatusComponent* statusComp = dino->MyCharacterStatusComponentField();
    if (!statusComp) return false;

    unsigned __int8* points = statusComp->NumberOfLevelUpPointsAppliedTamedField()();
    if (!points) return false;

    const int totalCap = ResolveTotalCap(className);
    const int total = SumTamedPoints(points);

    if (totalCap >= 0 && total > totalCap)
    {
        out->isTotal = true;
        out->statIndex = -1;
        out->applied = total;
        out->cap = totalCap;
        return true;
    }

    int caps[STAT_COUNT];
    ResolveDinoCaps(className, caps);

    for (int i = 0; i < STAT_COUNT; ++i)
    {
        if (caps[i] < 0) continue;

        const int applied = static_cast<int>(points[i]);
        if (applied <= caps[i]) continue;

        out->isTotal = false;
        out->statIndex = i;
        out->applied = applied;
        out->cap = caps[i];
        return true;
    }

    return false;
}

static void ReportBreach(APrimalDinoCharacter* dino, const std::string& className,
    const CapBreach& breach, AShooterPlayerController* pc, const char* trigger)
{
    std::wstring message;
    std::wstring color;
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        message = breach.isTotal ? g_total_over_cap_message : g_over_cap_message;
        color = g_message_color;
    }

    if (breach.isTotal)
    {
        Log::GetLog()->info("[StatCap] OVER_CAP_DESTROY trigger={} class={} reason=total applied={} cap={}",
            trigger, className, breach.applied, breach.cap);

        Notify(pc, BuildTotalMessage(message, breach.cap, breach.applied), color);
    }
    else
    {
        Log::GetLog()->info("[StatCap] OVER_CAP_DESTROY trigger={} class={} reason=stat stat={} applied={} cap={}",
            trigger, className, STAT_NAMES[breach.statIndex], breach.applied, breach.cap);

        Notify(pc, BuildStatMessage(message, breach.statIndex, breach.cap), color);
    }

    QueueDestroy(dino);
}

static void FlushPendingChecks()
{
    std::vector<APrimalDinoCharacter*> due;

    {
        std::lock_guard<std::mutex> lock(g_pending_mutex);
        if (g_pending_checks.empty()) return;

        const auto now = std::chrono::steady_clock::now();
        for (size_t i = 0; i < g_pending_checks.size(); )
        {
            if (now < g_pending_checks[i].checkAt)
            {
                ++i;
                continue;
            }

            due.push_back(g_pending_checks[i].dino);
            g_pending_checks.erase(g_pending_checks.begin() + i);
        }
    }

    if (due.empty()) return;

    bool enabled;
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        enabled = g_destroy_over_cap && g_check_on_begin_play;
    }

    if (!enabled) return;

    for (APrimalDinoCharacter* dino : due)
    {
        if (!dino) continue;
        if (!UVictoryCore::BPIsValidLowLevelFast(dino)) continue;
        if (dino->bActorIsBeingDestroyed()()) continue;
        if (dino->TamingTeamIDField() == 0) continue;

        const std::string className = GetDinoClassName(dino);

        CapBreach breach;
        if (!FindCapBreach(dino, className, &breach)) continue;

        ReportBreach(dino, className, breach, nullptr, "beginplay");
    }
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

    unsigned __int8* points = isDino
        ? forStatusComp->NumberOfLevelUpPointsAppliedTamedField()()
        : forStatusComp->NumberOfLevelUpPointsAppliedField()();

    if (!points)
    {
        Original_ServerRequestLevelUp(pc, forStatusComp, valueType);
        return;
    }

    const std::string className = isDino ? GetDinoClassName(owner) : "";

    if (isDino)
    {
        const int totalCap = ResolveTotalCap(className);
        if (totalCap >= 0 && SumTamedPoints(points) >= totalCap)
        {
            std::wstring message;
            std::wstring color;
            {
                std::lock_guard<std::mutex> lock(g_config_mutex);
                message = g_total_denied_message;
                color = g_message_color;
            }

            Notify(pc, BuildTotalMessage(message, totalCap, totalCap), color);
            return;
        }
    }

    int cap = CAP_NONE;
    if (isDino)
    {
        int caps[STAT_COUNT];
        ResolveDinoCaps(className, caps);
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

    Notify(pc, BuildStatMessage(message, idx, cap), color);
}

static void Detour_OnUncryo(APrimalDinoCharacter* dino, AShooterPlayerController* pc)
{
    Original_OnUncryo(dino, pc);

    if (!dino) return;

    bool enabled;
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        enabled = g_destroy_over_cap && g_check_on_uncryo;
    }

    if (!enabled) return;

    const std::string className = GetDinoClassName(dino);

    CapBreach breach;
    if (!FindCapBreach(dino, className, &breach)) return;

    ReportBreach(dino, className, breach, pc, "uncryo");
}

static void Detour_BeginPlay(APrimalDinoCharacter* dino)
{
    Original_BeginPlay(dino);

    if (!dino) return;

    bool enabled;
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        enabled = g_destroy_over_cap && g_check_on_begin_play;
    }

    if (!enabled) return;

    QueuePendingCheck(dino);
}

static void TimerCallback()
{
    FlushPendingChecks();
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

    AsaApi::GetHooks().SetHook(
        "APrimalDinoCharacter.BeginPlay()",
        (LPVOID)&Detour_BeginPlay,
        (LPVOID*)&Original_BeginPlay
    );

    AsaApi::GetCommands().AddOnTimerCallback(FString(L"StatCap_Timer"), &TimerCallback);

    Log::GetLog()->info("[StatCap] Plugin loaded");
}

static void PluginUnload()
{
    AsaApi::GetCommands().RemoveOnTimerCallback(FString(L"StatCap_Timer"));

    AsaApi::GetHooks().DisableHook(
        "APrimalDinoCharacter.BeginPlay()",
        (LPVOID)&Detour_BeginPlay
    );

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