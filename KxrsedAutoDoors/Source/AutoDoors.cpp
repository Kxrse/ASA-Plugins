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
 * Hook category: Structures
 *
 * Automatically closes doors after a configurable delay per player.
 * Default: ON for all players. /ad toggles, /ad {n} sets delay.
 *
 * Table: auto_doors
 *   PK (eos_id)
 *   Columns: eos_id, enabled, delay_seconds
 *
 * Hooks:
 *   APrimalStructureDoor.TryMultiUse — captures player + door interaction
 *   APrimalStructureDoor.NetGotoDoorState_Implementation — detects door open/close
 *
 * Close strategy:
 *   TryMultiUse sets a pending-door marker before calling original.
 *   NetGotoDoorState_Implementation (fired synchronously inside TryMultiUse)
 *   checks if the door opened and queues an auto-close.
 *   Tick callback processes the close queue via GotoDoorState(0).
 */

#include <API/ARK/Ark.h>
#include <json.hpp>
#include <Windows.h>
#include <fstream>
#include <string>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#pragma comment(lib, "AsaApi.lib")
#pragma warning(disable: 4191) // unsafe FARPROC conversion

// =============================================================================
// MariaDB — Dynamic Load
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

    if (!pmysql_init || !pmysql_real_connect || !pmysql_close ||
        !pmysql_query || !pmysql_error || !pmysql_real_escape_string || !pmysql_fetch_row)
    {
        Log::GetLog()->error("[AutoDoors] Failed to resolve required DB functions");
        return false;
    }

    g_mysql_loaded = true;
    return true;
}

// =============================================================================
// Configuration
// =============================================================================

static const std::string g_config_path = "ArkApi/Plugins/AutoDoors/config.json";
static std::string  g_db_host = "localhost";
static unsigned int g_db_port = 3306;
static std::string  g_db_user;
static std::string  g_db_pass;
static std::string  g_db_name;
static int          g_default_delay = 5;
static int          g_min_delay = 1;
static int          g_max_delay = 30;
static std::string  g_message_color = "1.0,1.0,1.0,1.0";

static time_t    g_config_last_modified = 0;
static uintmax_t g_config_last_size = 0;

static bool GetFileInfo(const std::string& path, time_t& mtime, uintmax_t& fsize)
{
    struct _stat st {};
    if (_stat(path.c_str(), &st) != 0) return false;
    mtime = st.st_mtime;
    fsize = static_cast<uintmax_t>(st.st_size);
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
        g_db_host       = j.value("DbHost", "localhost");
        g_db_port       = j.value("DbPort", 3306);
        g_db_user       = j.value("DbUser", "");
        g_db_pass       = j.value("DbPassword", "");
        g_db_name       = j.value("DbName", "");
        g_default_delay = j.value("DefaultDelay", 5);
        g_min_delay     = j.value("MinDelay", 1);
        g_max_delay     = j.value("MaxDelay", 30);
        g_message_color = j.value("MessageColor", "1.0,1.0,1.0,1.0");
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[AutoDoors] Config parse error: {}", ex.what());
        return false;
    }

    if (g_db_user.empty() || g_db_name.empty())
    {
        Log::GetLog()->error("[AutoDoors] Config requires DbUser and DbName");
        return false;
    }

    if (g_min_delay < 1)  g_min_delay = 1;
    if (g_max_delay > 60) g_max_delay = 60;
    if (g_min_delay > g_max_delay) g_min_delay = g_max_delay;
    if (g_default_delay < g_min_delay) g_default_delay = g_min_delay;
    if (g_default_delay > g_max_delay) g_default_delay = g_max_delay;

    return true;
}

// =============================================================================
// Database
// =============================================================================

static MYSQL* g_mysql = nullptr;
static std::mutex g_db_mutex;

static std::string EscapeUnsafe(const std::string& in)
{
    if (!g_mysql || !pmysql_real_escape_string) return in;
    std::string buf(in.size() * 2 + 1, '\0');
    unsigned long len = pmysql_real_escape_string(
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

static bool InitDatabase()
{
    if (!LoadMySQLLib()) return false;

    g_mysql = pmysql_init(nullptr);
    if (!g_mysql)
    {
        Log::GetLog()->error("[AutoDoors] mysql_init returned null");
        return false;
    }

    unsigned int timeout = 10;
    pmysql_options(g_mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

    if (!pmysql_real_connect(g_mysql,
        g_db_host.c_str(), g_db_user.c_str(), g_db_pass.c_str(),
        g_db_name.c_str(), g_db_port, nullptr, 0))
    {
        Log::GetLog()->error("[AutoDoors] DB connect failed: {}", pmysql_error(g_mysql));
        pmysql_close(g_mysql);
        g_mysql = nullptr;
        return false;
    }

    const std::string create_sql =
        "CREATE TABLE IF NOT EXISTS auto_doors ("
        "  eos_id         VARCHAR(128)    NOT NULL,"
        "  enabled        TINYINT         NOT NULL DEFAULT 1,"
        "  delay_seconds  INT             NOT NULL DEFAULT 5,"
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
    if (g_mysql)
    {
        pmysql_close(g_mysql);
        g_mysql = nullptr;
    }
}

// =============================================================================
// Player Preferences (game thread only — no mutex needed)
// =============================================================================

struct PlayerPref
{
    bool enabled = true;
    int  delay   = 5;
};

static std::unordered_map<std::string, PlayerPref> g_prefs;

static void LoadAllPrefs()
{
    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!g_mysql) return;

    if (pmysql_query(g_mysql, "SELECT eos_id, enabled, delay_seconds FROM auto_doors") != 0)
    {
        Log::GetLog()->error("[AutoDoors] Failed to load prefs: {}", pmysql_error(g_mysql));
        return;
    }

    MYSQL_RES* res = pmysql_store_result(g_mysql);
    if (!res) return;

    while (MYSQL_ROW row = pmysql_fetch_row(res))
    {
        if (!row[0]) continue;
        PlayerPref pref;
        pref.enabled = row[1] ? (std::atoi(row[1]) != 0) : true;
        pref.delay   = row[2] ? std::atoi(row[2]) : g_default_delay;
        g_prefs[row[0]] = pref;
    }

    pmysql_free_result(res);
    Log::GetLog()->info("[AutoDoors] Loaded {} player preferences", g_prefs.size());
}

static PlayerPref GetPref(const std::string& eosId)
{
    auto it = g_prefs.find(eosId);
    if (it != g_prefs.end()) return it->second;
    return { true, g_default_delay };
}

static void SavePref(const std::string& eosId, const PlayerPref& pref)
{
    g_prefs[eosId] = pref;

    const std::string eEos = EscapeUnsafe(eosId);

    char sql[256];
    std::snprintf(sql, sizeof(sql),
        "INSERT INTO auto_doors (eos_id, enabled, delay_seconds) "
        "VALUES ('%s', %d, %d) "
        "ON DUPLICATE KEY UPDATE enabled=VALUES(enabled), delay_seconds=VALUES(delay_seconds)",
        eEos.c_str(), pref.enabled ? 1 : 0, pref.delay);

    std::lock_guard<std::mutex> lock(g_db_mutex);
    ExecQuery(sql);
}

// =============================================================================
// Close Queue (game thread only — no mutex needed)
// =============================================================================

struct PendingClose
{
    APrimalStructureDoor*                          door;
    std::chrono::steady_clock::time_point          close_at;
};

static std::vector<PendingClose> g_close_queue;

// =============================================================================
// Pending Interaction (game thread only — single static, set/read/cleared in same call stack)
// =============================================================================

static APrimalStructureDoor* g_pending_door  = nullptr;
static int                   g_pending_delay = 0;
static bool                  g_auto_closing  = false;

// =============================================================================
// Helpers
// =============================================================================

static std::string FStr(const FString& f)
{
    const char* s = TCHAR_TO_UTF8(*f);
    return (s && s[0]) ? s : "unknown";
}

static std::string GetEosId(APlayerController* pc)
{
    if (!pc) return "";
    AShooterPlayerController* spc = static_cast<AShooterPlayerController*>(pc);
    AShooterPlayerState* ps =
        static_cast<AShooterPlayerState*>(spc->PlayerStateField().Get());
    if (!ps) return "";
    FString eosRaw;
    ps->GetUniqueNetIdAsString(&eosRaw);
    std::string eos = FStr(eosRaw);
    return (eos == "unknown") ? "" : eos;
}

static void Notify(AShooterPlayerController* pc, const std::wstring& msg)
{
    const std::wstring wColor(g_message_color.begin(), g_message_color.end());
    const std::wstring wFull =
        L"<RichColor Color=\"" + wColor + L"\">" + msg + L"</>";
    FString fSender(L"");
    FString fMsg(wFull.c_str());
    AsaApi::GetApiUtils().SendChatMessage(pc, fSender, L"{}", std::wstring_view(*fMsg));
}

// =============================================================================
// Detours
// =============================================================================

using TryMultiUse_t = bool(*)(APrimalStructureDoor*, APlayerController*, int, int);
using NetGotoDoorState_t = void(*)(APrimalStructureDoor*, char);

static TryMultiUse_t       Original_TryMultiUse = nullptr;
static NetGotoDoorState_t  Original_NetGotoDoorState = nullptr;

static bool Detour_TryMultiUse(APrimalStructureDoor* door, APlayerController* pc,
    int useIndex, int hitBodyIndex)
{
    // Set pending marker before calling original.
    // Original may synchronously call NetGotoDoorState_Implementation.
    g_pending_door  = nullptr;
    g_pending_delay = 0;

    if (door && pc)
    {
        const std::string eos = GetEosId(pc);
        if (!eos.empty())
        {
            const PlayerPref pref = GetPref(eos);
            if (pref.enabled)
            {
                g_pending_door  = door;
                g_pending_delay = pref.delay;
            }
        }
    }

    const bool result = Original_TryMultiUse(door, pc, useIndex, hitBodyIndex);

    // Clear pending marker after original returns (consumed or not).
    g_pending_door  = nullptr;
    g_pending_delay = 0;

    return result;
}

static void Detour_NetGotoDoorState(APrimalStructureDoor* door, char doorState)
{
    Original_NetGotoDoorState(door, doorState);

    if (!door) return;

    if (doorState == 0)
    {
        // Door closed — remove from close queue if present.
        // Skip if we're the ones closing it (re-entrancy from tick).
        if (!g_auto_closing)
        {
            for (auto it = g_close_queue.begin(); it != g_close_queue.end(); ++it)
            {
                if (it->door == door)
                {
                    g_close_queue.erase(it);
                    break;
                }
            }
        }
        return;
    }

    // Door opened — queue auto-close if this was triggered by a player interaction.
    if (g_pending_door != nullptr && g_pending_delay > 0)
    {
        const auto close_at = std::chrono::steady_clock::now()
            + std::chrono::seconds(g_pending_delay);

        // Update existing entry or add new.
        bool found = false;
        for (auto& entry : g_close_queue)
        {
            if (entry.door == door)
            {
                entry.close_at = close_at;
                found = true;
                break;
            }
        }

        if (!found)
            g_close_queue.push_back({ door, close_at });
    }
}

// =============================================================================
// Tick Callback
// =============================================================================

static float g_config_check_accumulator = 0.0f;

static void OnTick(float delta)
{
    // --- Process close queue ---
    const auto now = std::chrono::steady_clock::now();

    for (int i = static_cast<int>(g_close_queue.size()) - 1; i >= 0; --i)
    {
        if (now >= g_close_queue[i].close_at)
        {
            try
            {
                g_auto_closing = true;
                g_close_queue[i].door->GotoDoorState(0);
                g_auto_closing = false;
            }
            catch (...) { g_auto_closing = false; }

            g_close_queue.erase(g_close_queue.begin() + i);
        }
    }

    // --- Config hot-reload (every 10 seconds) ---
    g_config_check_accumulator += delta;
    if (g_config_check_accumulator >= 10.0f)
    {
        g_config_check_accumulator = 0.0f;

        time_t mtime = 0;
        uintmax_t fsize = 0;
        if (GetFileInfo(g_config_path, mtime, fsize) && fsize > 0 &&
            (mtime != g_config_last_modified || fsize != g_config_last_size))
        {
            if (LoadConfig())
            {
                g_config_last_modified = mtime;
                g_config_last_size     = fsize;
                Log::GetLog()->info("[AutoDoors] Config reloaded");
            }
        }
    }
}

// =============================================================================
// Chat Command: /ad
// =============================================================================

static void Cmd_AutoDoor(AShooterPlayerController* pc, FString* message,
    int /*mode*/, int /*platform*/)
{
    if (!pc) return;

    const std::string eos = GetEosId(static_cast<APlayerController*>(pc));
    if (eos.empty()) return;

    PlayerPref pref = GetPref(eos);

    // Parse argument: /ad or /ad {number}
    std::string arg;
    if (message)
    {
        std::string raw = FStr(*message);
        // Strip the command prefix.
        const size_t space = raw.find(' ');
        if (space != std::string::npos)
            arg = raw.substr(space + 1);
    }

    if (arg.empty())
    {
        // Toggle on/off.
        pref.enabled = !pref.enabled;
        SavePref(eos, pref);

        if (pref.enabled)
        {
            wchar_t buf[128];
            std::swprintf(buf, 128, L"Auto-doors enabled (%ds delay)", pref.delay);
            Notify(pc, buf);
        }
        else
        {
            Notify(pc, L"Auto-doors disabled");
        }
    }
    else
    {
        // Set delay.
        int val = std::atoi(arg.c_str());
        if (val <= 0)
        {
            Notify(pc, L"Usage: /ad [seconds]");
            return;
        }

        if (val < g_min_delay) val = g_min_delay;
        if (val > g_max_delay) val = g_max_delay;

        pref.delay   = val;
        pref.enabled = true;
        SavePref(eos, pref);

        wchar_t buf[128];
        std::swprintf(buf, 128, L"Auto-door delay set to %ds", val);
        Notify(pc, buf);
    }
}

// =============================================================================
// Plugin Init / Unload
// =============================================================================

static void Plugin_Init_Impl()
{
    Log::Get().Init("AutoDoors");

    if (!LoadConfig())
    {
        Log::GetLog()->error("[AutoDoors] Config load failed — plugin will not function");
        return;
    }

    // Record initial config file state for hot-reload.
    GetFileInfo(g_config_path, g_config_last_modified, g_config_last_size);

    if (!InitDatabase())
    {
        Log::GetLog()->error("[AutoDoors] Database init failed — plugin will not function");
        return;
    }

    LoadAllPrefs();

    // Hooks.
    AsaApi::GetHooks().SetHook(
        "APrimalStructureDoor.TryMultiUse(APlayerController*,int,int)",
        (LPVOID)&Detour_TryMultiUse,
        (LPVOID*)&Original_TryMultiUse
    );

    AsaApi::GetHooks().SetHook(
        "APrimalStructureDoor.NetGotoDoorState_Implementation(signedchar)",
        (LPVOID)&Detour_NetGotoDoorState,
        (LPVOID*)&Original_NetGotoDoorState
    );

    // Tick callback.
    AsaApi::GetCommands().AddOnTickCallback(FString(L"AutoDoors_Tick"), &OnTick);

    // Chat command.
    AsaApi::GetCommands().AddChatCommand(FString(L"/ad"), &Cmd_AutoDoor);

    Log::GetLog()->info("[AutoDoors] Plugin loaded");
}

static void Plugin_Unload_Impl()
{
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/ad"));
    AsaApi::GetCommands().RemoveOnTickCallback(FString(L"AutoDoors_Tick"));

    AsaApi::GetHooks().DisableHook(
        "APrimalStructureDoor.TryMultiUse(APlayerController*,int,int)",
        (LPVOID)&Detour_TryMultiUse
    );

    AsaApi::GetHooks().DisableHook(
        "APrimalStructureDoor.NetGotoDoorState_Implementation(signedchar)",
        (LPVOID)&Detour_NetGotoDoorState
    );

    g_close_queue.clear();
    g_prefs.clear();

    CloseDatabase();

    Log::GetLog()->info("[AutoDoors] Plugin unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { Plugin_Init_Impl(); }
    catch (const std::exception& ex) { Log::GetLog()->critical("[AutoDoors] Init exception: {}", ex.what()); }
    catch (...) { Log::GetLog()->critical("[AutoDoors] Init unknown exception"); }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { Plugin_Unload_Impl(); }
    catch (...) {}
}