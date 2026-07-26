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
 * Hooks:
 *   None. Config hot-reload runs on the AsaApi timer callback.
 *
 * Chat commands:
 *   /rename {newname}    rename the calling player's own survivor
 *
 * Config:
 *   ArkApi/Plugins/PlayerRename/config.json
 *   MinLength: minimum allowed name length (default 3)
 *   MaxLength: maximum allowed name length (default 24)
 *   BlockedNames: array of case insensitive substrings that reject a name
 *
 * Config Example:
 * {
 *     "MinLength": 3,
 *     "MaxLength": 24,
 *     "BlockedNames": [
 *         "admin",
 *         "moderator",
 *         "server"
 *     ]
 * }
 *
 * Mechanism:
 *   Resolves the caller's own AShooterCharacter and invokes AShooterCharacter::RenamePlayer
 *   directly, which updates the live character and fires the game's own name change path.
 *   The persisted name is then written into FPrimalPlayerDataStruct.MyPlayerCharacterConfig
 *   and the player data is saved, otherwise the old name is restored on reconnect.
 *
 *   No cheat manager is constructed, no admin flag is set, and no console command string is
 *   built, so there is no command injection surface and no interaction with the Permissions
 *   plugin admin registration path.
 *
 *   Names are restricted to ASCII letters, digits, space, underscore, hyphen and period.
 *   Any other byte rejects the rename.
 *
 *   A one hour per player cooldown is applied on success, keyed on EOS ID, to bound the
 *   rate of player data saves. The cooldown is held in memory and resets on server restart.
 */

#pragma warning(disable: 4191)
#pragma comment(lib, "AsaApi")

#include <API/ARK/Ark.h>
#include <json.hpp>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <cctype>
#include <sys/stat.h>

static const std::string g_config_path = "ArkApi/Plugins/PlayerRename/config.json";
static const long long g_cooldown_seconds = 3600;
static int g_min_length = 3;
static int g_max_length = 24;
static std::vector<std::string> g_blocked_names;
static std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_last_rename;
static time_t g_config_last_modified = 0;
static long long g_config_last_size = 0;
static unsigned int g_timer_ticks = 0;

static std::string ToLower(const std::string& s)
{
    std::string out = s;
    for (auto& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

static std::string FStr(const FString& f)
{
    if (f.Len() == 0) return "";
    const char* s = TCHAR_TO_UTF8(*f);
    return s ? s : "";
}

static std::wstring WStr(const std::string& s)
{
    return std::wstring(s.begin(), s.end());
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

static bool LoadConfig()
{
    std::ifstream file(g_config_path);
    if (!file.is_open())
    {
        Log::GetLog()->error("[PlayerRename] Cannot open config: {}", g_config_path);
        return false;
    }

    int newMin = 3;
    int newMax = 24;
    std::vector<std::string> newBlocked;

    try
    {
        nlohmann::json j;
        file >> j;

        newMin = j.value("MinLength", 3);
        newMax = j.value("MaxLength", 24);

        if (j.contains("BlockedNames"))
        {
            if (!j["BlockedNames"].is_array())
            {
                Log::GetLog()->error("[PlayerRename] BlockedNames must be an array");
                return false;
            }

            for (const auto& entry : j["BlockedNames"])
            {
                if (!entry.is_string())
                {
                    Log::GetLog()->error("[PlayerRename] BlockedNames entries must be strings");
                    return false;
                }

                const std::string lowered = ToLower(entry.get<std::string>());
                if (lowered.empty()) continue;
                newBlocked.push_back(lowered);
            }
        }
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[PlayerRename] Config parse error: {}", ex.what());
        return false;
    }

    if (newMin < 1)
    {
        Log::GetLog()->error("[PlayerRename] MinLength must be at least 1");
        return false;
    }

    if (newMax < newMin)
    {
        Log::GetLog()->error("[PlayerRename] MaxLength must be greater than or equal to MinLength");
        return false;
    }

    g_min_length = newMin;
    g_max_length = newMax;
    g_blocked_names = std::move(newBlocked);
    g_config_last_modified = GetFileModTime(g_config_path);
    g_config_last_size = GetFileSize(g_config_path);

    Log::GetLog()->info("[PlayerRename] Config loaded: MinLength={} MaxLength={} BlockedNames={}",
        g_min_length, g_max_length, g_blocked_names.size());
    return true;
}

static void CheckConfigReload()
{
    const long long size = GetFileSize(g_config_path);
    if (size == 0) return;

    const time_t modified = GetFileModTime(g_config_path);
    if (modified == g_config_last_modified && size == g_config_last_size) return;

    Log::GetLog()->info("[PlayerRename] Config change detected, reloading");
    LoadConfig();
}

static void PruneCooldowns()
{
    if (g_last_rename.empty()) return;

    const auto now = std::chrono::steady_clock::now();

    for (auto it = g_last_rename.begin(); it != g_last_rename.end(); )
    {
        const long long elapsed =
            std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();

        if (elapsed >= g_cooldown_seconds)
            it = g_last_rename.erase(it);
        else
            ++it;
    }
}

static void OnTimerTick()
{
    if (++g_timer_ticks % 10 != 0) return;

    CheckConfigReload();
    PruneCooldowns();
}

static AShooterPlayerState* GetState(AShooterPlayerController* pc)
{
    if (!pc) return nullptr;
    return static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
}

static std::string GetEos(AShooterPlayerState* ps)
{
    if (!ps) return "";

    FString raw;
    ps->GetUniqueNetIdAsString(&raw);

    const std::string eos = FStr(raw);
    return (eos == "unknown") ? "" : eos;
}

static bool OnCooldown(const std::string& eosId, long long& outRemaining)
{
    const auto it = g_last_rename.find(eosId);
    if (it == g_last_rename.end()) return false;

    const long long elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - it->second).count();

    if (elapsed >= g_cooldown_seconds)
    {
        g_last_rename.erase(it);
        return false;
    }

    outRemaining = g_cooldown_seconds - elapsed;
    return true;
}

static bool PersistName(AShooterPlayerState* ps, const FString& newName)
{
    if (!ps) return false;

    UPrimalPlayerData* playerData = ps->MyPlayerDataField();
    if (!playerData) return false;

    FPrimalPlayerDataStruct* data = playerData->MyDataField();
    if (!data) return false;

    data->MyPlayerCharacterConfigField().PlayerCharacterNameField() = newName;

    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return false;

    playerData->SavePlayerData(world, true);
    return true;
}

static bool IsAllowedName(const std::string& name)
{
    for (const unsigned char c : name)
    {
        if (c >= 'a' && c <= 'z') continue;
        if (c >= 'A' && c <= 'Z') continue;
        if (c >= '0' && c <= '9') continue;
        if (c == ' ' || c == '_' || c == '-' || c == '.') continue;
        return false;
    }
    return true;
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

static void Notify(AShooterPlayerController* pc, const std::wstring& text)
{
    if (!pc) return;

    const FLinearColor color{ 1.0f, 0.65f, 0.0f, 1.0f };
    AsaApi::GetApiUtils().SendNotification(pc, color, 1.5f, 5.0f, nullptr, L"{}", text.c_str());
}

static void Cmd_Rename(AShooterPlayerController* pc, FString* message, int, int)
{
    if (!pc || !message) return;

    const std::string raw = FStr(*message);

    std::string newName;
    const size_t split = raw.find(' ');
    if (split != std::string::npos)
        newName = raw.substr(split + 1);

    while (!newName.empty() && newName.front() == ' ')
        newName.erase(newName.begin());
    while (!newName.empty() && newName.back() == ' ')
        newName.pop_back();

    if (newName.empty())
    {
        Notify(pc, L"Usage: /rename <new name>");
        return;
    }

    AShooterCharacter* character = pc->BaseGetPlayerCharacter();
    if (!character)
    {
        Notify(pc, L"You must be in game to rename your survivor.");
        return;
    }

    AShooterPlayerState* ps = GetState(pc);
    if (!ps)
    {
        Notify(pc, L"Could not resolve your player state. Please try again.");
        return;
    }

    const std::string eosId = GetEos(ps);
    if (eosId.empty())
    {
        Notify(pc, L"Could not resolve your player id. Please try again.");
        return;
    }

    long long remaining = 0;
    if (OnCooldown(eosId, remaining))
    {
        const long long minutes = (remaining + 59) / 60;
        Notify(pc, L"You can rename again in " + std::to_wstring(minutes) + L" minutes.");
        return;
    }

    if ((int)newName.size() < g_min_length)
    {
        Notify(pc, L"Name must be at least " + std::to_wstring(g_min_length) + L" characters.");
        return;
    }

    if ((int)newName.size() > g_max_length)
    {
        Notify(pc, L"Name cannot exceed " + std::to_wstring(g_max_length) + L" characters.");
        return;
    }

    if (!IsAllowedName(newName))
    {
        Notify(pc, L"Names may only contain letters, numbers, spaces, underscores, hyphens and periods.");
        return;
    }

    if (IsBlocked(newName))
    {
        Notify(pc, L"That name is not allowed.");
        return;
    }

    const std::string currentName = FStr(character->PlayerNameField());
    if (currentName == newName)
    {
        Notify(pc, L"That is already your name.");
        return;
    }

    FString newFName(newName);
    character->RenamePlayer(&newFName);

    if (!PersistName(ps, newFName))
    {
        Log::GetLog()->error("[PlayerRename] Rename applied but not persisted eos_id={} new_name={}",
            eosId, newName);
        Notify(pc, L"Your name was changed but could not be saved. Contact an admin before relogging.");
        return;
    }

    g_last_rename[eosId] = std::chrono::steady_clock::now();

    Log::GetLog()->info("[PlayerRename] Renamed eos_id={} old_name={} new_name={}",
        eosId, currentName, newName);

    Notify(pc, L"Your name has been changed to " + WStr(newName) + L".");
}

static void InitPlugin()
{
    Log::Get().Init("PlayerRename");

    if (!LoadConfig())
    {
        Log::GetLog()->error("[PlayerRename] Halted, config error");
        return;
    }

    AsaApi::GetCommands().AddChatCommand(FString(L"/rename"), &Cmd_Rename);
    AsaApi::GetCommands().AddOnTimerCallback(FString(L"PlayerRename_Timer"), &OnTimerTick);

    Log::GetLog()->info("[PlayerRename] Loaded");
}

static void UnloadPlugin()
{
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/rename"));
    AsaApi::GetCommands().RemoveOnTimerCallback(FString(L"PlayerRename_Timer"));

    g_blocked_names.clear();
    g_last_rename.clear();
    g_timer_ticks = 0;

    Log::GetLog()->info("[PlayerRename] Unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { InitPlugin(); }
    catch (const std::exception& ex)
    {
        Log::GetLog()->critical("[PlayerRename] Init exception: {}", ex.what());
    }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { UnloadPlugin(); }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[PlayerRename] Unload exception: {}", ex.what());
    }
}