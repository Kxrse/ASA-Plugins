/*
Census - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * Census - ASA Plugin
 *
 * Per map online roster posted to Discord as a single edit in place embed.
 * Each map instance owns and updates its own message, one embed per map.
 *
 * Hooks:
 *   None, timer driven roster snapshot read from the game state player array.
 *
 * Config:
 *   ArkApi/Plugins/Census/config.json
 *   WebhookUrl: Discord webhook URL, shared cluster wide
 *   UpdateIntervalSeconds: edit cadence in seconds, minimum 5
 *   DefaultColor: fallback embed color as a hex string
 *   MapNames: object mapping the runtime map name to the embed title
 *   MapColors: object mapping the runtime map name to a hex embed color
 *
 * Config Example:
 * {
 *     "WebhookUrl": "https://discord.com/api/webhooks/ID/TOKEN",
 *     "UpdateIntervalSeconds": 30,
 *     "DefaultColor": "FF7A00",
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
 *     "MapColors": {
 *         "TheIsland_WP": "3BA55D",
 *         "ScorchedEarth_WP": "E67E22",
 *         "TheCenter_WP": "1ABC9C",
 *         "Aberration_WP": "9B59B6",
 *         "Extinction_WP": "95A5A6",
 *         "Ragnarok_WP": "3498DB",
 *         "Astraeos_WP": "F1C40F",
 *         "Genesis_WP": "E91E63"
 *     }
 * }
 *
 * State:
 *   ArkApi/Plugins/Census/census_state_<MapName>.json
 *   MessageId: persisted Discord message id used for edit in place across restarts
 *
 * Roster:
 *   Read from AShooterGameState PlayerArray. EOS id, survivor name, implant id and
 *   tribe all come off AShooterPlayerState, so no actor enumeration and no character
 *   dereference happen, and players still loading in are listed with real values.
 *
 * Traffic:
 *   The roster signature is hashed every interval and the embed is only sent when it
 *   changes, so an idle map costs no Discord requests. Rate limits are honoured by
 *   reading retry_after and backing off. A message deleted in Discord is detected by
 *   error code 10008 and reposted on the next interval.
 */

#include <API/ARK/Ark.h>

#pragma warning(disable: 4191)
#pragma comment(lib, "AsaApi.lib")

#include <Requests.h>
#include <json.hpp>
#include <Windows.h>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <ctime>

// =============================================================================
// State
// =============================================================================

static std::string g_webhook_url;
static int         g_update_interval = 30;
static int         g_default_color   = 0xFF7A00;

static std::unordered_map<std::string, std::string> g_map_names;
static std::unordered_map<std::string, int>         g_map_colors;

static std::string g_config_path  = "ArkApi/Plugins/Census/config.json";
static uintmax_t   g_config_size  = 0;
static FILETIME    g_config_mtime = {};

static std::string g_runtime_map;
static std::string g_message_id;

static bool   g_request_in_flight = false;
static size_t g_last_hash         = 0;
static size_t g_pending_hash      = 0;
static int    g_backoff           = 0;
static int    g_post_counter      = 0;
static int    g_config_counter    = 0;

struct PlayerRow
{
    std::string eosId;
    std::string survivorName;
    uint64_t    implantId = 0;
    std::string tribeName;
    int         tribeId = 0;
    bool        inTribe = false;
};

// =============================================================================
// Helpers
// =============================================================================

static std::string FStr(const FString& f)
{
    if (f.IsEmpty()) return "";
    return std::string(TCHAR_TO_UTF8(*f));
}

static std::string Sanitize(const std::string& in)
{
    std::string out;
    out.reserve(in.size());

    for (unsigned char c : in)
    {
        if (c == '`') continue;
        if (c < 0x20 || c == 0x7F) continue;
        out += (char)c;
    }

    if (out.size() > 64)
    {
        out.resize(64);
        while (!out.empty() && ((unsigned char)out.back() & 0xC0) == 0x80)
            out.pop_back();
        if (!out.empty() && ((unsigned char)out.back() & 0xC0) == 0xC0)
            out.pop_back();
    }

    if (out.empty()) out = "unknown";
    return out;
}

static int ParseHexColor(const std::string& s, int fallback)
{
    std::string h = s;
    if (!h.empty() && h[0] == '#') h.erase(0, 1);
    if (h.empty()) return fallback;

    try { return (int)std::stoul(h, nullptr, 16); }
    catch (...) { return fallback; }
}

static std::string IsoUtcNow()
{
    std::time_t t = std::time(nullptr);
    std::tm g{};
    gmtime_s(&g, &t);

    char buf[32]{};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &g);
    return buf;
}

// =============================================================================
// Config
// =============================================================================

static bool LoadConfig()
{
    std::ifstream f(g_config_path);
    if (!f.is_open())
    {
        Log::GetLog()->error("[Census] config.json not found");
        return false;
    }

    try
    {
        nlohmann::json j;
        f >> j;

        g_webhook_url     = j.value("WebhookUrl", "");
        g_update_interval = j.value("UpdateIntervalSeconds", 30);
        g_default_color   = ParseHexColor(j.value("DefaultColor", "FF7A00"), 0xFF7A00);

        g_map_names.clear();
        if (j.contains("MapNames") && j["MapNames"].is_object())
        {
            for (auto& [key, val] : j["MapNames"].items())
            {
                if (val.is_string())
                    g_map_names[key] = val.get<std::string>();
            }
        }

        g_map_colors.clear();
        if (j.contains("MapColors") && j["MapColors"].is_object())
        {
            for (auto& [key, val] : j["MapColors"].items())
            {
                if (val.is_string())
                    g_map_colors[key] = ParseHexColor(val.get<std::string>(), g_default_color);
            }
        }
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[Census] Config parse error: {}", ex.what());
        return false;
    }

    if (g_update_interval < 5) g_update_interval = 5;

    if (g_webhook_url.empty())
    {
        Log::GetLog()->error("[Census] Config requires WebhookUrl");
        return false;
    }

    Log::GetLog()->info("[Census] Config loaded: interval={}s, {} map name(s)",
        g_update_interval, g_map_names.size());
    return true;
}

static void SnapshotConfigFile()
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExA(g_config_path.c_str(), GetFileExInfoStandard, &data))
    {
        g_config_size  = ((uintmax_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
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

    return size != g_config_size ||
        CompareFileTime(&data.ftLastWriteTime, &g_config_mtime) != 0;
}

// =============================================================================
// State File
// =============================================================================

static std::string StatePath()
{
    return "ArkApi/Plugins/Census/census_state_" + g_runtime_map + ".json";
}

static void LoadState()
{
    std::ifstream f(StatePath());
    if (!f.is_open()) return;

    try
    {
        nlohmann::json j;
        f >> j;
        g_message_id = j.value("MessageId", "");
    }
    catch (...)
    {
        g_message_id.clear();
    }
}

static void SaveState()
{
    std::ofstream f(StatePath());
    if (!f.is_open())
    {
        Log::GetLog()->error("[Census] Cannot write state file");
        return;
    }

    nlohmann::json j;
    j["MessageId"] = g_message_id;
    f << j.dump();
}

static bool ResolveRuntimeMap()
{
    if (!g_runtime_map.empty()) return true;

    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return false;

    FString map;
    world->GetMapName(&map);
    g_runtime_map = FStr(map);
    if (g_runtime_map.empty()) return false;

    LoadState();
    Log::GetLog()->info("[Census] Runtime map detected: {}", g_runtime_map);
    return true;
}

static std::string ResolveTitle()
{
    auto it = g_map_names.find(g_runtime_map);
    if (it != g_map_names.end()) return it->second;

    std::string m = g_runtime_map;
    const std::string suffix = "_WP";
    if (m.size() >= suffix.size() &&
        m.compare(m.size() - suffix.size(), suffix.size(), suffix) == 0)
        m.erase(m.size() - suffix.size());

    for (auto& c : m)
        if (c >= 'a' && c <= 'z') c -= 32;

    return m;
}

static int ResolveColor()
{
    auto it = g_map_colors.find(g_runtime_map);
    if (it != g_map_colors.end()) return it->second;
    return g_default_color;
}

// =============================================================================
// Roster
// =============================================================================

static void BuildRoster(std::vector<PlayerRow>& out)
{
    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return;

    AGameStateBase* base = world->GameStateField().Get();
    if (!base) return;

    AShooterGameState* gs = static_cast<AShooterGameState*>(base);
    auto& states = gs->PlayerArrayField();

    for (int i = 0; i < states.Num(); ++i)
    {
        AShooterPlayerState* ps = static_cast<AShooterPlayerState*>(states[i].Get());
        if (!ps) continue;

        FString eosRaw;
        ps->GetUniqueNetIdAsString(&eosRaw);
        std::string eos = FStr(eosRaw);
        if (eos.empty()) continue;

        PlayerRow row;
        row.eosId = Sanitize(eos);

        FString nameRaw;
        ps->GetPlayerName(&nameRaw);
        row.survivorName = Sanitize(FStr(nameRaw));

        row.implantId = ps->MyPlayerDataStructField().PlayerDataIDField();

        row.inTribe = ps->IsInTribe();
        if (row.inTribe)
        {
            FTribeData& tribe = ps->MyTribeDataField();
            row.tribeName = Sanitize(FStr(tribe.TribeNameField()));
            row.tribeId   = tribe.TribeIDField();
            if (row.tribeId == 0) row.tribeId = ps->GetTribeId();
        }

        out.push_back(std::move(row));
    }
}

static std::string BuildDescription(const std::vector<PlayerRow>& players)
{
    if (players.empty()) return "No players online.";

    std::string desc;
    for (const auto& p : players)
    {
        std::string block;
        block += "`" + p.eosId + "`\n";
        block += "- **Survivor Name**: `" + p.survivorName + "`\n";
        block += "- **Implant ID**: `" + std::to_string(p.implantId) + "`\n";

        if (p.inTribe)
        {
            block += "- **Tribe Name**: `" + p.tribeName + "`\n";
            block += "- **Tribe ID**: `" + std::to_string(p.tribeId) + "`\n";
        }
        else
        {
            block += "- **Tribe Name**: `none`\n";
            block += "- **Tribe ID**: `0`\n";
        }

        block += "\n";

        if (desc.size() + block.size() > 3900)
        {
            desc += "...more players online\n";
            break;
        }

        desc += block;
    }

    return desc;
}

static std::string BuildPayload(const std::string& title, int color,
    const std::string& desc, size_t count)
{
    nlohmann::json embed;
    embed["title"]           = title;
    embed["color"]           = color;
    embed["description"]     = desc;
    embed["timestamp"]       = IsoUtcNow();
    embed["footer"]["text"]  = "Total Online - " + std::to_string(count);

    nlohmann::json payload;
    payload["embeds"]           = nlohmann::json::array({ embed });
    payload["allowed_mentions"] = { {"parse", nlohmann::json::array()} };

    return payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// =============================================================================
// Discord
// =============================================================================

static void HandleFailure(const std::string& body, const char* tag)
{
    try
    {
        nlohmann::json j = nlohmann::json::parse(body);

        if (j.contains("retry_after"))
        {
            double retry = j.value("retry_after", 1.0);
            g_backoff = (int)retry + 1;
            Log::GetLog()->warn("[Census] Rate limited, backing off {}s", g_backoff);
            return;
        }

        if (j.value("code", 0) == 10008)
        {
            g_message_id.clear();
            g_last_hash = 0;
            SaveState();
            Log::GetLog()->info("[Census] Message missing, reposting next interval");
            return;
        }

        Log::GetLog()->error("[Census] {} failed: {}", tag, body);
    }
    catch (...)
    {
        Log::GetLog()->error("[Census] {} transport failure", tag);
    }
}

static void OnPatchDone(bool success, std::string result)
{
    g_request_in_flight = false;

    if (!success)
    {
        HandleFailure(result, "Patch");
        return;
    }

    g_last_hash = g_pending_hash;
}

static void OnPostDone(bool success, std::string result,
    std::unordered_map<std::string, std::string> /*headers*/)
{
    g_request_in_flight = false;

    if (!success)
    {
        HandleFailure(result, "Post");
        return;
    }

    try
    {
        nlohmann::json j = nlohmann::json::parse(result);
        g_message_id = j.value("id", "");

        if (g_message_id.empty())
        {
            Log::GetLog()->error("[Census] Post returned no message id");
            return;
        }

        g_last_hash = g_pending_hash;
        SaveState();
        Log::GetLog()->info("[Census] Posted embed id={} map={}", g_message_id, g_runtime_map);
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[Census] Post parse error: {}", ex.what());
    }
}

static void Publish(const std::string& body)
{
    std::vector<std::string> headers = {
        "User-Agent: Census/1.0",
        "Accept: */*"
    };

    g_request_in_flight = true;

    if (g_message_id.empty())
    {
        if (!API::Requests::Get().CreatePostRequest(
            g_webhook_url + "?wait=true", &OnPostDone, body, "application/json", headers))
        {
            g_request_in_flight = false;
            Log::GetLog()->error("[Census] Post dispatch failed");
        }
        return;
    }

    if (!API::Requests::Get().CreatePatchRequest(
        g_webhook_url + "/messages/" + g_message_id, &OnPatchDone, body, "application/json", headers))
    {
        g_request_in_flight = false;
        Log::GetLog()->error("[Census] Patch dispatch failed");
    }
}

// =============================================================================
// Timer
// =============================================================================

static void OnTimer()
{
    if (++g_config_counter >= 10)
    {
        g_config_counter = 0;
        if (ConfigFileChanged())
        {
            if (LoadConfig())
            {
                SnapshotConfigFile();
                g_last_hash = 0;
                Log::GetLog()->info("[Census] Config hot-reloaded");
            }
        }
    }

    if (!ResolveRuntimeMap()) return;
    if (g_request_in_flight) return;

    if (g_backoff > 0)
    {
        --g_backoff;
        return;
    }

    if (++g_post_counter < g_update_interval) return;
    g_post_counter = 0;

    std::vector<PlayerRow> players;
    BuildRoster(players);

    const std::string title = ResolveTitle();
    const int         color = ResolveColor();
    const std::string desc  = BuildDescription(players);

    const std::string signature = title + "|" + std::to_string(color) + "|" + desc;
    const size_t      hash      = std::hash<std::string>{}(signature);

    if (hash == g_last_hash && !g_message_id.empty()) return;

    g_pending_hash = hash;
    Publish(BuildPayload(title, color, desc, players.size()));
}

// =============================================================================
// Plugin Entry Points
// =============================================================================

static void Plugin_Init_Impl()
{
    Log::Get().Init("Census");

    if (!LoadConfig())
    {
        Log::GetLog()->error("[Census] Halted, config error");
        return;
    }

    SnapshotConfigFile();
    g_post_counter = g_update_interval - 1;

    AsaApi::GetCommands().AddOnTimerCallback(FString(L"Census_Timer"), &OnTimer);

    Log::GetLog()->info("[Census] Plugin loaded");
}

static void Plugin_Unload_Impl()
{
    AsaApi::GetCommands().RemoveOnTimerCallback(FString(L"Census_Timer"));

    Log::GetLog()->info("[Census] Plugin unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { Plugin_Init_Impl(); }
    catch (const std::exception& ex) { Log::GetLog()->critical("[Census] Init exception: {}", ex.what()); }
    catch (...) { Log::GetLog()->critical("[Census] Init unknown exception"); }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { Plugin_Unload_Impl(); }
    catch (...) {}
}