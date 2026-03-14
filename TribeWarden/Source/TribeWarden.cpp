/*
TribeWarden - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

#include <API/ARK/Ark.h>
#include <Windows.h>
#include <string>
#include <unordered_map>
#include <mutex>
#include <random>
#include <cstdio>



// =============================================================================
// Hook Originals
// =============================================================================

using HandleRespawned_t = void(*)(AShooterPlayerController*, APawn*, bool);
static HandleRespawned_t Original_HandleRespawned = nullptr;

using NotifyLeft_t = void(*)(AShooterPlayerState*, FString&, FString&, bool);
static NotifyLeft_t Original_NotifyLeft = nullptr;

using GameModeTick_t = void(*)(AShooterGameMode*, float);
static GameModeTick_t Original_GameModeTick = nullptr;

// =============================================================================
// Enforcement Queue
// =============================================================================

struct EnforceState
{
    AShooterPlayerState* ps       = nullptr;
    ULONGLONG            due      = 0;
    int                  attempts = 0;
};

static std::unordered_map<std::string, EnforceState> g_queue;
static std::mutex                                     g_queue_mutex;

// =============================================================================
// Helpers
// =============================================================================

static std::string FStr(const FString& f)
{
    const char* s = TCHAR_TO_UTF8(*f);
    return (s && s[0]) ? s : "unknown";
}

static std::string GetEosId(AShooterPlayerState* ps)
{
    if (!ps) return "unknown";
    FString eos;
    ps->GetUniqueNetIdAsString(&eos);
    return FStr(eos);
}

static bool InTribe(AShooterPlayerState* ps)
{
    return ps && ps->GetTribeId() > 0;
}

static std::string RandomTribeName()
{
    static thread_local std::mt19937_64 rng{
        (uint64_t)GetTickCount64() ^ (uint64_t)(uintptr_t)&rng
    };
    std::uniform_int_distribution<int> dist(0, 9);

    std::string name;
    name.reserve(10);
    for (int i = 0; i < 10; ++i)
        name.push_back((char)('0' + dist(rng)));
    return name;
}

static bool CreateSoloTribe(AShooterPlayerState* ps)
{
    if (!ps) return false;

    const std::string name = RandomTribeName();
    FString fname(name.c_str());
    FTribeGovernment gov{};

    try
    {
        ps->ServerRequestCreateNewTribe(fname, gov);
        Sleep(200);
        return ps->GetTribeId() > 0;
    }
    catch (...) { return false; }
}

static void Schedule(const std::string& eos, AShooterPlayerState* ps)
{
    std::lock_guard<std::mutex> lock(g_queue_mutex);

    EnforceState& st = g_queue[eos];
    st.ps       = ps;
    st.due      = GetTickCount64() + 20000;
    st.attempts = 0;

    Log::GetLog()->info("[TribeWarden] Scheduled enforcement for {}", eos);
}

static FLinearColor MessageColor()
{
    return FLinearColor(0.918f, 0.918f, 0.918f, 1.0f);
}

static AShooterPlayerController* FindPC(AShooterPlayerState* ps)
{
    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world || !ps) return nullptr;
    auto& list = world->PlayerControllerListField();
    for (int i = 0; i < list.Num(); ++i)
    {
        APlayerController* ctrl = list[i];
        if (ctrl && ctrl->PlayerStateField().Get() == ps)
            return static_cast<AShooterPlayerController*>(ctrl);
    }
    return nullptr;
}

static void NotifyForcedJoin(AShooterPlayerState* ps)
{
    AShooterPlayerController* pc = FindPC(ps);
    if (!pc) { Log::GetLog()->warn("[TribeWarden] NotifyForcedJoin — PC not found"); return; }

    FString msg(L"You have been automatically placed into a tribe.");
    pc->ClientServerNotificationSingle(&msg, MessageColor(), 1.0f, 7.0f, nullptr, nullptr, 9001);

    Log::GetLog()->info("[TribeWarden] NotifyForcedJoin — notification sent");
}

// =============================================================================
// Hooks
// =============================================================================

static void Detour_HandleRespawned(AShooterPlayerController* pc, APawn* pawn, bool b)
{
    if (Original_HandleRespawned)
        Original_HandleRespawned(pc, pawn, b);

    if (!pc || !pawn) return;

    AShooterPlayerState* ps = static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
    if (!ps) return;

    const std::string eos = GetEosId(ps);
    if (eos == "unknown" || eos.empty()) return;

    if (InTribe(ps)) return;

    Schedule(eos, ps);
}

static void Detour_NotifyLeft(AShooterPlayerState* ps, FString& a, FString& b, bool c)
{
    if (Original_NotifyLeft)
        Original_NotifyLeft(ps, a, b, c);

    if (!ps) return;

    const std::string eos = GetEosId(ps);
    if (eos == "unknown" || eos.empty()) return;

    Schedule(eos, ps);
}

static void Detour_GameModeTick(AShooterGameMode* gm, float delta)
{
    if (Original_GameModeTick)
        Original_GameModeTick(gm, delta);

    const ULONGLONG now = GetTickCount64();

    std::lock_guard<std::mutex> lock(g_queue_mutex);

    for (auto it = g_queue.begin(); it != g_queue.end(); )
    {
        const std::string& eos = it->first;
        EnforceState& st = it->second;

        if (!st.ps)
        {
            it = g_queue.erase(it);
            continue;
        }

        if (InTribe(st.ps))
        {
            Log::GetLog()->info("[TribeWarden] {} is now in tribe — stopping", eos);
            it = g_queue.erase(it);
            continue;
        }

        if (st.attempts >= 10)
        {
            Log::GetLog()->warn("[TribeWarden] {} exhausted 10 attempts — giving up", eos);
            it = g_queue.erase(it);
            continue;
        }

        if (now < st.due)
        {
            ++it;
            continue;
        }

        st.attempts++;
        const bool ok = CreateSoloTribe(st.ps);

        Log::GetLog()->info("[TribeWarden] Attempt {}/10 for {} — {}", st.attempts, eos, ok ? "OK" : "FAIL");

        if (ok)
        {
            NotifyForcedJoin(st.ps);
            it = g_queue.erase(it);
            continue;
        }

        st.due = now + 5000;
        ++it;
    }
}

// =============================================================================
// Plugin Entry Points
// =============================================================================

extern "C" __declspec(dllexport) void Plugin_Init()
{
    Log::Get().Init("TribeWarden");

    AsaApi::GetHooks().SetHook(
        "AShooterPlayerController.HandleRespawned_Implementation(APawn*,bool)",
        (LPVOID)&Detour_HandleRespawned,
        (LPVOID*)&Original_HandleRespawned
    );

    AsaApi::GetHooks().SetHook(
        "AShooterPlayerState.NotifyPlayerLeftTribe(FString&,FString&,bool)",
        (LPVOID)&Detour_NotifyLeft,
        (LPVOID*)&Original_NotifyLeft
    );

    AsaApi::GetHooks().SetHook(
        "AShooterGameMode.Tick(float)",
        (LPVOID)&Detour_GameModeTick,
        (LPVOID*)&Original_GameModeTick
    );

    Log::GetLog()->info("[TribeWarden] Plugin loaded");
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    AsaApi::GetHooks().DisableHook(
        "AShooterPlayerController.HandleRespawned_Implementation(APawn*,bool)",
        (LPVOID)&Detour_HandleRespawned
    );

    AsaApi::GetHooks().DisableHook(
        "AShooterPlayerState.NotifyPlayerLeftTribe(FString&,FString&,bool)",
        (LPVOID)&Detour_NotifyLeft
    );

    AsaApi::GetHooks().DisableHook(
        "AShooterGameMode.Tick(float)",
        (LPVOID)&Detour_GameModeTick
    );

    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        g_queue.clear();
    }

    Log::GetLog()->info("[TribeWarden] Plugin unloaded");
}