/*
DinoPainter - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * DinoPainter - ASA Plugin
 *
 * Hook categories: Chat
 *
 * Table:
 *   paint_presets - PK (eos_id, preset_name)
 *     Columns: regions (VARCHAR 32), colors (VARCHAR 32)
 *
 * Chat commands:
 *   /paint <region> <colorID>               - paint aimed tribe dino, single region
 *   /paintall <presetName> <radius>          - paint all dinos of aimed type within radius using preset
 *   /savepaint <name> <r0,r1,...> <c0,c1,...> - save color preset to DB
 *   /paintpresets                             - list saved presets
 *   /paintdel <name>                          - delete a preset
 *
 * Permissions integration:
 *   Dynamically loads Ark:SA Permissions V1.1 at runtime.
 *   Per-group config: CanPaint, CanPaintAll, MaxRadius.
 */

#include <API/ARK/Ark.h>
#include <json.hpp>
#include <Windows.h>
#include <fstream>
#include <string>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <cstdio>
#include <cmath>
#include <cctype>
#include <algorithm>

#pragma warning(disable: 4191) // unsafe FARPROC conversion

// =============================================================================
// MariaDB - Dynamic Load
// =============================================================================

typedef struct st_mysql     MYSQL;
typedef struct st_mysql_res MYSQL_RES;
typedef char** MYSQL_ROW;

typedef MYSQL*        (__stdcall* mysql_init_t)               (MYSQL*);
typedef MYSQL*        (__stdcall* mysql_real_connect_t)        (MYSQL*, const char*, const char*, const char*, const char*, unsigned int, const char*, unsigned long);
typedef void          (__stdcall* mysql_close_t)               (MYSQL*);
typedef int           (__stdcall* mysql_query_t)               (MYSQL*, const char*);
typedef MYSQL_RES*    (__stdcall* mysql_store_result_t)        (MYSQL*);
typedef void          (__stdcall* mysql_free_result_t)         (MYSQL_RES*);
typedef const char*   (__stdcall* mysql_error_t)               (MYSQL*);
typedef unsigned long (__stdcall* mysql_real_escape_string_t)  (MYSQL*, char*, const char*, unsigned long);
typedef int           (__stdcall* mysql_options_t)             (MYSQL*, int, const void*);
typedef MYSQL_ROW     (__stdcall* mysql_fetch_row_t)           (MYSQL_RES*);
typedef unsigned int  (__stdcall* mysql_num_fields_t)          (MYSQL_RES*);

#define MYSQL_OPT_CONNECT_TIMEOUT 0

static HMODULE                    g_mysql_module            = nullptr;
static mysql_init_t               pmysql_init               = nullptr;
static mysql_real_connect_t       pmysql_real_connect        = nullptr;
static mysql_close_t              pmysql_close               = nullptr;
static mysql_query_t              pmysql_query               = nullptr;
static mysql_store_result_t       pmysql_store_result        = nullptr;
static mysql_free_result_t        pmysql_free_result         = nullptr;
static mysql_error_t              pmysql_error               = nullptr;
static mysql_real_escape_string_t pmysql_real_escape_string   = nullptr;
static mysql_options_t            pmysql_options              = nullptr;
static mysql_fetch_row_t          pmysql_fetch_row            = nullptr;
static mysql_num_fields_t         pmysql_num_fields           = nullptr;
static bool                       g_mysql_loaded             = false;

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
            Log::GetLog()->info("[DinoPainter] Loaded DB library: {}", candidates[i]);
            break;
        }
    }

    if (!g_mysql_module)
    {
        Log::GetLog()->error("[DinoPainter] Could not find libmariadb.dll or libmysql.dll");
        return false;
    }

    pmysql_init               = (mysql_init_t)              GetProcAddress(g_mysql_module, "mysql_init");
    pmysql_real_connect        = (mysql_real_connect_t)      GetProcAddress(g_mysql_module, "mysql_real_connect");
    pmysql_close               = (mysql_close_t)             GetProcAddress(g_mysql_module, "mysql_close");
    pmysql_query               = (mysql_query_t)             GetProcAddress(g_mysql_module, "mysql_query");
    pmysql_store_result        = (mysql_store_result_t)      GetProcAddress(g_mysql_module, "mysql_store_result");
    pmysql_free_result         = (mysql_free_result_t)       GetProcAddress(g_mysql_module, "mysql_free_result");
    pmysql_error               = (mysql_error_t)             GetProcAddress(g_mysql_module, "mysql_error");
    pmysql_real_escape_string   = (mysql_real_escape_string_t)GetProcAddress(g_mysql_module, "mysql_real_escape_string");
    pmysql_options              = (mysql_options_t)           GetProcAddress(g_mysql_module, "mysql_options");
    pmysql_fetch_row            = (mysql_fetch_row_t)         GetProcAddress(g_mysql_module, "mysql_fetch_row");
    pmysql_num_fields           = (mysql_num_fields_t)        GetProcAddress(g_mysql_module, "mysql_num_fields");

    if (!pmysql_init || !pmysql_real_connect || !pmysql_close || !pmysql_query ||
        !pmysql_store_result || !pmysql_free_result || !pmysql_error ||
        !pmysql_real_escape_string || !pmysql_options || !pmysql_fetch_row || !pmysql_num_fields)
    {
        Log::GetLog()->error("[DinoPainter] Failed to resolve one or more MySQL symbols");
        return false;
    }

    g_mysql_loaded = true;
    return true;
}

// =============================================================================
// Database Connection
// =============================================================================

static MYSQL*      g_db       = nullptr;
static std::mutex  g_db_mutex;
static std::string g_db_host, g_db_user, g_db_pass, g_db_name;
static int         g_db_port  = 3306;

static bool ConnectDB()
{
    if (g_db) return true;

    g_db = pmysql_init(nullptr);
    if (!g_db) return false;

    unsigned int timeout = 5;
    pmysql_options(g_db, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

    if (!pmysql_real_connect(g_db, g_db_host.c_str(), g_db_user.c_str(),
        g_db_pass.c_str(), g_db_name.c_str(), g_db_port, nullptr, 0))
    {
        Log::GetLog()->error("[DinoPainter] DB connect failed: {}", pmysql_error(g_db));
        pmysql_close(g_db);
        g_db = nullptr;
        return false;
    }

    Log::GetLog()->info("[DinoPainter] DB connected to {}:{}/{}", g_db_host, g_db_port, g_db_name);
    return true;
}

static std::string EscapeUnsafe(const std::string& input)
{
    if (!g_db || input.empty()) return input;
    std::string buf(input.size() * 2 + 1, '\0');
    unsigned long len = pmysql_real_escape_string(g_db, buf.data(),
        input.c_str(), (unsigned long)input.size());
    buf.resize(len);
    return buf;
}

static void EnsureTable()
{
    const char* sql =
        "CREATE TABLE IF NOT EXISTS paint_presets ("
        "  eos_id      VARCHAR(64)  NOT NULL,"
        "  preset_name VARCHAR(32)  NOT NULL,"
        "  regions     VARCHAR(32)  NOT NULL,"
        "  colors      VARCHAR(32)  NOT NULL,"
        "  PRIMARY KEY (eos_id, preset_name)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;";

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!ConnectDB()) return;
    if (pmysql_query(g_db, sql))
        Log::GetLog()->error("[DinoPainter] EnsureTable failed: {}", pmysql_error(g_db));
}

// =============================================================================
// Permissions - Dynamic Load
// =============================================================================

typedef TArray<FString>(*GetPlayerGroups_t)(const FString&);

static GetPlayerGroups_t pGetPlayerGroups    = nullptr;
static bool              g_permissions_loaded = false;
static bool              g_permissions_attempted = false;

static void LoadPermissionsAPI()
{
    if (g_permissions_attempted) return;
    g_permissions_attempted = true;

    HMODULE hMod = GetModuleHandleA("Permissions");
    if (!hMod)
    {
        Log::GetLog()->warn("[DinoPainter] Permissions.dll not found, using default tier");
        return;
    }

    pGetPlayerGroups = (GetPlayerGroups_t)GetProcAddress(hMod,
        "?GetPlayerGroups@Permissions@@YA?AV?$TArray@VFString@@V?$TSizedDefaultAllocator@$0CA@@@@@AEBVFString@@@Z");

    if (!pGetPlayerGroups)
    {
        Log::GetLog()->warn("[DinoPainter] Failed to resolve Permissions functions, using default tier");
        return;
    }

    g_permissions_loaded = true;
    Log::GetLog()->info("[DinoPainter] Permissions API loaded");
}

// =============================================================================
// Config
// =============================================================================

struct GroupTier
{
    bool  canPaint    = true;
    bool  canPaintAll = false;
    float maxRadius   = 5000.0f; // UE units (~50m)
    int   maxPresets  = 10;
};

static std::unordered_map<std::string, GroupTier> g_group_tiers;
static GroupTier                                   g_default_tier;
static std::string                                 g_message_color = "0, 1, 0.65, 1";
static float                                       g_max_aim_range = 5000.0f;
static float                                       g_aim_dot_threshold = 0.98f;

static std::string FStr(const FString& f)
{
    if (f.IsEmpty()) return "";
    const char* s = TCHAR_TO_UTF8(*f);
    return s ? s : "";
}

static std::string ToLower(const std::string& s)
{
    std::string out = s;
    for (char& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
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

static bool g_config_loaded_once = false;

static void LoadConfig()
{
    const std::string path = AsaApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/DinoPainter/config.json";
    std::ifstream file(path);
    if (!file.is_open())
    {
        if (!g_config_loaded_once)
            Log::GetLog()->error("[DinoPainter] Cannot open {}", path);
        return;
    }

    nlohmann::json cfg;
    try { file >> cfg; }
    catch (const std::exception& e)
    {
        Log::GetLog()->error("[DinoPainter] JSON parse error: {}", e.what());
        return;
    }

    if (cfg.contains("Database"))
    {
        auto& db     = cfg["Database"];
        g_db_host    = db.value("Host", "127.0.0.1");
        g_db_port    = db.value("Port", 3306);
        g_db_user    = db.value("User", "root");
        g_db_pass    = db.value("Password", "");
        g_db_name    = db.value("Name", "arkadius");
    }

    g_message_color    = cfg.value("MessageColor", g_message_color);
    g_max_aim_range    = cfg.value("MaxAimRange", 5000.0f);
    g_aim_dot_threshold = cfg.value("AimDotThreshold", 0.98f);

    // Default tier
    if (cfg.contains("DefaultTier"))
    {
        auto& dt               = cfg["DefaultTier"];
        g_default_tier.canPaint    = dt.value("CanPaint", true);
        g_default_tier.canPaintAll = dt.value("CanPaintAll", false);
        g_default_tier.maxRadius   = dt.value("MaxRadius", 5000.0f);
        g_default_tier.maxPresets  = dt.value("MaxPresets", 10);
    }

    // Group tiers
    g_group_tiers.clear();
    if (cfg.contains("Groups"))
    {
        for (auto& [key, val] : cfg["Groups"].items())
        {
            GroupTier t;
            t.canPaint    = val.value("CanPaint", true);
            t.canPaintAll = val.value("CanPaintAll", false);
            t.maxRadius   = val.value("MaxRadius", 5000.0f);
            t.maxPresets  = val.value("MaxPresets", 10);
            g_group_tiers[key] = t;
        }
    }

    if (!g_config_loaded_once)
    {
        Log::GetLog()->info("[DinoPainter] Config loaded ({} group tiers)", g_group_tiers.size());
        g_config_loaded_once = true;
    }
}

// =============================================================================
// Permission Tier Resolution
// =============================================================================

static GroupTier ResolveTier(const std::string& eosId)
{
    GroupTier best = g_default_tier;

    if (g_group_tiers.empty()) return best;

    if (!g_permissions_attempted) LoadPermissionsAPI();
    if (!g_permissions_loaded || !pGetPlayerGroups) return best;

    try
    {
        FString fEos(eosId.c_str());
        TArray<FString> groups = pGetPlayerGroups(fEos);

        bool matched = false;
        for (int i = 0; i < groups.Num(); ++i)
        {
            const std::string groupName = FStr(groups[i]);
            auto it = g_group_tiers.find(groupName);
            if (it != g_group_tiers.end())
            {
                if (!matched || it->second.maxRadius > best.maxRadius)
                {
                    best = it->second;
                    matched = true;
                }
            }
        }
    }
    catch (...)
    {
        Log::GetLog()->warn("[DinoPainter] GetPlayerGroups threw, using default tier");
    }

    return best;
}

// =============================================================================
// Aimed Dino Detection
// =============================================================================

static constexpr double kPi = 3.14159265358979323846;

static APrimalDinoCharacter* GetAimedDino(AShooterPlayerController* pc, int playerTeam)
{
    AActor* ch = pc->BaseGetPlayerCharacter();
    if (!ch) return nullptr;

    USceneComponent* root = ch->RootComponentField();
    if (!root) return nullptr;

    auto playerLoc = root->RelativeLocationField();

    // Get control rotation for aim direction
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

    APrimalDinoCharacter* bestDino = nullptr;
    double bestDot = g_aim_dot_threshold;

    for (int i = 0; i < actors.Num(); ++i)
    {
        AActor* actor = actors[i];
        if (!actor) continue;
        if (!actor->IsA(APrimalDinoCharacter::StaticClass())) continue;

        APrimalDinoCharacter* dino = static_cast<APrimalDinoCharacter*>(actor);

        // Only tribe-owned dinos
        if (dino->TargetingTeamField() != playerTeam) continue;

        USceneComponent* dRoot = dino->RootComponentField();
        if (!dRoot) continue;

        auto dLoc = dRoot->RelativeLocationField();
        double dx = dLoc.X - playerLoc.X;
        double dy = dLoc.Y - playerLoc.Y;
        double dz = dLoc.Z - playerLoc.Z;

        double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist < 1.0 || dist > g_max_aim_range) continue;

        // Normalize direction to dino
        double nx = dx / dist;
        double ny = dy / dist;
        double nz = dz / dist;

        double dot = nx * fwdX + ny * fwdY + nz * fwdZ;
        if (dot > bestDot)
        {
            bestDot = dot;
            bestDino = dino;
        }
    }

    return bestDino;
}

// =============================================================================
// Dino Color Helpers
// =============================================================================

static bool SetDinoRegionColor(APrimalDinoCharacter* dino, int region, int colorId)
{
    if (region < 0 || region > 5) return false;
    if (colorId < 0 || colorId > 255) return false;

    // ForceUpdateColorSets handles both color write and client replication
    dino->ForceUpdateColorSets(region, colorId);

    return true;
}

static std::string GetDinoBP(APrimalDinoCharacter* dino)
{
    return FStr(AsaApi::GetApiUtils().GetBlueprint(static_cast<AActor*>(dino)));
}

// =============================================================================
// Preset Helpers
// =============================================================================

struct PaintPreset
{
    std::string name;
    std::vector<int> regions;
    std::vector<int> colors;
};

static bool ParseCSV(const std::string& input, std::vector<int>& out)
{
    out.clear();
    std::string token;
    for (size_t i = 0; i <= input.size(); ++i)
    {
        if (i == input.size() || input[i] == ',')
        {
            if (token.empty()) return false;
            try { out.push_back(std::stoi(token)); }
            catch (...) { return false; }
            token.clear();
        }
        else
        {
            token += input[i];
        }
    }
    return !out.empty();
}

static std::string VecToCSV(const std::vector<int>& v)
{
    std::string out;
    for (size_t i = 0; i < v.size(); ++i)
    {
        if (i > 0) out += ",";
        out += std::to_string(v[i]);
    }
    return out;
}

// =============================================================================
// DB Operations
// =============================================================================

static bool SavePreset(const std::string& eosId, const PaintPreset& preset)
{
    std::string eSafe = EscapeUnsafe(eosId);
    std::string nSafe = EscapeUnsafe(preset.name);
    std::string rCSV  = EscapeUnsafe(VecToCSV(preset.regions));
    std::string cCSV  = EscapeUnsafe(VecToCSV(preset.colors));

    char sql[512];
    snprintf(sql, sizeof(sql),
        "INSERT INTO paint_presets (eos_id, preset_name, regions, colors) "
        "VALUES ('%s','%s','%s','%s') "
        "ON DUPLICATE KEY UPDATE regions='%s', colors='%s'",
        eSafe.c_str(), nSafe.c_str(), rCSV.c_str(), cCSV.c_str(),
        rCSV.c_str(), cCSV.c_str());

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!ConnectDB()) return false;
    if (pmysql_query(g_db, sql))
    {
        Log::GetLog()->error("[DinoPainter] SavePreset failed: {}", pmysql_error(g_db));
        return false;
    }
    return true;
}

static bool DeletePreset(const std::string& eosId, const std::string& name)
{
    std::string eSafe = EscapeUnsafe(eosId);
    std::string nSafe = EscapeUnsafe(name);

    char sql[256];
    snprintf(sql, sizeof(sql),
        "DELETE FROM paint_presets WHERE eos_id='%s' AND preset_name='%s'",
        eSafe.c_str(), nSafe.c_str());

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!ConnectDB()) return false;
    if (pmysql_query(g_db, sql))
    {
        Log::GetLog()->error("[DinoPainter] DeletePreset failed: {}", pmysql_error(g_db));
        return false;
    }
    return true;
}

static std::vector<PaintPreset> GetPresets(const std::string& eosId)
{
    std::vector<PaintPreset> results;

    std::string eSafe = EscapeUnsafe(eosId);

    char sql[256];
    snprintf(sql, sizeof(sql),
        "SELECT preset_name, regions, colors FROM paint_presets WHERE eos_id='%s'",
        eSafe.c_str());

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!ConnectDB()) return results;
    if (pmysql_query(g_db, sql))
    {
        Log::GetLog()->error("[DinoPainter] GetPresets failed: {}", pmysql_error(g_db));
        return results;
    }

    MYSQL_RES* res = pmysql_store_result(g_db);
    if (!res) return results;

    MYSQL_ROW row;
    while ((row = pmysql_fetch_row(res)))
    {
        PaintPreset p;
        p.name = row[0] ? row[0] : "";
        std::string regionStr = row[1] ? row[1] : "";
        std::string colorStr  = row[2] ? row[2] : "";
        ParseCSV(regionStr, p.regions);
        ParseCSV(colorStr, p.colors);
        results.push_back(p);
    }

    pmysql_free_result(res);
    return results;
}

static bool FindPreset(const std::string& eosId, const std::string& name, PaintPreset& out)
{
    std::string eSafe = EscapeUnsafe(eosId);
    std::string nSafe = EscapeUnsafe(name);

    char sql[256];
    snprintf(sql, sizeof(sql),
        "SELECT preset_name, regions, colors FROM paint_presets "
        "WHERE eos_id='%s' AND preset_name='%s'",
        eSafe.c_str(), nSafe.c_str());

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!ConnectDB()) return false;
    if (pmysql_query(g_db, sql))
    {
        Log::GetLog()->error("[DinoPainter] FindPreset failed: {}", pmysql_error(g_db));
        return false;
    }

    MYSQL_RES* res = pmysql_store_result(g_db);
    if (!res) return false;

    MYSQL_ROW row = pmysql_fetch_row(res);
    if (!row) { pmysql_free_result(res); return false; }

    out.name = row[0] ? row[0] : "";
    std::string regionStr = row[1] ? row[1] : "";
    std::string colorStr  = row[2] ? row[2] : "";
    ParseCSV(regionStr, out.regions);
    ParseCSV(colorStr, out.colors);

    pmysql_free_result(res);
    return true;
}

static int CountPresets(const std::string& eosId)
{
    std::string eSafe = EscapeUnsafe(eosId);

    char sql[256];
    snprintf(sql, sizeof(sql),
        "SELECT COUNT(*) FROM paint_presets WHERE eos_id='%s'",
        eSafe.c_str());

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!ConnectDB()) return 0;
    if (pmysql_query(g_db, sql)) return 0;

    MYSQL_RES* res = pmysql_store_result(g_db);
    if (!res) return 0;

    MYSQL_ROW row = pmysql_fetch_row(res);
    int count = (row && row[0]) ? std::atoi(row[0]) : 0;
    pmysql_free_result(res);
    return count;
}

// =============================================================================
// Chat Commands
// =============================================================================

// /paint <region> <colorID>
static void Cmd_Paint(AShooterPlayerController* pc, FString* message, int /*mode*/, int /*platform*/)
{
    const std::string eosId = GetEosId(pc);
    if (eosId.empty()) return;

    const GroupTier tier = ResolveTier(eosId);
    if (!tier.canPaint)
    {
        Notify(pc, L"You don't have permission to use /paint.");
        return;
    }

    // Parse args
    const std::string raw = FStr(*message);
    std::vector<std::string> args;
    {
        std::string token;
        for (size_t i = 0; i <= raw.size(); ++i)
        {
            if (i == raw.size() || raw[i] == ' ')
            {
                if (!token.empty()) { args.push_back(token); token.clear(); }
            }
            else
            {
                token += raw[i];
            }
        }
    }

    // args[0] = "/paint", args[1] = region, args[2] = colorID
    if (args.size() < 3)
    {
        Notify(pc, L"Usage: /paint <region 0-5> <colorID 0-255>");
        return;
    }

    int region, colorId;
    try
    {
        region  = std::stoi(args[1]);
        colorId = std::stoi(args[2]);
    }
    catch (...)
    {
        Notify(pc, L"Invalid region or color ID.");
        return;
    }

    if (region < 0 || region > 5)
    {
        Notify(pc, L"Region must be 0-5.");
        return;
    }
    if (colorId < 0 || colorId > 255)
    {
        Notify(pc, L"Color ID must be 0-255.");
        return;
    }

    // Get player team
    AActor* ch = pc->BaseGetPlayerCharacter();
    if (!ch) { Notify(pc, L"No character found."); return; }
    int playerTeam = ch->TargetingTeamField();
    if (playerTeam == 0) { Notify(pc, L"You must be in a tribe."); return; }

    APrimalDinoCharacter* dino = GetAimedDino(pc, playerTeam);
    if (!dino)
    {
        Notify(pc, L"No tribe dino found in your crosshair.");
        return;
    }

    if (!SetDinoRegionColor(dino, region, colorId))
    {
        Notify(pc, L"Failed to set color.");
        return;
    }

    Notify(pc, L"Painted region " + std::to_wstring(region) +
        L" with color " + std::to_wstring(colorId) + L".");

    Log::GetLog()->info("[DinoPainter] PAINT eos={} region={} color={}", eosId, region, colorId);
}

// /paintall <presetName> <radius>
static void Cmd_PaintAll(AShooterPlayerController* pc, FString* message, int /*mode*/, int /*platform*/)
{
    const std::string eosId = GetEosId(pc);
    if (eosId.empty()) return;

    const GroupTier tier = ResolveTier(eosId);
    if (!tier.canPaintAll)
    {
        Notify(pc, L"You don't have permission to use /paintall.");
        return;
    }

    const std::string raw = FStr(*message);
    std::vector<std::string> args;
    {
        std::string token;
        for (size_t i = 0; i <= raw.size(); ++i)
        {
            if (i == raw.size() || raw[i] == ' ')
            {
                if (!token.empty()) { args.push_back(token); token.clear(); }
            }
            else
            {
                token += raw[i];
            }
        }
    }

    // args[0] = "/paintall", args[1] = presetName, args[2] = radius (meters)
    if (args.size() < 3)
    {
        Notify(pc, L"Usage: /paintall <presetName> <radius in meters>");
        return;
    }

    const std::string presetName = args[1];
    float radiusMeters;
    try { radiusMeters = std::stof(args[2]); }
    catch (...) { Notify(pc, L"Invalid radius."); return; }

    if (radiusMeters <= 0.0f)
    {
        Notify(pc, L"Radius must be positive.");
        return;
    }

    // Convert meters to UE units (~100 UU per meter)
    float radiusUU = radiusMeters * 100.0f;
    if (radiusUU > tier.maxRadius)
    {
        Notify(pc, L"Max radius for your tier: " +
            std::to_wstring((int)(tier.maxRadius / 100.0f)) + L"m.");
        return;
    }

    // Get player team
    AActor* ch = pc->BaseGetPlayerCharacter();
    if (!ch) { Notify(pc, L"No character found."); return; }
    int playerTeam = ch->TargetingTeamField();
    if (playerTeam == 0) { Notify(pc, L"You must be in a tribe."); return; }

    // Find aimed dino to determine type
    APrimalDinoCharacter* aimedDino = GetAimedDino(pc, playerTeam);
    if (!aimedDino)
    {
        Notify(pc, L"No tribe dino found in your crosshair.");
        return;
    }

    const std::string targetBP = GetDinoBP(aimedDino);
    if (targetBP.empty())
    {
        Notify(pc, L"Could not identify dino type.");
        return;
    }

    // Load preset
    PaintPreset preset;
    if (!FindPreset(eosId, presetName, preset))
    {
        Notify(pc, L"Preset not found. Use /paintpresets to list.");
        return;
    }

    if (preset.regions.size() != preset.colors.size())
    {
        Notify(pc, L"Preset data is corrupt (region/color count mismatch).");
        return;
    }

    // Find all dinos of same type within radius
    USceneComponent* pRoot = ch->RootComponentField();
    if (!pRoot) return;
    auto pLoc = pRoot->RelativeLocationField();

    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return;
    ULevel* level = world->PersistentLevelField();
    if (!level) return;

    auto actors = level->ActorsField();
    int painted = 0;

    for (int i = 0; i < actors.Num(); ++i)
    {
        AActor* actor = actors[i];
        if (!actor) continue;
        if (!actor->IsA(APrimalDinoCharacter::StaticClass())) continue;

        APrimalDinoCharacter* dino = static_cast<APrimalDinoCharacter*>(actor);
        if (dino->TargetingTeamField() != playerTeam) continue;

        const std::string bp = GetDinoBP(dino);
        if (bp != targetBP) continue;

        USceneComponent* dRoot = dino->RootComponentField();
        if (!dRoot) continue;
        auto dLoc = dRoot->RelativeLocationField();

        double dx = dLoc.X - pLoc.X;
        double dy = dLoc.Y - pLoc.Y;
        double dz = dLoc.Z - pLoc.Z;
        double dist = std::sqrt(dx * dx + dy * dy + dz * dz);

        if (dist > (double)radiusUU) continue;

        for (size_t j = 0; j < preset.regions.size(); ++j)
            SetDinoRegionColor(dino, preset.regions[j], preset.colors[j]);

        ++painted;
    }

    Notify(pc, L"Painted " + std::to_wstring(painted) + L" dinos with preset '" +
        std::wstring(presetName.begin(), presetName.end()) + L"'.");

    Log::GetLog()->info("[DinoPainter] PAINTALL eos={} preset={} radius={}m count={}",
        eosId, presetName, radiusMeters, painted);
}

// /savepaint <name> <r0,r1,...> <c0,c1,...>
static void Cmd_SavePaint(AShooterPlayerController* pc, FString* message, int /*mode*/, int /*platform*/)
{
    const std::string eosId = GetEosId(pc);
    if (eosId.empty()) return;

    const GroupTier tier = ResolveTier(eosId);
    if (!tier.canPaint)
    {
        Notify(pc, L"You don't have permission to use /savepaint.");
        return;
    }

    const std::string raw = FStr(*message);
    std::vector<std::string> args;
    {
        std::string token;
        for (size_t i = 0; i <= raw.size(); ++i)
        {
            if (i == raw.size() || raw[i] == ' ')
            {
                if (!token.empty()) { args.push_back(token); token.clear(); }
            }
            else
            {
                token += raw[i];
            }
        }
    }

    // args[0] = "/savepaint", args[1] = name, args[2] = regions CSV, args[3] = colors CSV
    if (args.size() < 4)
    {
        Notify(pc, L"Usage: /savepaint <name> <0,1,2> <10,15,20>");
        return;
    }

    const std::string name = args[1];

    // Validate name
    if (name.empty() || name.size() > 20)
    {
        Notify(pc, L"Preset name must be 1-20 characters.");
        return;
    }
    for (char c : name)
    {
        if (!std::isalnum((unsigned char)c) && c != '_')
        {
            Notify(pc, L"Preset name: alphanumeric and underscore only.");
            return;
        }
    }

    PaintPreset preset;
    preset.name = name;

    if (!ParseCSV(args[2], preset.regions))
    {
        Notify(pc, L"Invalid regions format. Use comma-separated: 0,1,2");
        return;
    }
    if (!ParseCSV(args[3], preset.colors))
    {
        Notify(pc, L"Invalid colors format. Use comma-separated: 10,15,20");
        return;
    }

    if (preset.regions.size() != preset.colors.size())
    {
        Notify(pc, L"Region count must match color count.");
        return;
    }

    // Validate values
    for (int r : preset.regions)
    {
        if (r < 0 || r > 5)
        {
            Notify(pc, L"All regions must be 0-5.");
            return;
        }
    }
    for (int c : preset.colors)
    {
        if (c < 0 || c > 255)
        {
            Notify(pc, L"All color IDs must be 0-255.");
            return;
        }
    }

    // Check preset limit
    int existing = CountPresets(eosId);
    // Allow overwrite of existing preset
    PaintPreset tmp;
    bool isOverwrite = FindPreset(eosId, name, tmp);

    if (!isOverwrite && existing >= tier.maxPresets)
    {
        Notify(pc, L"Preset limit reached (" + std::to_wstring(tier.maxPresets) +
            L"). Delete one first.");
        return;
    }

    if (!SavePreset(eosId, preset))
    {
        Notify(pc, L"Failed to save preset.");
        return;
    }

    std::wstring wName(name.begin(), name.end());
    Notify(pc, L"Preset '" + wName + L"' saved (" +
        std::to_wstring(preset.regions.size()) + L" regions).");

    Log::GetLog()->info("[DinoPainter] SAVEPAINT eos={} preset={}", eosId, name);
}

// /paintpresets
static void Cmd_PaintPresets(AShooterPlayerController* pc, FString* /*message*/, int /*mode*/, int /*platform*/)
{
    const std::string eosId = GetEosId(pc);
    if (eosId.empty()) return;

    auto presets = GetPresets(eosId);
    if (presets.empty())
    {
        Notify(pc, L"You have no saved presets.");
        return;
    }

    Notify(pc, L"Your presets:");
    for (const auto& p : presets)
    {
        std::wstring wName(p.name.begin(), p.name.end());
        std::string detail;
        for (size_t i = 0; i < p.regions.size(); ++i)
        {
            if (i > 0) detail += ", ";
            detail += "R" + std::to_string(p.regions[i]) + "=" + std::to_string(p.colors[i]);
        }
        std::wstring wDetail(detail.begin(), detail.end());
        Notify(pc, L"  " + wName + L": " + wDetail);
    }
}

// /paintdel <name>
static void Cmd_PaintDel(AShooterPlayerController* pc, FString* message, int /*mode*/, int /*platform*/)
{
    const std::string eosId = GetEosId(pc);
    if (eosId.empty()) return;

    const std::string raw = FStr(*message);
    std::vector<std::string> args;
    {
        std::string token;
        for (size_t i = 0; i <= raw.size(); ++i)
        {
            if (i == raw.size() || raw[i] == ' ')
            {
                if (!token.empty()) { args.push_back(token); token.clear(); }
            }
            else
            {
                token += raw[i];
            }
        }
    }

    if (args.size() < 2)
    {
        Notify(pc, L"Usage: /paintdel <presetName>");
        return;
    }

    const std::string name = args[1];

    PaintPreset tmp;
    if (!FindPreset(eosId, name, tmp))
    {
        Notify(pc, L"Preset not found.");
        return;
    }

    if (!DeletePreset(eosId, name))
    {
        Notify(pc, L"Failed to delete preset.");
        return;
    }

    std::wstring wName(name.begin(), name.end());
    Notify(pc, L"Preset '" + wName + L"' deleted.");

    Log::GetLog()->info("[DinoPainter] PAINTDEL eos={} preset={}", eosId, name);
}

// =============================================================================
// Config Hot-Reload
// =============================================================================

static int g_reload_counter = 0;

static void OnTimerCallback()
{
    if (++g_reload_counter >= 10)
    {
        g_reload_counter = 0;
        LoadConfig();
    }
}

// =============================================================================
// Plugin Init / Shutdown
// =============================================================================

static void PluginInit()
{
    Log::Get().Init("DinoPainter");
    Log::GetLog()->info("[DinoPainter] Init");

    LoadConfig();

    if (!LoadMySQLLib())
    {
        Log::GetLog()->error("[DinoPainter] DB library load failed - plugin disabled");
        return;
    }

    EnsureTable();

    auto& cmds = AsaApi::GetCommands();
    cmds.AddChatCommand(L"/paint",        &Cmd_Paint);
    cmds.AddChatCommand(L"/paintall",     &Cmd_PaintAll);
    cmds.AddChatCommand(L"/savepaint",    &Cmd_SavePaint);
    cmds.AddChatCommand(L"/paintpresets", &Cmd_PaintPresets);
    cmds.AddChatCommand(L"/paintdel",     &Cmd_PaintDel);

    AsaApi::GetCommands().AddOnTimerCallback(L"DinoPainterReload", &OnTimerCallback);

    Log::GetLog()->info("[DinoPainter] Ready - 5 commands registered, hot-reload enabled");
}

static void PluginUnload()
{
    AsaApi::GetCommands().RemoveOnTimerCallback(L"DinoPainterReload");

    auto& cmds = AsaApi::GetCommands();
    cmds.RemoveChatCommand(L"/paint");
    cmds.RemoveChatCommand(L"/paintall");
    cmds.RemoveChatCommand(L"/savepaint");
    cmds.RemoveChatCommand(L"/paintpresets");
    cmds.RemoveChatCommand(L"/paintdel");

    {
        std::lock_guard<std::mutex> lock(g_db_mutex);
        if (g_db) { pmysql_close(g_db); g_db = nullptr; }
    }

    Log::GetLog()->info("[DinoPainter] Unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { PluginInit(); }
    catch (const std::exception& e)
    {
        Log::Get().Init("DinoPainter");
        Log::GetLog()->critical("[DinoPainter] Init crashed: {}", e.what());
    }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { PluginUnload(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->critical("[DinoPainter] Unload crashed: {}", e.what());
    }
}