/*
PlayerRename - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * PlayerRename - ASA Plugin
 *
 * Hook category: Chat
 *
 * Chat commands:
 *   /rename {newname} — rename the player's survivor
 *
 * Config:
 *   ArkApi/Plugins/PlayerRename/config.json
 *   MinLength, MaxLength, BlockedNames (case-insensitive substring match)
 *
 * Mechanism:
 *   Constructs a temporary UShooterCheatManager, swaps it onto the player controller,
 *   temporarily grants admin, executes RenamePlayer via ProcessConsoleExec,
 *   then restores original state and destroys the temporary cheat manager.
 */

#pragma warning(disable: 4191)
#pragma comment(lib, "AsaApi")

#include <API/ARK/Ark.h>
#include <json.hpp>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <sys/stat.h>

// =============================================================================
// Configuration
// =============================================================================

static const std::string g_config_path = "ArkApi/Plugins/PlayerRename/config.json";
static int g_min_length = 3;
static int g_max_length = 24;
static std::vector<std::string> g_blocked_names;
static time_t g_config_last_modified = 0;
static __int64 g_config_last_size = 0;
static float g_config_check_accumulator = 0.0f;

static std::string ToLower(const std::string& s)
{
    std::string out = s;
    for (auto& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

static bool LoadConfig()
{
    std::ifstream file(g_config_path);
    if (!file.is_open())
    {
        Log::GetLog()->error("[PlayerRename] Cannot open config: {}", g_config_path);
        return false;
    }

    try
    {
        nlohmann::json j;
        file >> j;
        g_min_length = j.value("MinLength", 3);
        g_max_length = j.value("MaxLength", 24);

        g_blocked_names.clear();
        if (j.contains("BlockedNames") && j["BlockedNames"].is_array())
        {
            for (const auto& entry : j["BlockedNames"])
            {
                if (entry.is_string())
                    g_blocked_names.push_back(ToLower(entry.get<std::string>()));
            }
        }
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[PlayerRename] Config parse error: {}", ex.what());
        return false;
    }

    struct _stat st;
    if (_stat(g_config_path.c_str(), &st) == 0)
    {
        g_config_last_modified = st.st_mtime;
        g_config_last_size = st.st_size;
    }

    Log::GetLog()->info("[PlayerRename] Config loaded: MinLength={} MaxLength={} BlockedNames={}",
        g_min_length, g_max_length, g_blocked_names.size());
    return true;
}

static void CheckConfigReload()
{
    struct _stat st;
    if (_stat(g_config_path.c_str(), &st) != 0) return;
    if (st.st_size == 0) return;
    if (st.st_mtime == g_config_last_modified && st.st_size == g_config_last_size) return;

    Log::GetLog()->info("[PlayerRename] Config change detected, reloading...");
    LoadConfig();
}

// =============================================================================
// Helpers
// =============================================================================

static std::string FStr(const FString& f)
{
    const char* s = TCHAR_TO_UTF8(*f);
    return (s && s[0]) ? s : "";
}

static std::string GetEos(AShooterPlayerController* pc)
{
    if (!pc) return "";
    AShooterPlayerState* ps =
        static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
    if (!ps) return "";
    FString eosRaw;
    ps->GetUniqueNetIdAsString(&eosRaw);
    std::string eos = FStr(eosRaw);
    return (eos == "unknown") ? "" : eos;
}

static std::string GetCurrentName(AShooterPlayerController* pc)
{
    if (!pc) return "";
    FString nameRaw;
    pc->GetPlayerCharacterName(&nameRaw);
    return FStr(nameRaw);
}

static bool IsBlocked(const std::string& name)
{
    const std::string lower = ToLower(name);
    for (const auto& blocked : g_blocked_names)
    {
        if (lower.find(blocked) != std::string::npos)
            return true;
    }
    return false;
}

static void SendMessage(AShooterPlayerController* pc, const wchar_t* text)
{
    FLinearColor color{ 1.0f, 0.65f, 0.0f, 1.0f };
    AsaApi::GetApiUtils().SendNotification(pc, color, 1.5f, 5.0f, nullptr, text);
}

// =============================================================================
// Safe Command Execution (UnlockAll pattern)
// =============================================================================

static bool ExecCheatCommand(AShooterPlayerController* pc, const std::wstring& cmd)
{
    UClass* cmClass = UShooterCheatManager::StaticClass();
    if (!cmClass) return false;

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
    if (!cm) return false;

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

    return true;
}

// =============================================================================
// Chat Command
// =============================================================================

static void Cmd_Rename(AShooterPlayerController* pc, FString* message, int, int)
{
    if (!pc) return;

    const std::string raw = FStr(*message);

    std::string newName;
    if (raw.size() > 8)
        newName = raw.substr(8);

    while (!newName.empty() && newName.front() == ' ')
        newName.erase(newName.begin());
    while (!newName.empty() && newName.back() == ' ')
        newName.pop_back();

    if (newName.empty())
    {
        SendMessage(pc, L"Usage: /rename <new name>");
        return;
    }

    if ((int)newName.size() < g_min_length)
    {
        std::wstring msg = L"Name must be at least " + std::to_wstring(g_min_length) + L" characters.";
        SendMessage(pc, msg.c_str());
        return;
    }

    if ((int)newName.size() > g_max_length)
    {
        std::wstring msg = L"Name cannot exceed " + std::to_wstring(g_max_length) + L" characters.";
        SendMessage(pc, msg.c_str());
        return;
    }

    if (IsBlocked(newName))
    {
        SendMessage(pc, L"That name is not allowed.");
        return;
    }

    const std::string currentName = GetCurrentName(pc);
    if (currentName.empty())
    {
        SendMessage(pc, L"Could not resolve your current name.");
        return;
    }

    if (currentName == newName)
    {
        SendMessage(pc, L"That is already your name.");
        return;
    }

    const std::string eosId = GetEos(pc);

    const std::string cmdStr = "RenamePlayer \"" + currentName + "\" " + newName;
    const std::wstring wCmd(cmdStr.begin(), cmdStr.end());

    if (!ExecCheatCommand(pc, wCmd))
    {
        Log::GetLog()->error("[PlayerRename] Failed to create cheat manager for eos_id={}", eosId);
        SendMessage(pc, L"Rename failed. Please try again.");
        return;
    }

    Log::GetLog()->info("[PlayerRename] RENAMED eos_id={} old_name={} new_name={}",
        eosId, currentName, newName);

    std::wstring wNew(newName.begin(), newName.end());
    std::wstring successMsg = L"Your name has been changed to " + wNew + L".";
    SendMessage(pc, successMsg.c_str());
}

// =============================================================================
// Tick (Config Hot-Reload)
// =============================================================================

using Tick_t = void(*)(AShooterGameMode*, float);
static Tick_t Original_Tick = nullptr;

static void Detour_Tick(AShooterGameMode* gm, float delta)
{
    Original_Tick(gm, delta);

    g_config_check_accumulator += delta;
    if (g_config_check_accumulator >= 10.0f)
    {
        g_config_check_accumulator = 0.0f;
        CheckConfigReload();
    }
}

// =============================================================================
// Plugin Entry Points
// =============================================================================

static void InitPlugin()
{
    Log::Get().Init("PlayerRename");

    if (!LoadConfig())
    {
        Log::GetLog()->error("[PlayerRename] Halted - config error");
        return;
    }

    AsaApi::GetCommands().AddChatCommand(
        FString(L"/rename"),
        &Cmd_Rename
    );

    AsaApi::GetHooks().SetHook(
        "AShooterGameMode.Tick(float)",
        (LPVOID)&Detour_Tick,
        (LPVOID*)&Original_Tick
    );

    Log::GetLog()->info("[PlayerRename] Plugin loaded");
}

static void UnloadPlugin()
{
    AsaApi::GetCommands().RemoveChatCommand(
        FString(L"/rename")
    );

    AsaApi::GetHooks().DisableHook(
        "AShooterGameMode.Tick(float)",
        (LPVOID)&Detour_Tick
    );

    Log::GetLog()->info("[PlayerRename] Plugin unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { InitPlugin(); }
    catch (const std::exception& ex) {
        Log::GetLog()->error("[PlayerRename] Init exception: {}", ex.what());
    }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { UnloadPlugin(); }
    catch (const std::exception& ex) {
        Log::GetLog()->error("[PlayerRename] Unload exception: {}", ex.what());
    }
}