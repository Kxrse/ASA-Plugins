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
 *   AShooterGameMode.PostLogin — queues existing characters for admin grant
 *   AShooterPlayerController.HandleRespawned_Implementation — queues new spawns for admin grant
 *   AShooterGameMode.Tick — processes pending grants after 10s delay, processes pending kicks, hot-reloads config every 10s
 *   ABasePlayerController.ServerCheat_Implementation — blocks cheat commands for restricted admins unless whitelisted
 *   AShooterGameMode.Logout — cleans up granted state
 *
 * Config:
 *   ArkApi/Plugins/AutoAdmin/config.json
 *   AdminPassword: server admin password string
 *   Admins: array of admin objects with ForceOnSpawn map and WhitelistedCommands array
 *
 * Command blocking:
 *   ServerCheat_Implementation receives commands without "cheat " prefix (e.g. "gcm" not "cheat gcm").
 *   Plugin prepends "cheat " for whitelist matching against config entries.
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
static std::unordered_map<std::string, AdminEntry> g_admins;
static std::unordered_map<std::string, AdminEntry> g_prev_admins;
static time_t g_config_last_modified = 0;

static std::unordered_set<std::string> g_granted;
static std::unordered_set<std::string> g_pending_kicks;

static time_t GetFileModTime(const std::string& path)
{
    struct _stat st {};
    if (_stat(path.c_str(), &st) == 0)
        return st.st_mtime;
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
        Log::GetLog()->error("[AutoAdmin] Cannot open config: {}", g_config_path);
        return false;
    }

    try
    {
        nlohmann::json j;
        file >> j;

        g_admin_password = j.value("AdminPassword", "");

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
        g_config_last_modified = GetFileModTime(g_config_path);
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

    int enabledCount = 0;
    for (const auto& [id, admin] : g_admins)
    {
        if (admin.enabled) ++enabledCount;
    }

    Log::GetLog()->info("[AutoAdmin] Config loaded: {} admin(s), {} enabled",
        g_admins.size(), enabledCount);
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

static bool g_bypass_block = false;

static void RunConsoleCommand(AShooterPlayerController* spc, const std::string& cmd)
{
    const std::wstring wCmd(cmd.begin(), cmd.end());
    FString fCmd(wCmd.c_str());
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
using Logout_t = void(*)(AShooterGameMode*, AController*);

static PostLogin_t       Original_PostLogin = nullptr;
static HandleRespawned_t Original_HandleRespawned = nullptr;
static Tick_t            Original_Tick = nullptr;
static ServerCheat_t     Original_ServerCheat = nullptr;
static Logout_t          Original_Logout = nullptr;

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

            g_pending_kicks.insert(eosId);
            Log::GetLog()->info("[AutoAdmin] Queued kick for {} ({}) due to config downgrade", alias, eosId);
        }
    }

    for (const auto& [eosId, admin] : g_admins)
    {
        if (!admin.enabled) continue;
        if (g_granted.count(eosId)) continue;
        if (g_pending_kicks.count(eosId)) continue;

        const std::wstring wEos(eosId.begin(), eosId.end());
        FString fEos(wEos.c_str());
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

    if (bpc && Msg)
    {
        const std::string eosId = GetEosFromController(bpc);

        if (!eosId.empty())
        {
            auto it = g_admins.find(eosId);
            if (it != g_admins.end() && it->second.enabled && !it->second.useAllCommands)
            {
                const std::string rawCmd = FStr(*Msg);
                const std::string lowerCmd = "cheat " + ToLower(rawCmd);

                if (IsCommandWhitelisted(lowerCmd, it->second.whitelistedCommands))
                {
                    Original_ServerCheat(bpc, Msg);
                    return;
                }

                Log::GetLog()->info("[AutoAdmin] Blocked command from {} ({}): {}",
                    it->second.alias, eosId, rawCmd);

                AShooterPlayerController* spc = static_cast<AShooterPlayerController*>(bpc);
                FLinearColor red{ 1.0f, 0.0f, 0.0f, 1.0f };
                AsaApi::GetApiUtils().SendNotification(spc, red, 1.5f, 5.0f, nullptr,
                    L"YOU AREN'T ALLOWED TO USE THIS COMMAND!");

                return;
            }
        }
    }

    Original_ServerCheat(bpc, Msg);
}

void Detour_Logout(AShooterGameMode* gm, AController* controller)
{
    if (controller)
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

    // Process pending kicks first
    if (!g_pending_kicks.empty())
    {
        std::unordered_set<std::string> kicksCopy;
        kicksCopy.swap(g_pending_kicks);

        for (const auto& eosId : kicksCopy)
        {
            const std::wstring wEos(eosId.begin(), eosId.end());
            FString fEos(wEos.c_str());
            gm->KickPlayer(&fEos);

            g_granted.erase(eosId);
            g_pending.erase(eosId);

            Log::GetLog()->info("[AutoAdmin] Kicked admin ({})", eosId);
        }

        return;
    }

    // Process pending admin grants (10 second delay)
    if (!g_pending.empty())
    {
        const auto now = std::chrono::steady_clock::now();

        for (auto it = g_pending.begin(); it != g_pending.end(); )
        {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - it->second).count();

            if (elapsed < 10)
            {
                ++it;
                continue;
            }

            const std::wstring wEos(it->first.begin(), it->first.end());
            FString fEos(wEos.c_str());

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

    // Check config for changes every 10 seconds
    g_config_check_accumulator += delta;
    if (g_config_check_accumulator >= 10.0f)
    {
        g_config_check_accumulator = 0.0f;

        const time_t modTime = GetFileModTime(g_config_path);
        if (modTime != 0 && modTime != g_config_last_modified)
        {
            Log::GetLog()->info("[AutoAdmin] Config change detected, reloading...");
            LoadConfig();
            SyncOnlinePlayersAfterReload();
        }
    }
}

// =============================================================================
// Plugin Entry Points
// =============================================================================

extern "C" __declspec(dllexport) void Plugin_Init()
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
        "AShooterGameMode.Logout(AController*)",
        (LPVOID)&Detour_Logout,
        (LPVOID*)&Original_Logout
    );

    Log::GetLog()->info("[AutoAdmin] Plugin loaded");
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    AsaApi::GetHooks().DisableHook(
        "ABasePlayerController.ServerCheat_Implementation(FString&)",
        (LPVOID)&Detour_ServerCheat
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