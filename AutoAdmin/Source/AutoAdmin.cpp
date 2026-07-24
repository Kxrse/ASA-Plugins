/*
AutoAdmin - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * AutoAdmin - ASA Plugin
 *
 * Hooks:
 *   AShooterGameMode.PostLogin                                     queue existing characters for admin grant
 *   AShooterPlayerController.HandleRespawned_Implementation        queue new spawns for admin grant
 *   AShooterGameMode.Tick                                          process pending grants after the configured delay, process pending kicks, hot-reload config every 10s
 *   ABasePlayerController.ServerCheat_Implementation               block cheat commands for restricted admins unless whitelisted
 *   AShooterPlayerController.RemoteServerCheat_Implementation      same block applied to the remote cheat path
 *   AShooterGameMode.Logout                                        clean up granted state on disconnect
 *
 * Dependencies:
 *   Permissions (optional). Resolved lazily via GetProcAddress from a loaded Permissions.dll,
 *   never statically linked. Only RemovePlayerFromGroup is used, to pull AutoAdmin-granted
 *   players from the admin group when they are downgraded. If Permissions.dll is absent the
 *   feature is skipped with a single warning.
 *
 * Config:
 *   ArkApi/Plugins/AutoAdmin/config.json
 *   AdminPassword: server admin password string
 *   ActivationDelaySeconds: seconds between spawn and admin activation (default 10)
 *   RevokePermissionsOnDowngrade: remove downgraded admins from the Permissions group (default true)
 *   PermissionsAdminGroup: Permissions group to remove downgraded admins from (default "Admins")
 *   Admins: array of admin objects with Alias, EosId, Enabled, UseAllCommands, ForceOnSpawn map, WhitelistedCommands array
 *
 * Config Example:
 * {
 *     "AdminPassword": "Password",
 *     "ActivationDelaySeconds": 10,
 *     "RevokePermissionsOnDowngrade": true,
 *     "PermissionsAdminGroup": "Admins",
 *     "Admins": [
 *         {
 *             "Alias": "Owner",
 *             "EosId": "EOS_ID",
 *             "Enabled": true,
 *             "UseAllCommands": true,
 *             "ForceOnSpawn": {
 *                 "gcm": true
 *             },
 *             "WhitelistedCommands": []
 *         },
 *         {
 *             "Alias": "Mod",
 *             "EosId": "EOS_ID",
 *             "Enabled": true,
 *             "UseAllCommands": false,
 *             "ForceOnSpawn": {},
 *             "WhitelistedCommands": [
 *                 "cheat fly",
 *                 "cheat walk"
 *             ]
 *         }
 *     ]
 * }
 *
 * Command blocking:
 *   Cheat hooks receive commands without the "cheat " prefix (e.g. "gcm" not "cheat gcm").
 *   Plugin prepends "cheat " for whitelist matching against config entries.
 *   Both the direct and remote cheat paths route through one shared gate.
 *   Restricted admins cannot chain commands: any '|' or control character is blocked before whitelist matching.
 *
 * Hot-reload:
 *   config.json rescanned every 10 seconds via size + last-write-time. Size 0 or locked files are rejected.
 *   Downgraded or removed admins are revoked from the Permissions group (if granted by AutoAdmin), then kicked.
 *   Newly enabled online admins are queued for grant.
 *   Kicks resolve the controller via FindPlayerFromEOSID then KickPlayerController.
 */

#include <API/ARK/Ark.h>

#pragma warning(disable: 4191)
#pragma comment(lib, "AsaApi.lib")

#include <json.hpp>
#include <Windows.h>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <filesystem>
#include <optional>
#include <algorithm>
#include <cctype>

// =============================================================================
// Configuration
// =============================================================================

struct AdminEntry
{
    std::string alias;
    std::string eosId;
    bool enabled = false;
    bool useAllCommands = false;
    std::unordered_map<std::string, bool> forceOnSpawn;
    std::vector<std::string> whitelistedCommands;
};

static const std::string g_config_path = "ArkApi/Plugins/AutoAdmin/config.json";
static std::string g_admin_password;
static int         g_activation_delay_seconds = 10;
static bool        g_revoke_permissions_on_downgrade = true;
static std::string g_permissions_admin_group = "Admins";
static std::unordered_map<std::string, AdminEntry> g_admins;
static std::unordered_map<std::string, AdminEntry> g_prev_admins;
static std::uintmax_t                  g_cfg_size = 0;
static std::filesystem::file_time_type g_cfg_mtime{};

static std::unordered_set<std::string> g_granted;
static std::unordered_set<std::string> g_pending_kicks;

static std::string ToLower(const std::string& in)
{
    std::string out = in;
    for (char& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

static void CaptureConfigStamp()
{
    try
    {
        g_cfg_size = std::filesystem::file_size(g_config_path);
        g_cfg_mtime = std::filesystem::last_write_time(g_config_path);
    }
    catch (...) {}
}

static bool LoadConfig()
{
    std::ifstream file(g_config_path);
    if (!file.is_open())
    {
        Log::GetLog()->error("[AutoAdmin] Cannot open config: {}", g_config_path);
        return false;
    }

    try
    {
        nlohmann::json j;
        file >> j;

        g_admin_password = j.value("AdminPassword", "");
        g_activation_delay_seconds = j.value("ActivationDelaySeconds", 10);
        if (g_activation_delay_seconds < 0)
            g_activation_delay_seconds = 0;

        g_revoke_permissions_on_downgrade = j.value("RevokePermissionsOnDowngrade", true);
        g_permissions_admin_group = j.value("PermissionsAdminGroup", "Admins");

        std::unordered_map<std::string, AdminEntry> newAdmins;
        if (j.contains("Admins") && j["Admins"].is_array())
        {
            for (const auto& entry : j["Admins"])
            {
                if (!entry.is_object()) continue;

                AdminEntry admin;
                admin.alias = entry.value("Alias", "");
                admin.eosId = entry.value("EosId", "");
                admin.enabled = entry.value("Enabled", false);
                admin.useAllCommands = entry.value("UseAllCommands", false);

                if (entry.contains("ForceOnSpawn") && entry["ForceOnSpawn"].is_object())
                {
                    for (auto& [cmd, val] : entry["ForceOnSpawn"].items())
                    {
                        if (val.is_boolean())
                            admin.forceOnSpawn[cmd] = val.get<bool>();
                    }
                }

                if (entry.contains("WhitelistedCommands") && entry["WhitelistedCommands"].is_array())
                {
                    for (const auto& cmd : entry["WhitelistedCommands"])
                    {
                        if (cmd.is_string())
                            admin.whitelistedCommands.push_back(ToLower(cmd.get<std::string>()));
                    }
                }

                if (!admin.eosId.empty())
                    newAdmins[admin.eosId] = admin;
            }
        }

        g_prev_admins = g_admins;
        g_admins = std::move(newAdmins);
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[AutoAdmin] Config file malformed! Keeping previous config. Error: {}", ex.what());
        return false;
    }

    if (g_admin_password.empty())
    {
        Log::GetLog()->error("[AutoAdmin] Config requires AdminPassword");
        return false;
    }

    CaptureConfigStamp();

    int enabledCount = 0;
    for (const auto& [id, admin] : g_admins)
    {
        if (admin.enabled) ++enabledCount;
    }

    Log::GetLog()->info("[AutoAdmin] Config loaded: {} admin(s), {} enabled, activation delay {}s",
        g_admins.size(), enabledCount, g_activation_delay_seconds);
    return true;
}

// =============================================================================
// Helpers
// =============================================================================

static std::string FStr(const FString& f)
{
    const char* s = TCHAR_TO_UTF8(*f);
    return (s && s[0]) ? s : "";
}

static FString ToFString(const std::string& s)
{
    return FString(UTF8_TO_TCHAR(s.c_str()));
}

static bool g_bypass_block = false;

static void RunConsoleCommand(AShooterPlayerController* spc, const std::string& cmd)
{
    FString fCmd = ToFString(cmd);
    FString result;
    g_bypass_block = true;
    spc->ConsoleCommand(&result, &fCmd, false);
    g_bypass_block = false;
}

static std::string GetEosFromController(APlayerController* pc)
{
    if (!pc) return "";
    AShooterPlayerState* ps =
        static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
    if (!ps) return "";
    FString eosRaw;
    ps->GetUniqueNetIdAsString(&eosRaw);
    const std::string eosId = FStr(eosRaw);
    return (eosId == "unknown") ? "" : eosId;
}

static bool HasChainOrControlChar(const std::string& raw)
{
    for (unsigned char c : raw)
    {
        if (c == '|') return true;
        if (c < 0x20) return true;
    }
    return false;
}

static bool IsCommandWhitelisted(const std::string& lowerCmd, const std::vector<std::string>& whitelist)
{
    for (const auto& allowed : whitelist)
    {
        if (lowerCmd.size() >= allowed.size() &&
            lowerCmd.compare(0, allowed.size(), allowed) == 0)
        {
            return true;
        }
    }
    return false;
}

static bool AnyFlagDowngraded(const AdminEntry& oldEntry, const AdminEntry& newEntry)
{
    if (oldEntry.enabled && !newEntry.enabled) return true;
    if (oldEntry.useAllCommands && !newEntry.useAllCommands) return true;

    for (const auto& [cmd, wasEnabled] : oldEntry.forceOnSpawn)
    {
        if (!wasEnabled) continue;
        auto it = newEntry.forceOnSpawn.find(cmd);
        if (it == newEntry.forceOnSpawn.end() || !it->second)
            return true;
    }

    std::unordered_set<std::string> newWhitelist(
        newEntry.whitelistedCommands.begin(),
        newEntry.whitelistedCommands.end());

    for (const auto& cmd : oldEntry.whitelistedCommands)
    {
        if (newWhitelist.find(cmd) == newWhitelist.end())
            return true;
    }

    return false;
}

static bool ShouldBlockCheat(APlayerController* pc, const FString* Msg)
{
    if (!pc || !Msg) return false;

    const std::string eosId = GetEosFromController(pc);
    if (eosId.empty()) return false;

    auto it = g_admins.find(eosId);
    if (it == g_admins.end() || !it->second.enabled || it->second.useAllCommands)
        return false;

    const std::string rawCmd = FStr(*Msg);

    bool blocked = false;
    if (HasChainOrControlChar(rawCmd))
    {
        blocked = true;
    }
    else
    {
        const std::string lowerCmd = "cheat " + ToLower(rawCmd);
        if (IsCommandWhitelisted(lowerCmd, it->second.whitelistedCommands))
            return false;
        blocked = true;
    }

    if (!blocked) return false;

    Log::GetLog()->info("[AutoAdmin] Blocked command from {} ({}): {}",
        it->second.alias, eosId, rawCmd);

    if (pc->IsA(AShooterPlayerController::GetPrivateStaticClass()))
    {
        AShooterPlayerController* spc = static_cast<AShooterPlayerController*>(pc);
        FLinearColor red{ 1.0f, 0.0f, 0.0f, 1.0f };
        AsaApi::GetApiUtils().SendNotification(spc, red, 1.5f, 5.0f, nullptr,
            L"YOU AREN'T ALLOWED TO USE THIS COMMAND!");
    }

    return true;
}

// =============================================================================
// Permissions Dependency (lazy)
// =============================================================================

typedef std::optional<std::string>(*RemovePlayerFromGroup_t)(const FString&, const FString&);

static const char* kPermRemovePlayerFromGroup =
    "?RemovePlayerFromGroup@Permissions@@YA?AV?$optional@V?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@std@@AEBVFString@@0@Z";

static RemovePlayerFromGroup_t p_RemovePlayerFromGroup = nullptr;
static bool g_perm_resolved = false;
static bool g_perm_logged_missing = false;

static bool LoadPermissionsApi()
{
    if (g_perm_resolved)
        return p_RemovePlayerFromGroup != nullptr;

    HMODULE permModule = GetModuleHandleA("Permissions.dll");
    if (!permModule)
    {
        if (!g_perm_logged_missing)
        {
            Log::GetLog()->warn("[AutoAdmin] Permissions.dll not loaded, group revoke skipped");
            g_perm_logged_missing = true;
        }
        return false;
    }

    p_RemovePlayerFromGroup =
        (RemovePlayerFromGroup_t)GetProcAddress(permModule, kPermRemovePlayerFromGroup);

    g_perm_resolved = true;

    if (!p_RemovePlayerFromGroup)
    {
        Log::GetLog()->error("[AutoAdmin] Failed to resolve Permissions RemovePlayerFromGroup");
        return false;
    }

    return true;
}

static void RevokePermissionsAdmin(const std::string& eosId)
{
    if (!g_revoke_permissions_on_downgrade) return;
    if (eosId.empty()) return;
    if (!LoadPermissionsApi()) return;

    FString fEos = ToFString(eosId);
    FString fGroup = ToFString(g_permissions_admin_group);

    std::optional<std::string> ret = p_RemovePlayerFromGroup(fEos, fGroup);
    if (ret.has_value())
        Log::GetLog()->warn("[AutoAdmin] Permissions revoke for {} returned: {}", eosId, ret.value());
    else
        Log::GetLog()->info("[AutoAdmin] Removed {} from Permissions group {}", eosId, g_permissions_admin_group);
}

// =============================================================================
// Pending Queue
// =============================================================================

static std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_pending;

// =============================================================================
// Hook Type Aliases
// =============================================================================

using PostLogin_t = void(*)(AShooterGameMode*, APlayerController*);
using HandleRespawned_t = void(*)(AShooterPlayerController*, APawn*, bool);
using Tick_t = void(*)(AShooterGameMode*, float);
using ServerCheat_t = void(*)(ABasePlayerController*, const FString*);
using RemoteServerCheat_t = void(*)(AShooterPlayerController*, const FString*);
using Logout_t = void(*)(AShooterGameMode*, AController*);

static PostLogin_t         Original_PostLogin = nullptr;
static HandleRespawned_t   Original_HandleRespawned = nullptr;
static Tick_t              Original_Tick = nullptr;
static ServerCheat_t       Original_ServerCheat = nullptr;
static RemoteServerCheat_t Original_RemoteServerCheat = nullptr;
static Logout_t            Original_Logout = nullptr;

// =============================================================================
// Shared Queue Helper
// =============================================================================

static void QueueIfAdmin(AShooterPlayerController* spc)
{
    if (!spc) return;

    const std::string eosId = GetEosFromController(spc);
    if (eosId.empty()) return;

    auto it = g_admins.find(eosId);
    if (it != g_admins.end() && it->second.enabled)
    {
        g_pending[eosId] = std::chrono::steady_clock::now();
        Log::GetLog()->info("[AutoAdmin] Queued admin grant for {} ({})",
            it->second.alias, eosId);
    }
}

// =============================================================================
// Config Reload Sync
// =============================================================================

static void SyncOnlinePlayersAfterReload()
{
    for (const auto& eosId : g_granted)
    {
        bool shouldKick = false;

        auto newIt = g_admins.find(eosId);
        auto oldIt = g_prev_admins.find(eosId);

        if (newIt == g_admins.end())
        {
            shouldKick = true;
        }
        else if (oldIt != g_prev_admins.end())
        {
            shouldKick = AnyFlagDowngraded(oldIt->second, newIt->second);
        }

        if (shouldKick)
        {
            std::string alias;
            if (newIt != g_admins.end())
                alias = newIt->second.alias;
            else if (oldIt != g_prev_admins.end())
                alias = oldIt->second.alias;
            else
                alias = eosId;

            RevokePermissionsAdmin(eosId);

            g_pending_kicks.insert(eosId);
            Log::GetLog()->info("[AutoAdmin] Queued kick for {} ({}) due to config downgrade", alias, eosId);
        }
    }

    for (const auto& [eosId, admin] : g_admins)
    {
        if (!admin.enabled) continue;
        if (g_granted.count(eosId)) continue;
        if (g_pending_kicks.count(eosId)) continue;

        FString fEos = ToFString(eosId);
        AShooterPlayerController* spc =
            AsaApi::GetApiUtils().FindPlayerFromEOSID(fEos);

        if (spc)
        {
            g_pending[eosId] = std::chrono::steady_clock::now();
            Log::GetLog()->info("[AutoAdmin] Queued new admin grant for {} ({})",
                admin.alias, eosId);
        }
    }
}

// =============================================================================
// Detours
// =============================================================================

void Detour_PostLogin(AShooterGameMode* gm, APlayerController* pc)
{
    Original_PostLogin(gm, pc);
    if (pc && pc->IsA(AShooterPlayerController::GetPrivateStaticClass()))
        QueueIfAdmin(static_cast<AShooterPlayerController*>(pc));
}

void Detour_HandleRespawned(AShooterPlayerController* spc, APawn* pawn, bool bNewPlayer)
{
    Original_HandleRespawned(spc, pawn, bNewPlayer);
    QueueIfAdmin(spc);
}

void Detour_ServerCheat(ABasePlayerController* bpc, const FString* Msg)
{
    if (g_bypass_block)
    {
        Original_ServerCheat(bpc, Msg);
        return;
    }

    if (ShouldBlockCheat(bpc, Msg))
        return;

    Original_ServerCheat(bpc, Msg);
}

void Detour_RemoteServerCheat(AShooterPlayerController* spc, const FString* Msg)
{
    if (g_bypass_block)
    {
        Original_RemoteServerCheat(spc, Msg);
        return;
    }

    if (ShouldBlockCheat(spc, Msg))
        return;

    Original_RemoteServerCheat(spc, Msg);
}

void Detour_Logout(AShooterGameMode* gm, AController* controller)
{
    if (controller && controller->IsA(AShooterPlayerController::GetPrivateStaticClass()))
    {
        AShooterPlayerController* spc = static_cast<AShooterPlayerController*>(controller);
        const std::string eosId = GetEosFromController(spc);
        if (!eosId.empty())
        {
            g_granted.erase(eosId);
            g_pending.erase(eosId);
            g_pending_kicks.erase(eosId);
        }
    }

    Original_Logout(gm, controller);
}

static float g_config_check_accumulator = 0.0f;

void Detour_Tick(AShooterGameMode* gm, float delta)
{
    Original_Tick(gm, delta);

    if (!g_pending_kicks.empty())
    {
        std::unordered_set<std::string> kicksCopy;
        kicksCopy.swap(g_pending_kicks);

        for (const auto& eosId : kicksCopy)
        {
            FString fEos = ToFString(eosId);
            AShooterPlayerController* spc =
                AsaApi::GetApiUtils().FindPlayerFromEOSID(fEos);

            if (spc)
            {
                FString reason = L"Admin access revoked";
                AsaApi::GetApiUtils().GetShooterGameMode()->KickPlayerController(spc, reason);
                Log::GetLog()->info("[AutoAdmin] Kicked admin ({})", eosId);
            }
            else
            {
                Log::GetLog()->warn("[AutoAdmin] Kick skipped, player not found for eos_id={}", eosId);
            }

            g_granted.erase(eosId);
            g_pending.erase(eosId);
        }

        return;
    }

    if (!g_pending.empty())
    {
        const auto now = std::chrono::steady_clock::now();

        for (auto it = g_pending.begin(); it != g_pending.end(); )
        {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - it->second).count();

            if (elapsed < g_activation_delay_seconds)
            {
                ++it;
                continue;
            }

            FString fEos = ToFString(it->first);

            AShooterPlayerController* spc =
                AsaApi::GetApiUtils().FindPlayerFromEOSID(fEos);

            if (spc)
            {
                auto adminIt = g_admins.find(it->first);
                if (adminIt != g_admins.end() && adminIt->second.enabled)
                {
                    const AdminEntry& admin = adminIt->second;

                    RunConsoleCommand(spc, "enablecheats " + g_admin_password);

                    FLinearColor green{ 0.0f, 1.0f, 0.0f, 1.0f };
                    AsaApi::GetApiUtils().SendNotification(spc, green, 1.5f, 5.0f, nullptr,
                        L"ADMIN MODE ENABLED");

                    Log::GetLog()->info("[AutoAdmin] Enabled admin for {} ({}) [AllCmds={}]",
                        admin.alias, it->first, admin.useAllCommands);

                    for (const auto& [cmd, active] : admin.forceOnSpawn)
                    {
                        if (active)
                            RunConsoleCommand(spc, cmd);
                    }

                    g_granted.insert(it->first);
                }
            }
            else
            {
                Log::GetLog()->warn("[AutoAdmin] Player not found for eos_id={}", it->first);
            }

            it = g_pending.erase(it);
        }
    }

    g_config_check_accumulator += delta;
    if (g_config_check_accumulator >= 10.0f)
    {
        g_config_check_accumulator = 0.0f;

        std::uintmax_t size = 0;
        std::filesystem::file_time_type mtime{};
        try
        {
            size = std::filesystem::file_size(g_config_path);
            mtime = std::filesystem::last_write_time(g_config_path);
        }
        catch (...) { size = 0; }

        if (size != 0 && (size != g_cfg_size || mtime != g_cfg_mtime))
        {
            Log::GetLog()->info("[AutoAdmin] Config change detected, reloading...");
            if (LoadConfig())
                SyncOnlinePlayersAfterReload();
        }
    }
}

// =============================================================================
// Plugin Entry Points
// =============================================================================

static void InitImpl()
{
    Log::Get().Init("AutoAdmin");

    if (!LoadConfig())
    {
        Log::GetLog()->error("[AutoAdmin] Halted - config error");
        return;
    }

    AsaApi::GetHooks().SetHook(
        "AShooterGameMode.PostLogin(APlayerController*)",
        (LPVOID)&Detour_PostLogin,
        (LPVOID*)&Original_PostLogin
    );

    AsaApi::GetHooks().SetHook(
        "AShooterPlayerController.HandleRespawned_Implementation(APawn*,bool)",
        (LPVOID)&Detour_HandleRespawned,
        (LPVOID*)&Original_HandleRespawned
    );

    AsaApi::GetHooks().SetHook(
        "AShooterGameMode.Tick(float)",
        (LPVOID)&Detour_Tick,
        (LPVOID*)&Original_Tick
    );

    AsaApi::GetHooks().SetHook(
        "ABasePlayerController.ServerCheat_Implementation(FString&)",
        (LPVOID)&Detour_ServerCheat,
        (LPVOID*)&Original_ServerCheat
    );

    AsaApi::GetHooks().SetHook(
        "AShooterPlayerController.RemoteServerCheat_Implementation(FString&)",
        (LPVOID)&Detour_RemoteServerCheat,
        (LPVOID*)&Original_RemoteServerCheat
    );

    AsaApi::GetHooks().SetHook(
        "AShooterGameMode.Logout(AController*)",
        (LPVOID)&Detour_Logout,
        (LPVOID*)&Original_Logout
    );

    Log::GetLog()->info("[AutoAdmin] Plugin loaded");
}

static void UnloadImpl()
{
    AsaApi::GetHooks().DisableHook(
        "ABasePlayerController.ServerCheat_Implementation(FString&)",
        (LPVOID)&Detour_ServerCheat
    );

    AsaApi::GetHooks().DisableHook(
        "AShooterPlayerController.RemoteServerCheat_Implementation(FString&)",
        (LPVOID)&Detour_RemoteServerCheat
    );

    AsaApi::GetHooks().DisableHook(
        "AShooterGameMode.Tick(float)",
        (LPVOID)&Detour_Tick
    );

    AsaApi::GetHooks().DisableHook(
        "AShooterPlayerController.HandleRespawned_Implementation(APawn*,bool)",
        (LPVOID)&Detour_HandleRespawned
    );

    AsaApi::GetHooks().DisableHook(
        "AShooterGameMode.PostLogin(APlayerController*)",
        (LPVOID)&Detour_PostLogin
    );

    AsaApi::GetHooks().DisableHook(
        "AShooterGameMode.Logout(AController*)",
        (LPVOID)&Detour_Logout
    );

    g_pending.clear();
    g_granted.clear();
    g_prev_admins.clear();
    g_pending_kicks.clear();

    Log::GetLog()->info("[AutoAdmin] Plugin unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { InitImpl(); }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[AutoAdmin] Plugin_Init exception: {}", ex.what());
    }
    catch (...)
    {
        Log::GetLog()->error("[AutoAdmin] Plugin_Init unknown exception");
    }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { UnloadImpl(); }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[AutoAdmin] Plugin_Unload exception: {}", ex.what());
    }
    catch (...)
    {
        Log::GetLog()->error("[AutoAdmin] Plugin_Unload unknown exception");
    }
}