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
 *
 * Grants engrams, explorer notes and skills to players once their character exists,
 * gated per Permissions group. Re-grants after a mindwipe and on new character creation.
 *
 * Hooks:
 *   AShooterGameMode.PostLogin(APlayerController*)                            queue on join, covers reconnects with an existing character
 *   AShooterPlayerController.HandleRespawned_Implementation(APawn*,bool)      queue on spawn, re-arm on new character
 *   AShooterPlayerState.DoRespec(UPrimalPlayerData*,AShooterCharacter*,bool)  re-arm after a mindwipe
 *   AShooterGameMode.OnLogout_Implementation(AController*)                    drop pending and processed state for the leaver
 *
 * Timer:
 *   UnlockAll_Timer  1s callback, drains the pending queue and checks the config every 10s
 *
 * Config:
 *   ArkApi/Plugins/UnlockAll/config.json
 *   GrantDelaySeconds: seconds to wait after a spawn before granting
 *   GrantTimeoutSeconds: seconds after which a pending entry is abandoned, must exceed GrantDelaySeconds
 *   Default: base tier applied to anyone with no matching group
 *   Groups: per Permissions group tier overrides. Every group requires a unique integer
 *           Priority. Lower Priority wins (1 is highest). Missing or duplicate Priority is
 *           a hard config error and the plugin halts. Group names match the Permissions
 *           group name exactly and are case sensitive.
 *   EngramMode: one of None, TekOnly, All. Any other value is a hard config error.
 *               All grants every engram including tek. TekOnly grants tek engrams only.
 *   ExplorerNotes: unlocks all explorer notes, plus the Lost Colony notes when the player owns it
 *   Skills: unlocks all skills
 *
 * Config Example:
 * {
 *     "GrantDelaySeconds": 10,
 *     "GrantTimeoutSeconds": 180,
 *     "Default": {
 *         "EngramMode": "All",
 *         "ExplorerNotes": true,
 *         "Skills": true
 *     },
 *     "Groups": {
 *         "Admins": {
 *             "Priority": 1,
 *             "EngramMode": "All",
 *             "ExplorerNotes": true,
 *             "Skills": true
 *         },
 *         "Vip": {
 *             "Priority": 2,
 *             "EngramMode": "TekOnly",
 *             "ExplorerNotes": true,
 *             "Skills": false
 *         }
 *     }
 * }
 */

#include <API/ARK/Ark.h>
#include <json.hpp>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <sys/stat.h>

#pragma comment(lib, "AsaApi.lib")
#pragma warning(disable: 4191)

enum class EngramMode
{
    None,
    TekOnly,
    All
};

struct GroupConfig
{
    int        priority = 0;
    EngramMode engramMode = EngramMode::All;
    bool       explorerNotes = true;
    bool       skills = true;
};

static const std::string g_config_path = "ArkApi/Plugins/UnlockAll/config.json";

static int         g_grant_delay_seconds = 10;
static int         g_grant_timeout_seconds = 180;
static GroupConfig g_default_group;
static std::unordered_map<std::string, GroupConfig> g_groups;

static time_t    g_config_last_modified = 0;
static long long g_config_last_size = 0;
static int       g_tick_counter = 0;

static std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_pending;
static std::unordered_set<std::string> g_processed;

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

static AShooterPlayerController* FindPlayer(const std::string& eosId)
{
    const std::wstring wEos(eosId.begin(), eosId.end());
    FString fEos(wEos.c_str());
    return AsaApi::GetApiUtils().FindPlayerFromEOSID(fEos);
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

static bool ParseEngramMode(const std::string& in, EngramMode& out)
{
    if (in == "None") { out = EngramMode::None; return true; }
    if (in == "TekOnly") { out = EngramMode::TekOnly; return true; }
    if (in == "All") { out = EngramMode::All; return true; }
    return false;
}

static bool ParseGroup(const nlohmann::json& j, const GroupConfig& fallback, const std::string& label, GroupConfig& out)
{
    out = fallback;
    out.priority = j.value("Priority", 0);

    if (j.contains("EngramMode"))
    {
        if (!j["EngramMode"].is_string())
        {
            Log::GetLog()->error("[UnlockAll] EngramMode on '{}' must be a string", label);
            return false;
        }
        if (!ParseEngramMode(j["EngramMode"].get<std::string>(), out.engramMode))
        {
            Log::GetLog()->error("[UnlockAll] Invalid EngramMode '{}' on '{}', expected None, TekOnly or All",
                j["EngramMode"].get<std::string>(), label);
            return false;
        }
    }

    out.explorerNotes = j.value("ExplorerNotes", fallback.explorerNotes);
    out.skills = j.value("Skills", fallback.skills);
    return true;
}

static bool LoadConfig()
{
    std::ifstream file(g_config_path);
    if (!file.is_open())
    {
        Log::GetLog()->error("[UnlockAll] Cannot open config: {}", g_config_path);
        return false;
    }

    int         newDelay = 10;
    int         newTimeout = 180;
    GroupConfig newDefault;
    std::unordered_map<std::string, GroupConfig> newGroups;

    try
    {
        nlohmann::json j;
        file >> j;

        newDelay = j.value("GrantDelaySeconds", 10);
        newTimeout = j.value("GrantTimeoutSeconds", 180);

        GroupConfig hardDefault;
        if (j.contains("Default") && j["Default"].is_object())
        {
            if (!ParseGroup(j["Default"], hardDefault, "Default", newDefault))
                return false;
        }
        else
        {
            newDefault = hardDefault;
        }

        std::unordered_set<int> seenPriorities;
        if (j.contains("Groups") && j["Groups"].is_object())
        {
            for (auto& [key, val] : j["Groups"].items())
            {
                if (!val.is_object()) continue;

                if (!val.contains("Priority") || !val["Priority"].is_number_integer())
                {
                    Log::GetLog()->error("[UnlockAll] Group '{}' is missing an integer Priority", key);
                    return false;
                }

                int p = val["Priority"].get<int>();
                if (!seenPriorities.insert(p).second)
                {
                    Log::GetLog()->error("[UnlockAll] Duplicate Priority {} on group '{}'", p, key);
                    return false;
                }

                GroupConfig gc;
                if (!ParseGroup(val, newDefault, key, gc))
                    return false;

                newGroups[key] = gc;
            }
        }
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[UnlockAll] Config parse error: {}", ex.what());
        return false;
    }

    if (newDelay < 0 || newTimeout <= newDelay)
    {
        Log::GetLog()->error("[UnlockAll] GrantTimeoutSeconds must be greater than GrantDelaySeconds and both non negative");
        return false;
    }

    g_grant_delay_seconds = newDelay;
    g_grant_timeout_seconds = newTimeout;
    g_default_group = newDefault;
    g_groups = std::move(newGroups);
    g_config_last_modified = GetFileModTime(g_config_path);
    g_config_last_size = GetFileSize(g_config_path);

    Log::GetLog()->info("[UnlockAll] Config loaded, {} groups", g_groups.size());
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

typedef TArray<FString>(*GetPlayerGroups_t)(const FString&);

static GetPlayerGroups_t pGetPlayerGroups = nullptr;
static bool              g_permissions_loaded = false;
static bool              g_permissions_attempted = false;

static void LoadPermissionsAPI()
{
    if (g_permissions_attempted) return;
    g_permissions_attempted = true;

    HMODULE hMod = GetModuleHandleA("Permissions");
    if (!hMod)
    {
        Log::GetLog()->warn("[UnlockAll] Permissions.dll not loaded, using default tier for all players");
        return;
    }

    pGetPlayerGroups = (GetPlayerGroups_t)GetProcAddress(hMod,
        "?GetPlayerGroups@Permissions@@YA?AV?$TArray@VFString@@V?$TSizedDefaultAllocator@$0CA@@@@@AEBVFString@@@Z");

    if (!pGetPlayerGroups)
    {
        Log::GetLog()->warn("[UnlockAll] Failed to resolve Permissions functions, using default tier");
        return;
    }

    g_permissions_loaded = true;
    Log::GetLog()->info("[UnlockAll] Permissions API loaded");
}

static GroupConfig ResolveTier(const std::string& eosId)
{
    GroupConfig best = g_default_group;

    if (g_groups.empty()) return best;

    if (!g_permissions_attempted) LoadPermissionsAPI();
    if (!g_permissions_loaded || !pGetPlayerGroups) return best;

    try
    {
        const std::wstring wEos(eosId.begin(), eosId.end());
        FString fEos(wEos.c_str());
        TArray<FString> playerGroups = pGetPlayerGroups(fEos);

        bool matched = false;
        for (int i = 0; i < playerGroups.Num(); ++i)
        {
            const std::string groupName = FStr(playerGroups[i]);
            auto it = g_groups.find(groupName);
            if (it != g_groups.end())
            {
                if (!matched || it->second.priority < best.priority)
                {
                    best = it->second;
                    matched = true;
                }
            }
        }
    }
    catch (...)
    {
        Log::GetLog()->warn("[UnlockAll] GetPlayerGroups threw, using default tier");
    }

    return best;
}

static bool OwnsLostColony(AShooterPlayerController* pc)
{
    if (!pc) return false;
    return UVictoryCore::PlayerOwnsLostColony(pc);
}

template <typename F>
static void WithCheatManager(AShooterPlayerController* pc, F&& fn)
{
    if (!pc) return;

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

static void RunConsoleGrant(UShooterCheatManager* cm, AShooterPlayerController* pc, const wchar_t* command)
{
    if (!cm->ProcessConsoleExec(command, nullptr, pc))
        Log::GetLog()->warn("[UnlockAll] Command was not handled by the cheat manager: {}", TCHAR_TO_UTF8(command));
}

static void Grant(AShooterPlayerController* pc, const GroupConfig& gc, bool ownsLC)
{
    WithCheatManager(pc, [&](UShooterCheatManager* cm)
    {
        if (gc.engramMode == EngramMode::All)
        {
            cm->GiveEngrams();
            cm->GiveEngramsTekOnly();
        }
        else if (gc.engramMode == EngramMode::TekOnly)
        {
            cm->GiveEngramsTekOnly();
        }

        if (gc.explorerNotes)
        {
            RunConsoleGrant(cm, pc, L"UnlockAllExplorerNotes");
            if (ownsLC)
                RunConsoleGrant(cm, pc, L"GiveAllExplorerNotesLC");
        }

        if (gc.skills)
            RunConsoleGrant(cm, pc, L"GiveAllSkills");
    });
}

static void QueuePlayer(AShooterPlayerController* pc)
{
    if (!pc) return;

    const std::string eos = GetEos(pc);
    if (eos.empty()) return;
    if (g_processed.count(eos)) return;
    if (g_pending.count(eos)) return;

    g_pending[eos] = std::chrono::steady_clock::now();
    Log::GetLog()->info("[UnlockAll] Queued eos={}", eos);
}

static void RearmPlayer(AShooterPlayerController* pc)
{
    if (!pc) return;

    const std::string eos = GetEos(pc);
    if (eos.empty()) return;

    g_processed.erase(eos);
    QueuePlayer(pc);
}

static void ProcessPending()
{
    if (g_pending.empty()) return;

    const auto now = std::chrono::steady_clock::now();

    for (auto it = g_pending.begin(); it != g_pending.end(); )
    {
        const std::string eos = it->first;

        if (g_processed.count(eos))
        {
            it = g_pending.erase(it);
            continue;
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();
        if (elapsed < g_grant_delay_seconds)
        {
            ++it;
            continue;
        }

        AShooterPlayerController* pc = FindPlayer(eos);
        if (!pc)
        {
            Log::GetLog()->info("[UnlockAll] Dropping pending entry, player is gone eos={}", eos);
            it = g_pending.erase(it);
            continue;
        }

        if (elapsed > g_grant_timeout_seconds)
        {
            Log::GetLog()->info("[UnlockAll] Gave up waiting for character eos={}", eos);
            it = g_pending.erase(it);
            continue;
        }

        if (!pc->BaseGetPlayerCharacter())
        {
            ++it;
            continue;
        }

        try
        {
            const GroupConfig gc = ResolveTier(eos);
            const bool ownsLC = OwnsLostColony(pc);

            Grant(pc, gc, ownsLC);

            g_processed.insert(eos);
            it = g_pending.erase(it);

            Log::GetLog()->info("[UnlockAll] Completed eos={} lc={}", eos, ownsLC);
        }
        catch (const std::exception& ex)
        {
            Log::GetLog()->error("[UnlockAll] Process exception eos={}: {}", eos, ex.what());
            ++it;
        }
    }
}

static void OnTimerTick()
{
    if (++g_tick_counter % 10 == 0)
        CheckConfigReload();

    ProcessPending();
}

using PostLogin_t = void(*)(AShooterGameMode*, APlayerController*);
static PostLogin_t Original_PostLogin = nullptr;

static void Detour_PostLogin(AShooterGameMode* gm, APlayerController* newPlayer)
{
    Original_PostLogin(gm, newPlayer);

    QueuePlayer(static_cast<AShooterPlayerController*>(newPlayer));
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

using Logout_t = void(*)(AShooterGameMode*, AController*);
static Logout_t Original_Logout = nullptr;

static void Detour_Logout(AShooterGameMode* gm, AController* exiting)
{
    const std::string eos = GetEos(static_cast<AShooterPlayerController*>(exiting));
    if (!eos.empty())
    {
        g_pending.erase(eos);
        g_processed.erase(eos);
    }

    Original_Logout(gm, exiting);
}

static void PluginInit()
{
    Log::Get().Init("UnlockAll");

    if (!LoadConfig())
        Log::GetLog()->error("[UnlockAll] Failed to load config, plugin will not function");

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
        "AShooterGameMode.OnLogout_Implementation(AController*)",
        (LPVOID)&Detour_Logout,
        (LPVOID*)&Original_Logout);

    AsaApi::GetCommands().AddOnTimerCallback(FString(L"UnlockAll_Timer"), &OnTimerTick);

    Log::GetLog()->info("[UnlockAll] Loaded");
}

static void PluginUnload()
{
    AsaApi::GetCommands().RemoveOnTimerCallback(FString(L"UnlockAll_Timer"));

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
        "AShooterGameMode.OnLogout_Implementation(AController*)",
        (LPVOID)&Detour_Logout);

    g_pending.clear();
    g_processed.clear();
    g_groups.clear();
    g_permissions_attempted = false;
    g_permissions_loaded = false;
    pGetPlayerGroups = nullptr;

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