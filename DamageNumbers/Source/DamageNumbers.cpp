/*
DamageNumbers - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * DamageNumbers - ASA Plugin
 *
 * Hooks:
 *   APrimalCharacter.TakeDamage(float,FDamageEvent&,AController*,AActor*)
 *     Character damage origination. Resolves the attacker team and draws for that team.
 *   APrimalStructure.TakeDamage(float,FDamageEvent&,AController*,AActor*)
 *     Structure damage origination. Resolves the attacker team and draws for that team.
 *   AShooterGameMode.PostLogin(APlayerController*)
 *     Player join. Queues a load of that player's saved preferences.
 *
 * Config:
 *   ArkApi/Plugins/DamageNumbers/config.json
 *   DbHost, DbPort, DbUser, DbPassword, DbName - MariaDB connection, shared cluster DB
 *   Optional. Without a valid config the plugin runs in memory only and preferences reset on restart.
 *
 * Tables:
 *   DamageNumbers_players  PK (eos_id)  per player preferences
 *     Columns: eos_id VARCHAR(32), visible TINYINT(1), color INT UNSIGNED
 *
 * The number is shown to every online player on the attacker's team, so friendly turrets and friendly dinos show to the owning tribe. Each player controls their own numbers with /dn, no args toggles, three values 0 to 255 set the RGB colour. Preferences load on join and save on change, both off the game thread on a background worker.
 */

#include <API/ARK/Ark.h>

#pragma warning(disable: 4191)
#pragma comment(lib, "AsaApi.lib")

#include <API/UE/Math/BoxSphereBounds.h>
#include <json.hpp>
#include <Windows.h>
#include <exception>
#include <string>
#include <vector>
#include <unordered_map>
#include <cmath>
#include <cstdlib>
#include <random>
#include <fstream>
#include <mutex>
#include <thread>
#include <atomic>
#include <ctime>
#include <sys/stat.h>

typedef struct st_mysql     MYSQL;
typedef struct st_mysql_res MYSQL_RES;
typedef char** MYSQL_ROW;

#define MYSQL_OPT_CONNECT_TIMEOUT 0

typedef MYSQL* (__stdcall* mysql_init_t)               (MYSQL*);
typedef MYSQL* (__stdcall* mysql_real_connect_t)       (MYSQL*, const char*, const char*, const char*, const char*, unsigned int, const char*, unsigned long);
typedef void(__stdcall* mysql_close_t)                 (MYSQL*);
typedef int(__stdcall* mysql_query_t)                  (MYSQL*, const char*);
typedef MYSQL_RES* (__stdcall* mysql_store_result_t)   (MYSQL*);
typedef MYSQL_ROW(__stdcall* mysql_fetch_row_t)        (MYSQL_RES*);
typedef void(__stdcall* mysql_free_result_t)           (MYSQL_RES*);
typedef const char* (__stdcall* mysql_error_t)         (MYSQL*);
typedef unsigned long(__stdcall* mysql_real_escape_string_t) (MYSQL*, char*, const char*, unsigned long);
typedef int(__stdcall* mysql_options_t)                (MYSQL*, int, const void*);
typedef int(__stdcall* mysql_ping_t)                   (MYSQL*);

static HMODULE                    g_mysql_module = nullptr;
static mysql_init_t               pmysql_init = nullptr;
static mysql_real_connect_t       pmysql_real_connect = nullptr;
static mysql_close_t              pmysql_close = nullptr;
static mysql_query_t              pmysql_query = nullptr;
static mysql_store_result_t       pmysql_store_result = nullptr;
static mysql_fetch_row_t          pmysql_fetch_row = nullptr;
static mysql_free_result_t        pmysql_free_result = nullptr;
static mysql_error_t              pmysql_error = nullptr;
static mysql_real_escape_string_t pmysql_real_escape_string = nullptr;
static mysql_options_t            pmysql_options = nullptr;
static mysql_ping_t               pmysql_ping = nullptr;
static bool                       g_mysql_loaded = false;

static MYSQL*     g_mysql = nullptr;
static std::mutex g_db_mutex;
static bool       g_db_enabled = false;

static const std::string g_config_path = "ArkApi/Plugins/DamageNumbers/config.json";
static std::string  g_db_host = "127.0.0.1";
static unsigned int g_db_port = 3306;
static std::string  g_db_user;
static std::string  g_db_pass;
static std::string  g_db_name;
static time_t       g_config_last_modified = 0;
static uintmax_t    g_config_last_size = 0;
static float        g_config_check_accumulator = 0.0f;

static std::thread       g_worker_thread;
static std::atomic<bool> g_worker_running{ false };

struct SaveJob
{
    std::string eos;
    int visible;
    unsigned color;
};

static std::vector<SaveJob>      g_save_queue;
static std::mutex                g_save_mutex;
static std::vector<std::string>  g_load_queue;
static std::mutex                g_load_mutex;

struct PlayerPrefs
{
    bool visible = true;
    FColor color = FColor(255, 255, 255, 255);
};

static std::unordered_map<std::string, PlayerPrefs> g_prefs;
static std::mutex                                   g_prefs_mutex;

static unsigned PackColor(FColor c)
{
    return (static_cast<unsigned>(c.R) << 16) | (static_cast<unsigned>(c.G) << 8) | static_cast<unsigned>(c.B);
}

static FColor UnpackColor(unsigned c)
{
    return FColor(static_cast<uint8>((c >> 16) & 0xFF), static_cast<uint8>((c >> 8) & 0xFF), static_cast<uint8>(c & 0xFF), 255);
}

static std::string FStr(const FString& f)
{
    const char* s = TCHAR_TO_UTF8(*f);
    return (s && s[0]) ? s : "";
}

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
    if (!file.is_open()) return false;

    try
    {
        nlohmann::json j;
        file >> j;

        g_db_host = j.value("DbHost", "127.0.0.1");
        g_db_port = j.value("DbPort", 3306);
        g_db_user = j.value("DbUser", "");
        g_db_pass = j.value("DbPassword", "");
        g_db_name = j.value("DbName", "");
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[DamageNumbers] Config parse error: {}", ex.what());
        return false;
    }

    if (g_db_user.empty() || g_db_name.empty())
    {
        Log::GetLog()->warn("[DamageNumbers] Config missing DbUser or DbName, running in memory only");
        return false;
    }

    return true;
}

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
        if (g_mysql_module) break;
    }

    if (!g_mysql_module)
    {
        Log::GetLog()->error("[DamageNumbers] Could not find libmariadb.dll or libmysql.dll");
        return false;
    }

    pmysql_init = (mysql_init_t)GetProcAddress(g_mysql_module, "mysql_init");
    pmysql_real_connect = (mysql_real_connect_t)GetProcAddress(g_mysql_module, "mysql_real_connect");
    pmysql_close = (mysql_close_t)GetProcAddress(g_mysql_module, "mysql_close");
    pmysql_query = (mysql_query_t)GetProcAddress(g_mysql_module, "mysql_query");
    pmysql_store_result = (mysql_store_result_t)GetProcAddress(g_mysql_module, "mysql_store_result");
    pmysql_fetch_row = (mysql_fetch_row_t)GetProcAddress(g_mysql_module, "mysql_fetch_row");
    pmysql_free_result = (mysql_free_result_t)GetProcAddress(g_mysql_module, "mysql_free_result");
    pmysql_error = (mysql_error_t)GetProcAddress(g_mysql_module, "mysql_error");
    pmysql_real_escape_string = (mysql_real_escape_string_t)GetProcAddress(g_mysql_module, "mysql_real_escape_string");
    pmysql_options = (mysql_options_t)GetProcAddress(g_mysql_module, "mysql_options");
    pmysql_ping = (mysql_ping_t)GetProcAddress(g_mysql_module, "mysql_ping");

    if (!pmysql_init || !pmysql_real_connect || !pmysql_close ||
        !pmysql_query || !pmysql_store_result || !pmysql_fetch_row ||
        !pmysql_free_result || !pmysql_error || !pmysql_real_escape_string)
    {
        Log::GetLog()->error("[DamageNumbers] Failed to resolve required DB functions");
        return false;
    }

    g_mysql_loaded = true;
    return true;
}

static bool Escape(const std::string& in, std::string& out)
{
    out.clear();
    if (!g_mysql || !pmysql_real_escape_string) return false;

    std::string buf(in.size() * 2 + 1, '\0');
    const unsigned long len = pmysql_real_escape_string(
        g_mysql, buf.data(), in.c_str(), (unsigned long)in.size());
    buf.resize(len);
    out = std::move(buf);
    return true;
}

static bool ExecQuery(const std::string& sql)
{
    if (!g_mysql) return false;
    if (pmysql_query(g_mysql, sql.c_str()) != 0)
    {
        Log::GetLog()->error("[DamageNumbers] Query error: {}", pmysql_error(g_mysql));
        return false;
    }
    if (MYSQL_RES* res = pmysql_store_result(g_mysql))
        pmysql_free_result(res);
    return true;
}

static bool OpenConnection()
{
    g_mysql = pmysql_init(nullptr);
    if (!g_mysql)
    {
        Log::GetLog()->error("[DamageNumbers] mysql_init failed");
        return false;
    }

    if (pmysql_options)
    {
        unsigned int timeout = 5;
        pmysql_options(g_mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    }

    if (!pmysql_real_connect(g_mysql,
        g_db_host.c_str(), g_db_user.c_str(), g_db_pass.c_str(),
        g_db_name.c_str(), g_db_port, nullptr, 0))
    {
        Log::GetLog()->error("[DamageNumbers] DB connect failed: {}", pmysql_error(g_mysql));
        pmysql_close(g_mysql);
        g_mysql = nullptr;
        return false;
    }

    return true;
}

static bool EnsureConnected()
{
    if (g_mysql)
    {
        if (!pmysql_ping) return true;
        if (pmysql_ping(g_mysql) == 0) return true;
        pmysql_close(g_mysql);
        g_mysql = nullptr;
    }
    if (!g_mysql_loaded) return false;
    if (!OpenConnection()) return false;
    Log::GetLog()->info("[DamageNumbers] DB reconnected");
    return true;
}

static bool InitDatabase()
{
    if (!LoadMySQLLib()) return false;

    std::lock_guard<std::mutex> dbLock(g_db_mutex);
    if (!OpenConnection()) return false;

    const bool ok = ExecQuery(
        "CREATE TABLE IF NOT EXISTS DamageNumbers_players ("
        "  eos_id     VARCHAR(32)     NOT NULL,"
        "  visible    TINYINT(1)      NOT NULL DEFAULT 1,"
        "  color      INT UNSIGNED    NOT NULL DEFAULT 16777215,"
        "  updated_at TIMESTAMP       NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
        "  PRIMARY KEY (eos_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4"
    );

    if (!ok)
    {
        Log::GetLog()->error("[DamageNumbers] Failed to create DamageNumbers_players table");
        pmysql_close(g_mysql);
        g_mysql = nullptr;
        return false;
    }

    return true;
}

static void EnqueueSave(const std::string& eos, bool visible, unsigned color)
{
    if (!g_db_enabled) return;
    std::lock_guard<std::mutex> lock(g_save_mutex);
    g_save_queue.push_back({ eos, visible ? 1 : 0, color });
}

static void EnqueueLoad(const std::string& eos)
{
    if (!g_db_enabled) return;
    std::lock_guard<std::mutex> lock(g_load_mutex);
    g_load_queue.push_back(eos);
}

static void FlushSaves()
{
    std::vector<SaveJob> jobs;
    {
        std::lock_guard<std::mutex> lock(g_save_mutex);
        jobs.swap(g_save_queue);
    }
    if (jobs.empty()) return;

    std::lock_guard<std::mutex> dbLock(g_db_mutex);
    if (!EnsureConnected())
    {
        std::lock_guard<std::mutex> lock(g_save_mutex);
        for (auto& j : jobs) g_save_queue.push_back(j);
        return;
    }

    for (auto& j : jobs)
    {
        std::string esc;
        if (!Escape(j.eos, esc)) continue;
        const std::string sql =
            "INSERT INTO DamageNumbers_players (eos_id, visible, color) VALUES ('"
            + esc + "'," + std::to_string(j.visible) + "," + std::to_string(j.color)
            + ") ON DUPLICATE KEY UPDATE visible=VALUES(visible), color=VALUES(color)";
        ExecQuery(sql);
    }
}

static void ProcessLoads()
{
    std::vector<std::string> jobs;
    {
        std::lock_guard<std::mutex> lock(g_load_mutex);
        jobs.swap(g_load_queue);
    }
    if (jobs.empty()) return;

    std::lock_guard<std::mutex> dbLock(g_db_mutex);
    if (!EnsureConnected())
    {
        std::lock_guard<std::mutex> lock(g_load_mutex);
        for (auto& e : jobs) g_load_queue.push_back(e);
        return;
    }

    for (auto& eos : jobs)
    {
        std::string esc;
        if (!Escape(eos, esc)) continue;

        const std::string sql = "SELECT visible, color FROM DamageNumbers_players WHERE eos_id='" + esc + "'";
        if (pmysql_query(g_mysql, sql.c_str()) != 0)
        {
            Log::GetLog()->error("[DamageNumbers] Load query error: {}", pmysql_error(g_mysql));
            continue;
        }

        MYSQL_RES* res = pmysql_store_result(g_mysql);
        if (!res) continue;

        if (MYSQL_ROW row = pmysql_fetch_row(res))
        {
            const int vis = row[0] ? std::atoi(row[0]) : 1;
            const unsigned col = row[1] ? static_cast<unsigned>(std::strtoul(row[1], nullptr, 10)) : 16777215u;

            std::lock_guard<std::mutex> lock(g_prefs_mutex);
            PlayerPrefs& p = g_prefs[eos];
            p.visible = (vis != 0);
            p.color = UnpackColor(col);
        }

        pmysql_free_result(res);
    }
}

static void WorkerLoop()
{
    while (g_worker_running.load())
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!g_worker_running.load()) break;
        ProcessLoads();
        FlushSaves();
    }
    FlushSaves();
}

static void OnTick(float delta)
{
    g_config_check_accumulator += delta;
    if (g_config_check_accumulator < 10.0f) return;
    g_config_check_accumulator = 0.0f;

    time_t mtime = 0;
    uintmax_t fsize = 0;
    if (GetFileInfo(g_config_path, mtime, fsize) && fsize > 0 &&
        (mtime != g_config_last_modified || fsize != g_config_last_size))
    {
        if (LoadConfig())
        {
            g_config_last_modified = mtime;
            g_config_last_size = fsize;
        }
    }
}

static bool IsPlayerController(AController* c)
{
    return c && c->IsA(AShooterPlayerController::GetPrivateStaticClass());
}

static std::string GetEosId(AShooterPlayerController* pc)
{
    if (!pc) return "";
    AShooterPlayerState* ps = static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
    if (!ps) return "";
    FString eos;
    ps->GetUniqueNetIdAsString(&eos);
    return FStr(eos);
}

static int GetAttackerTeam(AController* instigator, AActor* damageCauser)
{
    if (instigator)
    {
        APawn* pawn = instigator->PawnField().Get();
        if (pawn) return pawn->TargetingTeamField();
    }
    if (damageCauser) return damageCauser->TargetingTeamField();
    return 0;
}

static void Notify(AShooterPlayerController* pc, const std::wstring& msg)
{
    if (!pc) return;
    const std::wstring wFull = L"<RichColor Color=\"1,1,1,1\">" + msg + L"</>";
    FString fSender(L"");
    FString fMsg(wFull.c_str());
    AsaApi::GetApiUtils().SendChatMessage(pc, fSender, L"{}", std::wstring_view(*fMsg));
}

static double Jitter(double range)
{
    static thread_local std::mt19937 rng{ std::random_device{}() };
    std::uniform_real_distribution<double> dist(-range, range);
    return dist(rng);
}

static float GetCurrentHealth(APrimalCharacter* c)
{
    if (!c) return 0.0f;
    UPrimalCharacterStatusComponent* comp = c->MyCharacterStatusComponentField();
    if (!comp) return 0.0f;
    return comp->BPGetCurrentStatusValue(EPrimalCharacterStatusValue::Health);
}

static UE::Math::TVector<double> TopOf(USceneComponent* root)
{
    auto& b = root->BoundsField();
    return UE::Math::TVector<double>(b.Origin.X, b.Origin.Y, b.Origin.Z + b.BoxExtent.Z + 30.0);
}

static void DrawNumber(AShooterPlayerController* pc, const UE::Math::TVector<double>& at, int amount, FColor color)
{
    if (!pc) return;

    FVector_NetQuantize loc;
    loc.X = at.X + Jitter(110.0);
    loc.Y = at.Y + Jitter(110.0);
    loc.Z = at.Z + Jitter(45.0);

    FString text(std::to_wstring(amount));
    UE::Math::TVector<double> vel(0.0, 0.0, 100.0);

    NativeCall<void, FVector_NetQuantize*, const FString*, FColor, float, float, float, UE::Math::TVector<double>*, float, float, float>(
        pc,
        "AShooterPlayerController.ClientAddFloatingText(FVector_NetQuantize,FString&,FColor,float,float,float,UE::Math::TVector<double>,float,float,float)",
        &loc, &text, color, 1.0f, 1.0f, 3.0f, &vel, 1.0f, 0.1f, 1.5f);
}

static void DrawToTeam(int team, const UE::Math::TVector<double>& loc, int amount)
{
    if (team == 0) return;

    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return;

    auto& controllers = world->PlayerControllerListField();
    for (int i = 0; i < controllers.Num(); ++i)
    {
        APlayerController* apc = controllers[i].Get();
        if (!IsPlayerController(apc)) continue;

        AShooterPlayerController* pc = static_cast<AShooterPlayerController*>(apc);
        AShooterCharacter* ch = pc->BaseGetPlayerCharacter();
        if (!ch) continue;
        if (ch->TargetingTeamField() != team) continue;

        std::string eos = GetEosId(pc);

        bool visible = true;
        FColor color(255, 255, 255, 255);
        {
            std::lock_guard<std::mutex> lock(g_prefs_mutex);
            PlayerPrefs& p = g_prefs[eos];
            visible = p.visible;
            color = p.color;
        }

        if (visible)
            DrawNumber(pc, loc, amount, color);
    }
}

static void ProcessHit(const UE::Math::TVector<double>& loc, float lost, AController* instigator, AActor* causer)
{
    if (lost <= 0.0f) return;

    int amount = static_cast<int>(std::lround(lost));
    int attackerTeam = GetAttackerTeam(instigator, causer);

    DrawToTeam(attackerTeam, loc, amount);
}

static void DnCommand(AShooterPlayerController* pc, FString* message, int, int)
{
    if (!pc || !message) return;

    std::string eos = GetEosId(pc);
    if (eos.empty()) return;

    std::string msg = FStr(*message);

    std::vector<std::string> parts;
    std::string cur;
    for (char ch : msg)
    {
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
        {
            if (!cur.empty()) { parts.push_back(cur); cur.clear(); }
        }
        else
        {
            cur.push_back(ch);
        }
    }
    if (!cur.empty()) parts.push_back(cur);

    size_t argc = parts.empty() ? 0 : parts.size() - 1;

    if (argc == 0)
    {
        bool nowVisible;
        unsigned packed;
        {
            std::lock_guard<std::mutex> lock(g_prefs_mutex);
            PlayerPrefs& p = g_prefs[eos];
            p.visible = !p.visible;
            nowVisible = p.visible;
            packed = PackColor(p.color);
        }
        EnqueueSave(eos, nowVisible, packed);
        Notify(pc, nowVisible ? L"Damage numbers ON" : L"Damage numbers OFF");
        return;
    }

    if (argc == 3)
    {
        int rgb[3];
        for (int i = 0; i < 3; ++i)
        {
            const std::string& t = parts[i + 1];
            bool ok = !t.empty() && t.size() <= 3;
            if (ok) for (char ch : t) if (ch < '0' || ch > '9') { ok = false; break; }
            if (!ok)
            {
                Notify(pc, L"Usage: /dn to toggle, or /dn <r> <g> <b> with each 0 to 255");
                return;
            }
            int v = std::atoi(t.c_str());
            if (v < 0 || v > 255)
            {
                Notify(pc, L"Colour values must be 0 to 255");
                return;
            }
            rgb[i] = v;
        }

        bool nowVisible;
        unsigned packed;
        {
            std::lock_guard<std::mutex> lock(g_prefs_mutex);
            PlayerPrefs& p = g_prefs[eos];
            p.color = FColor(static_cast<uint8>(rgb[0]), static_cast<uint8>(rgb[1]), static_cast<uint8>(rgb[2]), 255);
            nowVisible = p.visible;
            packed = PackColor(p.color);
        }
        EnqueueSave(eos, nowVisible, packed);
        Notify(pc, L"Damage number colour updated");
        return;
    }

    Notify(pc, L"Usage: /dn to toggle, or /dn <r> <g> <b> with each 0 to 255");
}

DECLARE_HOOK(APrimalCharacter_TakeDamage, float, APrimalCharacter*, float, FDamageEvent*, AController*, AActor*);
DECLARE_HOOK(APrimalStructure_TakeDamage, float, APrimalStructure*, float, FDamageEvent*, AController*, AActor*);
DECLARE_HOOK(AShooterGameMode_PostLogin, void, AShooterGameMode*, APlayerController*);

float Hook_APrimalCharacter_TakeDamage(APrimalCharacter* _this, float Damage, FDamageEvent* DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
    float before = GetCurrentHealth(_this);
    float result = APrimalCharacter_TakeDamage_original(_this, Damage, DamageEvent, EventInstigator, DamageCauser);
    float after = GetCurrentHealth(_this);

    if (!_this) return result;

    USceneComponent* root = _this->RootComponentField().Get();
    if (!root) return result;

    ProcessHit(TopOf(root), before - after, EventInstigator, DamageCauser);
    return result;
}

float Hook_APrimalStructure_TakeDamage(APrimalStructure* _this, float Damage, FDamageEvent* DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
    float before = _this ? _this->HealthField() : 0.0f;
    float result = APrimalStructure_TakeDamage_original(_this, Damage, DamageEvent, EventInstigator, DamageCauser);
    float after = _this ? _this->HealthField() : 0.0f;

    if (!_this) return result;

    USceneComponent* root = _this->RootComponentField().Get();
    if (!root) return result;

    ProcessHit(TopOf(root), before - after, EventInstigator, DamageCauser);
    return result;
}

void Hook_AShooterGameMode_PostLogin(AShooterGameMode* gm, APlayerController* pc)
{
    AShooterGameMode_PostLogin_original(gm, pc);

    if (!pc || !pc->IsA(AShooterPlayerController::GetPrivateStaticClass())) return;
    AShooterPlayerController* spc = static_cast<AShooterPlayerController*>(pc);

    std::string eos = GetEosId(spc);
    if (!eos.empty()) EnqueueLoad(eos);
}

static void PluginInit()
{
    Log::Get().Init("DamageNumbers");

    if (LoadConfig())
    {
        GetFileInfo(g_config_path, g_config_last_modified, g_config_last_size);
        if (InitDatabase())
        {
            g_db_enabled = true;
            g_worker_running.store(true);
            g_worker_thread = std::thread(WorkerLoop);
            Log::GetLog()->info("[DamageNumbers] Database ready");
        }
        else
        {
            Log::GetLog()->warn("[DamageNumbers] Database unavailable, running in memory only");
        }
    }
    else
    {
        Log::GetLog()->warn("[DamageNumbers] No valid config, running in memory only");
    }

    AsaApi::GetHooks().SetHook(
        "APrimalCharacter.TakeDamage(float,FDamageEvent&,AController*,AActor*)",
        (LPVOID)&Hook_APrimalCharacter_TakeDamage,
        (LPVOID*)&APrimalCharacter_TakeDamage_original);

    AsaApi::GetHooks().SetHook(
        "APrimalStructure.TakeDamage(float,FDamageEvent&,AController*,AActor*)",
        (LPVOID)&Hook_APrimalStructure_TakeDamage,
        (LPVOID*)&APrimalStructure_TakeDamage_original);

    AsaApi::GetHooks().SetHook(
        "AShooterGameMode.PostLogin(APlayerController*)",
        (LPVOID)&Hook_AShooterGameMode_PostLogin,
        (LPVOID*)&AShooterGameMode_PostLogin_original);

    AsaApi::GetCommands().AddChatCommand(FString(L"/dn"), &DnCommand);
    AsaApi::GetCommands().AddOnTickCallback(FString(L"DamageNumbers.Tick"), &OnTick);

    Log::GetLog()->info("[DamageNumbers] Loaded");
}

static void PluginUnload()
{
    g_worker_running.store(false);
    if (g_worker_thread.joinable())
        g_worker_thread.join();

    AsaApi::GetHooks().DisableHook(
        "APrimalCharacter.TakeDamage(float,FDamageEvent&,AController*,AActor*)",
        (LPVOID)&Hook_APrimalCharacter_TakeDamage);

    AsaApi::GetHooks().DisableHook(
        "APrimalStructure.TakeDamage(float,FDamageEvent&,AController*,AActor*)",
        (LPVOID)&Hook_APrimalStructure_TakeDamage);

    AsaApi::GetHooks().DisableHook(
        "AShooterGameMode.PostLogin(APlayerController*)",
        (LPVOID)&Hook_AShooterGameMode_PostLogin);

    AsaApi::GetCommands().RemoveChatCommand(FString(L"/dn"));
    AsaApi::GetCommands().RemoveOnTickCallback(FString(L"DamageNumbers.Tick"));

    {
        std::lock_guard<std::mutex> dbLock(g_db_mutex);
        if (g_mysql)
        {
            pmysql_close(g_mysql);
            g_mysql = nullptr;
        }
    }

    Log::GetLog()->info("[DamageNumbers] Unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { PluginInit(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->critical("[DamageNumbers] Init exception: {}", e.what());
    }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { PluginUnload(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->error("[DamageNumbers] Unload exception: {}", e.what());
    }
}