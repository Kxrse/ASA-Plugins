/*
EasyEOS - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * EasyEOS - ASA Plugin
 *
 * Hooks:
 *   None. One chat command and a 10 second config hot reload timer only.
 *
 * Config:
 *   ArkApi/Plugins/EasyEOS/config.json
 *   WebhookUrl   optional Discord webhook. When empty or absent, no webhook is sent.
 *                Config file is optional. Missing config means webhook disabled, command still works.
 *
 * Chat commands:
 *   /eos   replies with the caller's raw EOS ID, on a 60 second per player cooldown.
 *          If WebhookUrl is set, also posts the caller EOS ID, survivor name, and
 *          survivor ID to the webhook.
 *
 * The survivor name is sanitised before it reaches the webhook (backticks and control
 * characters stripped) so a crafted name cannot break out of the Discord code block.
 */

#include <API/ARK/Ark.h>

#pragma comment(lib, "AsaApi")
#pragma warning(disable: 4191)

#include <Requests.h>
#include <json.hpp>
#include <Windows.h>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <chrono>

static std::string g_webhook_url;
static std::string g_config_path = "ArkApi/Plugins/EasyEOS/config.json";
static uintmax_t   g_config_size = 0;
static FILETIME    g_config_mtime = {};
static int         g_timer_ticks = 0;

static const int64_t g_cooldown_seconds = 60;
static std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_last_use;

static std::string FStr(const FString& f)
{
    if (f.Len() == 0) return "";
    return std::string(TCHAR_TO_UTF8(*f));
}

static std::wstring ToWide(const std::string& in)
{
    if (in.empty()) return std::wstring();
    const int len = MultiByteToWideChar(CP_UTF8, 0, in.c_str(), (int)in.size(), nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring out((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, in.c_str(), (int)in.size(), out.data(), len);
    return out;
}

static std::string SanitizeForCodeBlock(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in)
    {
        if (c == '`') continue;
        if (c < 0x20) continue;
        out += (char)c;
    }
    return out;
}

static void SnapshotConfigMeta()
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExA(g_config_path.c_str(), GetFileExInfoStandard, &data))
    {
        g_config_size = ((uintmax_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
        g_config_mtime = data.ftLastWriteTime;
    }
}

static bool ConfigChanged()
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExA(g_config_path.c_str(), GetFileExInfoStandard, &data))
        return false;

    const uintmax_t size = ((uintmax_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    if (size == 0) return false;

    return size != g_config_size ||
        CompareFileTime(&data.ftLastWriteTime, &g_config_mtime) != 0;
}

static bool LoadConfig()
{
    std::ifstream file(g_config_path);
    if (!file.is_open())
    {
        g_webhook_url.clear();
        Log::GetLog()->info("[EasyEOS] No config present, webhook disabled");
        return true;
    }

    std::string newWebhook;
    try
    {
        nlohmann::json j;
        file >> j;
        newWebhook = j.value("WebhookUrl", "");
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[EasyEOS] Config parse error: {}", ex.what());
        return false;
    }

    g_webhook_url = std::move(newWebhook);
    SnapshotConfigMeta();
    Log::GetLog()->info("[EasyEOS] Config loaded, webhook {}", g_webhook_url.empty() ? "disabled" : "enabled");
    return true;
}

static void CheckConfigReload()
{
    if (!ConfigChanged()) return;
    LoadConfig();
}

static void OnTimer()
{
    if (++g_timer_ticks < 10) return;
    g_timer_ticks = 0;
    CheckConfigReload();
}

static std::string GetEos(AShooterPlayerController* pc)
{
    if (!pc) return "";
    AShooterPlayerState* ps = static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
    if (!ps) return "";
    FString raw;
    ps->GetUniqueNetIdAsString(&raw);
    const std::string eos = FStr(raw);
    return (eos == "unknown") ? "" : eos;
}

static void Notify(AShooterPlayerController* pc, const std::wstring& msg)
{
    if (!pc) return;
    FString sender(L"EasyEOS");
    AsaApi::GetApiUtils().SendChatMessage(pc, sender, L"{}", std::wstring_view(msg));
}

static void OnWebhookDone(bool success, std::string result,
    std::unordered_map<std::string, std::string> /*headers*/)
{
    if (!success)
        Log::GetLog()->error("[EasyEOS] Webhook failed: {}", result);
}

static void PostWebhook(const std::string& eos, const std::string& survivorName, uint64_t survivorId)
{
    const std::string safeName = SanitizeForCodeBlock(survivorName);
    const std::string nameText = safeName.empty() ? "unknown" : safeName;
    const std::string idText = survivorId != 0 ? std::to_string(survivorId) : "unknown";

    std::string content;
    content += "**Player requested their EOS ID**\n\n";
    content += "EOS: `" + eos + "`\n";
    content += "Survivor Name: `" + nameText + "`\n";
    content += "Survivor ID: `" + idText + "`";

    nlohmann::json j;
    j["content"] = content;
    const std::string body = j.dump();

    std::vector<std::string> headers = {
        "User-Agent: EasyEOS/1.0",
        "Accept: */*"
    };

    if (!API::Requests::Get().CreatePostRequest(g_webhook_url, &OnWebhookDone, body, "application/json", headers))
        Log::GetLog()->error("[EasyEOS] Webhook dispatch failed");
}

static void Cmd_Eos(AShooterPlayerController* pc, FString* /*message*/, int, int)
{
    if (!pc) return;

    const std::string eos = GetEos(pc);
    if (eos.empty())
    {
        Notify(pc, L"Could not resolve your EOS ID.");
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    auto it = g_last_use.find(eos);
    if (it != g_last_use.end())
    {
        const int64_t elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();
        if (elapsed < g_cooldown_seconds)
            return;
    }
    g_last_use[eos] = now;

    Notify(pc, ToWide(eos));

    std::string survivorName;
    uint64_t survivorId = 0;

    AActor* actor = pc->BaseGetPlayerCharacter();
    if (actor)
    {
        AShooterCharacter* ch = static_cast<AShooterCharacter*>(actor);
        survivorName = FStr(ch->PlayerNameField());
        survivorId = ch->LinkedPlayerDataIDField();
    }

    Log::GetLog()->info("[EasyEOS] eos={} survivor='{}' survivorId={}", eos, survivorName, survivorId);

    if (!g_webhook_url.empty())
        PostWebhook(eos, survivorName, survivorId);
}

static void PluginInit()
{
    Log::Get().Init("EasyEOS");

    LoadConfig();

    AsaApi::GetCommands().AddChatCommand(FString(L"/eos"), &Cmd_Eos);
    AsaApi::GetCommands().AddOnTimerCallback(FString(L"EasyEOS_ConfigCheck"), &OnTimer);

    Log::GetLog()->info("[EasyEOS] Loaded");
}

static void PluginUnload()
{
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/eos"));
    AsaApi::GetCommands().RemoveOnTimerCallback(FString(L"EasyEOS_ConfigCheck"));

    g_webhook_url.clear();
    g_last_use.clear();

    Log::GetLog()->info("[EasyEOS] Unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { PluginInit(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->critical("[EasyEOS] Init exception: {}", e.what());
    }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { PluginUnload(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->error("[EasyEOS] Unload exception: {}", e.what());
    }
}