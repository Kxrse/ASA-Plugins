/*
LinkInChat - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

#include <API/ARK/Ark.h>
#include <json.hpp>
#include <fstream>
#include <string>
#include <unordered_map>
#include <mutex>
#include <cctype>
#include <chrono>

// =============================================================================
// Configuration
// =============================================================================

static std::string g_message_color = "0.0,0.749,1.0,1.0";
static std::string g_sender = "Server";
static int         g_cooldown_seconds = 5;
static std::unordered_map<std::string, std::string> g_commands;
static std::mutex g_config_mutex;

static void LoadConfig()
{
    const std::string path = "ArkApi/Plugins/LinkInChat/config.json";
    std::ifstream f(path);
    if (!f.is_open())
    {
        Log::GetLog()->warn("[LinkInChat] config.json not found, using defaults");
        return;
    }

    try
    {
        nlohmann::json j;
        f >> j;

        g_message_color = j.value("message_color", "0.0,0.749,1.0,1.0");
        g_sender = j.value("sender", "Server");
        g_cooldown_seconds = j.value("cooldown_seconds", 5);

        std::lock_guard<std::mutex> lock(g_config_mutex);
        g_commands.clear();

        if (j.contains("commands") && j["commands"].is_object())
        {
            for (auto& [key, val] : j["commands"].items())
            {
                if (val.is_string())
                {
                    std::string lower = key;
                    for (char& c : lower) c = (char)std::tolower((unsigned char)c);
                    g_commands[lower] = val.get<std::string>();
                }
            }
        }

        Log::GetLog()->info("[LinkInChat] Config loaded — {} command(s), cooldown={}s",
            g_commands.size(), g_cooldown_seconds);
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[LinkInChat] Config parse error: {}", ex.what());
    }
}

// =============================================================================
// Cooldown Tracker
// =============================================================================

static std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_cooldowns;
static std::mutex g_cooldown_mutex;

static bool IsOnCooldown(const std::string& eosId)
{
    if (g_cooldown_seconds <= 0) return false;

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_cooldown_mutex);

    auto it = g_cooldowns.find(eosId);
    if (it != g_cooldowns.end())
    {
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();
        if (elapsed < g_cooldown_seconds)
            return true;
    }

    g_cooldowns[eosId] = now;
    return false;
}

// =============================================================================
// Helpers
// =============================================================================

static std::string FStr(const FString& f)
{
    const char* s = TCHAR_TO_UTF8(*f);
    return (s && s[0]) ? s : "";
}

// =============================================================================
// Hook
// =============================================================================

using ServerSendChatMessage_t = void(*)(AShooterPlayerController*, FString&, EChatSendMode::Type, int);
static ServerSendChatMessage_t Original_ServerSendChatMessage = nullptr;

void Detour_ServerSendChatMessage(AShooterPlayerController* pc,
    FString& chatMessage,
    EChatSendMode::Type sendMode,
    int                 senderPlatform)
{
    const std::string msg = FStr(chatMessage);

    if (!msg.empty() && msg[0] == '/')
    {
        std::string cmd = msg.substr(1);
        const auto space = cmd.find(' ');
        if (space != std::string::npos)
            cmd = cmd.substr(0, space);

        for (char& c : cmd) c = (char)std::tolower((unsigned char)c);

        std::string response;
        {
            std::lock_guard<std::mutex> lock(g_config_mutex);
            auto it = g_commands.find(cmd);
            if (it != g_commands.end())
                response = it->second;
        }

        if (!response.empty())
        {
            // Resolve EOS ID for cooldown tracking
            std::string eosId;
            if (AShooterPlayerState* ps = static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get()))
            {
                FString eosRaw;
                ps->GetUniqueNetIdAsString(&eosRaw);
                eosId = FStr(eosRaw);
            }

            if (!eosId.empty() && IsOnCooldown(eosId))
            {
                Log::GetLog()->info("[LinkInChat] /{} blocked — {} on cooldown", cmd, eosId);
                return; // Suppress message, say nothing
            }

            const std::wstring wColor(g_message_color.begin(), g_message_color.end());
            const std::wstring wSender(g_sender.begin(), g_sender.end());
            const std::wstring wResponse(response.begin(), response.end());

            const std::wstring wFull =
                L"<RichColor Color=\"" + wColor + L"\">" + wSender +
                L"</> <RichColor Color=\"1,1,1,1\">" + wResponse + L"</>";

            FString fSender(L"");
            FString fMsg(wFull.c_str());

            AsaApi::GetApiUtils().SendChatMessage(pc, fSender, L"{}", std::wstring_view(*fMsg));

            Log::GetLog()->info("[LinkInChat] /{} triggered by {} — response sent", cmd, eosId);
            return;
        }
    }

    Original_ServerSendChatMessage(pc, chatMessage, sendMode, senderPlatform);
}

// =============================================================================
// Plugin Entry Points
// =============================================================================

extern "C" __declspec(dllexport) void Plugin_Init()
{
    Log::Get().Init("LinkInChat");

    LoadConfig();

    AsaApi::GetHooks().SetHook(
        "AShooterPlayerController.ServerSendChatMessage_Implementation(FString&,EChatSendMode::Type,int)",
        (LPVOID)&Detour_ServerSendChatMessage,
        (LPVOID*)&Original_ServerSendChatMessage
    );

    Log::GetLog()->info("[LinkInChat] Plugin loaded");
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    AsaApi::GetHooks().DisableHook(
        "AShooterPlayerController.ServerSendChatMessage_Implementation(FString&,EChatSendMode::Type,int)",
        (LPVOID)&Detour_ServerSendChatMessage
    );

    {
        std::lock_guard<std::mutex> lock(g_cooldown_mutex);
        g_cooldowns.clear();
    }

    Log::GetLog()->info("[LinkInChat] Plugin unloaded");
}