/*
Relay - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * Relay - ASA Plugin
 *
 * Hooks:
 *   AShooterPlayerController.ServerSendChatMessage_Implementation  relay player chat to Discord, slash commands never relayed
 *
 * Config:
 *   ArkApi/Plugins/Relay/config.json
 *   MapNames: object mapping the runtime map name to a display name used as the webhook username and cross map source tag
 *   Discord.Webhook: webhook URL for game to Discord relay
 *   Discord.BotToken: bot token for polling the channel
 *   Discord.ChannelID: channel polled for Discord to game relay
 *   Discord.PollSeconds: poll interval in seconds
 *   Discord.SenderFormat: sender template, {survivor} and {tribe} substituted
 *   Discord.BlockedPrefixes: substrings that suppress a message from being relayed out
 *
 * Config Example:
 * {
 *     "MapNames": {
 *         "TheIsland_WP": "The Island",
 *         "ScorchedEarth_WP": "Scorched Earth",
 *         "TheCenter_WP": "The Center",
 *         "Aberration_WP": "Aberration",
 *         "Extinction_WP": "Extinction",
 *         "Ragnarok_WP": "Ragnarok",
 *         "Astraeos_WP": "Astraeos",
 *         "Genesis_WP": "Genesis 1"
 *     },
 *     "Discord": {
 *         "Webhook": "https://discord.com/api/webhooks/ID/TOKEN",
 *         "BotToken": "BOT_TOKEN",
 *         "ChannelID": "CHANNEL_ID",
 *         "PollSeconds": 5,
 *         "SenderFormat": "{survivor} [{tribe}]",
 *         "BlockedPrefixes": ["http", "https"]
 *     }
 * }
 *
 * Game to Discord:
 *   Regular chat relayed to the Discord channel via webhook.
 *   Webhook username is this map's display name resolved from MapNames.
 *   Message content is the sender format result, then " - ", then the message.
 *   Slash commands are never relayed.
 *
 * Discord to Game:
 *   Polls the Discord channel via bot token. Three message types:
 *   1. Webhook whose username matches this map's display name, skipped for echo prevention
 *   2. Webhook from another map, relayed in game as [SourceMap] followed by the content
 *   3. Real Discord user, displayed as [Discord] DisplayName followed by the content
 *
 * Cluster:
 *   Each map runs its own instance and auto detects its runtime map name.
 *   The runtime name is resolved to a display name via MapNames, shared cluster wide.
 *   Maps not present in MapNames fall back to the raw runtime name.
 *   All maps share the same webhook, bot token, and channel.
 *   Echo filtering compares the webhook username against this map's display name.
 */

#include <API/ARK/Ark.h>

#pragma warning(disable: 4191)
#pragma comment(lib, "AsaApi.lib")

#include <Requests.h>
#include <json.hpp>
#include <Windows.h>
#include <fstream>
#include <string>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <cctype>

// =============================================================================
// Configuration
// =============================================================================

static std::string g_discord_webhook;
static std::string g_discord_bot_token;
static std::string g_discord_channel_id;
static int         g_discord_poll_seconds = 5;
static std::string g_sender_format = "{survivor} [{tribe}]";
static std::vector<std::string> g_blocked_prefixes;
static std::mutex               g_blocked_mutex;

static std::unordered_map<std::string, std::string> g_map_names;
static std::string                                  g_runtime_map;
static std::mutex                                   g_map_names_mutex;

static std::string  g_config_path = "ArkApi/Plugins/Relay/config.json";
static uintmax_t    g_config_size = 0;
static FILETIME     g_config_mtime = {};

static bool LoadConfig()
{
    std::ifstream f(g_config_path);
    if (!f.is_open())
    {
        Log::GetLog()->error("[Relay] config.json not found");
        return false;
    }

    try
    {
        nlohmann::json j;
        f >> j;

        {
            std::lock_guard<std::mutex> lock(g_map_names_mutex);
            g_map_names.clear();
            if (j.contains("MapNames") && j["MapNames"].is_object())
            {
                for (auto& [key, val] : j["MapNames"].items())
                {
                    if (val.is_string())
                        g_map_names[key] = val.get<std::string>();
                }
            }
        }

        if (j.contains("Discord"))
        {
            auto& d = j["Discord"];
            g_discord_webhook      = d.value("Webhook", "");
            g_discord_bot_token    = d.value("BotToken", "");
            g_discord_channel_id   = d.value("ChannelID", "");
            g_discord_poll_seconds = d.value("PollSeconds", 5);
            g_sender_format        = d.value("SenderFormat", "{survivor} [{tribe}]");

            {
                std::lock_guard<std::mutex> lock(g_blocked_mutex);
                g_blocked_prefixes.clear();
                if (d.contains("BlockedPrefixes") && d["BlockedPrefixes"].is_array())
                {
                    for (auto& v : d["BlockedPrefixes"])
                    {
                        if (v.is_string())
                        {
                            std::string s = v.get<std::string>();
                            for (char& c : s) c = (char)std::tolower((unsigned char)c);
                            if (!s.empty())
                                g_blocked_prefixes.push_back(s);
                        }
                    }
                }
                else
                {
                    g_blocked_prefixes.push_back("http");
                    g_blocked_prefixes.push_back("https");
                }
            }
        }

        Log::GetLog()->info("[Relay] Config loaded: {} map name(s), poll={}s",
            g_map_names.size(), g_discord_poll_seconds);
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[Relay] Config parse error: {}", ex.what());
        return false;
    }

    return true;
}

static void SnapshotConfigFile()
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExA(g_config_path.c_str(), GetFileExInfoStandard, &data))
    {
        g_config_size = ((uintmax_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
        g_config_mtime = data.ftLastWriteTime;
    }
}

static bool ConfigFileChanged()
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExA(g_config_path.c_str(), GetFileExInfoStandard, &data))
        return false;

    uintmax_t size = ((uintmax_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    if (size == 0) return false;

    if (size != g_config_size ||
        CompareFileTime(&data.ftLastWriteTime, &g_config_mtime) != 0)
    {
        return true;
    }

    return false;
}

// =============================================================================
// Helpers
// =============================================================================

static std::string FStr(const FString& f)
{
    if (f.IsEmpty()) return "";
    return std::string(TCHAR_TO_UTF8(*f));
}

static std::string ResolveMapDisplayName()
{
    std::lock_guard<std::mutex> lock(g_map_names_mutex);

    if (g_runtime_map.empty())
    {
        UWorld* world = AsaApi::GetApiUtils().GetWorld();
        if (world)
        {
            FString map;
            world->GetMapName(&map);
            g_runtime_map = FStr(map);
            if (!g_runtime_map.empty())
                Log::GetLog()->info("[Relay] Runtime map detected: {}", g_runtime_map);
        }
    }

    auto it = g_map_names.find(g_runtime_map);
    if (it != g_map_names.end())
        return it->second;

    return g_runtime_map.empty() ? "Unknown" : g_runtime_map;
}

static std::string GetSurvivorName(AShooterPlayerController* pc)
{
    if (!pc) return "";
    AShooterCharacter* sc = pc->BaseGetPlayerCharacter();
    if (!sc) return "";
    return FStr(sc->PlayerNameField());
}

static std::string GetTribeName(AShooterPlayerController* pc)
{
    if (!pc) return "";
    AShooterCharacter* sc = pc->BaseGetPlayerCharacter();
    if (!sc) return "";
    return FStr(sc->TribeNameField());
}

static void BroadcastChat(const std::string& sender, const std::string& message)
{
    std::wstring wSender(sender.begin(), sender.end());
    std::wstring wMsg(message.begin(), message.end());

    FString fSender(wSender.c_str());
    FString fMsg(wMsg.c_str());

    AsaApi::GetApiUtils().SendChatMessageToAll(fSender, L"{}", *fMsg);
}

// =============================================================================
// Game -> Discord (Webhook)
// =============================================================================

static void WebhookCallback(bool success, std::string result,
    std::unordered_map<std::string, std::string> /*headers*/)
{
    if (!success)
        Log::GetLog()->error("[Relay] Webhook POST failed: {}", result);
}

static std::string ApplySenderFormat(const std::string& survivorName, const std::string& tribeName)
{
    std::string out = g_sender_format;

    auto replaceAll = [](std::string& s, const std::string& from, const std::string& to)
    {
        if (from.empty()) return;
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos)
        {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    };

    replaceAll(out, "{survivor}", survivorName);
    replaceAll(out, "{tribe}", tribeName);
    replaceAll(out, "[]", "");

    std::string collapsed;
    collapsed.reserve(out.size());
    bool prevSpace = false;
    for (char c : out)
    {
        if (c == ' ')
        {
            if (prevSpace) continue;
            prevSpace = true;
        }
        else
        {
            prevSpace = false;
        }
        collapsed += c;
    }

    size_t start = collapsed.find_first_not_of(' ');
    if (start == std::string::npos) return "";
    size_t end = collapsed.find_last_not_of(' ');
    return collapsed.substr(start, end - start + 1);
}

static bool ContainsBlockedPrefix(const std::string& message)
{
    std::string lower = message;
    for (char& c : lower) c = (char)std::tolower((unsigned char)c);

    std::lock_guard<std::mutex> lock(g_blocked_mutex);
    for (const auto& p : g_blocked_prefixes)
    {
        if (!p.empty() && lower.find(p) != std::string::npos)
            return true;
    }
    return false;
}

static void SendToDiscord(const std::string& survivorName, const std::string& tribeName,
    const std::string& message)
{
    if (g_discord_webhook.empty()) return;

    std::string senderPart = ApplySenderFormat(survivorName, tribeName);
    std::string content = senderPart.empty() ? message : senderPart + " - " + message;

    nlohmann::json payload;
    payload["content"] = content;
    payload["username"] = ResolveMapDisplayName();
    payload["allowed_mentions"] = { {"parse", nlohmann::json::array()} };
    std::string body = payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);

    std::vector<std::string> headers = {
        "Content-Type: application/json",
        "User-Agent: Relay/1.0",
        "Accept: */*"
    };

    API::Requests::Get().CreatePostRequest(
        g_discord_webhook, &WebhookCallback, body, "application/json", headers);
}

// =============================================================================
// Discord -> Game (Poll)
// =============================================================================

static std::string g_last_message_id;
static bool g_primed = false;

static void PollCallback(bool success, std::string result)
{
    if (!success || result.empty()) return;

    try
    {
        nlohmann::json arr = nlohmann::json::parse(result);
        if (!arr.is_array() || arr.empty()) return;

        nlohmann::json& msg = arr[0];
        if (msg.is_null()) return;

        std::string msgId = msg.value("id", "");
        if (msgId.empty() || msgId == g_last_message_id) return;

        g_last_message_id = msgId;

        if (!g_primed)
        {
            g_primed = true;
            return;
        }

        std::string content = msg.value("content", "");
        if (content.empty()) return;

        if (content[0] == '/' || content[0] == '!') return;

        bool isWebhook = msg.contains("webhook_id");

        if (isWebhook)
        {
            std::string webhookUsername = msg["author"].value("username", "");

            if (webhookUsername == ResolveMapDisplayName())
                return;

            std::string inGameSender = "[" + webhookUsername + "]";
            BroadcastChat(inGameSender, content);

            Log::GetLog()->info("[Relay] CROSSMAP from={}", webhookUsername);
            return;
        }

        if (msg.contains("author") && msg["author"].value("bot", false))
            return;

        std::string authorName = msg["author"].value("global_name", "");
        if (authorName.empty())
            authorName = msg["author"].value("username", "Unknown");

        std::string sender = "[Discord] " + authorName;
        BroadcastChat(sender, content);

        Log::GetLog()->info("[Relay] DISCORD_IN name={}", authorName);
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[Relay] Poll parse error: {}", ex.what());
    }
}

static void PollDiscord()
{
    if (g_discord_bot_token.empty() || g_discord_channel_id.empty()) return;

    std::string url = "https://discord.com/api/v10/channels/" +
        g_discord_channel_id + "/messages?limit=1";

    std::vector<std::string> headers = {
        "Content-Type: application/json",
        "User-Agent: Relay/1.0",
        "Accept: */*",
        "Authorization: Bot " + g_discord_bot_token
    };

    API::Requests::Get().CreateGetRequest(url, &PollCallback, headers);
}

// =============================================================================
// Chat Hook
// =============================================================================

using ServerSendChatMessage_t = void(*)(AShooterPlayerController*, const FString*, EChatSendMode::Type, int);
static ServerSendChatMessage_t Original_ServerSendChatMessage = nullptr;

void Detour_ServerSendChatMessage(AShooterPlayerController* pc,
    const FString*      chatMessage,
    EChatSendMode::Type sendMode,
    int                 senderPlatform)
{
    const std::string msg = chatMessage ? FStr(*chatMessage) : "";

    if (!msg.empty() && msg[0] == '/')
    {
        Original_ServerSendChatMessage(pc, chatMessage, sendMode, senderPlatform);
        return;
    }

    try
    {
        if (!ContainsBlockedPrefix(msg))
        {
            std::string survivorName = GetSurvivorName(pc);
            std::string tribeName = GetTribeName(pc);
            SendToDiscord(survivorName, tribeName, msg);
        }
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[Relay] Relay exception: {}", ex.what());
    }
    catch (...)
    {
        Log::GetLog()->error("[Relay] Relay unknown exception");
    }

    Original_ServerSendChatMessage(pc, chatMessage, sendMode, senderPlatform);
}

// =============================================================================
// Timer
// =============================================================================

static int g_tick_counter = 0;

static void OnTimerTick()
{
    g_tick_counter++;

    if (g_discord_poll_seconds > 0 && (g_tick_counter % g_discord_poll_seconds == 0))
        PollDiscord();

    if (g_tick_counter % 10 == 0)
    {
        if (ConfigFileChanged())
        {
            LoadConfig();
            SnapshotConfigFile();
            Log::GetLog()->info("[Relay] Config hot-reloaded");
        }
    }
}

// =============================================================================
// Plugin Entry Points
// =============================================================================

static void Plugin_Init_Impl()
{
    Log::Get().Init("Relay");

    if (!LoadConfig())
    {
        Log::GetLog()->error("[Relay] Halted, config error");
        return;
    }

    SnapshotConfigFile();

    AsaApi::GetHooks().SetHook(
        "AShooterPlayerController.ServerSendChatMessage_Implementation(FString&,EChatSendMode::Type,int)",
        (LPVOID)&Detour_ServerSendChatMessage,
        (LPVOID*)&Original_ServerSendChatMessage
    );

    AsaApi::GetCommands().AddOnTimerCallback(FString(L"Relay_Timer"), &OnTimerTick);

    Log::GetLog()->info("[Relay] Plugin loaded");
}

static void Plugin_Unload_Impl()
{
    AsaApi::GetCommands().RemoveOnTimerCallback(FString(L"Relay_Timer"));

    AsaApi::GetHooks().DisableHook(
        "AShooterPlayerController.ServerSendChatMessage_Implementation(FString&,EChatSendMode::Type,int)",
        (LPVOID)&Detour_ServerSendChatMessage
    );

    Log::GetLog()->info("[Relay] Plugin unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { Plugin_Init_Impl(); }
    catch (const std::exception& ex) { Log::GetLog()->critical("[Relay] Init exception: {}", ex.what()); }
    catch (...) { Log::GetLog()->critical("[Relay] Init unknown exception"); }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { Plugin_Unload_Impl(); }
    catch (...) {}
}