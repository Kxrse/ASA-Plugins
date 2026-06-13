/*
YutyAutoRoar - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * YutyAutoRoar
 *
 * Hooks:
 *   AShooterGameMode.Tick(float)                                      - config hot-reload + roar interval pass
 *   APrimalDinoCharacter.Die(float,FDamageEvent&,AController*,AActor*) - clears a tracked Yuty on death
 *
 * Config:
 *   roar_interval_seconds - seconds between roar passes
 *   fear_attack_index     - Yuty attack index for the fear roar (-1 = unset)
 *   courage_attack_index  - Yuty attack index for the courage roar (-1 = unset)
 *   yuty_bp_keyword       - lowercased blueprint substring used to match Yutys
 *   max_aim_range         - max distance to the aimed Yuty in UE units
 *   aim_dot_threshold     - how tightly the Yuty must be centered in view
 *   refill_stamina        - refill the Yuty to full stamina before each roar
 *   Database              - Host / Port / User / Password / Name
 *
 * Table:
 *   yuty_auto_roar - PK (dino_id)
 *   Columns: dino_id BIGINT UNSIGNED, roar_type TINYINT (fear=1, courage=2)
 *
 * /yar fear    - aimed tribe Yuty fear roars on the interval until stopped
 * /yar courage - aimed tribe Yuty courage roars on the interval until stopped
 * /yar off     - stops the aimed tribe Yuty
 */

#include <API/ARK/Ark.h>
#include <json.hpp>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <sys/stat.h>

#pragma warning(disable: 4191)
#pragma comment(lib, "AsaApi")

static constexpr double kPi = 3.14159265358979323846;

static const std::string g_config_path = "ArkApi/Plugins/YutyAutoRoar/config.json";

static float       g_roar_interval     = 10.0f;
static int         g_fear_index        = -1;
static int         g_courage_index     = -1;
static std::string g_yuty_keyword      = "yutyrannus";
static float       g_max_aim_range     = 5000.0f;
static double      g_aim_dot_threshold = 0.98;
static bool        g_refill_stamina    = true;

static std::string  g_db_host = "127.0.0.1";
static unsigned int g_db_port = 3306;
static std::string  g_db_user;
static std::string  g_db_pass;
static std::string  g_db_name;

static time_t    g_config_mtime = 0;
static uintmax_t g_config_size  = 0;

enum class RoarType { Fear, Courage };

static std::unordered_map<uint64_t, RoarType> g_active;
static std::mutex                             g_state_mutex;

static void(*Original_Tick)(AShooterGameMode*, float) = nullptr;

using DinoDie_t = bool(*)(APrimalDinoCharacter*, float, FDamageEvent&, AController*, AActor*);
static DinoDie_t Original_DinoDie = nullptr;

static float g_config_accum = 0.0f;
static float g_roar_accum   = 0.0f;

typedef struct st_mysql     MYSQL;
typedef struct st_mysql_res MYSQL_RES;
typedef char** MYSQL_ROW;

typedef MYSQL* (__stdcall* mysql_init_t)             (MYSQL*);
typedef MYSQL* (__stdcall* mysql_real_connect_t)     (MYSQL*, const char*, const char*, const char*, const char*, unsigned int, const char*, unsigned long);
typedef void   (__stdcall* mysql_close_t)            (MYSQL*);
typedef int    (__stdcall* mysql_query_t)            (MYSQL*, const char*);
typedef MYSQL_RES* (__stdcall* mysql_store_result_t) (MYSQL*);
typedef void   (__stdcall* mysql_free_result_t)      (MYSQL_RES*);
typedef const char* (__stdcall* mysql_error_t)       (MYSQL*);
typedef int    (__stdcall* mysql_options_t)          (MYSQL*, int, const void*);
typedef MYSQL_ROW (__stdcall* mysql_fetch_row_t)     (MYSQL_RES*);

#define MYSQL_OPT_CONNECT_TIMEOUT 0

static HMODULE               g_mysql_module = nullptr;
static mysql_init_t          pmysql_init = nullptr;
static mysql_real_connect_t  pmysql_real_connect = nullptr;
static mysql_close_t         pmysql_close = nullptr;
static mysql_query_t         pmysql_query = nullptr;
static mysql_store_result_t  pmysql_store_result = nullptr;
static mysql_free_result_t   pmysql_free_result = nullptr;
static mysql_error_t         pmysql_error = nullptr;
static mysql_options_t       pmysql_options = nullptr;
static mysql_fetch_row_t     pmysql_fetch_row = nullptr;
static bool                  g_mysql_loaded = false;

#pragma warning(push)
#pragma warning(disable: 4191)

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
            Log::GetLog()->info("[YutyAutoRoar] Loaded DB library: {}", candidates[i]);
            break;
        }
    }

    if (!g_mysql_module)
    {
        Log::GetLog()->error("[YutyAutoRoar] Could not find libmariadb.dll or libmysql.dll");
        return false;
    }

    pmysql_init         = (mysql_init_t)GetProcAddress(g_mysql_module, "mysql_init");
    pmysql_real_connect = (mysql_real_connect_t)GetProcAddress(g_mysql_module, "mysql_real_connect");
    pmysql_close        = (mysql_close_t)GetProcAddress(g_mysql_module, "mysql_close");
    pmysql_query        = (mysql_query_t)GetProcAddress(g_mysql_module, "mysql_query");
    pmysql_store_result = (mysql_store_result_t)GetProcAddress(g_mysql_module, "mysql_store_result");
    pmysql_free_result  = (mysql_free_result_t)GetProcAddress(g_mysql_module, "mysql_free_result");
    pmysql_error        = (mysql_error_t)GetProcAddress(g_mysql_module, "mysql_error");
    pmysql_options      = (mysql_options_t)GetProcAddress(g_mysql_module, "mysql_options");
    pmysql_fetch_row    = (mysql_fetch_row_t)GetProcAddress(g_mysql_module, "mysql_fetch_row");

    if (!pmysql_init || !pmysql_real_connect || !pmysql_close ||
        !pmysql_query || !pmysql_error || !pmysql_fetch_row)
    {
        Log::GetLog()->error("[YutyAutoRoar] Failed to resolve required DB functions");
        return false;
    }

    g_mysql_loaded = true;
    return true;
}

#pragma warning(pop)

static MYSQL*     g_mysql = nullptr;
static std::mutex g_db_mutex;

static time_t GetFileMTime(const std::string& path)
{
    struct _stat st {};
    if (_stat(path.c_str(), &st) == 0) return st.st_mtime;
    return 0;
}

static uintmax_t GetFileSize(const std::string& path)
{
    struct _stat st {};
    if (_stat(path.c_str(), &st) == 0) return static_cast<uintmax_t>(st.st_size);
    return 0;
}

static std::string FStr(const FString& f)
{
    const char* s = TCHAR_TO_UTF8(*f);
    return (s && s[0]) ? s : "";
}

static int RoarTypeToInt(RoarType t)
{
    return (t == RoarType::Fear) ? 1 : 2;
}

static bool LoadConfig()
{
    std::ifstream file(g_config_path);
    if (!file.is_open())
    {
        Log::GetLog()->error("[YutyAutoRoar] Cannot open config: {}", g_config_path);
        return false;
    }

    try
    {
        nlohmann::json j;
        file >> j;

        g_roar_interval     = j.value("roar_interval_seconds", 10.0f);
        g_fear_index        = j.value("fear_attack_index", -1);
        g_courage_index     = j.value("courage_attack_index", -1);
        g_yuty_keyword      = j.value("yuty_bp_keyword", std::string("yutyrannus"));
        g_max_aim_range     = j.value("max_aim_range", 5000.0f);
        g_aim_dot_threshold = j.value("aim_dot_threshold", 0.98);
        g_refill_stamina    = j.value("refill_stamina", true);

        if (j.contains("Database") && j["Database"].is_object())
        {
            auto& db  = j["Database"];
            g_db_host = db.value("Host", std::string("127.0.0.1"));
            g_db_port = db.value("Port", 3306u);
            g_db_user = db.value("User", std::string(""));
            g_db_pass = db.value("Password", std::string(""));
            g_db_name = db.value("Name", std::string(""));
        }

        if (g_roar_interval < 1.0f) g_roar_interval = 1.0f;
        std::transform(g_yuty_keyword.begin(), g_yuty_keyword.end(),
            g_yuty_keyword.begin(), ::tolower);
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[YutyAutoRoar] Config parse error: {}", ex.what());
        return false;
    }

    g_config_size  = GetFileSize(g_config_path);
    g_config_mtime = GetFileMTime(g_config_path);

    Log::GetLog()->info("[YutyAutoRoar] Config loaded (interval={}s, fear={}, courage={}, keyword={}, refillStamina={})",
        g_roar_interval, g_fear_index, g_courage_index, g_yuty_keyword, g_refill_stamina);
    return true;
}

static bool ExecQuery(const std::string& sql)
{
    if (!g_mysql) return false;
    if (pmysql_query(g_mysql, sql.c_str()) != 0)
    {
        Log::GetLog()->error("[YutyAutoRoar] Query error: {}", pmysql_error(g_mysql));
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
        Log::GetLog()->error("[YutyAutoRoar] mysql_init returned null");
        return false;
    }

    unsigned int timeout = 10;
    if (pmysql_options) pmysql_options(g_mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

    if (!pmysql_real_connect(g_mysql,
        g_db_host.c_str(), g_db_user.c_str(), g_db_pass.c_str(),
        g_db_name.c_str(), g_db_port, nullptr, 0))
    {
        Log::GetLog()->error("[YutyAutoRoar] DB connect failed: {}", pmysql_error(g_mysql));
        pmysql_close(g_mysql);
        g_mysql = nullptr;
        return false;
    }

    const std::string create =
        "CREATE TABLE IF NOT EXISTS yuty_auto_roar ("
        "  dino_id   BIGINT UNSIGNED NOT NULL,"
        "  roar_type TINYINT         NOT NULL,"
        "  PRIMARY KEY (dino_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!ExecQuery(create))
    {
        Log::GetLog()->error("[YutyAutoRoar] Failed to create table");
        return false;
    }

    Log::GetLog()->info("[YutyAutoRoar] Database ready");
    return true;
}

static void CloseDatabase()
{
    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (g_mysql) { pmysql_close(g_mysql); g_mysql = nullptr; }
}

static void DBLoadAll()
{
    std::vector<std::pair<uint64_t, RoarType>> rows;
    {
        std::lock_guard<std::mutex> lock(g_db_mutex);
        if (!g_mysql) return;

        if (pmysql_query(g_mysql, "SELECT dino_id, roar_type FROM yuty_auto_roar") != 0)
        {
            Log::GetLog()->error("[YutyAutoRoar] Load query failed: {}", pmysql_error(g_mysql));
            return;
        }

        MYSQL_RES* res = pmysql_store_result(g_mysql);
        if (!res) return;

        MYSQL_ROW row;
        while ((row = pmysql_fetch_row(res)) != nullptr)
        {
            if (!row[0] || !row[1]) continue;
            uint64_t id = std::strtoull(row[0], nullptr, 10);
            int rt = std::atoi(row[1]);
            rows.emplace_back(id, rt == 1 ? RoarType::Fear : RoarType::Courage);
        }

        pmysql_free_result(res);
    }

    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        for (auto& r : rows) g_active[r.first] = r.second;
    }

    Log::GetLog()->info("[YutyAutoRoar] Loaded {} active Yutys from DB", rows.size());
}

static void DBUpsert(uint64_t id, RoarType type)
{
    char sql[160];
    std::snprintf(sql, sizeof(sql),
        "INSERT INTO yuty_auto_roar (dino_id, roar_type) VALUES (%llu, %d) "
        "ON DUPLICATE KEY UPDATE roar_type = VALUES(roar_type)",
        static_cast<unsigned long long>(id), RoarTypeToInt(type));

    std::lock_guard<std::mutex> lock(g_db_mutex);
    ExecQuery(sql);
}

static void DBDelete(uint64_t id)
{
    char sql[96];
    std::snprintf(sql, sizeof(sql),
        "DELETE FROM yuty_auto_roar WHERE dino_id = %llu",
        static_cast<unsigned long long>(id));

    std::lock_guard<std::mutex> lock(g_db_mutex);
    ExecQuery(sql);
}

static void Notify(AShooterPlayerController* pc, const std::wstring& msg,
    const std::wstring& color = L"0.2,1.0,0.2,1.0")
{
    const std::wstring rich =
        L"<RichColor Color=\"" + color + L"\">" + msg + L"</>";
    FString fSender(L"");
    FString fMsg(rich.c_str());
    AsaApi::GetApiUtils().SendChatMessage(pc, fSender, L"{}", std::wstring_view(*fMsg));
}

static int GetPlayerTeam(AShooterPlayerController* pc)
{
    if (!pc) return 0;
    AActor* baseChar = pc->BaseGetPlayerCharacter();
    AShooterCharacter* ch = baseChar ? static_cast<AShooterCharacter*>(baseChar) : nullptr;
    if (!ch) return 0;
    return ch->TargetingTeamField();
}

static uint64_t DinoId64(APrimalDinoCharacter* dino)
{
    uint32_t a = static_cast<uint32_t>(dino->DinoID1Field());
    uint32_t b = static_cast<uint32_t>(dino->DinoID2Field());
    return (static_cast<uint64_t>(a) << 32) | static_cast<uint64_t>(b);
}

static bool IsYuty(APrimalDinoCharacter* dino)
{
    std::string bp = FStr(AsaApi::GetApiUtils().GetBlueprint(dino));
    std::transform(bp.begin(), bp.end(), bp.begin(), ::tolower);
    return bp.find(g_yuty_keyword) != std::string::npos;
}

static APrimalDinoCharacter* GetAimedYuty(AShooterPlayerController* pc, int playerTeam)
{
    AActor* ch = pc->BaseGetPlayerCharacter();
    if (!ch) return nullptr;

    USceneComponent* root = ch->RootComponentField();
    if (!root) return nullptr;

    auto playerLoc = root->RelativeLocationField();

    auto rot = pc->ControlRotationField();
    double pitchRad = rot.Pitch * (kPi / 180.0);
    double yawRad   = rot.Yaw   * (kPi / 180.0);

    double fwdX = cos(pitchRad) * cos(yawRad);
    double fwdY = cos(pitchRad) * sin(yawRad);
    double fwdZ = sin(pitchRad);

    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return nullptr;

    ULevel* level = world->PersistentLevelField();
    if (!level) return nullptr;

    auto actors = level->ActorsField();

    APrimalDinoCharacter* best = nullptr;
    double bestDot = g_aim_dot_threshold;

    for (int i = 0; i < actors.Num(); ++i)
    {
        AActor* actor = actors[i];
        if (!actor) continue;
        if (!actor->IsA(APrimalDinoCharacter::StaticClass())) continue;

        APrimalDinoCharacter* dino = static_cast<APrimalDinoCharacter*>(actor);
        if (dino->TargetingTeamField() != playerTeam) continue;
        if (!IsYuty(dino)) continue;

        USceneComponent* dRoot = dino->RootComponentField();
        if (!dRoot) continue;

        auto dLoc = dRoot->RelativeLocationField();
        double dx = dLoc.X - playerLoc.X;
        double dy = dLoc.Y - playerLoc.Y;
        double dz = dLoc.Z - playerLoc.Z;

        double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist < 1.0 || dist > g_max_aim_range) continue;

        double nx = dx / dist;
        double ny = dy / dist;
        double nz = dz / dist;

        double dot = nx * fwdX + ny * fwdY + nz * fwdZ;
        if (dot > bestDot)
        {
            bestDot = dot;
            best = dino;
        }
    }

    return best;
}

static void DoRoarPass()
{
    std::unordered_map<uint64_t, RoarType> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        if (g_active.empty()) return;
        snapshot = g_active;
    }

    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return;

    ULevel* level = world->PersistentLevelField();
    if (!level) return;

    auto actors = level->ActorsField();

    for (int i = 0; i < actors.Num(); ++i)
    {
        AActor* actor = actors[i];
        if (!actor) continue;
        if (!actor->IsA(APrimalDinoCharacter::StaticClass())) continue;

        APrimalDinoCharacter* dino = static_cast<APrimalDinoCharacter*>(actor);
        uint64_t id = DinoId64(dino);

        auto it = snapshot.find(id);
        if (it == snapshot.end()) continue;

        auto rider = dino->RiderField();
        if (rider) continue;

        int index = (it->second == RoarType::Fear) ? g_fear_index : g_courage_index;
        if (index < 0) continue;

        if (g_refill_stamina)
        {
            UPrimalCharacterStatusComponent* status = dino->MyCharacterStatusComponentField();
            if (status)
            {
                float maxStam = status->BPGetMaxStatusValue(EPrimalCharacterStatusValue::Stamina);
                status->BPDirectSetCurrentStatusValue(EPrimalCharacterStatusValue::Stamina, maxStam);
            }
        }

        dino->DoAttack(index, true, true);
    }
}

static bool Detour_DinoDie(APrimalDinoCharacter* victim, float damage,
    FDamageEvent& damageEvent, AController* killer, AActor* damageCauser)
{
    if (victim)
    {
        uint64_t id = DinoId64(victim);

        bool wasActive;
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            wasActive = g_active.erase(id) > 0;
        }

        if (wasActive) DBDelete(id);
    }

    return Original_DinoDie(victim, damage, damageEvent, killer, damageCauser);
}

static void Cmd_Yar(AShooterPlayerController* pc, FString* message, int, int)
{
    if (!pc || !message) return;

    std::string raw = FStr(*message);
    std::string arg;
    size_t sp = raw.find(' ');
    if (sp != std::string::npos) arg = raw.substr(sp + 1);

    size_t b = arg.find_first_not_of(" \t");
    size_t e = arg.find_last_not_of(" \t");
    arg = (b == std::string::npos) ? "" : arg.substr(b, e - b + 1);
    std::transform(arg.begin(), arg.end(), arg.begin(), ::tolower);

    if (arg != "fear" && arg != "courage" && arg != "off")
    {
        Notify(pc, L"Usage: /yar fear | courage | off", L"1.0,0.5,0.2,1.0");
        return;
    }

    int team = GetPlayerTeam(pc);
    if (team <= 0)
    {
        Notify(pc, L"No tribe team found.", L"1.0,0.2,0.2,1.0");
        return;
    }

    APrimalDinoCharacter* yuty = GetAimedYuty(pc, team);
    if (!yuty)
    {
        Notify(pc, L"No tribe Yuty in view.", L"1.0,0.2,0.2,1.0");
        return;
    }

    uint64_t id = DinoId64(yuty);

    if (arg == "off")
    {
        bool wasActive;
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            wasActive = g_active.erase(id) > 0;
        }
        if (wasActive)
        {
            DBDelete(id);
            Notify(pc, L"Auto roar stopped on aimed Yuty.", L"1.0,0.5,0.2,1.0");
        }
        else
        {
            Notify(pc, L"That Yuty was not roaring.", L"1.0,0.5,0.2,1.0");
        }
        return;
    }

    RoarType type = (arg == "fear") ? RoarType::Fear : RoarType::Courage;
    int index = (type == RoarType::Fear) ? g_fear_index : g_courage_index;
    if (index < 0)
    {
        Notify(pc, L"That roar index is not set in config.", L"1.0,0.2,0.2,1.0");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_active[id] = type;
    }
    DBUpsert(id, type);

    if (type == RoarType::Fear)
        Notify(pc, L"Fear roar enabled on aimed Yuty.");
    else
        Notify(pc, L"Courage roar enabled on aimed Yuty.");
}

static void Detour_Tick(AShooterGameMode* gm, float delta)
{
    Original_Tick(gm, delta);

    g_config_accum += delta;
    if (g_config_accum >= 10.0f)
    {
        g_config_accum = 0.0f;

        const uintmax_t newSize  = GetFileSize(g_config_path);
        const time_t    newMtime = GetFileMTime(g_config_path);

        if (newSize != 0 && newMtime != 0 &&
            (newSize != g_config_size || newMtime != g_config_mtime))
        {
            Log::GetLog()->info("[YutyAutoRoar] Config change detected, reloading...");
            LoadConfig();
        }
    }

    g_roar_accum += delta;
    if (g_roar_accum >= g_roar_interval)
    {
        g_roar_accum = 0.0f;
        DoRoarPass();
    }
}

static void PluginInit()
{
    Log::Get().Init("YutyAutoRoar");

    if (!LoadConfig())
    {
        Log::GetLog()->error("[YutyAutoRoar] Halted - config error");
        return;
    }

    if (!InitDatabase())
    {
        Log::GetLog()->error("[YutyAutoRoar] Halted - database error");
        return;
    }

    DBLoadAll();

    AsaApi::GetCommands().AddChatCommand(FString(L"/yar"), &Cmd_Yar);

    AsaApi::GetHooks().SetHook(
        "AShooterGameMode.Tick(float)",
        (LPVOID)&Detour_Tick,
        (LPVOID*)&Original_Tick
    );

    AsaApi::GetHooks().SetHook(
        "APrimalDinoCharacter.Die(float,FDamageEvent&,AController*,AActor*)",
        (LPVOID)&Detour_DinoDie,
        (LPVOID*)&Original_DinoDie
    );

    Log::GetLog()->info("[YutyAutoRoar] Plugin loaded");
}

static void PluginUnload()
{
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/yar"));

    AsaApi::GetHooks().DisableHook(
        "AShooterGameMode.Tick(float)",
        (LPVOID)&Detour_Tick
    );

    AsaApi::GetHooks().DisableHook(
        "APrimalDinoCharacter.Die(float,FDamageEvent&,AController*,AActor*)",
        (LPVOID)&Detour_DinoDie
    );

    CloseDatabase();

    Log::GetLog()->info("[YutyAutoRoar] Plugin unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { PluginInit(); }
    catch (const std::exception& ex)
    {
        Log::GetLog()->critical("[YutyAutoRoar] Init exception: {}", ex.what());
    }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { PluginUnload(); }
    catch (const std::exception& ex)
    {
        Log::GetLog()->critical("[YutyAutoRoar] Unload exception: {}", ex.what());
    }
}