/*
SurvivorStats - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * SurvivorStats - ASA Plugin
 *
 * Tables:
 *   survivor_stats — PK (eos_id, survivor_id), tracks per-character stats
 *   tribe_stats    — reserved, no columns yet
 *
 * Hooks:
 *   AShooterGameMode.StartNewShooterPlayer(...)                           — resolves survivor_id
 *   AShooterPlayerController.HandleRespawned_Implementation(APawn*,bool)  — seeds level on spawn
 *   AShooterPlayerController.ClientNotifyLevelUp(APrimalCharacter*,int)   — updates level
 *   AShooterCharacter.Die(float,FDamageEvent&,AController*,AActor*)       — player kills/deaths
 *   APrimalDinoCharacter.Die(float,FDamageEvent&,AController*,AActor*)    — dino kills
 */

#include <API/ARK/Ark.h>
#include <json.hpp>
#include <Windows.h>
#include <fstream>
#include <string>
#include <mutex>
#include <unordered_map>
#include <cstdio>

 // =============================================================================
 // MariaDB — Dynamic Load
 // =============================================================================

typedef struct st_mysql     MYSQL;
typedef struct st_mysql_res MYSQL_RES;
typedef char** MYSQL_ROW;

typedef MYSQL* (__stdcall* mysql_init_t)             (MYSQL*);
typedef MYSQL* (__stdcall* mysql_real_connect_t)     (MYSQL*, const char*, const char*, const char*, const char*, unsigned int, const char*, unsigned long);
typedef void(__stdcall* mysql_close_t)            (MYSQL*);
typedef int(__stdcall* mysql_query_t)            (MYSQL*, const char*);
typedef MYSQL_RES* (__stdcall* mysql_store_result_t)     (MYSQL*);
typedef void(__stdcall* mysql_free_result_t)      (MYSQL_RES*);
typedef const char* (__stdcall* mysql_error_t)            (MYSQL*);
typedef unsigned long(__stdcall* mysql_real_escape_string_t)(MYSQL*, char*, const char*, unsigned long);
typedef int(__stdcall* mysql_options_t)          (MYSQL*, int, const void*);

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
            Log::GetLog()->info("[SurvivorStats] Loaded DB library: {}", candidates[i]);
            break;
        }
    }

    if (!g_mysql_module)
    {
        Log::GetLog()->error("[SurvivorStats] Could not find libmariadb.dll or libmysql.dll");
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

    if (!pmysql_init || !pmysql_real_connect || !pmysql_close ||
        !pmysql_query || !pmysql_error || !pmysql_real_escape_string)
    {
        Log::GetLog()->error("[SurvivorStats] Failed to resolve required DB functions");
        return false;
    }

    g_mysql_loaded = true;
    return true;
}

// =============================================================================
// Configuration
// =============================================================================

static std::string  g_db_host = "localhost";
static unsigned int g_db_port = 3306;
static std::string  g_db_user;
static std::string  g_db_pass;
static std::string  g_db_name;

static bool LoadConfig()
{
    const std::string path = "ArkApi/Plugins/SurvivorStats/config.json";
    std::ifstream file(path);
    if (!file.is_open())
    {
        Log::GetLog()->error("[SurvivorStats] Cannot open config: {}", path);
        return false;
    }

    try
    {
        nlohmann::json j;
        file >> j;
        g_db_host = j.value("DbHost", "localhost");
        g_db_port = j.value("DbPort", 3306);
        g_db_user = j.value("DbUser", "");
        g_db_pass = j.value("DbPassword", "");
        g_db_name = j.value("DbName", "");
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[SurvivorStats] Config parse error: {}", ex.what());
        return false;
    }

    if (g_db_user.empty() || g_db_name.empty())
    {
        Log::GetLog()->error("[SurvivorStats] Config requires DbUser and DbName");
        return false;
    }

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
        Log::GetLog()->error("[SurvivorStats] Query error: {}", pmysql_error(g_mysql));
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
        Log::GetLog()->error("[SurvivorStats] mysql_init returned null");
        return false;
    }

    unsigned int timeout = 10;
    pmysql_options(g_mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

    if (!pmysql_real_connect(g_mysql,
        g_db_host.c_str(), g_db_user.c_str(), g_db_pass.c_str(),
        g_db_name.c_str(), g_db_port, nullptr, 0))
    {
        Log::GetLog()->error("[SurvivorStats] DB connect failed: {}", pmysql_error(g_mysql));
        pmysql_close(g_mysql);
        g_mysql = nullptr;
        return false;
    }

    const char* survivor_stats_ddl =
        "CREATE TABLE IF NOT EXISTS survivor_stats ("
        "  eos_id              VARCHAR(128)    NOT NULL,"
        "  survivor_id         BIGINT UNSIGNED NOT NULL DEFAULT 0,"
        "  survivor_level      INT UNSIGNED    NOT NULL DEFAULT 1,"
        "  survivor_kills      INT UNSIGNED    NOT NULL DEFAULT 0,"
        "  deaths_by_survivor  INT UNSIGNED    NOT NULL DEFAULT 0,"
        "  dino_kills          INT UNSIGNED    NOT NULL DEFAULT 0,"
        "  deaths_by_dino      INT UNSIGNED    NOT NULL DEFAULT 0,"
        "  PRIMARY KEY (eos_id, survivor_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;";

    const char* tribe_stats_ddl =
        "CREATE TABLE IF NOT EXISTS tribe_stats ("
        "  tribe_id INT NOT NULL,"
        "  PRIMARY KEY (tribe_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;";

    if (!ExecQuery(survivor_stats_ddl) || !ExecQuery(tribe_stats_ddl))
    {
        Log::GetLog()->error("[SurvivorStats] Failed to create tables");
        return false;
    }

    Log::GetLog()->info("[SurvivorStats] Database ready");
    return true;
}

static void CloseDatabase()
{
    if (g_mysql) { pmysql_close(g_mysql); g_mysql = nullptr; }
}

static void UpsertSurvivorLevel(const std::string& eosId, uint64_t survivorId, int level)
{
    if (survivorId == 0) return;

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!g_mysql) return;

    const std::string e_eos = EscapeUnsafe(eosId);

    char sql[256]{};
    std::snprintf(sql, sizeof(sql),
        "INSERT INTO survivor_stats (eos_id, survivor_id, survivor_level) "
        "VALUES ('%s', %llu, %d) "
        "ON DUPLICATE KEY UPDATE survivor_level = VALUES(survivor_level);",
        e_eos.c_str(), (unsigned long long)survivorId, level);

    ExecQuery(sql);
}

static void IncrementCounter(const std::string& eosId, uint64_t survivorId, const char* column)
{
    if (survivorId == 0) return;

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!g_mysql) return;

    const std::string e_eos = EscapeUnsafe(eosId);

    char sql[512]{};
    std::snprintf(sql, sizeof(sql),
        "INSERT INTO survivor_stats (eos_id, survivor_id, %s) "
        "VALUES ('%s', %llu, 1) "
        "ON DUPLICATE KEY UPDATE %s = %s + 1;",
        column,
        e_eos.c_str(), (unsigned long long)survivorId,
        column, column);

    ExecQuery(sql);
}

// =============================================================================
// Cache
// =============================================================================

struct StatsRecord
{
    uint64_t survivorId = 0;
};

static std::unordered_map<std::string, StatsRecord> g_cache;
static std::mutex                                    g_cache_mutex;

static uint64_t GetCachedSurvivorId(const std::string& eosId)
{
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    auto it = g_cache.find(eosId);
    return it != g_cache.end() ? it->second.survivorId : 0;
}

// =============================================================================
// Helpers
// =============================================================================

static std::string FStr(const FString& f)
{
    const char* s = TCHAR_TO_UTF8(*f);
    return (s && s[0]) ? s : "unknown";
}

static std::string GetEosIdFromController(AController* controller)
{
    if (!controller) return "";
    AShooterPlayerState* ps =
        static_cast<AShooterPlayerState*>(controller->PlayerStateField().Get());
    if (!ps) return "";
    FString eosRaw;
    ps->GetUniqueNetIdAsString(&eosRaw);
    const std::string eosId = FStr(eosRaw);
    return (eosId == "unknown") ? "" : eosId;
}

// =============================================================================
// Hook Type Aliases
// =============================================================================

using StartNewShooterPlayer_t = void(*)(AShooterGameMode*, APlayerController*, bool, bool, FPrimalPlayerCharacterConfigStruct&, UPrimalPlayerData*, bool);
using HandleRespawned_t = void(*)(AShooterPlayerController*, APawn*, bool);
using ClientNotifyLevelUp_t = void(*)(AShooterPlayerController*, APrimalCharacter*, int);
using ShooterCharacterDie_t = void(*)(AShooterCharacter*, float, FDamageEvent&, AController*, AActor*);
using DinoDie_t = bool(*)(APrimalDinoCharacter*, float, FDamageEvent&, AController*, AActor*);

static StartNewShooterPlayer_t Original_StartNewShooterPlayer = nullptr;
static HandleRespawned_t       Original_HandleRespawned = nullptr;
static ClientNotifyLevelUp_t   Original_ClientNotifyLevelUp = nullptr;
static ShooterCharacterDie_t   Original_ShooterCharacterDie = nullptr;
static DinoDie_t               Original_DinoDie = nullptr;

// =============================================================================
// Detours
// =============================================================================

void Detour_StartNewShooterPlayer(AShooterGameMode* gm,
    APlayerController* pc,
    bool                                b1,
    bool                                b2,
    FPrimalPlayerCharacterConfigStruct& cfg,
    UPrimalPlayerData* playerData,
    bool                                b3)
{
    Original_StartNewShooterPlayer(gm, pc, b1, b2, cfg, playerData, b3);

    if (!pc) return;

    AShooterPlayerState* ps =
        static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
    if (!ps) return;

    FString eosRaw;
    ps->GetUniqueNetIdAsString(&eosRaw);
    const std::string eosId = FStr(eosRaw);
    if (eosId.empty() || eosId == "unknown") return;

    APawn* pawn = pc->PawnField().Get();
    if (!pawn) return;

    AShooterCharacter* ch = static_cast<AShooterCharacter*>(pawn);
    if (!ch) return;

    const uint64_t newSurvivorId = ch->GetLinkedPlayerDataID();
    if (newSurvivorId == 0) return;

    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        g_cache[eosId].survivorId = newSurvivorId;
    }

    const int level = ps->GetCharacterLevel();
    UpsertSurvivorLevel(eosId, newSurvivorId, level > 0 ? level : 1);

    Log::GetLog()->info(
        "[SurvivorStats] SURVIVOR_ID_RESOLVED eos_id={} survivor_id={} survivor_level={}",
        eosId, newSurvivorId, level > 0 ? level : 1);
}

void Detour_HandleRespawned(AShooterPlayerController* pc, APawn* pawn, bool newPlayer)
{
    Original_HandleRespawned(pc, pawn, newPlayer);

    if (!pc) return;

    AShooterPlayerState* ps =
        static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
    if (!ps) return;

    FString eosRaw;
    ps->GetUniqueNetIdAsString(&eosRaw);
    const std::string eosId = FStr(eosRaw);
    if (eosId.empty() || eosId == "unknown") return;

    APawn* p = pc->PawnField().Get();
    if (!p) return;

    AShooterCharacter* ch = static_cast<AShooterCharacter*>(p);
    if (!ch) return;

    const uint64_t survivorId = ch->GetLinkedPlayerDataID();
    if (survivorId == 0) return;

    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        g_cache[eosId].survivorId = survivorId;
    }

    const int level = ps->GetCharacterLevel();
    if (level <= 0) return;

    UpsertSurvivorLevel(eosId, survivorId, level);

    Log::GetLog()->info(
        "[SurvivorStats] SPAWN eos_id={} survivor_id={} survivor_level={}",
        eosId, survivorId, level);
}

void Detour_ClientNotifyLevelUp(AShooterPlayerController* pc,
    APrimalCharacter* forChar,
    int                       newLevel)
{
    Original_ClientNotifyLevelUp(pc, forChar, newLevel);

    if (!pc || newLevel <= 0) return;

    AShooterPlayerState* ps =
        static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
    if (!ps) return;

    FString eosRaw;
    ps->GetUniqueNetIdAsString(&eosRaw);
    const std::string eosId = FStr(eosRaw);
    if (eosId.empty() || eosId == "unknown") return;

    const uint64_t survivorId = GetCachedSurvivorId(eosId);
    UpsertSurvivorLevel(eosId, survivorId, newLevel + 1);

    Log::GetLog()->info(
        "[SurvivorStats] LEVEL_UP eos_id={} survivor_id={} survivor_level={}",
        eosId, survivorId, newLevel + 1);
}

void Detour_ShooterCharacterDie(AShooterCharacter* victim,
    float              damage,
    FDamageEvent& damageEvent,
    AController* killer,
    AActor* damageCauser)
{
    std::string victimEosId;
    uint64_t    victimSurvivorId = 0;

    if (victim)
    {
        AShooterPlayerState* victimPs =
            static_cast<AShooterPlayerState*>(victim->PlayerStateField().Get());
        if (victimPs)
        {
            FString victimEosRaw;
            victimPs->GetUniqueNetIdAsString(&victimEosRaw);
            victimEosId = FStr(victimEosRaw);
            if (victimEosId == "unknown") victimEosId = "";
            victimSurvivorId = GetCachedSurvivorId(victimEosId);
        }
    }

    const bool killerIsPlayer = killer && killer->IsA(AShooterPlayerController::StaticClass());
    const bool killerIsDino = !killerIsPlayer && killer &&
        killer->IsA(APrimalDinoAIController::StaticClass());

    std::string killerEosId;
    uint64_t    killerSurvivorId = 0;
    if (killerIsPlayer)
    {
        killerEosId = GetEosIdFromController(killer);
        killerSurvivorId = GetCachedSurvivorId(killerEosId);
    }

    Original_ShooterCharacterDie(victim, damage, damageEvent, killer, damageCauser);

    if (victimEosId.empty() || victimSurvivorId == 0) return;

    if (killerIsPlayer && !killerEosId.empty() && killerSurvivorId != 0)
    {
        IncrementCounter(killerEosId, killerSurvivorId, "survivor_kills");
        IncrementCounter(victimEosId, victimSurvivorId, "deaths_by_survivor");
    }
    else if (killerIsDino)
    {
        IncrementCounter(victimEosId, victimSurvivorId, "deaths_by_dino");
    }
}

bool Detour_DinoDie(APrimalDinoCharacter* victim,
    float                 damage,
    FDamageEvent& damageEvent,
    AController* killer,
    AActor* damageCauser)
{
    bool result = Original_DinoDie(victim, damage, damageEvent, killer, damageCauser);

    if (!result || !victim) return result;

    const bool killerIsPlayer = killer && killer->IsA(AShooterPlayerController::StaticClass());
    if (!killerIsPlayer) return result;

    const std::string killerEosId = GetEosIdFromController(killer);
    if (killerEosId.empty()) return result;

    const uint64_t killerSurvivorId = GetCachedSurvivorId(killerEosId);
    IncrementCounter(killerEosId, killerSurvivorId, "dino_kills");

    return result;
}

// =============================================================================
// Plugin Entry Points
// =============================================================================

extern "C" __declspec(dllexport) void Plugin_Init()
{
    Log::Get().Init("SurvivorStats");

    if (!LoadConfig())
    {
        Log::GetLog()->error("[SurvivorStats] Halted - config error");
        return;
    }

    if (!InitDatabase())
    {
        Log::GetLog()->error("[SurvivorStats] Halted - database error");
        return;
    }

    AsaApi::GetHooks().SetHook(
        "AShooterGameMode.StartNewShooterPlayer(APlayerController*,bool,bool,FPrimalPlayerCharacterConfigStruct&,UPrimalPlayerData*,bool)",
        (LPVOID)&Detour_StartNewShooterPlayer,
        (LPVOID*)&Original_StartNewShooterPlayer
    );

    AsaApi::GetHooks().SetHook(
        "AShooterPlayerController.HandleRespawned_Implementation(APawn*,bool)",
        (LPVOID)&Detour_HandleRespawned,
        (LPVOID*)&Original_HandleRespawned
    );

    AsaApi::GetHooks().SetHook(
        "AShooterPlayerController.ClientNotifyLevelUp(APrimalCharacter*,int)",
        (LPVOID)&Detour_ClientNotifyLevelUp,
        (LPVOID*)&Original_ClientNotifyLevelUp
    );

    AsaApi::GetHooks().SetHook(
        "AShooterCharacter.Die(float,FDamageEvent&,AController*,AActor*)",
        (LPVOID)&Detour_ShooterCharacterDie,
        (LPVOID*)&Original_ShooterCharacterDie
    );

    AsaApi::GetHooks().SetHook(
        "APrimalDinoCharacter.Die(float,FDamageEvent&,AController*,AActor*)",
        (LPVOID)&Detour_DinoDie,
        (LPVOID*)&Original_DinoDie
    );

    Log::GetLog()->info("[SurvivorStats] Plugin loaded");
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    AsaApi::GetHooks().DisableHook(
        "AShooterGameMode.StartNewShooterPlayer(APlayerController*,bool,bool,FPrimalPlayerCharacterConfigStruct&,UPrimalPlayerData*,bool)",
        (LPVOID)&Detour_StartNewShooterPlayer
    );

    AsaApi::GetHooks().DisableHook(
        "AShooterPlayerController.HandleRespawned_Implementation(APawn*,bool)",
        (LPVOID)&Detour_HandleRespawned
    );

    AsaApi::GetHooks().DisableHook(
        "AShooterPlayerController.ClientNotifyLevelUp(APrimalCharacter*,int)",
        (LPVOID)&Detour_ClientNotifyLevelUp
    );

    AsaApi::GetHooks().DisableHook(
        "AShooterCharacter.Die(float,FDamageEvent&,AController*,AActor*)",
        (LPVOID)&Detour_ShooterCharacterDie
    );

    AsaApi::GetHooks().DisableHook(
        "APrimalDinoCharacter.Die(float,FDamageEvent&,AController*,AActor*)",
        (LPVOID)&Detour_DinoDie
    );

    CloseDatabase();

    Log::GetLog()->info("[SurvivorStats] Plugin unloaded");
}