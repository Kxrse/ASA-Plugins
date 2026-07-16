/*
AutoDoors - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * AutoDoors - ASA Plugin
 *
 * Hooks:
 *   APrimalStructureDoor.TryMultiUse      stamps the interacting player's delay for the state change that follows
 *   APrimalStructureDoor.GotoDoorState    arms the native delayed close when a stamp is live and the door opens
 *
 * Config:
 *   ArkApi/Plugins/AutoDoors/config.json
 *   DbHost, DbPort, DbUser, DbPassword, DbName: MariaDB connection, DbHost defaults to 127.0.0.1 so the link is TCP
 *   DefaultEnabled: auto close state applied to a player who has never run /ad
 *   DefaultDelay: seconds before an opened door closes for a player who has never run /ad
 *   MinDelay, MaxDelay: bounds a requested delay is clamped into, hard floor 1 and hard ceiling 60
 *   EnabledMessage, DisabledMessage, DelayMessage, UsageMessage: chat text, {delay} expands to the player's delay
 *   MessageColor: RGBA string used for all messages, must not be blank or the RichColor tag is malformed and the
 *                 message body renders empty
 *   Hot-reloaded every 10s.
 *
 * Config Example:
 * {
 *     "DbHost": "127.0.0.1",
 *     "DbPort": 3306,
 *     "DbUser": "User",
 *     "DbPassword": "Password",
 *     "DbName": "Database",
 *     "DefaultEnabled": true,
 *     "DefaultDelay": 5,
 *     "MinDelay": 1,
 *     "MaxDelay": 30,
 *     "EnabledMessage": "Auto doors on, {delay}s delay.",
 *     "DisabledMessage": "Auto doors off.",
 *     "DelayMessage": "Auto door delay set to {delay}s.",
 *     "UsageMessage": "Usage: /ad to toggle, /ad <seconds> to set the delay.",
 *     "MessageColor": "1.0,1.0,1.0,1.0"
 * }
 *
 * Table: auto_doors
 *   PK (eos_id)
 *   Columns: eos_id, enabled, delay_seconds
 *
 * Close strategy:
 *   TryMultiUse stamps the player's delay before calling the original, then clears it once the original returns.
 *   GotoDoorState fires inside that synchronous window on the resulting state change. When a stamp is live and
 *   the new state is not closed, the close is handed to the engine's own DelayedGotoDoorState timer, which lives
 *   on the door actor and dies with it. The plugin holds no door pointer past the call stack and runs no tick.
 *   A timer that fires on an already closed door is a no-op, and the stamp is clear on the timer driven state
 *   change, so re-entry cannot chain.
 *
 *   The stamp deliberately carries no door pointer. APrimalStructureDoor implements IMultiUseInterface, so
 *   TryMultiUse is entered on the interface subobject while GotoDoorState is entered on the actor base. The two
 *   this pointers differ by the size of UObject and never compare equal, so identity is established by the
 *   synchronous window rather than by address. Only GotoDoorState's pointer is ever dereferenced. TryMultiUse's
 *   door parameter is passed straight back to the original and is never read.
 *
 * Write strategy:
 *   Preferences are preloaded into memory at init. /ad mutates the in memory copy and stamps a dirty entry, last
 *   write wins per player. A worker thread drains the set every second, pings the link, and reconnects if it
 *   dropped. On reconnect failure the drained entries are merged back, with entries queued since the drain taking
 *   precedence. Config is rescanned every 10 seconds on the same worker, size plus last write time, reloaded only
 *   on a confirmed stable change. No database work runs on the game thread.
 */

#include <API/ARK/Ark.h>

#pragma warning(disable: 4191)
#pragma comment(lib, "AsaApi.lib")

#include <json.hpp>
#include <Windows.h>
#include <fstream>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <unordered_map>
#include <cstdio>
#include <cstdlib>

namespace fs = std::filesystem;

// =============================================================================
// Constants
// =============================================================================

static constexpr int DELAY_FLOOR   = 1;
static constexpr int DELAY_CEILING = 60;
static constexpr int MAX_ARG_CHARS = 6;

// =============================================================================
// MariaDB - Dynamic Load
// =============================================================================

typedef struct st_mysql     MYSQL;
typedef struct st_mysql_res MYSQL_RES;
typedef char** MYSQL_ROW;

typedef MYSQL*        (__stdcall* mysql_init_t)               (MYSQL*);
typedef MYSQL*        (__stdcall* mysql_real_connect_t)       (MYSQL*, const char*, const char*, const char*, const char*, unsigned int, const char*, unsigned long);
typedef void          (__stdcall* mysql_close_t)              (MYSQL*);
typedef int           (__stdcall* mysql_query_t)              (MYSQL*, const char*);
typedef MYSQL_RES*    (__stdcall* mysql_store_result_t)       (MYSQL*);
typedef void          (__stdcall* mysql_free_result_t)        (MYSQL_RES*);
typedef const char*   (__stdcall* mysql_error_t)              (MYSQL*);
typedef unsigned long (__stdcall* mysql_real_escape_string_t) (MYSQL*, char*, const char*, unsigned long);
typedef int           (__stdcall* mysql_options_t)            (MYSQL*, int, const void*);
typedef MYSQL_ROW     (__stdcall* mysql_fetch_row_t)          (MYSQL_RES*);
typedef int           (__stdcall* mysql_ping_t)               (MYSQL*);

#define MYSQL_OPT_CONNECT_TIMEOUT 0

static HMODULE                    g_mysql_module = nullptr;
static mysql_init_t               pmysql_init = nullptr;
static mysql_real_connect_t       pmysql_real_connect = nullptr;
static mysql_close_t              pmysql_close = nullptr;
static mysql_query_t              pmysql_query = nullptr;
static mysql_store_result_t       pmysql_store_result = nullptr;
static mysql_free_result_t        pmysql_free_result = nullptr;
static mysql_error_t              pmysql_error = nullptr;
static mysql_real_escape_string_t pmysql_real_escape_string = nullptr;
static mysql_options_t            pmysql_options = nullptr;
static mysql_fetch_row_t          pmysql_fetch_row = nullptr;
static mysql_ping_t               pmysql_ping = nullptr;
static bool                       g_mysql_loaded = false;

static bool LoadMySQLLib()
{
    if (g_mysql_loaded) return true;

    const char* candidates[] = {
        "libmariadb.dll",
        ".\\libmariadb.dll",
        "ArkApi\\Plugins\\libmariadb.dll",
        "libmysql.dll",
        ".\\libmysql.dll",
        nullptr
    };

    for (int i = 0; candidates[i]; ++i)
    {
        g_mysql_module = LoadLibraryA(candidates[i]);
        if (g_mysql_module)
        {
            Log::GetLog()->info("[AutoDoors] Loaded DB library: {}", candidates[i]);
            break;
        }
    }

    if (!g_mysql_module)
    {
        Log::GetLog()->error("[AutoDoors] Could not find libmariadb.dll or libmysql.dll");
        return false;
    }

    pmysql_init               = (mysql_init_t)GetProcAddress(g_mysql_module, "mysql_init");
    pmysql_real_connect       = (mysql_real_connect_t)GetProcAddress(g_mysql_module, "mysql_real_connect");
    pmysql_close              = (mysql_close_t)GetProcAddress(g_mysql_module, "mysql_close");
    pmysql_query              = (mysql_query_t)GetProcAddress(g_mysql_module, "mysql_query");
    pmysql_store_result       = (mysql_store_result_t)GetProcAddress(g_mysql_module, "mysql_store_result");
    pmysql_free_result        = (mysql_free_result_t)GetProcAddress(g_mysql_module, "mysql_free_result");
    pmysql_error              = (mysql_error_t)GetProcAddress(g_mysql_module, "mysql_error");
    pmysql_real_escape_string = (mysql_real_escape_string_t)GetProcAddress(g_mysql_module, "mysql_real_escape_string");
    pmysql_options            = (mysql_options_t)GetProcAddress(g_mysql_module, "mysql_options");
    pmysql_fetch_row          = (mysql_fetch_row_t)GetProcAddress(g_mysql_module, "mysql_fetch_row");
    pmysql_ping               = (mysql_ping_t)GetProcAddress(g_mysql_module, "mysql_ping");

    if (!pmysql_init || !pmysql_real_connect || !pmysql_close || !pmysql_query ||
        !pmysql_error || !pmysql_real_escape_string || !pmysql_fetch_row || !pmysql_ping)
    {
        Log::GetLog()->error("[AutoDoors] Failed to resolve required DB functions");
        return false;
    }

    g_mysql_loaded = true;
    return true;
}

// =============================================================================
// String Helpers
// =============================================================================

static std::string FStr(const FString& f)
{
    const char* s = TCHAR_TO_UTF8(*f);
    return (s && s[0]) ? s : "";
}

static std::wstring Widen(const std::string& s)
{
    if (s.empty()) return L"";
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring out((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), len);
    return out;
}

static std::wstring ExpandDelay(const std::wstring& text, int delay)
{
    const std::wstring token = L"{delay}";
    const std::wstring value = std::to_wstring(delay);
    std::wstring out = text;
    size_t pos = 0;
    while ((pos = out.find(token, pos)) != std::wstring::npos)
    {
        out.replace(pos, token.size(), value);
        pos += value.size();
    }
    return out;
}

static std::string Trim(const std::string& in)
{
    const size_t first = in.find_first_not_of(" \t");
    if (first == std::string::npos) return "";
    const size_t last = in.find_last_not_of(" \t");
    return in.substr(first, last - first + 1);
}

// =============================================================================
// Configuration
// =============================================================================

static const std::string g_config_path = "ArkApi/Plugins/AutoDoors/config.json";

static std::mutex   g_config_mutex;
static std::string  g_db_host = "127.0.0.1";
static unsigned int g_db_port = 3306;
static std::string  g_db_user;
static std::string  g_db_pass;
static std::string  g_db_name;
static bool         g_default_enabled = true;
static int          g_default_delay = 5;
static int          g_min_delay = 1;
static int          g_max_delay = 30;
static std::wstring g_enabled_message  = L"Auto doors on, {delay}s delay.";
static std::wstring g_disabled_message = L"Auto doors off.";
static std::wstring g_delay_message    = L"Auto door delay set to {delay}s.";
static std::wstring g_usage_message    = L"Usage: /ad to toggle, /ad <seconds> to set the delay.";
static std::wstring g_message_color    = L"1.0,1.0,1.0,1.0";

static uintmax_t          g_last_size = 0;
static fs::file_time_type g_last_write;
static bool               g_config_seen = false;

static bool ApplyConfig(const nlohmann::json& j)
{
    std::string  host = j.value("DbHost", std::string("127.0.0.1"));
    unsigned int port = (unsigned int)j.value("DbPort", 3306);
    std::string  user = j.value("DbUser", std::string(""));
    std::string  pass = j.value("DbPassword", std::string(""));
    std::string  name = j.value("DbName", std::string(""));

    if (host.empty()) host = "127.0.0.1";

    if (user.empty() || name.empty())
    {
        Log::GetLog()->error("[AutoDoors] Config requires DbUser and DbName");
        return false;
    }

    bool defEnabled = j.value("DefaultEnabled", true);
    int  defDelay   = j.value("DefaultDelay", 5);
    int  minDelay   = j.value("MinDelay", 1);
    int  maxDelay   = j.value("MaxDelay", 30);

    if (minDelay < DELAY_FLOOR)   minDelay = DELAY_FLOOR;
    if (maxDelay > DELAY_CEILING) maxDelay = DELAY_CEILING;
    if (maxDelay < DELAY_FLOOR)   maxDelay = DELAY_FLOOR;
    if (minDelay > maxDelay)      minDelay = maxDelay;
    if (defDelay < minDelay)      defDelay = minDelay;
    if (defDelay > maxDelay)      defDelay = maxDelay;

    std::wstring enabledMsg  = Widen(j.value("EnabledMessage",  std::string("Auto doors on, {delay}s delay.")));
    std::wstring disabledMsg = Widen(j.value("DisabledMessage", std::string("Auto doors off.")));
    std::wstring delayMsg    = Widen(j.value("DelayMessage",    std::string("Auto door delay set to {delay}s.")));
    std::wstring usageMsg    = Widen(j.value("UsageMessage",    std::string("Usage: /ad to toggle, /ad <seconds> to set the delay.")));
    std::wstring color       = Widen(j.value("MessageColor",    std::string("1.0,1.0,1.0,1.0")));

    std::lock_guard<std::mutex> lock(g_config_mutex);
    g_db_host          = std::move(host);
    g_db_port          = port;
    g_db_user          = std::move(user);
    g_db_pass          = std::move(pass);
    g_db_name          = std::move(name);
    g_default_enabled  = defEnabled;
    g_default_delay    = defDelay;
    g_min_delay        = minDelay;
    g_max_delay        = maxDelay;
    g_enabled_message  = std::move(enabledMsg);
    g_disabled_message = std::move(disabledMsg);
    g_delay_message    = std::move(delayMsg);
    g_usage_message    = std::move(usageMsg);
    g_message_color    = std::move(color);
    return true;
}

static bool LoadConfig()
{
    std::ifstream file(g_config_path);
    if (!file.is_open())
    {
        Log::GetLog()->error("[AutoDoors] Cannot open config: {}", g_config_path);
        return false;
    }

    try
    {
        nlohmann::json j;
        file >> j;
        if (!ApplyConfig(j)) return false;
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[AutoDoors] Config parse error: {}", ex.what());
        return false;
    }

    try
    {
        if (fs::exists(g_config_path))
        {
            g_last_size   = fs::file_size(g_config_path);
            g_last_write  = fs::last_write_time(g_config_path);
            g_config_seen = true;
        }
    }
    catch (...) {}

    return true;
}

static void ReloadConfigIfChanged()
{
    try
    {
        if (!fs::exists(g_config_path)) return;

        const uintmax_t sz = fs::file_size(g_config_path);
        if (sz == 0) return;

        const fs::file_time_type wt = fs::last_write_time(g_config_path);
        if (g_config_seen && sz == g_last_size && wt == g_last_write) return;

        std::ifstream file(g_config_path);
        if (!file.is_open()) return;

        nlohmann::json j;
        file >> j;
        if (!ApplyConfig(j)) return;

        g_last_size   = sz;
        g_last_write  = wt;
        g_config_seen = true;

        Log::GetLog()->info("[AutoDoors] Config reloaded");
    }
    catch (...)
    {
        return;
    }
}

// =============================================================================
// Database
// =============================================================================

static MYSQL*     g_mysql = nullptr;
static std::mutex g_db_mutex;

static std::string EscapeUnsafe(const std::string& in)
{
    if (!g_mysql || !pmysql_real_escape_string) return in;
    std::string buf(in.size() * 2 + 1, '\0');
    const unsigned long len = pmysql_real_escape_string(
        g_mysql, buf.data(), in.c_str(), (unsigned long)in.size());
    buf.resize(len);
    return buf;
}

static bool ExecQuery(const std::string& sql)
{
    if (!g_mysql) return false;
    if (pmysql_query(g_mysql, sql.c_str()) != 0)
    {
        Log::GetLog()->error("[AutoDoors] Query error: {}", pmysql_error(g_mysql));
        return false;
    }
    if (MYSQL_RES* res = pmysql_store_result(g_mysql))
        pmysql_free_result(res);
    return true;
}

static bool Connect()
{
    if (g_mysql)
    {
        pmysql_close(g_mysql);
        g_mysql = nullptr;
    }

    std::string  host, user, pass, name;
    unsigned int port;
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        host = g_db_host;
        user = g_db_user;
        pass = g_db_pass;
        name = g_db_name;
        port = g_db_port;
    }

    g_mysql = pmysql_init(nullptr);
    if (!g_mysql)
    {
        Log::GetLog()->error("[AutoDoors] mysql_init returned null");
        return false;
    }

    unsigned int timeout = 10;
    pmysql_options(g_mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

    if (!pmysql_real_connect(g_mysql, host.c_str(), user.c_str(), pass.c_str(),
        name.c_str(), port, nullptr, 0))
    {
        Log::GetLog()->error("[AutoDoors] DB connect failed: {}", pmysql_error(g_mysql));
        pmysql_close(g_mysql);
        g_mysql = nullptr;
        return false;
    }

    return true;
}

static bool EnsureConnection()
{
    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (g_mysql && pmysql_ping(g_mysql) == 0) return true;
    if (!Connect()) return false;
    Log::GetLog()->info("[AutoDoors] Database reconnected");
    return true;
}

static bool InitDatabase()
{
    if (!LoadMySQLLib()) return false;

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!Connect()) return false;

    const std::string create_sql =
        "CREATE TABLE IF NOT EXISTS auto_doors ("
        "  eos_id         VARCHAR(128) NOT NULL,"
        "  enabled        TINYINT      NOT NULL DEFAULT 1,"
        "  delay_seconds  INT          NOT NULL DEFAULT 5,"
        "  PRIMARY KEY (eos_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;";

    if (!ExecQuery(create_sql))
    {
        Log::GetLog()->error("[AutoDoors] Failed to create auto_doors table");
        return false;
    }

    Log::GetLog()->info("[AutoDoors] Database ready");
    return true;
}

static void CloseDatabase()
{
    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (g_mysql)
    {
        pmysql_close(g_mysql);
        g_mysql = nullptr;
    }
}

// =============================================================================
// Preferences
// =============================================================================

struct PlayerPref
{
    bool enabled = true;
    int  delay   = 5;
};

static std::unordered_map<std::string, PlayerPref> g_prefs;

static std::mutex                                  g_dirty_mutex;
static std::unordered_map<std::string, PlayerPref> g_dirty;

static void LoadAllPrefs()
{
    int  defDelay, minDelay, maxDelay;
    bool defEnabled;
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        defDelay   = g_default_delay;
        minDelay   = g_min_delay;
        maxDelay   = g_max_delay;
        defEnabled = g_default_enabled;
    }

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!g_mysql) return;

    if (pmysql_query(g_mysql, "SELECT eos_id, enabled, delay_seconds FROM auto_doors") != 0)
    {
        Log::GetLog()->error("[AutoDoors] Failed to load preferences: {}", pmysql_error(g_mysql));
        return;
    }

    MYSQL_RES* res = pmysql_store_result(g_mysql);
    if (!res) return;

    while (MYSQL_ROW row = pmysql_fetch_row(res))
    {
        if (!row[0]) continue;

        PlayerPref pref;
        pref.enabled = row[1] ? (std::atoi(row[1]) != 0) : defEnabled;
        pref.delay   = row[2] ? std::atoi(row[2]) : defDelay;

        if (pref.delay < minDelay) pref.delay = minDelay;
        if (pref.delay > maxDelay) pref.delay = maxDelay;

        g_prefs[row[0]] = pref;
    }

    pmysql_free_result(res);
    Log::GetLog()->info("[AutoDoors] Loaded {} player preferences", g_prefs.size());
}

static PlayerPref GetPref(const std::string& eosId)
{
    const auto it = g_prefs.find(eosId);
    if (it != g_prefs.end()) return it->second;

    std::lock_guard<std::mutex> lock(g_config_mutex);
    PlayerPref pref;
    pref.enabled = g_default_enabled;
    pref.delay   = g_default_delay;
    return pref;
}

static void SetPref(const std::string& eosId, const PlayerPref& pref)
{
    g_prefs[eosId] = pref;

    std::lock_guard<std::mutex> lock(g_dirty_mutex);
    g_dirty[eosId] = pref;
}

static void FlushDirty()
{
    std::unordered_map<std::string, PlayerPref> batch;
    {
        std::lock_guard<std::mutex> lock(g_dirty_mutex);
        if (g_dirty.empty()) return;
        batch.swap(g_dirty);
    }

    if (!EnsureConnection())
    {
        std::lock_guard<std::mutex> lock(g_dirty_mutex);
        for (const auto& kv : batch)
            g_dirty.emplace(kv.first, kv.second);
        return;
    }

    for (const auto& kv : batch)
    {
        const std::string eos = EscapeUnsafe(kv.first);

        char sql[512];
        std::snprintf(sql, sizeof(sql),
            "INSERT INTO auto_doors (eos_id, enabled, delay_seconds) "
            "VALUES ('%s', %d, %d) "
            "ON DUPLICATE KEY UPDATE enabled=VALUES(enabled), delay_seconds=VALUES(delay_seconds)",
            eos.c_str(), kv.second.enabled ? 1 : 0, kv.second.delay);

        std::lock_guard<std::mutex> lock(g_db_mutex);
        ExecQuery(sql);
    }
}

// =============================================================================
// Player Helpers
// =============================================================================

static std::string GetEos(AShooterPlayerController* pc)
{
    if (!pc) return "";
    FString eosRaw;
    pc->GetUniqueNetIdAsString(&eosRaw);
    return FStr(eosRaw);
}

static void Notify(AShooterPlayerController* pc, const std::wstring& msg, const std::wstring& color)
{
    if (!pc) return;

    const std::wstring rich = L"<RichColor Color=\"" + color + L"\">" + msg + L"</>";
    FString fSender(L"");
    FString fMsg(rich.c_str());
    AsaApi::GetApiUtils().SendChatMessage(pc, fSender, L"{}", std::wstring_view(*fMsg));
}

// =============================================================================
// Hooks
// =============================================================================

using TryMultiUse_t   = bool(*)(APrimalStructureDoor*, APlayerController*, int, int);
using GotoDoorState_t = void(*)(APrimalStructureDoor*, char);

static TryMultiUse_t   Original_TryMultiUse   = nullptr;
static GotoDoorState_t Original_GotoDoorState = nullptr;

static int g_stamped_delay = 0;

static void Detour_GotoDoorState(APrimalStructureDoor* door, char doorState)
{
    Original_GotoDoorState(door, doorState);

    if (!door || doorState == 0) return;
    if (g_stamped_delay <= 0) return;

    try { door->DelayedGotoDoorState(0, (float)g_stamped_delay); }
    catch (...) {}
}

static bool Detour_TryMultiUse(APrimalStructureDoor* door, APlayerController* pc,
    int useIndex, int hitBodyIndex)
{
    g_stamped_delay = 0;

    if (pc)
    {
        const std::string eos = GetEos(static_cast<AShooterPlayerController*>(pc));
        if (!eos.empty())
        {
            const PlayerPref pref = GetPref(eos);
            if (pref.enabled && pref.delay > 0)
                g_stamped_delay = pref.delay;
        }
    }

    const bool result = Original_TryMultiUse(door, pc, useIndex, hitBodyIndex);

    g_stamped_delay = 0;

    return result;
}

// =============================================================================
// Chat Command
// =============================================================================

static void Cmd_AutoDoors(AShooterPlayerController* pc, FString* message, int, int)
{
    if (!pc) return;

    const std::string eos = GetEos(pc);
    if (eos.empty()) return;

    std::string arg;
    if (message)
    {
        const std::string raw = FStr(*message);
        const size_t sp = raw.find(' ');
        if (sp != std::string::npos)
            arg = Trim(raw.substr(sp + 1));
    }

    int minDelay, maxDelay;
    std::wstring enabledMsg, disabledMsg, delayMsg, usageMsg, color;
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        minDelay    = g_min_delay;
        maxDelay    = g_max_delay;
        enabledMsg  = g_enabled_message;
        disabledMsg = g_disabled_message;
        delayMsg    = g_delay_message;
        usageMsg    = g_usage_message;
        color       = g_message_color;
    }

    PlayerPref pref = GetPref(eos);

    if (arg.empty())
    {
        pref.enabled = !pref.enabled;
        SetPref(eos, pref);
        Notify(pc, pref.enabled ? ExpandDelay(enabledMsg, pref.delay) : disabledMsg, color);
        return;
    }

    if (arg.size() > MAX_ARG_CHARS ||
        arg.find_first_not_of("0123456789") != std::string::npos)
    {
        Notify(pc, usageMsg, color);
        return;
    }

    int val = std::atoi(arg.c_str());
    if (val <= 0)
    {
        Notify(pc, usageMsg, color);
        return;
    }

    if (val < minDelay) val = minDelay;
    if (val > maxDelay) val = maxDelay;

    pref.delay   = val;
    pref.enabled = true;
    SetPref(eos, pref);

    Notify(pc, ExpandDelay(delayMsg, val), color);
}

// =============================================================================
// Worker
// =============================================================================

static std::atomic<bool> g_worker_running{ false };
static std::thread       g_worker_thread;

static void WorkerLoop()
{
    int config_counter = 0;

    while (g_worker_running.load())
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!g_worker_running.load()) break;

        try { FlushDirty(); }
        catch (...) {}

        if (++config_counter >= 10)
        {
            config_counter = 0;
            try { ReloadConfigIfChanged(); }
            catch (...) {}
        }
    }

    try { FlushDirty(); }
    catch (...) {}
}

// =============================================================================
// Plugin Entry Points
// =============================================================================

static void InitImpl()
{
    Log::Get().Init("AutoDoors");

    if (!LoadConfig())
    {
        Log::GetLog()->error("[AutoDoors] Halted, config error");
        return;
    }

    if (!InitDatabase())
    {
        Log::GetLog()->error("[AutoDoors] Halted, database error");
        return;
    }

    LoadAllPrefs();

    AsaApi::GetHooks().SetHook(
        "APrimalStructureDoor.TryMultiUse(APlayerController*,int,int)",
        (LPVOID)&Detour_TryMultiUse,
        (LPVOID*)&Original_TryMultiUse
    );

    AsaApi::GetHooks().SetHook(
        "APrimalStructureDoor.GotoDoorState(signedchar)",
        (LPVOID)&Detour_GotoDoorState,
        (LPVOID*)&Original_GotoDoorState
    );

    AsaApi::GetCommands().AddChatCommand(FString(L"/ad"), &Cmd_AutoDoors);

    g_worker_running.store(true);
    g_worker_thread = std::thread(WorkerLoop);

    Log::GetLog()->info("[AutoDoors] Plugin loaded");
}

static void UnloadImpl()
{
    g_worker_running.store(false);
    if (g_worker_thread.joinable())
        g_worker_thread.join();

    AsaApi::GetCommands().RemoveChatCommand(FString(L"/ad"));

    AsaApi::GetHooks().DisableHook(
        "APrimalStructureDoor.TryMultiUse(APlayerController*,int,int)",
        (LPVOID)&Detour_TryMultiUse
    );

    AsaApi::GetHooks().DisableHook(
        "APrimalStructureDoor.GotoDoorState(signedchar)",
        (LPVOID)&Detour_GotoDoorState
    );

    g_stamped_delay = 0;
    g_prefs.clear();

    {
        std::lock_guard<std::mutex> lock(g_dirty_mutex);
        g_dirty.clear();
    }

    CloseDatabase();

    Log::GetLog()->info("[AutoDoors] Plugin unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { InitImpl(); }
    catch (const std::exception& ex)
    {
        Log::GetLog()->critical("[AutoDoors] Init exception: {}", ex.what());
    }
    catch (...)
    {
        Log::GetLog()->critical("[AutoDoors] Init unknown exception");
    }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { UnloadImpl(); }
    catch (const std::exception& ex)
    {
        Log::GetLog()->critical("[AutoDoors] Unload exception: {}", ex.what());
    }
    catch (...)
    {
        Log::GetLog()->critical("[AutoDoors] Unload unknown exception");
    }
}