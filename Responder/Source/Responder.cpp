/*
Responder - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * Responder - ASA Plugin
 *
 * Maps chat commands to fixed text responses read from config, so links and canned
 * answers can be changed without a rebuild.
 *
 * Hooks:
 *   None, a chat message callback registered with AddOnChatMessageCallback under the
 *   id Responder. The callback key is the plugin id rather than a command name, so the
 *   plugin never adds or removes entries in the shared chat command registry and cannot
 *   disturb another plugin's commands.
 *
 * Config:
 *   ArkApi/Plugins/Responder/config.json
 *   SenderName: prefix shown before the response text
 *   MessageColor: RichColor value applied to the sender prefix
 *   CooldownSeconds: per player cooldown shared across every command, 0 disables it
 *   Commands: object mapping a command to its response text, the leading slash is optional
 *
 * Config Example:
 * {
 *     "SenderName": "Server",
 *     "MessageColor": "0.0,0.749,1.0,1.0",
 *     "CooldownSeconds": 5,
 *     "Commands": {
 *         "/discord": "Join the Discord at https://discord.gg/example",
 *         "/rules": "Server rules are pinned in the Discord rules channel",
 *         "/nextwipe": "Full wipes are announced two weeks ahead in the Discord"
 *     }
 * }
 *
 * Behavior:
 *   The API runs registered chat commands before this callback and reports the result as
 *   command_executed. When that is true the callback yields immediately, so any plugin
 *   that owns a name as a real command always wins and a duplicated name in this config
 *   only means Responder stays silent for it.
 *
 *   Every chat message reaches the callback, so the hot path scans the raw character
 *   buffer for a leading slash and returns before allocating anything when there is none.
 *   Leading spaces and tabs are skipped, and the command is the run of characters up to
 *   the next space or tab.
 *
 *   Config is reloaded every 10 seconds on a size plus last write time change. A file of
 *   size zero is treated as an in progress write and skipped. A config that fails to parse
 *   is not adopted, the last good config stays live, and the error is logged again on every
 *   retry rather than being suppressed. Reload only swaps the response map, it registers
 *   nothing and removes nothing.
 *
 *   The cooldown is per player and shared across every command, keyed on EOS id. A player
 *   inside the window is denied silently with nothing written to the log, and the message
 *   is still suppressed so the command text does not reach global chat. When a cooldown is
 *   configured and the EOS id cannot be resolved the command is denied rather than allowed
 *   through unlimited. Expired entries are pruned on the reload timer.
 */

#include <API/ARK/Ark.h>
#include <Timer.h>
#include <Tools.h>
#include <json.hpp>
#include <fstream>
#include <string>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <cctype>
#include <ctime>
#include <sys/stat.h>

#pragma comment(lib, "AsaApi.lib")
#pragma warning(disable: 4191)

static const std::string  g_config_path = "ArkApi/Plugins/Responder/config.json";
static const std::wstring g_callback_id = L"Responder";
static const std::wstring g_reload_timer_id = L"Responder_ConfigReload";

static std::mutex g_state_mutex;

static std::wstring g_sender_name = L"Server";
static std::wstring g_message_color = L"0.0,0.749,1.0,1.0";
static int          g_cooldown_seconds = 5;

static std::unordered_map<std::wstring, std::wstring> g_responses;
static std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_cooldowns;

static long long g_config_last_size = 0;
static time_t    g_config_last_modified = 0;

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

static std::wstring WideOf(const FString& value)
{
    if (value.Len() == 0) return std::wstring();
    return std::wstring(*value);
}

static std::string NarrowOf(const std::wstring& value)
{
    if (value.empty()) return std::string();
    return AsaApi::Tools::Utf8Encode(value);
}

static void LowerAscii(std::wstring& value)
{
    for (wchar_t& c : value)
    {
        if (c < 128) c = (wchar_t)std::tolower((int)c);
    }
}

static bool OnChatMessage(AShooterPlayerController* player_controller, FString* message, int, int, bool, bool command_executed)
{
    if (command_executed) return false;
    if (!player_controller || !message) return false;
    if (message->Len() == 0) return false;

    const TCHAR* raw = **message;
    if (!raw) return false;

    int start = 0;
    while (raw[start] == L' ' || raw[start] == L'\t') ++start;
    if (raw[start] != L'/') return false;

    int end = start;
    while (raw[end] != L'\0' && raw[end] != L' ' && raw[end] != L'\t') ++end;

    std::wstring command(raw + start, raw + end);
    LowerAscii(command);

    std::wstring response;
    std::wstring sender;
    std::wstring color;
    int cooldown = 0;

    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        const auto it = g_responses.find(command);
        if (it == g_responses.end()) return false;
        response = it->second;
        sender = g_sender_name;
        color = g_message_color;
        cooldown = g_cooldown_seconds;
    }

    const std::string eos_id = NarrowOf(WideOf(AsaApi::GetApiUtils().GetEOSIDFromController(player_controller)));

    if (cooldown > 0)
    {
        if (eos_id.empty())
        {
            Log::GetLog()->warn("[Responder] {} denied, EOS id unresolved", NarrowOf(command));
            return true;
        }

        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(g_state_mutex);

        const auto it = g_cooldowns.find(eos_id);
        if (it != g_cooldowns.end())
        {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();
            if (elapsed < cooldown)
                return true;
        }

        g_cooldowns[eos_id] = now;
    }

    const std::wstring full =
        L"<RichColor Color=\"" + color + L"\">" + sender +
        L"</> <RichColor Color=\"1,1,1,1\">" + response + L"</>";

    AsaApi::GetApiUtils().SendChatMessage(player_controller, FString(L""), L"{}", full);

    Log::GetLog()->info("[Responder] {} answered for {}", NarrowOf(command), eos_id);
    return true;
}

static bool LoadConfig()
{
    std::ifstream file(g_config_path);
    if (!file.is_open())
    {
        Log::GetLog()->error("[Responder] config.json not found at {}", g_config_path);
        return false;
    }

    nlohmann::json j;

    try
    {
        file >> j;
    }
    catch (const std::exception& e)
    {
        Log::GetLog()->error("[Responder] Config parse error, keeping last good config: {}", e.what());
        return false;
    }

    if (!j.is_object())
    {
        Log::GetLog()->error("[Responder] Config root is not an object, keeping last good config");
        return false;
    }

    const std::string sender_name = j.value("SenderName", std::string("Server"));
    const std::string message_color = j.value("MessageColor", std::string("0.0,0.749,1.0,1.0"));
    const int cooldown_seconds = j.value("CooldownSeconds", 5);

    if (cooldown_seconds < 0)
    {
        Log::GetLog()->error("[Responder] CooldownSeconds is negative, keeping last good config");
        return false;
    }

    std::unordered_map<std::wstring, std::wstring> responses;

    if (j.contains("Commands") && j["Commands"].is_object())
    {
        for (const auto& entry : j["Commands"].items())
        {
            if (!entry.value().is_string()) continue;

            std::wstring key = AsaApi::Tools::Utf8Decode(entry.key());
            if (key.empty()) continue;
            if (key[0] != L'/') key.insert(key.begin(), L'/');
            LowerAscii(key);

            if (key.find(L' ') != std::wstring::npos || key.find(L'\t') != std::wstring::npos)
            {
                Log::GetLog()->error("[Responder] Command {} contains whitespace, keeping last good config", NarrowOf(key));
                return false;
            }

            const std::wstring value = AsaApi::Tools::Utf8Decode(entry.value().get<std::string>());
            if (value.empty()) continue;

            if (responses.find(key) != responses.end())
            {
                Log::GetLog()->error("[Responder] Duplicate command {}, keeping last good config", NarrowOf(key));
                return false;
            }

            responses[key] = value;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_sender_name = AsaApi::Tools::Utf8Decode(sender_name);
        g_message_color = AsaApi::Tools::Utf8Decode(message_color);
        g_cooldown_seconds = cooldown_seconds;
        g_responses = responses;
    }

    g_config_last_size = GetFileSize(g_config_path);
    g_config_last_modified = GetFileModTime(g_config_path);

    Log::GetLog()->info("[Responder] Config loaded, {} command(s), cooldown {}s", responses.size(), cooldown_seconds);
    return true;
}

static void PruneCooldowns()
{
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_cooldown_seconds <= 0)
    {
        g_cooldowns.clear();
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    for (auto it = g_cooldowns.begin(); it != g_cooldowns.end(); )
    {
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();
        if (elapsed >= g_cooldown_seconds)
            it = g_cooldowns.erase(it);
        else
            ++it;
    }
}

static void CheckConfigReload()
{
    PruneCooldowns();

    const long long size = GetFileSize(g_config_path);
    if (size == 0) return;

    const time_t modified = GetFileModTime(g_config_path);
    if (size == g_config_last_size && modified == g_config_last_modified) return;

    LoadConfig();
}

static void PluginInit()
{
    Log::Get().Init("Responder");

    LoadConfig();

    AsaApi::GetCommands().AddOnChatMessageCallback(FString(g_callback_id.c_str()), &OnChatMessage);

    API::Timer::Get().RecurringExecute(FString(g_reload_timer_id.c_str()), &CheckConfigReload, 10, -1, false);

    Log::GetLog()->info("[Responder] Loaded");
}

static void PluginUnload()
{
    AsaApi::GetCommands().RemoveOnChatMessageCallback(FString(g_callback_id.c_str()));

    API::Timer::Get().UnloadAllTimers();

    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_responses.clear();
        g_cooldowns.clear();
    }

    Log::GetLog()->info("[Responder] Unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { PluginInit(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->critical("[Responder] Init exception: {}", e.what());
    }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { PluginUnload(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->error("[Responder] Unload exception: {}", e.what());
    }
}