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
 * Per-map online roster posted to Discord as a single edit-in-place embed.
 * Each map instance owns and updates its own message; one embed per map.
 *
 * Hooks:
 *   None - timer-driven actor enumeration.
 *
 * Config (ArkApi/Plugins/Census/config.json):
 *   WebhookUrl            - Discord webhook URL
 *   UpdateIntervalSeconds - edit cadence in seconds (minimum 5)
 *   DefaultColor          - fallback embed color, hex string
 *   MapNames              - raw map name to display name overrides
 *   MapColors             - raw map name to hex color overrides
 *
 * State (ArkApi/Plugins/Census/census_state_<MapName>.json):
 *   MessageId - persisted Discord message id for edit-in-place
 */

#include <API/ARK/Ark.h>
#include <json.hpp>
#include <Windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <fstream>
#include <chrono>
#include <ctime>
#include <cstdint>

#pragma warning(disable: 4191)
#pragma comment(lib, "AsaApi")

extern bool CensusHttpSend(const std::string& verb, const std::string& url,
    const std::string& body, std::string& outResp, int& outStatus);

static std::string  g_webhook_url;
static int          g_update_interval = 30;
static int          g_default_color = 0xFF7A00;

static std::unordered_map<std::string, std::string> g_map_names;
static std::unordered_map<std::string, int>         g_map_colors;

static std::string  g_config_path = "ArkApi/Plugins/Census/config.json";
static time_t       g_config_last_modified = 0;
static uintmax_t    g_config_last_size = 0;

static std::string  g_message_id;
static bool         g_state_loaded = false;

struct PlayerRow
{
    std::string eosId;
    std::string survivorName;
    uint64_t    survivorId = 0;
    std::string tribeName;
    int         tribeId = 0;
    bool        inTribe = false;
};

struct Snapshot
{
    std::string             mapName;
    std::string             title;
    std::string             webhookUrl;
    int                     color = 0xFF7A00;
    std::vector<PlayerRow>  players;
};

static std::mutex        g_snap_mutex;
static Snapshot          g_pending;
static std::atomic<bool> g_has_pending{ false };

static std::thread       g_worker;
static std::atomic<bool> g_worker_running{ false };

static int g_post_counter = 0;
static int g_config_counter = 0;

static std::string FStr(const FString& f)
{
    const char* s = TCHAR_TO_UTF8(*f);
    return (s && s[0]) ? s : "unknown";
}

static int ParseHexColor(const std::string& s, int fallback)
{
    std::string h = s;
    if (!h.empty() && h[0] == '#') h.erase(0, 1);
    if (h.empty()) return fallback;
    try { return (int)std::stoul(h, nullptr, 16); }
    catch (...) { return fallback; }
}

static std::string GetMapName()
{
    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return "unknown";
    FString map;
    world->GetMapName(&map);
    return FStr(map);
}

static bool GetFileInfo(const std::string& path, time_t& mtime, uintmax_t& size)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) return false;
    ULARGE_INTEGER s;
    s.HighPart = data.nFileSizeHigh;
    s.LowPart = data.nFileSizeLow;
    size = s.QuadPart;
    ULARGE_INTEGER t;
    t.HighPart = data.ftLastWriteTime.dwHighDateTime;
    t.LowPart = data.ftLastWriteTime.dwLowDateTime;
    mtime = (time_t)(t.QuadPart / 10000000ULL - 11644473600ULL);
    return true;
}

static bool LoadConfig()
{
    std::ifstream file(g_config_path);
    if (!file.is_open())
    {
        Log::GetLog()->error("[Census] Cannot open config: {}", g_config_path);
        return false;
    }

    try
    {
        nlohmann::json j;
        file >> j;
        g_webhook_url = j.value("WebhookUrl", "");
        g_update_interval = j.value("UpdateIntervalSeconds", 30);
        g_default_color = ParseHexColor(j.value("DefaultColor", "FF7A00"), 0xFF7A00);

        g_map_names.clear();
        if (j.contains("MapNames") && j["MapNames"].is_object())
            for (auto& kv : j["MapNames"].items())
                g_map_names[kv.key()] = kv.value().get<std::string>();

        g_map_colors.clear();
        if (j.contains("MapColors") && j["MapColors"].is_object())
            for (auto& kv : j["MapColors"].items())
                g_map_colors[kv.key()] = ParseHexColor(kv.value().get<std::string>(), g_default_color);
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

    return true;
}

static void CheckConfigReload()
{
    time_t mtime = 0;
    uintmax_t fsize = 0;
    if (GetFileInfo(g_config_path, mtime, fsize) && fsize > 0 &&
        (mtime != g_config_last_modified || fsize != g_config_last_size))
    {
        if (LoadConfig())
        {
            g_config_last_modified = mtime;
            g_config_last_size = fsize;
            Log::GetLog()->info("[Census] Config reloaded");
        }
    }
}

static std::string StatePath(const std::string& mapName)
{
    return "ArkApi/Plugins/Census/census_state_" + mapName + ".json";
}

static void LoadState(const std::string& mapName)
{
    std::ifstream file(StatePath(mapName));
    if (!file.is_open()) { g_state_loaded = true; return; }
    try
    {
        nlohmann::json j;
        file >> j;
        g_message_id = j.value("MessageId", "");
    }
    catch (...) { g_message_id.clear(); }
    g_state_loaded = true;
}

static void SaveState(const std::string& mapName)
{
    std::ofstream file(StatePath(mapName));
    if (!file.is_open()) return;
    nlohmann::json j;
    j["MessageId"] = g_message_id;
    file << j.dump();
}

static void BuildRoster(std::vector<PlayerRow>& out)
{
    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return;
    ULevel* level = world->PersistentLevelField();
    if (!level) return;

    auto actors = level->ActorsField();
    for (int i = 0; i < actors.Num(); ++i)
    {
        AActor* actor = actors[i];
        if (!actor) continue;
        if (!actor->IsA(AShooterPlayerController::StaticClass())) continue;

        AShooterPlayerController* pc = static_cast<AShooterPlayerController*>(actor);

        AShooterPlayerState* ps =
            static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
        if (!ps) continue;

        FString eosRaw;
        ps->GetUniqueNetIdAsString(&eosRaw);
        std::string eos = FStr(eosRaw);
        if (eos.empty() || eos == "unknown") continue;

        PlayerRow row;
        row.eosId = eos;

        FString nameRaw;
        ps->GetPlayerName(&nameRaw);
        row.survivorName = FStr(nameRaw);

        AShooterCharacter* ch =
            static_cast<AShooterCharacter*>(pc->BaseGetPlayerCharacter());
        if (ch)
            row.survivorId = ch->GetLinkedPlayerDataID();

        row.inTribe = ps->IsInTribe();
        row.tribeId = ps->GetTribeId();
        if (row.inTribe && ch)
            row.tribeName = FStr(ch->TribeNameField());

        out.push_back(std::move(row));
    }
}

static std::string FormatMapTitle(const std::string& raw)
{
    auto it = g_map_names.find(raw);
    if (it != g_map_names.end()) return it->second;

    std::string m = raw;
    const std::string suffix = "_WP";
    if (m.size() >= suffix.size() &&
        m.compare(m.size() - suffix.size(), suffix.size(), suffix) == 0)
        m.erase(m.size() - suffix.size());
    for (auto& c : m)
        if (c >= 'a' && c <= 'z') c -= 32;
    return m;
}

static int ResolveColor(const std::string& raw)
{
    auto it = g_map_colors.find(raw);
    if (it != g_map_colors.end()) return it->second;
    return g_default_color;
}

static std::string BuildEmbedJson(const Snapshot& snap)
{
    std::string desc;
    if (snap.players.empty())
    {
        desc = "No players online.";
    }
    else
    {
        for (const auto& p : snap.players)
        {
            std::string block;
            block += "`" + p.eosId + "`\n";
            block += "- **Survivor Name**: `" + p.survivorName + "`\n";
            block += "- **Implant ID**: `" + std::to_string(p.survivorId) + "`\n";
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
    }

    nlohmann::json embed;
    embed["title"] = snap.title;
    embed["color"] = snap.color;
    embed["description"] = desc;
    embed["footer"]["text"] = "Total Online - " + std::to_string(snap.players.size());

    nlohmann::json payload;
    payload["embeds"] = nlohmann::json::array({ embed });
    return payload.dump();
}

static void PublishSnapshot(const Snapshot& snap)
{
    if (snap.webhookUrl.empty()) return;

    if (!g_state_loaded) LoadState(snap.mapName);

    const std::string body = BuildEmbedJson(snap);

    if (g_message_id.empty())
    {
        std::string url = snap.webhookUrl + "?wait=true";
        std::string resp;
        int status = 0;
        if (CensusHttpSend("POST", url, body, resp, status) &&
            (status == 200 || status == 201))
        {
            try
            {
                nlohmann::json j = nlohmann::json::parse(resp);
                g_message_id = j.value("id", "");
                if (!g_message_id.empty())
                {
                    SaveState(snap.mapName);
                    Log::GetLog()->info("[Census] Posted embed id={} map={}",
                        g_message_id, snap.mapName);
                }
            }
            catch (const std::exception& ex)
            {
                Log::GetLog()->error("[Census] Post parse error: {}", ex.what());
            }
        }
        else
        {
            Log::GetLog()->error("[Census] Post failed status={}", status);
        }
        return;
    }

    std::string url = snap.webhookUrl + "/messages/" + g_message_id;
    std::string resp;
    int status = 0;
    if (CensusHttpSend("PATCH", url, body, resp, status))
    {
        if (status == 404)
        {
            g_message_id.clear();
            SaveState(snap.mapName);
            Log::GetLog()->info("[Census] Message missing - will repost map={}",
                snap.mapName);
        }
        else if (status != 200)
        {
            Log::GetLog()->error("[Census] Patch failed status={}", status);
        }
    }
    else
    {
        Log::GetLog()->error("[Census] Patch transport failure");
    }
}

static void WorkerFunc()
{
    while (g_worker_running.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (!g_has_pending.exchange(false)) continue;

        Snapshot snap;
        {
            std::lock_guard<std::mutex> lock(g_snap_mutex);
            snap = g_pending;
        }
        PublishSnapshot(snap);
    }
}

static void OnTimer()
{
    if (++g_config_counter >= 10)
    {
        g_config_counter = 0;
        CheckConfigReload();
    }

    if (++g_post_counter < g_update_interval) return;
    g_post_counter = 0;

    Snapshot snap;
    snap.mapName = GetMapName();
    snap.title = FormatMapTitle(snap.mapName);
    snap.color = ResolveColor(snap.mapName);
    snap.webhookUrl = g_webhook_url;
    BuildRoster(snap.players);

    {
        std::lock_guard<std::mutex> lock(g_snap_mutex);
        g_pending = std::move(snap);
    }
    g_has_pending.store(true);
}

static void Plugin_Init_Impl()
{
    Log::Get().Init("Census");

    if (!LoadConfig())
    {
        Log::GetLog()->error("[Census] Halted - config error");
        return;
    }

    GetFileInfo(g_config_path, g_config_last_modified, g_config_last_size);

    g_worker_running.store(true);
    g_worker = std::thread(WorkerFunc);

    AsaApi::GetCommands().AddOnTimerCallback(FString(L"Census_Timer"), &OnTimer);

    Log::GetLog()->info("[Census] Plugin loaded");
}

static void Plugin_Unload_Impl()
{
    AsaApi::GetCommands().RemoveOnTimerCallback(FString(L"Census_Timer"));

    g_worker_running.store(false);
    if (g_worker.joinable())
        g_worker.join();

    Log::GetLog()->info("[Census] Plugin unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { Plugin_Init_Impl(); }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[Census] Init exception: {}", ex.what());
    }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { Plugin_Unload_Impl(); }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[Census] Unload exception: {}", ex.what());
    }
}