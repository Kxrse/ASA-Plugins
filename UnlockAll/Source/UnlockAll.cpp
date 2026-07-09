/*
UnlockAll - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * UnlockAll - ASA Plugin
 * Grants engrams, tek engrams and explorer notes to players on spawn, gated per Permissions group.
 * Re-grants after a mindwipe and on new character creation.
 *
 * Hooks:
 *   AShooterGameMode.PostLogin                                - queue player on join
 *   AShooterPlayerController.HandleRespawned_Implementation   - queue on spawn, re-arm on new character
 *   AShooterPlayerState.DoRespec                              - re-arm after mindwipe
 *   AShooterGameMode.Tick                                     - drain queue, config hot-reload
 *   AShooterGameMode.OnLogout_Implementation                  - clear processed state
 *
 * Config:
 *   Groups - per Permissions group toggles for Engrams, TekEngrams, ExplorerNotes
 */

#include <API/ARK/Ark.h>
#include <json.hpp>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <sys/stat.h>
#include <algorithm>
#include <cctype>

#pragma comment(lib, "AsaApi.lib")
#pragma warning(disable: 4191)

static constexpr int g_initial_defer_seconds = 10;
static constexpr int g_pending_deadline_seconds = 180;

static std::string FStr(const FString& f)
{
    const char* s = TCHAR_TO_UTF8(*f);
    return (s && s[0]) ? s : "";
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

struct GroupConfig
{
    bool engrams = true;
    bool tekEngrams = true;
    bool explorerNotes = true;
};

static const std::string g_config_path = "ArkApi/Plugins/UnlockAll/config.json";
static std::unordered_map<std::string, GroupConfig> g_groups;
static time_t g_config_last_modified = 0;
static long long g_config_last_size = 0;

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

static std::string ToLower(const std::string& in)
{
    std::string out = in;
    for (char& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

static bool LoadConfig()
{
    std::ifstream file(g_config_path);
    if (!file.is_open())
    {
        Log::GetLog()->error("[UnlockAll] Cannot open config: {}", g_config_path);
        return false;
    }

    try
    {
        nlohmann::json j;
        file >> j;

        std::unordered_map<std::string, GroupConfig> newGroups;
        if (j.contains("Groups") && j["Groups"].is_object())
        {
            for (auto& [name, val] : j["Groups"].items())
            {
                if (!val.is_object()) continue;
                GroupConfig gc;
                gc.engrams = val.value("Engrams", true);
                gc.tekEngrams = val.value("TekEngrams", true);
                gc.explorerNotes = val.value("ExplorerNotes", true);
                newGroups[ToLower(name)] = gc;
            }
        }

        g_groups = std::move(newGroups);
        g_config_last_modified = GetFileModTime(g_config_path);
        g_config_last_size = GetFileSize(g_config_path);
        Log::GetLog()->info("[UnlockAll] Config loaded, {} groups", g_groups.size());
        return true;
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[UnlockAll] Config parse error: {}", ex.what());
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

typedef TArray<FString>(*GetPlayerGroups_t)(const FString&);

static GetPlayerGroups_t pGetPlayerGroups = nullptr;
static bool g_permissions_loaded = false;
static bool g_permissions_attempted = false;

static void LoadPermissionsAPI()
{
    if (g_permissions_attempted) return;
    g_permissions_attempted = true;

    HMODULE hMod = GetModuleHandleA("Permissions");
    if (!hMod)
    {
        Log::GetLog()->warn("[UnlockAll] Permissions.dll not found, using Default group for all players");
        return;
    }

    pGetPlayerGroups = (GetPlayerGroups_t)GetProcAddress(hMod,
        "?GetPlayerGroups@Permissions@@YA?AV?$TArray@VFString@@V?$TSizedDefaultAllocator@$0CA@@@@@AEBVFString@@@Z");

    if (!pGetPlayerGroups)
    {
        Log::GetLog()->warn("[UnlockAll] Failed to resolve Permissions functions");
        return;
    }

    g_permissions_loaded = true;
    Log::GetLog()->info("[UnlockAll] Permissions API loaded");
}

static GroupConfig GetBestGroup(const std::string& eosId)
{
    if (g_permissions_loaded && pGetPlayerGroups)
    {
        std::wstring wEos(eosId.begin(), eosId.end());
        FString fEos(wEos.c_str());
        TArray<FString> groups = pGetPlayerGroups(fEos);

        for (int i = 0; i < groups.Num(); ++i)
        {
            std::string gName = ToLower(FStr(groups[i]));
            if (gName == "default") continue;
            auto it = g_groups.find(gName);
            if (it != g_groups.end())
                return it->second;
        }
    }

    auto def = g_groups.find("default");
    if (def != g_groups.end())
        return def->second;

    return {};
}

struct PendingUnlock
{
    AShooterPlayerController* controller = nullptr;
    std::string eosId;
    std::chrono::steady_clock::time_point queuedAt;
};

static std::vector<PendingUnlock> g_pending;
static std::unordered_set<std::string> g_processed;
static std::chrono::steady_clock::time_point g_last_config_check;

static void QueuePlayer(AShooterPlayerController* pc)
{
    if (!pc) return;
    std::string eos = GetEos(pc);
    if (eos.empty()) return;
    if (g_processed.count(eos)) return;

    for (const auto& p : g_pending)
    {
        if (p.eosId == eos) return;
    }

    PendingUnlock entry;
    entry.controller = pc;
    entry.eosId = eos;
    entry.queuedAt = std::chrono::steady_clock::now();
    g_pending.push_back(entry);
    Log::GetLog()->info("[UnlockAll] Queued eos={}", eos);
}

static void RearmPlayer(AShooterPlayerController* pc)
{
    if (!pc) return;
    std::string eos = GetEos(pc);
    if (eos.empty()) return;

    g_processed.erase(eos);
    QueuePlayer(pc);
}

static bool CheckOwnsLC(AShooterPlayerController* pc)
{
    try
    {
        return UVictoryCore::PlayerOwnsLostColony(pc);
    }
    catch (...)
    {
        return false;
    }
}

template <typename F>
static void WithCheatManager(AShooterPlayerController* pc, F&& fn)
{
    UClass* cmClass = UShooterCheatManager::StaticClass();
    if (!cmClass)
    {
        Log::GetLog()->error("[UnlockAll] Failed to get UShooterCheatManager class");
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
        Log::GetLog()->error("[UnlockAll] Failed to create cheat manager instance");
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
        fn(cm);
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[UnlockAll] Grant callback exception: {}", ex.what());
    }

    if (!wasAdmin)
        pc->bIsAdmin() = false;

    *cmRawPtr = savedCMPtr;
    cm->ConditionalBeginDestroy();
}

static void GrantBase(AShooterPlayerController* pc, const GroupConfig& gc, bool ownsLC)
{
    int count = 0;
    WithCheatManager(pc, [&](UShooterCheatManager* cm)
    {
        if (gc.engrams)
        {
            cm->GiveEngrams();
            count++;
        }
        if (gc.tekEngrams)
        {
            cm->GiveEngramsTekOnly();
            count++;
        }
        if (gc.explorerNotes)
        {
            cm->ProcessConsoleExec(L"UnlockAllExplorerNotes", nullptr, pc);
            count++;
            if (ownsLC)
            {
                cm->ProcessConsoleExec(L"GiveAllExplorerNotesLC", nullptr, pc);
                count++;
            }
        }
    });
    Log::GetLog()->info("[UnlockAll] Applied {} unlocks lc={}", count, ownsLC);
}

static void ProcessPending()
{
    if (g_pending.empty()) return;

    auto now = std::chrono::steady_clock::now();
    std::vector<PendingUnlock> remaining;

    for (auto& p : g_pending)
    {
        if (g_processed.count(p.eosId))
            continue;

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - p.queuedAt).count();

        if (elapsed < g_initial_defer_seconds)
        {
            remaining.push_back(p);
            continue;
        }

        std::string currentEos = GetEos(p.controller);
        if (currentEos != p.eosId)
            continue;

        if (elapsed > g_pending_deadline_seconds)
        {
            Log::GetLog()->warn("[UnlockAll] Gave up waiting for character eos={}", p.eosId);
            continue;
        }

        AActor* ch = p.controller->BaseGetPlayerCharacter();
        if (!ch)
        {
            remaining.push_back(p);
            continue;
        }

        try
        {
            LoadPermissionsAPI();
            GroupConfig gc = GetBestGroup(p.eosId);
            bool ownsLC = CheckOwnsLC(p.controller);

            GrantBase(p.controller, gc, ownsLC);

            g_processed.insert(p.eosId);
            Log::GetLog()->info("[UnlockAll] Completed eos={}", p.eosId);
        }
        catch (const std::exception& ex)
        {
            Log::GetLog()->error("[UnlockAll] Process exception: {}", ex.what());
            remaining.push_back(p);
        }
    }

    g_pending = std::move(remaining);
}

using PostLogin_t = void(*)(AShooterGameMode*, APlayerController*);
static PostLogin_t Original_PostLogin = nullptr;

static void Detour_PostLogin(AShooterGameMode* gm, APlayerController* newPlayer)
{
    Original_PostLogin(gm, newPlayer);
    AShooterPlayerController* spc = static_cast<AShooterPlayerController*>(newPlayer);
    QueuePlayer(spc);
}

using HandleRespawned_t = void(*)(AShooterPlayerController*, APawn*, bool);
static HandleRespawned_t Original_HandleRespawned = nullptr;

static void Detour_HandleRespawned(AShooterPlayerController* pc, APawn* pawn, bool bIsNewCharacter)
{
    Original_HandleRespawned(pc, pawn, bIsNewCharacter);

    if (bIsNewCharacter)
        RearmPlayer(pc);
    else
        QueuePlayer(pc);
}

using DoRespec_t = void(*)(AShooterPlayerState*, UPrimalPlayerData*, AShooterCharacter*, bool);
static DoRespec_t Original_DoRespec = nullptr;

static void Detour_DoRespec(AShooterPlayerState* ps, UPrimalPlayerData* forPlayerData, AShooterCharacter* forCharacter, bool bSetRespecedAtCharacterLevel)
{
    Original_DoRespec(ps, forPlayerData, forCharacter, bSetRespecedAtCharacterLevel);

    if (!ps) return;

    AShooterPlayerController* pc = ps->GetShooterController();
    if (!pc) return;

    Log::GetLog()->info("[UnlockAll] Respec detected, re-arming eos={}", GetEos(pc));
    RearmPlayer(pc);
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
        CheckConfigReload();
    }

    ProcessPending();
}

using Logout_t = void(*)(AShooterGameMode*, AController*);
static Logout_t Original_Logout = nullptr;

static void Detour_Logout(AShooterGameMode* gm, AController* exiting)
{
    AShooterPlayerController* pc = static_cast<AShooterPlayerController*>(exiting);
    std::string eos = GetEos(pc);
    if (!eos.empty())
        g_processed.erase(eos);

    Original_Logout(gm, exiting);
}

static void PluginInit()
{
    Log::Get().Init("UnlockAll");

    if (!LoadConfig())
    {
        Log::GetLog()->error("[UnlockAll] Failed to load config, plugin will not function");
    }

    g_last_config_check = std::chrono::steady_clock::now();

    AsaApi::GetHooks().SetHook(
        "AShooterGameMode.PostLogin(APlayerController*)",
        (LPVOID)&Detour_PostLogin,
        (LPVOID*)&Original_PostLogin);

    AsaApi::GetHooks().SetHook(
        "AShooterPlayerController.HandleRespawned_Implementation(APawn*,bool)",
        (LPVOID)&Detour_HandleRespawned,
        (LPVOID*)&Original_HandleRespawned);

    AsaApi::GetHooks().SetHook(
        "AShooterPlayerState.DoRespec(UPrimalPlayerData*,AShooterCharacter*,bool)",
        (LPVOID)&Detour_DoRespec,
        (LPVOID*)&Original_DoRespec);

    AsaApi::GetHooks().SetHook(
        "AShooterGameMode.Tick(float)",
        (LPVOID)&Detour_Tick,
        (LPVOID*)&Original_Tick);

    AsaApi::GetHooks().SetHook(
        "AShooterGameMode.OnLogout_Implementation(AController*)",
        (LPVOID)&Detour_Logout,
        (LPVOID*)&Original_Logout);

    Log::GetLog()->info("[UnlockAll] Loaded");
}

static void PluginUnload()
{
    AsaApi::GetHooks().DisableHook(
        "AShooterGameMode.PostLogin(APlayerController*)",
        (LPVOID)&Detour_PostLogin);

    AsaApi::GetHooks().DisableHook(
        "AShooterPlayerController.HandleRespawned_Implementation(APawn*,bool)",
        (LPVOID)&Detour_HandleRespawned);

    AsaApi::GetHooks().DisableHook(
        "AShooterPlayerState.DoRespec(UPrimalPlayerData*,AShooterCharacter*,bool)",
        (LPVOID)&Detour_DoRespec);

    AsaApi::GetHooks().DisableHook(
        "AShooterGameMode.Tick(float)",
        (LPVOID)&Detour_Tick);

    AsaApi::GetHooks().DisableHook(
        "AShooterGameMode.OnLogout_Implementation(AController*)",
        (LPVOID)&Detour_Logout);

    Log::GetLog()->info("[UnlockAll] Unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { PluginInit(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->critical("[UnlockAll] Init exception: {}", e.what());
    }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { PluginUnload(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->error("[UnlockAll] Unload exception: {}", e.what());
    }
}