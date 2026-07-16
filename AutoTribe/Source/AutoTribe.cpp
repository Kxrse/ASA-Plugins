/*
AutoTribe - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * AutoTribe - ASA Plugin
 *
 * Hooks:
 *   AShooterPlayerController.HandleRespawned_Implementation(APawn*,bool)  queues a tribeless player on spawn, respawn and transfer arrival
 *   AShooterPlayerState.NotifyPlayerLeftTribe(FString&,FString&,bool)     queues a player when they leave or are kicked from a tribe
 *   AShooterGameMode.Tick(float)                                          config reload (10s), enforcement queue drain (idle while queue is empty)
 *
 * Config:
 *   ArkApi/Plugins/AutoTribe/config.json
 *   MessageColor: notification colour, "r,g,b,a" floats
 *   JoinMessage: notification shown once a player has been placed into a tribe, blank to suppress
 *   RetryIntervalMs: gap between tribe creation attempts for one player, floored at 100
 *   LeaveSettleMs: grace before the first attempt on the leave path, letting the engine finish tearing the old tribe down
 *   MaxAttempts: attempts before AutoTribe gives up on a player until their next respawn
 *   MaxNameLength: character ceiling for a generated tribe name, floored at 8
 *
 * Config Example:
 * {
 *   "MessageColor": "0.902,0.365,0.137,1",
 *   "JoinMessage": "You have been automatically placed into a tribe.",
 *   "RetryIntervalMs": 1000,
 *   "LeaveSettleMs": 1000,
 *   "MaxAttempts": 30,
 *   "MaxNameLength": 24
 * }
 *
 * Forces any tribeless player into a solo tribe with a generated name, and does
 * nothing else. No database, no name persistence, no cross map state, no
 * governance handling. A player may rename or leave freely afterwards, and
 * leaving simply re-queues them.
 *
 * The leave path needs a grace period because tribe teardown is deferred. The
 * PlayerState reports tribe 0 while the game mode still holds the player, and a
 * creation request made in that window is rejected cleanly, leaving no orphan
 * tribe. The window is not a fixed length, so LeaveSettleMs only covers the
 * common case and the retry loop absorbs the rest rather than paying a large
 * fixed delay on every leave.
 *
 * Word lists live in ArkApi/Plugins/AutoTribe/words.json as three string arrays,
 * Openers, Adjectives and Nouns, feeding the [Opener] [Adjective] [Adjective] [Noun]
 * format. Both adjectives are drawn distinct. The file is hot-reloaded on the same
 * 10s size and modified-time check as config.json, and is kept out of config.json
 * so the Nexus reflector never tries to scaffold a word list as a settings form.
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
#include <chrono>
#include <mutex>
#include <atomic>
#include <random>
#include <ctime>
#include <cstdio>
#include <sys/stat.h>

static const std::string g_config_path = "ArkApi/Plugins/AutoTribe/config.json";
static const std::string g_words_path = "ArkApi/Plugins/AutoTribe/words.json";

static std::mutex g_config_mutex;
static FLinearColor g_message_color{ 0.902f, 0.365f, 0.137f, 1.0f };
static std::wstring g_join_message = L"You have been automatically placed into a tribe.";

static std::atomic<long long> g_retry_interval_ms{ 1000 };
static std::atomic<long long> g_leave_settle_ms{ 1000 };
static std::atomic<int> g_max_attempts{ 30 };
static std::atomic<size_t> g_max_name_length{ 24 };

static long long g_config_last_size = 0;
static time_t g_config_last_modified = 0;
static long long g_words_last_size = 0;
static time_t g_words_last_modified = 0;

static std::chrono::steady_clock::time_point g_last_config_check;

struct WordList
{
    std::vector<std::string> words;
    std::vector<size_t> lengths;
};

static std::mutex g_words_mutex;
static WordList g_openers;
static WordList g_adjectives;
static WordList g_nouns;
static std::string g_fallback_name;
static bool g_words_ready = false;

struct PendingEntry
{
    long long dueMs = 0;
    int attempts = 0;
};

static std::mutex g_queue_mutex;
static std::unordered_map<std::string, PendingEntry> g_queue;
static std::atomic<size_t> g_queue_size{ 0 };

using HandleRespawned_t = void(*)(AShooterPlayerController*, APawn*, bool);
using NotifyPlayerLeftTribe_t = void(*)(AShooterPlayerState*, FString&, FString&, bool);
using Tick_t = void(*)(AShooterGameMode*, float);

static HandleRespawned_t Original_HandleRespawned = nullptr;
static NotifyPlayerLeftTribe_t Original_NotifyPlayerLeftTribe = nullptr;
static Tick_t Original_Tick = nullptr;

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

static std::wstring Widen(const std::string& s)
{
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring out((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), len);
    return out;
}

static size_t CharLength(const std::string& s)
{
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

static std::string Trim(const std::string& s)
{
    size_t start = 0;
    size_t end = s.size();
    while (start < end && (unsigned char)s[start] <= 0x20) ++start;
    while (end > start && (unsigned char)s[end - 1] <= 0x20) --end;
    return s.substr(start, end - start);
}

static FLinearColor ParseColor(const std::string& s)
{
    float r = 0.902f, g = 0.365f, b = 0.137f, a = 1.0f;
    std::sscanf(s.c_str(), "%f,%f,%f,%f", &r, &g, &b, &a);
    return FLinearColor(r, g, b, a);
}

static bool LoadConfig()
{
    std::ifstream file(g_config_path);
    if (!file.is_open())
    {
        Log::GetLog()->error("[AutoTribe] Cannot open config: {}", g_config_path);
        return false;
    }

    try
    {
        nlohmann::json j;
        file >> j;

        const std::string color = j.value("MessageColor", std::string("0.902,0.365,0.137,1"));
        const std::wstring msg = Widen(j.value("JoinMessage", std::string("You have been automatically placed into a tribe.")));

        long long retry = j.value("RetryIntervalMs", 1000LL);
        long long settle = j.value("LeaveSettleMs", 1000LL);
        int attempts = j.value("MaxAttempts", 30);
        int maxLen = j.value("MaxNameLength", 24);

        if (retry < 100) retry = 100;
        if (settle < 0) settle = 0;
        if (attempts < 1) attempts = 1;
        if (maxLen < 8) maxLen = 8;

        {
            std::lock_guard<std::mutex> lock(g_config_mutex);
            g_message_color = ParseColor(color);
            g_join_message = msg;
        }

        g_retry_interval_ms.store(retry, std::memory_order_relaxed);
        g_leave_settle_ms.store(settle, std::memory_order_relaxed);
        g_max_attempts.store(attempts, std::memory_order_relaxed);
        g_max_name_length.store((size_t)maxLen, std::memory_order_relaxed);
    }
    catch (const std::exception& e)
    {
        Log::GetLog()->error("[AutoTribe] Config parse error: {}", e.what());
        return false;
    }

    g_config_last_size = GetFileSize(g_config_path);
    g_config_last_modified = GetFileModTime(g_config_path);
    Log::GetLog()->info("[AutoTribe] Config loaded");
    return true;
}

static bool ReadWordArray(const nlohmann::json& j, const char* key, WordList& out)
{
    if (!j.contains(key) || !j[key].is_array())
    {
        Log::GetLog()->error("[AutoTribe] words.json missing array: {}", key);
        return false;
    }

    for (const auto& v : j[key])
    {
        if (!v.is_string()) continue;
        const std::string w = Trim(v.get<std::string>());
        if (w.empty()) continue;
        out.words.push_back(w);
        out.lengths.push_back(CharLength(w));
    }
    return true;
}

static size_t ShortestIndex(const WordList& list, size_t skip)
{
    size_t best = 0;
    size_t bestLen = SIZE_MAX;
    for (size_t i = 0; i < list.words.size(); ++i)
    {
        if (i == skip) continue;
        if (list.lengths[i] < bestLen)
        {
            bestLen = list.lengths[i];
            best = i;
        }
    }
    return best;
}

static bool LoadWords()
{
    std::ifstream file(g_words_path);
    if (!file.is_open())
    {
        Log::GetLog()->error("[AutoTribe] Cannot open words: {}", g_words_path);
        return false;
    }

    WordList openers;
    WordList adjectives;
    WordList nouns;

    try
    {
        nlohmann::json j;
        file >> j;

        if (!ReadWordArray(j, "Openers", openers)) return false;
        if (!ReadWordArray(j, "Adjectives", adjectives)) return false;
        if (!ReadWordArray(j, "Nouns", nouns)) return false;
    }
    catch (const std::exception& e)
    {
        Log::GetLog()->error("[AutoTribe] words.json parse error: {}", e.what());
        return false;
    }

    if (openers.words.empty() || adjectives.words.size() < 2 || nouns.words.empty())
    {
        Log::GetLog()->error("[AutoTribe] words.json needs at least 1 opener, 2 adjectives and 1 noun");
        return false;
    }

    const size_t fo = ShortestIndex(openers, SIZE_MAX);
    const size_t fa1 = ShortestIndex(adjectives, SIZE_MAX);
    const size_t fa2 = ShortestIndex(adjectives, fa1);
    const size_t fn = ShortestIndex(nouns, SIZE_MAX);

    std::string fallback;
    fallback.append(openers.words[fo]).append(" ")
        .append(adjectives.words[fa1]).append(" ")
        .append(adjectives.words[fa2]).append(" ")
        .append(nouns.words[fn]);

    const size_t openerCount = openers.words.size();
    const size_t adjectiveCount = adjectives.words.size();
    const size_t nounCount = nouns.words.size();

    {
        std::lock_guard<std::mutex> lock(g_words_mutex);
        g_openers = std::move(openers);
        g_adjectives = std::move(adjectives);
        g_nouns = std::move(nouns);
        g_fallback_name = fallback;
        g_words_ready = true;
    }

    g_words_last_size = GetFileSize(g_words_path);
    g_words_last_modified = GetFileModTime(g_words_path);
    Log::GetLog()->info("[AutoTribe] Words loaded: {} openers, {} adjectives, {} nouns",
        openerCount, adjectiveCount, nounCount);
    return true;
}

static void CheckConfigReload()
{
    const long long csz = GetFileSize(g_config_path);
    if (csz > 0)
    {
        const time_t cmt = GetFileModTime(g_config_path);
        if (cmt != g_config_last_modified || csz != g_config_last_size)
            LoadConfig();
    }

    const long long wsz = GetFileSize(g_words_path);
    if (wsz > 0)
    {
        const time_t wmt = GetFileModTime(g_words_path);
        if (wmt != g_words_last_modified || wsz != g_words_last_size)
            LoadWords();
    }
}

static std::string GenerateTribeName()
{
    const size_t maxLen = g_max_name_length.load(std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(g_words_mutex);
    if (!g_words_ready) return std::string();

    static thread_local std::mt19937_64 rng{ (uint64_t)GetTickCount64() ^ (uint64_t)(uintptr_t)&rng };

    std::uniform_int_distribution<size_t> distO(0, g_openers.words.size() - 1);
    std::uniform_int_distribution<size_t> distA(0, g_adjectives.words.size() - 1);
    std::uniform_int_distribution<size_t> distN(0, g_nouns.words.size() - 1);

    for (int i = 0; i < 100; ++i)
    {
        const size_t o = distO(rng);
        const size_t a1 = distA(rng);
        const size_t a2 = distA(rng);
        if (a1 == a2) continue;

        const size_t n = distN(rng);
        const size_t total = g_openers.lengths[o] + g_adjectives.lengths[a1]
            + g_adjectives.lengths[a2] + g_nouns.lengths[n] + 3;
        if (total > maxLen) continue;

        std::string name;
        name.reserve(total);
        name.append(g_openers.words[o]).append(" ")
            .append(g_adjectives.words[a1]).append(" ")
            .append(g_adjectives.words[a2]).append(" ")
            .append(g_nouns.words[n]);
        return name;
    }

    Log::GetLog()->warn("[AutoTribe] Name picker exhausted 100 draws under MaxNameLength={}, using fallback", maxLen);
    return g_fallback_name;
}

static std::string GetEos(AShooterPlayerState* ps)
{
    if (!ps) return std::string();
    FString eos;
    ps->GetUniqueNetIdAsString(&eos);
    const char* s = TCHAR_TO_UTF8(*eos);
    return (s && s[0]) ? std::string(s) : std::string();
}

static AShooterPlayerController* FindByEos(const std::string& eos)
{
    if (eos.empty()) return nullptr;
    FString f(eos.c_str());
    return AsaApi::GetApiUtils().FindPlayerFromEOSID(f);
}

static void Notify(AShooterPlayerController* pc)
{
    if (!pc) return;

    FLinearColor color;
    std::wstring msg;
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        color = g_message_color;
        msg = g_join_message;
    }

    if (msg.empty()) return;
    AsaApi::GetApiUtils().SendNotification(pc, color, 1.3f, 7.0f, nullptr, L"{}", msg.c_str());
}

static void Enqueue(const std::string& eos, long long delayMs)
{
    if (eos.empty()) return;

    std::lock_guard<std::mutex> lock(g_queue_mutex);
    PendingEntry& e = g_queue[eos];
    e.dueMs = (long long)GetTickCount64() + delayMs;
    e.attempts = 0;
    g_queue_size.store(g_queue.size(), std::memory_order_relaxed);
}

static void ProcessQueue()
{
    if (g_queue_size.load(std::memory_order_relaxed) == 0) return;

    const long long now = (long long)GetTickCount64();
    const long long retry = g_retry_interval_ms.load(std::memory_order_relaxed);
    const int maxAttempts = g_max_attempts.load(std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(g_queue_mutex);

    for (auto it = g_queue.begin(); it != g_queue.end(); )
    {
        if (now < it->second.dueMs)
        {
            ++it;
            continue;
        }

        AShooterPlayerController* pc = FindByEos(it->first);
        if (!pc)
        {
            it = g_queue.erase(it);
            continue;
        }

        AShooterPlayerState* ps = static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
        if (!ps)
        {
            it = g_queue.erase(it);
            continue;
        }

        const int tribeId = ps->GetTribeId();
        if (tribeId > 0)
        {
            if (it->second.attempts > 0)
            {
                Notify(pc);
                Log::GetLog()->info("[AutoTribe] {} placed in tribe_id={}", it->first, tribeId);
            }
            it = g_queue.erase(it);
            continue;
        }

        if (it->second.attempts >= maxAttempts)
        {
            Log::GetLog()->warn("[AutoTribe] Gave up on {} after {} attempts", it->first, it->second.attempts);
            it = g_queue.erase(it);
            continue;
        }

        const std::string name = GenerateTribeName();
        if (name.empty())
        {
            Log::GetLog()->error("[AutoTribe] Word lists unavailable, dropping {}", it->first);
            it = g_queue.erase(it);
            continue;
        }

        it->second.attempts++;
        it->second.dueMs = now + retry;

        Log::GetLog()->info("[AutoTribe] Attempt {}/{} for {} name={}",
            it->second.attempts, maxAttempts, it->first, name);

        FString tribeName(name.c_str());
        FTribeGovernment gov{};
        ps->ServerRequestCreateNewTribe(tribeName, gov);

        ++it;
    }

    g_queue_size.store(g_queue.size(), std::memory_order_relaxed);
}

void Detour_HandleRespawned(AShooterPlayerController* pc, APawn* pawn, bool bNewPlayer)
{
    if (Original_HandleRespawned) Original_HandleRespawned(pc, pawn, bNewPlayer);

    if (!pc) return;
    AShooterPlayerState* ps = static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
    if (!ps) return;
    if (ps->GetTribeId() > 0) return;

    Enqueue(GetEos(ps), 0);
}

void Detour_NotifyPlayerLeftTribe(AShooterPlayerState* ps, FString& playerName, FString& tribeName, bool joinee)
{
    if (Original_NotifyPlayerLeftTribe) Original_NotifyPlayerLeftTribe(ps, playerName, tribeName, joinee);

    if (!ps) return;
    Enqueue(GetEos(ps), g_leave_settle_ms.load(std::memory_order_relaxed));
}

void Detour_Tick(AShooterGameMode* gm, float delta)
{
    if (Original_Tick) Original_Tick(gm, delta);

    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - g_last_config_check).count() >= 10)
    {
        g_last_config_check = now;
        CheckConfigReload();
    }

    ProcessQueue();
}

static void PluginInit()
{
    Log::Get().Init("AutoTribe");

    if (!LoadConfig())
        Log::GetLog()->error("[AutoTribe] Failed to load config, running on defaults");

    if (!LoadWords())
        Log::GetLog()->error("[AutoTribe] Failed to load words.json, enforcement idles until it loads");

    g_last_config_check = std::chrono::steady_clock::now();

    AsaApi::GetHooks().SetHook(
        "AShooterPlayerController.HandleRespawned_Implementation(APawn*,bool)",
        (LPVOID)&Detour_HandleRespawned,
        (LPVOID*)&Original_HandleRespawned);

    AsaApi::GetHooks().SetHook(
        "AShooterPlayerState.NotifyPlayerLeftTribe(FString&,FString&,bool)",
        (LPVOID)&Detour_NotifyPlayerLeftTribe,
        (LPVOID*)&Original_NotifyPlayerLeftTribe);

    AsaApi::GetHooks().SetHook(
        "AShooterGameMode.Tick(float)",
        (LPVOID)&Detour_Tick,
        (LPVOID*)&Original_Tick);

    Log::GetLog()->info("[AutoTribe] Loaded");
}

static void PluginUnload()
{
    AsaApi::GetHooks().DisableHook(
        "AShooterPlayerController.HandleRespawned_Implementation(APawn*,bool)",
        (LPVOID)&Detour_HandleRespawned);

    AsaApi::GetHooks().DisableHook(
        "AShooterPlayerState.NotifyPlayerLeftTribe(FString&,FString&,bool)",
        (LPVOID)&Detour_NotifyPlayerLeftTribe);

    AsaApi::GetHooks().DisableHook(
        "AShooterGameMode.Tick(float)",
        (LPVOID)&Detour_Tick);

    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        g_queue.clear();
        g_queue_size.store(0, std::memory_order_relaxed);
    }

    Log::GetLog()->info("[AutoTribe] Unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { PluginInit(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->critical("[AutoTribe] Init exception: {}", e.what());
    }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { PluginUnload(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->critical("[AutoTribe] Unload exception: {}", e.what());
    }
}