/*
ShadowBan - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * ShadowBan - ASA Plugin
 *
 * Hooks: None - console/RCON commands and timers only
 *
 * Tables:
 *   shadow_bans - PK (eos_id)
 *   shadow_ban_pins - PK (eos_id, map_name)
 *     Columns: x (DOUBLE), y (DOUBLE), z (DOUBLE)
 *
 * Commands (console and RCON):
 *   shadowban {eos_id}    - add the player to the cluster ban roster; if online here, pin to current location
 *   unshadowban {eos_id}  - remove the player from the roster and clear all pins across all maps
 *
 * Behaviour:
 *   An in memory cache of the ban roster and this map's pins is refreshed from the shared DB every RefreshIntervalSeconds.
 *   Every tick each online banned player is checked against the cache, no DB access on the hot path.
 *   A banned player with no pin on this map is pinned to their current location once they have spawned in.
 *   A banned player beyond LeashDistance from their pin is teleported back to it.
 *   Pins are per map, so a transfer creates a fresh pin on the new map after the next refresh.
 *
 * Config:
 *   DbHost, DbPort, DbUser, DbPassword, DbName
 *   LeashDistance (default 3000)
 *   RefreshIntervalSeconds (default 30)
 */

#include <API/ARK/Ark.h>

#pragma warning(disable: 4191)
#pragma comment(lib, "AsaApi")

#include <json.hpp>
#include <Windows.h>
#include <fstream>
#include <string>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <cstdio>
#include <cmath>
#include <cctype>

typedef struct st_mysql     MYSQL;
typedef struct st_mysql_res MYSQL_RES;
typedef char** MYSQL_ROW;

typedef MYSQL* (__stdcall* mysql_init_t)             (MYSQL*);
typedef MYSQL* (__stdcall* mysql_real_connect_t)     (MYSQL*, const char*, const char*, const char*, const char*, unsigned int, const char*, unsigned long);
typedef void(__stdcall* mysql_close_t)            (MYSQL*);
typedef int(__stdcall* mysql_query_t)             (MYSQL*, const char*);
typedef MYSQL_RES* (__stdcall* mysql_store_result_t) (MYSQL*);
typedef void(__stdcall* mysql_free_result_t)      (MYSQL_RES*);
typedef const char* (__stdcall* mysql_error_t)       (MYSQL*);
typedef unsigned long(__stdcall* mysql_real_escape_string_t)(MYSQL*, char*, const char*, unsigned long);
typedef int(__stdcall* mysql_options_t)          (MYSQL*, int, const void*);
typedef MYSQL_ROW(__stdcall* mysql_fetch_row_t)      (MYSQL_RES*);

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
            Log::GetLog()->info("[ShadowBan] Loaded DB library: {}", candidates[i]);
            break;
        }
    }

    if (!g_mysql_module)
    {
        Log::GetLog()->error("[ShadowBan] Could not find libmariadb.dll or libmysql.dll");
        return false;
    }

    pmysql_init = (mysql_init_t)GetProcAddress(g_mysql_module, "mysql_init");
    pmysql_real_connect = (mysql_real_connect_t)GetProcAddress(g_mysql_module, "mysql_real_connect");
    pmysql_close = (mysql_close_t)GetProcAddress(g_mysql_module, "mysql_close");
    pmysql_query = (mysql_query_t)GetProcAddress(g_mysql_module, "mysql_query");
    pmysql_store_result = (mysql_store_result_t)GetProcAddress(g_mysql_module, "mysql_store_result");
    pmysql_free_result = (mysql_free_result_t)GetProcAddress(g_mysql_module, "mysql_free_result");
    pmysql_error = (mysql_error_t)GetProcAddress(g_mysql_module, "mysql_error");
    pmysql_real_escape_string = (mysql_real_escape_string_t)GetProcAddress(g_mysql_module, "mysql_real_escape_string");
    pmysql_options = (mysql_options_t)GetProcAddress(g_mysql_module, "mysql_options");
    pmysql_fetch_row = (mysql_fetch_row_t)GetProcAddress(g_mysql_module, "mysql_fetch_row");

    if (!pmysql_init || !pmysql_real_connect || !pmysql_close ||
        !pmysql_query || !pmysql_error || !pmysql_real_escape_string || !pmysql_fetch_row)
    {
        Log::GetLog()->error("[ShadowBan] Failed to resolve required DB functions");
        return false;
    }

    g_mysql_loaded = true;
    return true;
}

static std::string  g_db_host = "127.0.0.1";
static unsigned int g_db_port = 3306;
static std::string  g_db_user;
static std::string  g_db_pass;
static std::string  g_db_name;
static double       g_leash_distance = 3000.0;
static int          g_refresh_interval = 30;

static constexpr double LOCATION_READY_EPSILON = 1.0;
static const char* CONFIG_PATH = "ArkApi/Plugins/ShadowBan/config.json";

static bool LoadConfig()
{
    std::ifstream file(CONFIG_PATH);
    if (!file.is_open())
    {
        Log::GetLog()->error("[ShadowBan] Cannot open config: {}", CONFIG_PATH);
        return false;
    }

    try
    {
        nlohmann::json j;
        file >> j;

        g_db_host = j.value("DbHost", "127.0.0.1");
        g_db_port = j.value("DbPort", 3306);
        g_db_user = j.value("DbUser", "");
        g_db_pass = j.value("DbPassword", "");
        g_db_name = j.value("DbName", "");
        g_leash_distance = j.value("LeashDistance", 3000.0);
        g_refresh_interval = j.value("RefreshIntervalSeconds", 30);

        if (g_leash_distance < 0.0) g_leash_distance = 0.0;
        if (g_refresh_interval < 1) g_refresh_interval = 1;
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[ShadowBan] Config parse error: {}", ex.what());
        return false;
    }

    if (g_db_user.empty() || g_db_name.empty())
    {
        Log::GetLog()->error("[ShadowBan] Config requires DbUser and DbName");
        return false;
    }

    Log::GetLog()->info("[ShadowBan] Config loaded: leash={} refresh={}s",
        g_leash_distance, g_refresh_interval);
    return true;
}

static int        g_reload_counter = 0;
static DWORD      g_cfg_last_size = 0;
static FILETIME   g_cfg_last_write = {};

static void ReloadConfigTick()
{
    if (++g_reload_counter < 10) return;
    g_reload_counter = 0;

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(CONFIG_PATH, GetFileExInfoStandard, &fad)) return;
    if (fad.nFileSizeLow == 0) return;

    if (fad.nFileSizeLow == g_cfg_last_size &&
        CompareFileTime(&fad.ftLastWriteTime, &g_cfg_last_write) == 0)
        return;

    std::ifstream file(CONFIG_PATH);
    if (!file.is_open()) return;

    try
    {
        nlohmann::json j;
        file >> j;

        double leash = j.value("LeashDistance", g_leash_distance);
        int    refresh = j.value("RefreshIntervalSeconds", g_refresh_interval);
        if (leash < 0.0) leash = 0.0;
        if (refresh < 1) refresh = 1;

        g_leash_distance = leash;
        g_refresh_interval = refresh;

        g_cfg_last_size = fad.nFileSizeLow;
        g_cfg_last_write = fad.ftLastWriteTime;

        Log::GetLog()->info("[ShadowBan] Config reloaded: leash={} refresh={}s",
            g_leash_distance, g_refresh_interval);
    }
    catch (...) {}
}

static std::string FStr(const FString& f)
{
    const char* s = TCHAR_TO_UTF8(*f);
    return (s && s[0]) ? s : "";
}

static std::string GetMapName()
{
    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return "unknown";
    FString map;
    world->GetMapName(&map);
    return FStr(map);
}

static std::string GetEosId(AShooterPlayerController* pc)
{
    if (!pc) return "";
    AShooterPlayerState* ps =
        static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
    if (!ps) return "";
    FString eos;
    ps->GetUniqueNetIdAsString(&eos);
    return FStr(eos);
}

static std::string GetLastToken(const std::string& line)
{
    size_t end = line.find_last_not_of(" \t\r\n");
    if (end == std::string::npos) return "";
    size_t start = line.find_last_of(" \t\r\n", end);
    if (start == std::string::npos) return line.substr(0, end + 1);
    return line.substr(start + 1, end - start);
}

static bool IsValidEos(std::string& eos)
{
    if (eos.size() != 32) return false;
    for (char& c : eos)
    {
        if (!std::isxdigit((unsigned char)c)) return false;
        c = (char)std::tolower((unsigned char)c);
    }
    return true;
}

static bool IsLocationReady(AShooterPlayerController* pc, double& x, double& y, double& z)
{
    APawn* pawn = pc->PawnField().Get();
    if (!pawn) return false;
    USceneComponent* root = pawn->RootComponentField();
    if (!root) return false;

    auto loc = root->RelativeLocationField();
    x = loc.X; y = loc.Y; z = loc.Z;

    if (std::fabs(x) < LOCATION_READY_EPSILON &&
        std::fabs(y) < LOCATION_READY_EPSILON &&
        std::fabs(z) < LOCATION_READY_EPSILON)
        return false;

    return true;
}

static void TeleportPawnTo(AShooterPlayerController* pc, double x, double y, double z)
{
    APawn* pawn = pc->PawnField().Get();
    if (!pawn) return;

    USceneComponent* root = pawn->RootComponentField();
    UE::Math::TRotator<double> rot{ 0.0, 0.0, 0.0 };
    if (root) rot = root->RelativeRotationField();

    UE::Math::TVector<double> dest{ x, y, z };
    static_cast<APrimalCharacter*>(pawn)->TeleportTo(&dest, &rot, false, true);
}

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
        Log::GetLog()->error("[ShadowBan] Query error: {}", pmysql_error(g_mysql));
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
        Log::GetLog()->error("[ShadowBan] mysql_init returned null");
        return false;
    }

    unsigned int timeout = 10;
    if (pmysql_options) pmysql_options(g_mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

    if (!pmysql_real_connect(g_mysql,
        g_db_host.c_str(), g_db_user.c_str(), g_db_pass.c_str(),
        g_db_name.c_str(), g_db_port, nullptr, 0))
    {
        Log::GetLog()->error("[ShadowBan] DB connect failed: {}", pmysql_error(g_mysql));
        pmysql_close(g_mysql);
        g_mysql = nullptr;
        return false;
    }

    const std::string createBans =
        "CREATE TABLE IF NOT EXISTS shadow_bans ("
        "  eos_id VARCHAR(64) NOT NULL,"
        "  PRIMARY KEY (eos_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";

    const std::string createPins =
        "CREATE TABLE IF NOT EXISTS shadow_ban_pins ("
        "  eos_id   VARCHAR(64) NOT NULL,"
        "  map_name VARCHAR(64) NOT NULL,"
        "  x        DOUBLE      NOT NULL,"
        "  y        DOUBLE      NOT NULL,"
        "  z        DOUBLE      NOT NULL,"
        "  PRIMARY KEY (eos_id, map_name)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!ExecQuery(createBans) || !ExecQuery(createPins))
    {
        Log::GetLog()->error("[ShadowBan] Failed to create tables");
        return false;
    }

    Log::GetLog()->info("[ShadowBan] Database ready");
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

static void AddBan(const std::string& eosId)
{
    const std::string eEos = EscapeUnsafe(eosId);

    char sql[256];
    std::snprintf(sql, sizeof(sql),
        "INSERT IGNORE INTO shadow_bans (eos_id) VALUES ('%s')",
        eEos.c_str());

    std::lock_guard<std::mutex> lock(g_db_mutex);
    ExecQuery(sql);
}

static void RemoveBan(const std::string& eosId)
{
    const std::string eEos = EscapeUnsafe(eosId);

    char sql1[256];
    std::snprintf(sql1, sizeof(sql1),
        "DELETE FROM shadow_bans WHERE eos_id='%s'", eEos.c_str());

    char sql2[256];
    std::snprintf(sql2, sizeof(sql2),
        "DELETE FROM shadow_ban_pins WHERE eos_id='%s'", eEos.c_str());

    std::lock_guard<std::mutex> lock(g_db_mutex);
    ExecQuery(sql1);
    ExecQuery(sql2);
}

static void SetPin(const std::string& eosId, const std::string& mapName,
    double x, double y, double z)
{
    const std::string eEos = EscapeUnsafe(eosId);
    const std::string eMap = EscapeUnsafe(mapName);

    char sql[384];
    std::snprintf(sql, sizeof(sql),
        "REPLACE INTO shadow_ban_pins (eos_id, map_name, x, y, z) "
        "VALUES ('%s','%s',%.6f,%.6f,%.6f)",
        eEos.c_str(), eMap.c_str(), x, y, z);

    std::lock_guard<std::mutex> lock(g_db_mutex);
    ExecQuery(sql);
}

static void InsertPinIfAbsent(const std::string& eosId, const std::string& mapName,
    double x, double y, double z)
{
    const std::string eEos = EscapeUnsafe(eosId);
    const std::string eMap = EscapeUnsafe(mapName);

    char sql[384];
    std::snprintf(sql, sizeof(sql),
        "INSERT IGNORE INTO shadow_ban_pins (eos_id, map_name, x, y, z) "
        "VALUES ('%s','%s',%.6f,%.6f,%.6f)",
        eEos.c_str(), eMap.c_str(), x, y, z);

    std::lock_guard<std::mutex> lock(g_db_mutex);
    ExecQuery(sql);
}

static std::unordered_set<std::string> LoadBannedSet()
{
    std::unordered_set<std::string> banned;

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!g_mysql) return banned;

    if (pmysql_query(g_mysql, "SELECT eos_id FROM shadow_bans") != 0)
    {
        Log::GetLog()->error("[ShadowBan] LoadBannedSet error: {}", pmysql_error(g_mysql));
        return banned;
    }

    MYSQL_RES* res = pmysql_store_result(g_mysql);
    if (!res) return banned;

    while (MYSQL_ROW row = pmysql_fetch_row(res))
        if (row[0]) banned.insert(row[0]);

    pmysql_free_result(res);
    return banned;
}

struct PinPos { double x, y, z; };

static std::unordered_map<std::string, PinPos> LoadPinsForMap(const std::string& mapName)
{
    std::unordered_map<std::string, PinPos> pins;

    const std::string eMap = EscapeUnsafe(mapName);

    char sql[256];
    std::snprintf(sql, sizeof(sql),
        "SELECT eos_id, x, y, z FROM shadow_ban_pins WHERE map_name='%s'",
        eMap.c_str());

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!g_mysql) return pins;

    if (pmysql_query(g_mysql, sql) != 0)
    {
        Log::GetLog()->error("[ShadowBan] LoadPinsForMap error: {}", pmysql_error(g_mysql));
        return pins;
    }

    MYSQL_RES* res = pmysql_store_result(g_mysql);
    if (!res) return pins;

    while (MYSQL_ROW row = pmysql_fetch_row(res))
    {
        if (row[0] && row[1] && row[2] && row[3])
        {
            PinPos p;
            p.x = std::atof(row[1]);
            p.y = std::atof(row[2]);
            p.z = std::atof(row[3]);
            pins[row[0]] = p;
        }
    }

    pmysql_free_result(res);
    return pins;
}

static std::unordered_set<std::string>        g_banned_cache;
static std::unordered_map<std::string, PinPos> g_pin_cache;
static std::mutex                              g_cache_mutex;

static void SyncCache()
{
    std::unordered_set<std::string> banned = LoadBannedSet();
    std::unordered_map<std::string, PinPos> pins = LoadPinsForMap(GetMapName());

    std::lock_guard<std::mutex> lock(g_cache_mutex);
    g_banned_cache = std::move(banned);
    g_pin_cache = std::move(pins);
}

static int  g_sync_counter = 0;
static bool g_sync_primed = false;

static void SyncTick()
{
    if (!g_sync_primed)
    {
        g_sync_primed = true;
        g_sync_counter = 0;
        SyncCache();
        return;
    }

    if (++g_sync_counter < g_refresh_interval) return;
    g_sync_counter = 0;
    SyncCache();
}

static void EnforceTick()
{
    std::unordered_set<std::string> bannedLocal;
    std::unordered_map<std::string, PinPos> pinsLocal;

    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        if (g_banned_cache.empty()) return;
        bannedLocal = g_banned_cache;
        pinsLocal = g_pin_cache;
    }

    const std::string mapName = GetMapName();

    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return;

    auto& controllers = world->PlayerControllerListField();
    for (int i = 0; i < controllers.Num(); ++i)
    {
        AShooterPlayerController* pc = static_cast<AShooterPlayerController*>(controllers[i].Get());
        if (!pc) continue;

        std::string eos = GetEosId(pc);
        if (eos.empty()) continue;
        if (bannedLocal.find(eos) == bannedLocal.end()) continue;

        double x, y, z;
        if (!IsLocationReady(pc, x, y, z)) continue;

        auto it = pinsLocal.find(eos);
        if (it == pinsLocal.end())
        {
            InsertPinIfAbsent(eos, mapName, x, y, z);
            {
                std::lock_guard<std::mutex> lock(g_cache_mutex);
                g_pin_cache[eos] = { x, y, z };
            }
            Log::GetLog()->info("[ShadowBan] PINNED eos={} map={} ({:.0f},{:.0f},{:.0f})",
                eos, mapName, x, y, z);
            continue;
        }

        double dx = it->second.x - x;
        double dy = it->second.y - y;
        if (std::sqrt(dx * dx + dy * dy) > g_leash_distance)
        {
            TeleportPawnTo(pc, it->second.x, it->second.y, it->second.z);
            Log::GetLog()->info("[ShadowBan] RETURNED eos={} map={}", eos, mapName);
        }
    }
}

static void ApplyShadowBan(const std::string& eosId)
{
    AddBan(eosId);
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        g_banned_cache.insert(eosId);
    }

    FString fEos(eosId.c_str());
    AShooterPlayerController* pc = AsaApi::GetApiUtils().FindPlayerFromEOSID(fEos);
    if (pc)
    {
        double x, y, z;
        if (IsLocationReady(pc, x, y, z))
        {
            const std::string mapName = GetMapName();
            SetPin(eosId, mapName, x, y, z);
            {
                std::lock_guard<std::mutex> lock(g_cache_mutex);
                g_pin_cache[eosId] = { x, y, z };
            }
        }
    }

    Log::GetLog()->info("[ShadowBan] BANNED eos={}", eosId);
}

static void RemoveShadowBan(const std::string& eosId)
{
    RemoveBan(eosId);
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        g_banned_cache.erase(eosId);
        g_pin_cache.erase(eosId);
    }
    Log::GetLog()->info("[ShadowBan] UNBANNED eos={}", eosId);
}

static void NotifyAdmin(APlayerController* pc, const std::wstring& msg)
{
    if (!pc) return;
    AShooterPlayerController* spc = static_cast<AShooterPlayerController*>(pc);
    FString fSender(L"ShadowBan");
    AsaApi::GetApiUtils().SendChatMessage(spc, fSender, L"{}", std::wstring_view(msg));
}

static void RconReply(RCONClientConnection* conn, RCONPacket* packet, const std::wstring& msg)
{
    if (!conn || !packet) return;
    FString reply(msg.c_str());
    conn->SendMessageW(packet->Id, 0, &reply);
}

static void Cmd_ShadowBan_Console(APlayerController* pc, FString* cmd, bool)
{
    std::string eos = GetLastToken(FStr(*cmd));
    if (!IsValidEos(eos))
    {
        NotifyAdmin(pc, L"Usage: shadowban {eos_id}");
        return;
    }

    ApplyShadowBan(eos);
    std::wstring weos(eos.begin(), eos.end());
    NotifyAdmin(pc, L"Shadowbanned " + weos);
}

static void Cmd_ShadowBan_Rcon(RCONClientConnection* conn, RCONPacket* packet, UWorld*)
{
    std::string eos = GetLastToken(FStr(packet->Body));
    if (!IsValidEos(eos))
    {
        RconReply(conn, packet, L"Usage: shadowban {eos_id}");
        return;
    }

    ApplyShadowBan(eos);
    std::wstring weos(eos.begin(), eos.end());
    RconReply(conn, packet, L"Shadowbanned " + weos);
}

static void Cmd_UnShadowBan_Console(APlayerController* pc, FString* cmd, bool)
{
    std::string eos = GetLastToken(FStr(*cmd));
    if (!IsValidEos(eos))
    {
        NotifyAdmin(pc, L"Usage: unshadowban {eos_id}");
        return;
    }

    RemoveShadowBan(eos);
    std::wstring weos(eos.begin(), eos.end());
    NotifyAdmin(pc, L"Removed shadowban for " + weos);
}

static void Cmd_UnShadowBan_Rcon(RCONClientConnection* conn, RCONPacket* packet, UWorld*)
{
    std::string eos = GetLastToken(FStr(packet->Body));
    if (!IsValidEos(eos))
    {
        RconReply(conn, packet, L"Usage: unshadowban {eos_id}");
        return;
    }

    RemoveShadowBan(eos);
    std::wstring weos(eos.begin(), eos.end());
    RconReply(conn, packet, L"Removed shadowban for " + weos);
}

static void Init_Internal()
{
    if (!LoadConfig())
    {
        Log::GetLog()->error("[ShadowBan] Halted: config error");
        return;
    }

    if (!InitDatabase())
    {
        Log::GetLog()->error("[ShadowBan] Halted: database error");
        return;
    }

    AsaApi::GetCommands().AddConsoleCommand(FString(L"shadowban"), &Cmd_ShadowBan_Console);
    AsaApi::GetCommands().AddRconCommand(FString(L"shadowban"), &Cmd_ShadowBan_Rcon);
    AsaApi::GetCommands().AddConsoleCommand(FString(L"unshadowban"), &Cmd_UnShadowBan_Console);
    AsaApi::GetCommands().AddRconCommand(FString(L"unshadowban"), &Cmd_UnShadowBan_Rcon);

    AsaApi::GetCommands().AddOnTimerCallback(FString(L"ShadowBan_Tick"), &EnforceTick);
    AsaApi::GetCommands().AddOnTimerCallback(FString(L"ShadowBan_Sync"), &SyncTick);
    AsaApi::GetCommands().AddOnTimerCallback(FString(L"ShadowBan_Reload"), &ReloadConfigTick);

    Log::GetLog()->info("[ShadowBan] Plugin loaded");
}

static void Unload_Internal()
{
    AsaApi::GetCommands().RemoveConsoleCommand(FString(L"shadowban"));
    AsaApi::GetCommands().RemoveRconCommand(FString(L"shadowban"));
    AsaApi::GetCommands().RemoveConsoleCommand(FString(L"unshadowban"));
    AsaApi::GetCommands().RemoveRconCommand(FString(L"unshadowban"));

    AsaApi::GetCommands().RemoveOnTimerCallback(FString(L"ShadowBan_Tick"));
    AsaApi::GetCommands().RemoveOnTimerCallback(FString(L"ShadowBan_Sync"));
    AsaApi::GetCommands().RemoveOnTimerCallback(FString(L"ShadowBan_Reload"));

    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        g_banned_cache.clear();
        g_pin_cache.clear();
    }

    CloseDatabase();

    Log::GetLog()->info("[ShadowBan] Plugin unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    Log::Get().Init("ShadowBan");
    try { Init_Internal(); }
    catch (const std::exception& e) { Log::GetLog()->error("[ShadowBan] Init exception: {}", e.what()); }
    catch (...) { Log::GetLog()->error("[ShadowBan] Init unknown exception"); }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { Unload_Internal(); }
    catch (const std::exception& e) { Log::GetLog()->error("[ShadowBan] Unload exception: {}", e.what()); }
    catch (...) { Log::GetLog()->error("[ShadowBan] Unload unknown exception"); }
}