/*
CloudStorage - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * CloudStorage - ASA Plugin
 *
 * Hooks: None - chat commands and config reload timer only
 *
 * Config (ArkApi/Plugins/CloudStorage/config.json):
 *   DbHost, DbPort, DbUser, DbPassword, DbName
 *   MessageColor
 *   UploadCommand   - chat command for uploading (default "/upload")
 *   DownloadCommand - chat command for downloading (default "/download")
 *   ListCommand     - chat command for listing storage (default "/ulist")
 *   DefaultSlots  - slots granted when a player matches no configured group
 *   Groups        - map of permission group name to slot count
 *   Abbreviations - map of friendly item name to abbreviation, e.g.
 *                   "Advanced Rifle Bullet": "arb"
 *   BlacklistedBlueprints - array of exact blueprint paths blocked from upload
 *
 * Table:
 *   cloudstorage - PK (eos_id, item_name)
 *   Columns: eos_id, item_name, item_bp_path, quantity, slots, stack, updated_at
 *
 * Commands (names configurable via UploadCommand / DownloadCommand / ListCommand):
 *   /upload {item} {amount|all}   - moves items from inventory into storage
 *   /download {item} {amount|all} - moves items from storage into inventory
 *   /ulist [page]                 - lists the caller's stored items, paginated
 *
 * Slot model:
 *   A slot is one in-game stack. Slots used by an item equal
 *   ceil(quantity / max stack size). Stored per row so capacity is a SUM.
 */

#include <API/ARK/Ark.h>
#include <json.hpp>
#include <Windows.h>
#include <fstream>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <sstream>
#include <mutex>
#include <unordered_map>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <sys/stat.h>

#pragma warning(disable: 4191)
#pragma comment(lib, "AsaApi")

// =============================================================================
// MariaDB - Dynamic Load
// =============================================================================

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
            Log::GetLog()->info("[CloudStorage] Loaded DB library: {}", candidates[i]);
            break;
        }
    }

    if (!g_mysql_module)
    {
        Log::GetLog()->error("[CloudStorage] Could not find libmariadb.dll or libmysql.dll");
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
        !pmysql_error || !pmysql_real_escape_string)
    {
        Log::GetLog()->error("[CloudStorage] Failed to resolve required DB functions");
        return false;
    }

    g_mysql_loaded = true;
    return true;
}

// =============================================================================
// Configuration
// =============================================================================

static const std::string g_config_path = "ArkApi/Plugins/CloudStorage/config.json";

static std::string  g_db_host = "127.0.0.1";
static unsigned int g_db_port = 3306;
static std::string  g_db_user;
static std::string  g_db_pass;
static std::string  g_db_name;

static std::string g_message_color = "0, 1, 0.65, 1";
static int         g_default_slots = 5;
static bool        g_allow_blueprints = false;
static std::string g_cmd_upload = "/upload";
static std::string g_cmd_download = "/download";
static std::string g_cmd_list = "/ulist";
static std::string g_cmd_uploadall = "/uploadall";
static std::string g_cmd_downloadall = "/downloadall";

static std::wstring g_reg_upload;
static std::wstring g_reg_download;
static std::wstring g_reg_list;
static std::wstring g_reg_uploadall;
static std::wstring g_reg_downloadall;

static std::unordered_map<std::string, int>         g_groups;
static std::unordered_map<std::string, std::string> g_abbrev_to_name;
static std::set<std::string>                        g_blacklist_bp;

static time_t    g_config_last_modified = 0;
static uintmax_t g_config_last_size = 0;

static std::string ToLower(const std::string& s)
{
    std::string out = s;
    for (char& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

static bool LoadConfig()
{
    std::ifstream file(g_config_path);
    if (!file.is_open())
    {
        Log::GetLog()->error("[CloudStorage] Cannot open config: {}", g_config_path);
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
        g_message_color = j.value("MessageColor", "0, 1, 0.65, 1");
        g_default_slots = j.value("DefaultSlots", 5);
        g_allow_blueprints = j.value("AllowBlueprints", false);
        g_cmd_upload = j.value("UploadCommand", "/upload");
        g_cmd_download = j.value("DownloadCommand", "/download");
        g_cmd_list = j.value("ListCommand", "/ulist");
        g_cmd_uploadall = j.value("UploadAllCommand", "/uploadall");
        g_cmd_downloadall = j.value("DownloadAllCommand", "/downloadall");

        std::unordered_map<std::string, int> groups;
        if (j.contains("Groups") && j["Groups"].is_object())
        {
            for (auto& [key, val] : j["Groups"].items())
                groups[key] = val.get<int>();
        }
        g_groups = std::move(groups);

        std::unordered_map<std::string, std::string> abbrevs;
        if (j.contains("Abbreviations") && j["Abbreviations"].is_object())
        {
            for (auto& [name, abbrev] : j["Abbreviations"].items())
                abbrevs[ToLower(abbrev.get<std::string>())] = name;
        }
        g_abbrev_to_name = std::move(abbrevs);

        std::set<std::string> blacklist;
        if (j.contains("BlacklistedBlueprints") && j["BlacklistedBlueprints"].is_array())
        {
            for (auto& entry : j["BlacklistedBlueprints"])
                if (entry.is_string()) blacklist.insert(entry.get<std::string>());
        }
        g_blacklist_bp = std::move(blacklist);
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[CloudStorage] Config parse error: {}", ex.what());
        return false;
    }

    if (g_db_user.empty() || g_db_name.empty())
    {
        Log::GetLog()->error("[CloudStorage] Config requires DbUser and DbName");
        return false;
    }

    Log::GetLog()->info("[CloudStorage] Config loaded: {} group(s), {} abbreviation(s), default_slots={}",
        g_groups.size(), g_abbrev_to_name.size(), g_default_slots);
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
        Log::GetLog()->error("[CloudStorage] Query error: {}", pmysql_error(g_mysql));
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
        Log::GetLog()->error("[CloudStorage] mysql_init failed");
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
        Log::GetLog()->error("[CloudStorage] DB connect failed: {}", pmysql_error(g_mysql));
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
    Log::GetLog()->info("[CloudStorage] DB reconnected");
    return true;
}

static bool InitDatabase()
{
    if (!LoadMySQLLib()) return false;
    if (!OpenConnection()) return false;

    const bool ok = ExecQuery(
        "CREATE TABLE IF NOT EXISTS cloudstorage ("
        "  eos_id       VARCHAR(64)      NOT NULL,"
        "  item_name    VARCHAR(128)     NOT NULL,"
        "  item_bp_path VARCHAR(512)     NOT NULL,"
        "  quantity     BIGINT UNSIGNED  NOT NULL DEFAULT 0,"
        "  slots        INT UNSIGNED     NOT NULL DEFAULT 0,"
        "  stack        INT UNSIGNED     NOT NULL DEFAULT 1,"
        "  updated_at   TIMESTAMP        NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
        "  PRIMARY KEY (eos_id, item_name)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4"
    );

    if (!ok)
    {
        Log::GetLog()->error("[CloudStorage] Failed to create cloudstorage table");
        pmysql_close(g_mysql);
        g_mysql = nullptr;
        return false;
    }

    ExecQuery("ALTER TABLE cloudstorage ADD COLUMN IF NOT EXISTS stack INT UNSIGNED NOT NULL DEFAULT 1");

    const bool okInstances = ExecQuery(
        "CREATE TABLE IF NOT EXISTS cloudstorage_instances ("
        "  id         BIGINT UNSIGNED  NOT NULL AUTO_INCREMENT,"
        "  eos_id     VARCHAR(64)      NOT NULL,"
        "  item_name  VARCHAR(128)     NOT NULL,"
        "  item_blob  MEDIUMTEXT       NOT NULL,"
        "  updated_at TIMESTAMP        NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
        "  PRIMARY KEY (id),"
        "  INDEX idx_eos (eos_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4"
    );

    if (!okInstances)
    {
        Log::GetLog()->error("[CloudStorage] Failed to create cloudstorage_instances table");
        pmysql_close(g_mysql);
        g_mysql = nullptr;
        return false;
    }

    Log::GetLog()->info("[CloudStorage] Database ready");
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

static bool GetStoredItem(const std::string& eosId, const std::string& itemName,
    long long& outQty, int& outSlots, int& outStack, std::string& outName, std::string& outPath)
{
    outQty = 0;
    outSlots = 0;
    outStack = 1;
    outName.clear();
    outPath.clear();

    const std::string eEos = EscapeUnsafe(eosId);
    const std::string eName = EscapeUnsafe(itemName);

    const std::string sql =
        "SELECT quantity, slots, stack, item_name, item_bp_path FROM cloudstorage WHERE eos_id='" + eEos +
        "' AND item_name='" + eName + "'";

    if (!g_mysql) return false;
    if (pmysql_query(g_mysql, sql.c_str()) != 0)
    {
        Log::GetLog()->error("[CloudStorage] GetStoredItem query error: {}", pmysql_error(g_mysql));
        return false;
    }

    MYSQL_RES* res = pmysql_store_result(g_mysql);
    if (!res) return false;

    bool found = false;
    if (MYSQL_ROW row = pmysql_fetch_row(res))
    {
        if (row[0]) outQty = std::atoll(row[0]);
        if (row[1]) outSlots = std::atoi(row[1]);
        if (row[2]) outStack = std::atoi(row[2]);
        if (row[3]) outName = row[3];
        if (row[4]) outPath = row[4];
        found = true;
    }

    pmysql_free_result(res);
    return found;
}

static long long GetTotalSlots(const std::string& eosId)
{
    const std::string eEos = EscapeUnsafe(eosId);
    const std::string sql =
        "SELECT COALESCE(SUM(slots),0) FROM cloudstorage WHERE eos_id='" + eEos + "'";

    if (!g_mysql) return 0;
    if (pmysql_query(g_mysql, sql.c_str()) != 0)
    {
        Log::GetLog()->error("[CloudStorage] GetTotalSlots query error: {}", pmysql_error(g_mysql));
        return 0;
    }

    MYSQL_RES* res = pmysql_store_result(g_mysql);
    if (!res) return 0;

    long long total = 0;
    if (MYSQL_ROW row = pmysql_fetch_row(res))
        if (row[0]) total = std::atoll(row[0]);

    pmysql_free_result(res);
    return total;
}

struct StoredEntry { std::string name; long long qty; int slots; std::string path; int stack; };

static std::vector<StoredEntry> GetAllStored(const std::string& eosId)
{
    std::vector<StoredEntry> out;

    const std::string eEos = EscapeUnsafe(eosId);
    const std::string sql =
        "SELECT item_name, quantity, slots, item_bp_path, stack FROM cloudstorage WHERE eos_id='" + eEos +
        "' ORDER BY item_name";

    if (!g_mysql) return out;
    if (pmysql_query(g_mysql, sql.c_str()) != 0)
    {
        Log::GetLog()->error("[CloudStorage] GetAllStored query error: {}", pmysql_error(g_mysql));
        return out;
    }

    MYSQL_RES* res = pmysql_store_result(g_mysql);
    if (!res) return out;

    while (MYSQL_ROW row = pmysql_fetch_row(res))
    {
        StoredEntry e;
        e.name = row[0] ? row[0] : "";
        e.qty = row[1] ? std::atoll(row[1]) : 0;
        e.slots = row[2] ? std::atoi(row[2]) : 0;
        e.path = row[3] ? row[3] : "";
        e.stack = row[4] ? std::atoi(row[4]) : 1;
        out.push_back(e);
    }

    pmysql_free_result(res);
    return out;
}

static long long GetInstanceCount(const std::string& eosId)
{
    const std::string eEos = EscapeUnsafe(eosId);
    const std::string sql =
        "SELECT COUNT(*) FROM cloudstorage_instances WHERE eos_id='" + eEos + "'";

    if (!g_mysql) return 0;
    if (pmysql_query(g_mysql, sql.c_str()) != 0)
    {
        Log::GetLog()->error("[CloudStorage] GetInstanceCount query error: {}", pmysql_error(g_mysql));
        return 0;
    }

    MYSQL_RES* res = pmysql_store_result(g_mysql);
    if (!res) return 0;

    long long n = 0;
    if (MYSQL_ROW row = pmysql_fetch_row(res))
        if (row[0]) n = std::atoll(row[0]);

    pmysql_free_result(res);
    return n;
}

static bool WriteInstance(const std::string& eosId, const std::string& itemName, const std::string& blob)
{
    const std::string eEos = EscapeUnsafe(eosId);
    const std::string eName = EscapeUnsafe(itemName);
    const std::string eBlob = EscapeUnsafe(blob);

    const std::string sql =
        "INSERT INTO cloudstorage_instances (eos_id, item_name, item_blob) VALUES ('" +
        eEos + "', '" + eName + "', '" + eBlob + "')";

    return ExecQuery(sql);
}

struct InstanceRow { long long id; std::string name; bool blueprint = false; };

static bool GetInstanceById(const std::string& eosId, long long id, std::string& outName, std::string& outBlob)
{
    const std::string eEos = EscapeUnsafe(eosId);
    char idbuf[32];
    std::snprintf(idbuf, sizeof(idbuf), "%lld", id);

    const std::string sql =
        "SELECT item_name, item_blob FROM cloudstorage_instances WHERE eos_id='" + eEos +
        "' AND id=" + idbuf + " LIMIT 1";

    if (!g_mysql) return false;
    if (pmysql_query(g_mysql, sql.c_str()) != 0)
    {
        Log::GetLog()->error("[CloudStorage] GetInstanceById query error: {}", pmysql_error(g_mysql));
        return false;
    }

    MYSQL_RES* res = pmysql_store_result(g_mysql);
    if (!res) return false;

    bool found = false;
    if (MYSQL_ROW row = pmysql_fetch_row(res))
    {
        outName = row[0] ? row[0] : "";
        outBlob = row[1] ? row[1] : "";
        found = true;
    }

    pmysql_free_result(res);
    return found;
}

static bool DeleteInstance(const std::string& eosId, long long id)
{
    const std::string eEos = EscapeUnsafe(eosId);
    char idbuf[32];
    std::snprintf(idbuf, sizeof(idbuf), "%lld", id);

    const std::string sql =
        "DELETE FROM cloudstorage_instances WHERE eos_id='" + eEos + "' AND id=" + idbuf;

    return ExecQuery(sql);
}

static std::vector<InstanceRow> GetInstanceList(const std::string& eosId)
{
    std::vector<InstanceRow> out;
    const std::string eEos = EscapeUnsafe(eosId);

    const std::string sql =
        "SELECT id, item_name, item_blob FROM cloudstorage_instances WHERE eos_id='" + eEos + "' ORDER BY id";

    if (!g_mysql) return out;
    if (pmysql_query(g_mysql, sql.c_str()) != 0)
    {
        Log::GetLog()->error("[CloudStorage] GetInstanceList query error: {}", pmysql_error(g_mysql));
        return out;
    }

    MYSQL_RES* res = pmysql_store_result(g_mysql);
    if (!res) return out;

    while (MYSQL_ROW row = pmysql_fetch_row(res))
    {
        InstanceRow r;
        r.id = row[0] ? std::atoll(row[0]) : 0;
        r.name = row[1] ? row[1] : "";
        if (row[2])
        {
            try { r.blueprint = (nlohmann::json::parse(row[2]).value("kind", std::string()) == "blueprint"); }
            catch (...) { r.blueprint = false; }
        }
        out.push_back(r);
    }

    pmysql_free_result(res);
    return out;
}

static bool WriteStoredItem(const std::string& eosId, const std::string& itemName,
    const std::string& path, long long quantity, int slots, int stack)
{
    const std::string eEos = EscapeUnsafe(eosId);
    const std::string eName = EscapeUnsafe(itemName);
    const std::string ePath = EscapeUnsafe(path);

    if (quantity <= 0)
    {
        const std::string del =
            "DELETE FROM cloudstorage WHERE eos_id='" + eEos +
            "' AND item_name='" + eName + "'";
        return ExecQuery(del);
    }

    char tail[96];
    std::snprintf(tail, sizeof(tail), "%lld, %d, %d", quantity, slots, stack);

    const std::string sql =
        "INSERT INTO cloudstorage (eos_id, item_name, item_bp_path, quantity, slots, stack) VALUES ('" +
        eEos + "', '" + eName + "', '" + ePath + "', " + std::string(tail) +
        ") ON DUPLICATE KEY UPDATE item_name=VALUES(item_name), item_bp_path=VALUES(item_bp_path), quantity=VALUES(quantity), slots=VALUES(slots), stack=VALUES(stack)";

    return ExecQuery(sql);
}

// =============================================================================
// Permissions - Dynamic Load
// =============================================================================

typedef TArray<FString>(*GetPlayerGroups_t)(const FString&);

static GetPlayerGroups_t pGetPlayerGroups = nullptr;
static bool              g_permissions_loaded = false;
static bool              g_permissions_attempted = false;

static void LoadPermissionsAPI()
{
    if (g_permissions_attempted) return;
    g_permissions_attempted = true;

    HMODULE hMod = GetModuleHandleA("Permissions");
    if (!hMod)
    {
        Log::GetLog()->warn("[CloudStorage] Permissions.dll not found, using default slots");
        return;
    }

    pGetPlayerGroups = (GetPlayerGroups_t)GetProcAddress(hMod,
        "?GetPlayerGroups@Permissions@@YA?AV?$TArray@VFString@@V?$TSizedDefaultAllocator@$0CA@@@@@AEBVFString@@@Z");

    if (!pGetPlayerGroups)
    {
        Log::GetLog()->warn("[CloudStorage] Failed to resolve Permissions functions, using default slots");
        return;
    }

    g_permissions_loaded = true;
    Log::GetLog()->info("[CloudStorage] Permissions API loaded");
}

static int ResolveSlots(const std::string& eosId)
{
    int best = g_default_slots;

    if (g_groups.empty()) return best;

    if (!g_permissions_attempted) LoadPermissionsAPI();
    if (!g_permissions_loaded || !pGetPlayerGroups) return best;

    try
    {
        FString fEos(eosId.c_str());
        TArray<FString> groups = pGetPlayerGroups(fEos);

        for (int i = 0; i < groups.Num(); ++i)
        {
            const char* raw = TCHAR_TO_UTF8(*groups[i]);
            const std::string groupName = raw ? raw : "";
            auto it = g_groups.find(groupName);
            if (it != g_groups.end() && it->second > best)
                best = it->second;
        }
    }
    catch (...)
    {
        Log::GetLog()->warn("[CloudStorage] GetPlayerGroups threw, using default slots");
    }

    return best;
}

// =============================================================================
// Helpers
// =============================================================================

static std::string FStr(const FString& f)
{
    if (f.IsEmpty()) return "";
    return std::string(static_cast<const char*>(TCHAR_TO_UTF8(*f)));
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

static UPrimalInventoryComponent* GetPlayerInventory(AShooterPlayerController* pc)
{
    if (!pc) return nullptr;
    AActor* ch = pc->BaseGetPlayerCharacter();
    if (!ch) return nullptr;
    AShooterCharacter* character = static_cast<AShooterCharacter*>(ch);
    return character->MyInventoryComponentField();
}

static std::vector<std::string> SplitWords(const std::string& s)
{
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string word;
    while (iss >> word) out.push_back(word);
    return out;
}

enum class NameMatch { Resolved, Ambiguous, None };

static NameMatch ResolveAbbrevName(const std::string& phrase,
    std::string& outName, std::vector<std::string>& outCandidates)
{
    const std::string pl = ToLower(phrase);
    std::map<std::string, std::string> exact;
    std::map<std::string, std::string> prefix;

    for (auto& [abbrevLower, name] : g_abbrev_to_name)
    {
        if (abbrevLower == pl) exact[abbrevLower] = name;
        else if (abbrevLower.rfind(pl, 0) == 0) prefix[abbrevLower] = name;
    }

    std::map<std::string, std::string>& pick = !exact.empty() ? exact : prefix;
    if (pick.empty()) return NameMatch::None;
    if (pick.size() == 1) { outName = pick.begin()->second; return NameMatch::Resolved; }
    for (auto& [k, v] : pick) outCandidates.push_back(v);
    return NameMatch::Ambiguous;
}

static bool IsAmountToken(const std::string& s)
{
    if (s.empty()) return false;
    if (ToLower(s) == "all") return true;
    for (char c : s) if (!std::isdigit((unsigned char)c)) return false;
    return true;
}

static NameMatch ResolveInventoryName(UPrimalInventoryComponent* inv, const std::string& phrase,
    std::string& outName, std::vector<std::string>& outCandidates)
{
    const std::string pl = ToLower(phrase);
    std::map<std::string, std::string> exact;
    std::map<std::string, std::string> prefix;

    TArray<UPrimalItem*>& items = inv->InventoryItemsField();
    for (int i = 0; i < items.Num(); ++i)
    {
        UPrimalItem* item = items[i];
        if (!item) continue;
        if (item->bIsBlueprint()()) continue;
        if (item->IsItemSkin(true)) continue;
        const std::string disp = FStr(item->DescriptiveNameBaseField());
        if (disp.empty()) continue;
        const std::string dl = ToLower(disp);
        if (dl == pl) exact[dl] = disp;
        else if (dl.rfind(pl, 0) == 0) prefix[dl] = disp;
    }

    std::map<std::string, std::string>& pick = !exact.empty() ? exact : prefix;
    if (pick.empty()) return NameMatch::None;
    if (pick.size() == 1) { outName = pick.begin()->second; return NameMatch::Resolved; }
    for (auto& [k, v] : pick) outCandidates.push_back(v);
    return NameMatch::Ambiguous;
}

static bool IsRealBlueprint(UPrimalItem* item)
{
    if (!item) return false;
    return item->bIsBlueprint()() && !item->bIsEngram()();
}

static NameMatch ResolveBlueprintName(UPrimalInventoryComponent* inv, const std::string& phrase,
    std::string& outName, std::vector<std::string>& outCandidates)
{
    const std::string pl = ToLower(phrase);
    std::map<std::string, std::string> exact;
    std::map<std::string, std::string> prefix;

    TArray<UPrimalItem*>& items = inv->InventoryItemsField();
    for (int i = 0; i < items.Num(); ++i)
    {
        UPrimalItem* item = items[i];
        if (!item) continue;
        if (!IsRealBlueprint(item)) continue;
        const std::string disp = FStr(item->DescriptiveNameBaseField());
        if (disp.empty()) continue;
        const std::string dl = ToLower(disp);
        if (dl == pl) exact[dl] = disp;
        else if (dl.rfind(pl, 0) == 0) prefix[dl] = disp;
    }

    std::map<std::string, std::string>& pick = !exact.empty() ? exact : prefix;
    if (pick.empty()) return NameMatch::None;
    if (pick.size() == 1) { outName = pick.begin()->second; return NameMatch::Resolved; }
    for (auto& [k, v] : pick) outCandidates.push_back(v);
    return NameMatch::Ambiguous;
}

static int SlotsFor(long long quantity, int stack)
{
    if (stack <= 0) stack = 1;
    if (quantity <= 0) return 0;
    return (int)((quantity + stack - 1) / stack);
}

static bool HoldsMatchingBlueprint(UPrimalInventoryComponent* inv, const std::string& phrase)
{
    if (!inv) return false;
    const std::string pl = ToLower(phrase);
    TArray<UPrimalItem*>& items = inv->InventoryItemsField();
    for (int i = 0; i < items.Num(); ++i)
    {
        UPrimalItem* item = items[i];
        if (!item) continue;
        if (!item->bIsBlueprint()()) continue;
        if (item->bIsEngram()()) continue;
        const std::string dl = ToLower(FStr(item->DescriptiveNameBaseField()));
        if (dl.empty()) continue;
        if (dl == pl || dl.rfind(pl, 0) == 0) return true;
    }
    return false;
}

static NameMatch ResolveStoredName(const std::string& eosId, const std::string& phrase,
    std::string& outName, std::vector<std::string>& outCandidates)
{
    const std::string pl = ToLower(phrase);
    std::vector<StoredEntry> entries = GetAllStored(eosId);
    std::map<std::string, std::string> exact;
    std::map<std::string, std::string> prefix;

    for (auto& e : entries)
    {
        const std::string dl = ToLower(e.name);
        if (dl == pl) exact[dl] = e.name;
        else if (dl.rfind(pl, 0) == 0) prefix[dl] = e.name;
    }

    std::map<std::string, std::string>& pick = !exact.empty() ? exact : prefix;
    if (pick.empty()) return NameMatch::None;
    if (pick.size() == 1) { outName = pick.begin()->second; return NameMatch::Resolved; }
    for (auto& [k, v] : pick) outCandidates.push_back(v);
    return NameMatch::Ambiguous;
}

static bool ExactInventoryName(UPrimalInventoryComponent* inv, const std::string& phrase, std::string& outName)
{
    const std::string pl = ToLower(phrase);
    TArray<UPrimalItem*>& items = inv->InventoryItemsField();
    for (int i = 0; i < items.Num(); ++i)
    {
        UPrimalItem* item = items[i];
        if (!item) continue;
        if (item->bIsBlueprint()()) continue;
        if (item->IsItemSkin(true)) continue;
        const std::string disp = FStr(item->DescriptiveNameBaseField());
        if (!disp.empty() && ToLower(disp) == pl) { outName = disp; return true; }    }
    return false;
}

static bool ExactStoredName(const std::string& eosId, const std::string& phrase, std::string& outName)
{
    const std::string pl = ToLower(phrase);
    std::vector<StoredEntry> entries = GetAllStored(eosId);
    for (auto& e : entries)
        if (ToLower(e.name) == pl) { outName = e.name; return true; }
    return false;
}

static NameMatch ResolveInstanceName(const std::vector<InstanceRow>& insts, const std::string& phrase,
    std::string& outName, std::vector<std::string>& outCandidates)
{
    const std::string pl = ToLower(phrase);
    std::map<std::string, std::string> exact;
    std::map<std::string, std::string> prefix;

    for (auto& r : insts)
    {
        const std::string dl = ToLower(r.name);
        if (dl == pl) exact[dl] = r.name;
        else if (dl.rfind(pl, 0) == 0) prefix[dl] = r.name;
    }

    std::map<std::string, std::string>& pick = !exact.empty() ? exact : prefix;
    if (pick.empty()) return NameMatch::None;
    if (pick.size() == 1) { outName = pick.begin()->second; return NameMatch::Resolved; }
    for (auto& [k, v] : pick) outCandidates.push_back(v);
    return NameMatch::Ambiguous;
}

static bool ItemIsGear(UPrimalItem* item)
{
    if (!item) return false;
    return item->bUseItemDurability()() && item->CustomItemDatasField().Num() == 0;
}

static bool ItemHasNestedData(UPrimalItem* item)
{
    if (!item) return false;
    return item->CustomItemDatasField().Num() > 0;
}

static std::string GearToJson(UPrimalItem* item)
{
    FItemNetInfo* info = AsaApi::IApiUtils::AllocateStruct<FItemNetInfo>();
    if (!info) return "";
    item->GetItemNetInfo(info, false);

    nlohmann::json j;
    j["v"] = 1;
    j["kind"] = IsRealBlueprint(item) ? "blueprint" : "gear";
    j["bp"] = FStr(AsaApi::GetApiUtils().GetBlueprint(item));
    j["qty"] = (unsigned int)info->ItemQuantityField();
    j["durability"] = info->ItemDurabilityField();
    j["rating"] = item->ItemRatingField();
    j["qualityIndex"] = (int)item->ItemQualityIndexField();
    j["craftingSkill"] = info->CraftingSkillField();
    j["craftedBonus"] = info->CraftedSkillBonusField();
    j["crafterChar"] = FStr(info->CrafterCharacterNameField());
    j["crafterTribe"] = FStr(info->CrafterTribeNameField());
    j["customName"] = FStr(info->CustomItemNameField());
    j["customDesc"] = FStr(info->CustomItemDescriptionField());
    j["clipAmmo"] = (unsigned int)info->WeaponClipAmmoField();

    nlohmann::json stats = nlohmann::json::array();
    unsigned short* sv = info->ItemStatValuesField()();
    if (sv) for (int i = 0; i < 8; ++i) stats.push_back((unsigned int)sv[i]);
    j["stats"] = stats;

    nlohmann::json colors = nlohmann::json::array();
    short* cv = info->ItemColorIDField()();
    if (cv) for (int i = 0; i < 6; ++i) colors.push_back((int)cv[i]);
    j["colors"] = colors;

    AsaApi::IApiUtils::FreeStruct(info);
    return j.dump();
}

static bool JsonToInventory(UPrimalInventoryComponent* inv, const std::string& blob)
{
    if (!inv) return false;

    nlohmann::json j;
    try { j = nlohmann::json::parse(blob); }
    catch (...) { return false; }

    const std::string kind = j.value("kind", std::string("gear"));
    const bool isBp = (kind == "blueprint");
    if (kind != "gear" && !isBp) return false;

    const std::string bp = j.value("bp", std::string());
    if (bp.empty()) return false;

    const std::wstring wPath(bp.begin(), bp.end());
    FString fPath(wPath.c_str());
    UClass* cls = UVictoryCore::BPLoadClass(fPath);
    if (!cls) return false;

    int qty = (int)j.value("qty", 1u);
    if (qty < 1) qty = 1;

    TSubclassOf<UPrimalItem> itemSub = cls;
    TSubclassOf<UPrimalItem> noSkin{};
    UPrimalItem* item = UPrimalItem::AddNewItem(
        itemSub, inv, false, false, 0.0f, false, qty, isBp, 0.0f, true,
        noSkin, 0.0f, false, true, true, true, true, false, AsaApi::GetApiUtils().GetWorld());
    if (!item) return false;

    item->ItemDurabilityField() = j.value("durability", 0.0f);
    item->ItemRatingField() = j.value("rating", 0.0f);
    item->ItemQualityIndexField() = (unsigned char)j.value("qualityIndex", 0);
    item->CraftingSkillField() = j.value("craftingSkill", 0.0f);
    item->CraftedSkillBonusField() = j.value("craftedBonus", 0.0f);
    item->WeaponClipAmmoField() = (int)j.value("clipAmmo", 0u);

    {
        const std::string s = j.value("crafterChar", std::string());
        const std::wstring w(s.begin(), s.end());
        item->CrafterCharacterNameField() = FString(w.c_str());
    }
    {
        const std::string s = j.value("crafterTribe", std::string());
        const std::wstring w(s.begin(), s.end());
        item->CrafterTribeNameField() = FString(w.c_str());
    }
    {
        const std::string s = j.value("customName", std::string());
        const std::wstring w(s.begin(), s.end());
        item->CustomItemNameField() = FString(w.c_str());
    }
    {
        const std::string s = j.value("customDesc", std::string());
        const std::wstring w(s.begin(), s.end());
        item->CustomItemDescriptionField() = FString(w.c_str());
    }

    if (j.contains("stats") && j["stats"].is_array())
    {
        unsigned short* sv = item->ItemStatValuesField()();
        if (sv)
            for (int i = 0; i < 8 && i < (int)j["stats"].size(); ++i)
                sv[i] = (unsigned short)j["stats"][i].get<unsigned int>();
    }
    if (j.contains("colors") && j["colors"].is_array())
    {
        short* cv = item->ItemColorIDField()();
        if (cv)
            for (int i = 0; i < 6 && i < (int)j["colors"].size(); ++i)
                cv[i] = (short)j["colors"][i].get<int>();
    }

    item->UpdatedItem(true, false);
    return true;
}

// =============================================================================
// Command - /upload
// =============================================================================

static void UploadCommand(AShooterPlayerController* pc, FString* message, int, int)
{
    if (!pc) return;

    const std::string eosId = GetEosId(pc);
    if (eosId.empty()) { Notify(pc, L"Could not resolve your id."); return; }

    std::vector<std::string> tokens = SplitWords(FStr(*message));
    if (tokens.size() < 2)
    {
        Notify(pc, L"Usage: /upload {item} [amount or all]");
        return;
    }

    bool wantBlueprint = false;
    size_t phraseStart = 1;
    if (ToLower(tokens[1]) == "bp") { wantBlueprint = true; phraseStart = 2; }

    std::string amountStr = "all";
    size_t itemEnd = tokens.size();
    if (tokens.size() >= phraseStart + 2 && IsAmountToken(tokens.back()))
    {
        amountStr = ToLower(tokens.back());
        itemEnd = tokens.size() - 1;
    }

    std::string phrase;
    for (size_t i = phraseStart; i < itemEnd; ++i)
    {
        if (!phrase.empty()) phrase += " ";
        phrase += tokens[i];
    }
    if (phrase.empty()) { Notify(pc, L"Usage: /upload {item} [amount or all]"); return; }

    UPrimalInventoryComponent* inv = GetPlayerInventory(pc);
    if (!inv) { Notify(pc, L"No inventory found."); return; }

    if (wantBlueprint && !g_allow_blueprints) { Notify(pc, L"Blueprints can't be stored."); return; }

    std::string itemName;
    if (wantBlueprint)
    {
        std::string resolved;
        std::vector<std::string> candidates;
        const NameMatch m = ResolveBlueprintName(inv, phrase, resolved, candidates);
        if (m == NameMatch::None) { Notify(pc, L"No matching blueprint."); return; }
        if (m == NameMatch::Ambiguous)
        {
            std::string list;
            for (size_t i = 0; i < candidates.size(); ++i) { if (i) list += ", "; list += candidates[i]; }
            const std::wstring wList(list.begin(), list.end());
            Notify(pc, L"Multiple matches: " + wList + L". Be more specific.");
            return;
        }
        itemName = resolved;
    }
    else
    {
        std::string abbrevTarget;
        std::vector<std::string> abbrevCands;
        const NameMatch am = ResolveAbbrevName(phrase, abbrevTarget, abbrevCands);
        if (am == NameMatch::Ambiguous)
        {
            std::string list;
            for (size_t i = 0; i < abbrevCands.size(); ++i) { if (i) list += ", "; list += abbrevCands[i]; }
            const std::wstring wList(list.begin(), list.end());
            Notify(pc, L"Multiple matches: " + wList + L". Be more specific.");
            return;
        }
        if (am == NameMatch::Resolved)
        {
            std::string exactName;
            if (ExactInventoryName(inv, phrase, exactName) && ToLower(exactName) != ToLower(abbrevTarget))
            {
                const std::string list = abbrevTarget + ", " + exactName;
                const std::wstring wList(list.begin(), list.end());
                Notify(pc, L"Multiple matches: " + wList + L". Be more specific.");
                return;
            }
            itemName = abbrevTarget;
        }
    }
    if (itemName.empty() && !wantBlueprint)
    {
        std::string resolved;
        std::vector<std::string> candidates;
        const NameMatch m = ResolveInventoryName(inv, phrase, resolved, candidates);
        if (m == NameMatch::None)
        {
            if (HoldsMatchingBlueprint(inv, phrase))
            {
                if (g_allow_blueprints) Notify(pc, L"That's a blueprint - use the bp prefix to store it.");
                else Notify(pc, L"Blueprints can't be stored.");
                return;
            }
            Notify(pc, L"No matching item."); return;
        }
        if (m == NameMatch::Ambiguous)
        {
            std::string list;
            for (size_t i = 0; i < candidates.size(); ++i) { if (i) list += ", "; list += candidates[i]; }
            const std::wstring wList(list.begin(), list.end());
            Notify(pc, L"Multiple matches: " + wList + L". Be more specific.");
            return;
        }
        itemName = resolved;
    }

    UWorld* world = AsaApi::GetApiUtils().GetWorld();

    const std::string nameLower = ToLower(itemName);
    long long available = 0;
    int stack = 1;
    std::string bpPath;
    std::string realName;

    struct MatchStack { FItemNetID id; long long qty; };
    std::vector<MatchStack> matches;
    std::vector<UPrimalItem*> instanceItems;
    std::string instanceName;
    bool sawBlocked = false;

    TArray<UPrimalItem*>& items = inv->InventoryItemsField();
    for (int i = 0; i < items.Num(); ++i)
    {
        UPrimalItem* item = items[i];
        if (!item) continue;
        if (item->bIsBlueprint()() && item->bIsEngram()()) continue;
        if (item->IsItemSkin(true)) continue;
        if (ToLower(FStr(item->DescriptiveNameBaseField())) != nameLower) continue;
        if (wantBlueprint)
        {
            if (IsRealBlueprint(item))
            {
                if (instanceName.empty()) instanceName = FStr(item->DescriptiveNameBaseField());
                instanceItems.push_back(item);
            }
            continue;
        }
        if (IsRealBlueprint(item)) continue;
        if (item->bIsEgg()()) { sawBlocked = true; continue; }
        if (ItemHasNestedData(item)) { sawBlocked = true; continue; }
        if (ItemIsGear(item))
        {
            if (instanceName.empty()) instanceName = FStr(item->DescriptiveNameBaseField());
            instanceItems.push_back(item);
            continue;
        }
        const long long q = item->GetItemQuantity();
        available += q;
        if (stack <= 1) stack = item->GetMaxItemQuantity(reinterpret_cast<UObject*>(world));
        if (bpPath.empty()) bpPath = FStr(AsaApi::GetApiUtils().GetBlueprint(item));
        if (realName.empty()) realName = FStr(item->DescriptiveNameBaseField());
        matches.push_back({ item->ItemIDField(), q });
    }

    if (available <= 0 && !instanceItems.empty())
    {
        const std::string instBp = FStr(AsaApi::GetApiUtils().GetBlueprint(instanceItems[0]));

        std::lock_guard<std::mutex> lock(g_db_mutex);
        if (!EnsureConnected()) { Notify(pc, L"Storage is temporarily unavailable."); return; }

        if (g_blacklist_bp.count(instBp)) { Notify(pc, L"This item can't be stored."); return; }

        const int allowance = ResolveSlots(eosId);
        const long long usedBefore = GetTotalSlots(eosId) + GetInstanceCount(eosId);
        const long long have = (long long)instanceItems.size();

        if (usedBefore + have > allowance)
        {
            Notify(pc, L"Not enough storage slots. Used " +
                std::to_wstring(usedBefore) + L"/" + std::to_wstring(allowance) + L".");
            return;
        }

        int banked = 0;
        for (UPrimalItem* it : instanceItems)
        {
            if (!it) continue;
            const std::string js = GearToJson(it);
            if (js.empty()) continue;
            if (!WriteInstance(eosId, instanceName, js)) continue;

            FItemNetID id = it->ItemIDField();
            inv->RemoveItem(&id, false, false, true, false);
            ++banked;
        }

        const long long usedAfter = GetTotalSlots(eosId) + GetInstanceCount(eosId);
        const std::wstring wItem(instanceName.begin(), instanceName.end());
        const std::wstring tag = wantBlueprint ? L"[BP] " : L"";
        Notify(pc, L"Uploaded " + std::to_wstring(banked) + L" " + tag + wItem +
            L" (" + std::to_wstring(usedAfter) + L"/" + std::to_wstring(allowance) + L" slots)");
        return;
    }

    if (available <= 0)
    {
        if (sawBlocked) Notify(pc, L"This item can't be stored.");
        else Notify(pc, L"You have none of that item.");
        return;
    }
    if (stack <= 0) stack = 1;
    if (bpPath.empty()) { Notify(pc, L"Could not resolve that item path."); return; }
    if (g_blacklist_bp.count(bpPath)) { Notify(pc, L"This item can't be stored."); return; }
    if (!realName.empty()) itemName = realName;

    long long take = 0;
    if (amountStr == "all") take = available;
    else
    {
        take = std::atoll(amountStr.c_str());
        if (take <= 0) { Notify(pc, L"Invalid amount."); return; }
        if (take > available) take = available;
    }

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!EnsureConnected()) { Notify(pc, L"Storage is temporarily unavailable."); return; }

    long long existingQty = 0;
    int existingSlots = 0;
    int existingStack = 1;
    std::string existingName;
    std::string existingPath;
    GetStoredItem(eosId, itemName, existingQty, existingSlots, existingStack, existingName, existingPath);

    const long long newQty = existingQty + take;
    const int newSlots = SlotsFor(newQty, stack);

    const int allowance = ResolveSlots(eosId);
    const long long otherSlots = GetTotalSlots(eosId) - existingSlots;

    if (otherSlots + newSlots > allowance)
    {
        Notify(pc, L"Not enough storage slots. Used " +
            std::to_wstring(otherSlots + existingSlots) + L"/" +
            std::to_wstring(allowance) + L".");
        return;
    }

    long long remaining = take;
    UClass* cls = nullptr;

    for (auto& m : matches)
    {
        if (remaining <= 0) break;

        if (remaining >= m.qty)
        {
            inv->RemoveItem(&m.id, false, false, true, false);
            remaining -= m.qty;
        }
        else
        {
            inv->RemoveItem(&m.id, false, false, true, false);
            const long long keep = m.qty - remaining;
            remaining = 0;

            if (!cls)
            {
                const std::wstring wPath(bpPath.begin(), bpPath.end());
                FString fPath(wPath.c_str());
                cls = UVictoryCore::BPLoadClass(fPath);
            }
            if (cls)
            {
                UPrimalItem::AddNewItem(
                    cls, inv, false, false, 0.0f, false,
                    (int)keep, false, 0.0f, false,
                    TSubclassOf<UPrimalItem>(), 0.0f, false, false, false,
                    false, true, false, AsaApi::GetApiUtils().GetWorld());
            }
        }
    }

    if (!WriteStoredItem(eosId, itemName, bpPath, newQty, newSlots, stack))
    {
        Notify(pc, L"Storage write failed.");
        return;
    }

    const long long usedSlots = otherSlots + newSlots;
    const std::wstring wItem(itemName.begin(), itemName.end());
    Notify(pc, L"Uploaded " + std::to_wstring(take) + L" " + wItem +
        L" (" + std::to_wstring(usedSlots) + L"/" + std::to_wstring(allowance) + L" slots)");
}

// =============================================================================
// Command - /download
// =============================================================================

static void DownloadCommand(AShooterPlayerController* pc, FString* message, int, int)
{
    if (!pc) return;

    const std::string eosId = GetEosId(pc);
    if (eosId.empty()) { Notify(pc, L"Could not resolve your id."); return; }

    std::vector<std::string> tokens = SplitWords(FStr(*message));
    if (tokens.size() < 2)
    {
        Notify(pc, L"Usage: /download {item} [amount or all]");
        return;
    }

    bool wantBlueprint = false;
    size_t phraseStart = 1;
    if (ToLower(tokens[1]) == "bp") { wantBlueprint = true; phraseStart = 2; }

    std::string amountStr = "all";
    size_t itemEnd = tokens.size();
    if (tokens.size() >= phraseStart + 2 && IsAmountToken(tokens.back()))
    {
        amountStr = ToLower(tokens.back());
        itemEnd = tokens.size() - 1;
    }

    std::string phrase;
    for (size_t i = phraseStart; i < itemEnd; ++i)
    {
        if (!phrase.empty()) phrase += " ";
        phrase += tokens[i];
    }
    if (phrase.empty()) { Notify(pc, L"Usage: /download {item} [amount or all]"); return; }

    UPrimalInventoryComponent* inv = GetPlayerInventory(pc);
    if (!inv) { Notify(pc, L"No inventory found."); return; }

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!EnsureConnected()) { Notify(pc, L"Storage is temporarily unavailable."); return; }

    {
        std::vector<InstanceRow> all = GetInstanceList(eosId);
        std::vector<InstanceRow> insts;
        for (auto& r : all) if (r.blueprint == wantBlueprint) insts.push_back(r);

        std::string matchName;
        std::vector<std::string> icands;
        const NameMatch im = ResolveInstanceName(insts, phrase, matchName, icands);
        if (im == NameMatch::Ambiguous)
        {
            std::string list;
            for (size_t i = 0; i < icands.size(); ++i) { if (i) list += ", "; list += icands[i]; }
            const std::wstring wList(list.begin(), list.end());
            Notify(pc, L"Multiple matches: " + wList + L". Be more specific.");
            return;
        }
        if (im == NameMatch::Resolved)
        {
            long long bestId = -1;
            for (auto& r : insts)
                if (ToLower(r.name) == ToLower(matchName) && (bestId < 0 || r.id < bestId))
                    bestId = r.id;

            std::string instName, instBlob;
            if (bestId < 0 || !GetInstanceById(eosId, bestId, instName, instBlob))
            {
                Notify(pc, L"No stored item with that id.");
                return;
            }
            if (!DeleteInstance(eosId, bestId))
            {
                Notify(pc, L"Failed to withdraw that item.");
                return;
            }
            if (!JsonToInventory(inv, instBlob))
            {
                WriteInstance(eosId, instName, instBlob);
                Notify(pc, L"Failed to rebuild that item.");
                return;
            }

            const long long usedSlots = GetTotalSlots(eosId) + GetInstanceCount(eosId);
            const int allowance = ResolveSlots(eosId);
            const std::wstring wItem(instName.begin(), instName.end());
            Notify(pc, L"Downloaded " + std::wstring(wantBlueprint ? L"[BP] " : L"") + wItem +
                L" (" + std::to_wstring(usedSlots) + L"/" + std::to_wstring(allowance) + L" slots)");
            return;
        }
    }

    if (wantBlueprint) { Notify(pc, L"No stored blueprint by that name."); return; }

    std::string itemName;
    {
        std::string abbrevTarget;
        std::vector<std::string> abbrevCands;
        const NameMatch am = ResolveAbbrevName(phrase, abbrevTarget, abbrevCands);
        if (am == NameMatch::Ambiguous)
        {
            std::string list;
            for (size_t i = 0; i < abbrevCands.size(); ++i) { if (i) list += ", "; list += abbrevCands[i]; }
            const std::wstring wList(list.begin(), list.end());
            Notify(pc, L"Multiple matches: " + wList + L". Be more specific.");
            return;
        }
        if (am == NameMatch::Resolved)
        {
            std::string exactName;
            if (ExactStoredName(eosId, phrase, exactName) && ToLower(exactName) != ToLower(abbrevTarget))
            {
                const std::string list = abbrevTarget + ", " + exactName;
                const std::wstring wList(list.begin(), list.end());
                Notify(pc, L"Multiple matches: " + wList + L". Be more specific.");
                return;
            }
            itemName = abbrevTarget;
        }
    }
    if (itemName.empty())
    {
        std::string resolved;
        std::vector<std::string> candidates;
        const NameMatch m = ResolveStoredName(eosId, phrase, resolved, candidates);
        if (m == NameMatch::None) { Notify(pc, L"You have none of that item stored."); return; }
        if (m == NameMatch::Ambiguous)
        {
            std::string list;
            for (size_t i = 0; i < candidates.size(); ++i) { if (i) list += ", "; list += candidates[i]; }
            const std::wstring wList(list.begin(), list.end());
            Notify(pc, L"Multiple matches: " + wList + L". Be more specific.");
            return;
        }
        itemName = resolved;
    }

    long long storedQty = 0;
    int storedSlots = 0;
    int stack = 1;
    std::string storedName;
    std::string bpPath;
    if (!GetStoredItem(eosId, itemName, storedQty, storedSlots, stack, storedName, bpPath) || storedQty <= 0)
    {
        Notify(pc, L"You have none of that item stored.");
        return;
    }
    if (bpPath.empty()) { Notify(pc, L"Stored item path is missing."); return; }
    if (stack <= 0) stack = 1;
    if (!storedName.empty()) itemName = storedName;

    long long take = 0;
    if (amountStr == "all") take = storedQty;
    else
    {
        take = std::atoll(amountStr.c_str());
        if (take <= 0) { Notify(pc, L"Invalid amount."); return; }
        if (take > storedQty) take = storedQty;
    }

    const std::wstring wPath(bpPath.begin(), bpPath.end());
    FString fPath(wPath.c_str());
    UClass* cls = UVictoryCore::BPLoadClass(fPath);
    if (!cls)
    {
        Notify(pc, L"Failed to load that item.");
        return;
    }

    long long given = 0;
    long long remaining = take;
    while (remaining > 0)
    {
        const int give = (int)((remaining < stack) ? remaining : stack);
        UPrimalItem* added = UPrimalItem::AddNewItem(
            cls, inv, false, false, 0.0f, false,
            give, false, 0.0f, false,
            TSubclassOf<UPrimalItem>(), 0.0f, false, false, false,
            false, true, false, AsaApi::GetApiUtils().GetWorld());
        if (!added) break;
        given += give;
        remaining -= give;
    }

    if (given <= 0)
    {
        Notify(pc, L"Failed to give that item.");
        return;
    }

    const long long newQty = storedQty - given;
    const int newSlots = SlotsFor(newQty, stack);

    if (!WriteStoredItem(eosId, itemName, bpPath, newQty, newSlots, stack))
        Log::GetLog()->error("[CloudStorage] Failed to update storage after download for {}", eosId);

    const long long usedSlots = GetTotalSlots(eosId);
    const int allowance = ResolveSlots(eosId);
    const std::wstring wItem(itemName.begin(), itemName.end());
    Notify(pc, L"Downloaded " + std::to_wstring(given) + L" " + wItem +
        L" (" + std::to_wstring(usedSlots) + L"/" + std::to_wstring(allowance) + L" slots)");
}

// =============================================================================
// Command - /ulist
// =============================================================================

static const int g_list_page_size = 5;

static void ListCommand(AShooterPlayerController* pc, FString* message, int, int)
{
    if (!pc) return;

    const std::string eosId = GetEosId(pc);
    if (eosId.empty()) { Notify(pc, L"Could not resolve your id."); return; }

    int page = 1;
    std::vector<std::string> tokens = SplitWords(FStr(*message));
    if (tokens.size() >= 2)
    {
        const int p = std::atoi(tokens[1].c_str());
        if (p > 0) page = p;
    }

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!EnsureConnected()) { Notify(pc, L"Storage is temporarily unavailable."); return; }

    std::vector<StoredEntry> entries = GetAllStored(eosId);
    std::vector<InstanceRow> insts = GetInstanceList(eosId);
    if (entries.empty() && insts.empty()) { Notify(pc, L"You have nothing stored."); return; }

    std::vector<std::wstring> lines;
    for (auto& e : entries)
    {
        const std::wstring wName(e.name.begin(), e.name.end());
        lines.push_back(wName + L" - " + std::to_wstring(e.qty) +
            L" (" + std::to_wstring(e.slots) + L" slots)");
    }
    for (auto& r : insts)
    {
        const std::wstring wName(r.name.begin(), r.name.end());
        const std::wstring tag = r.blueprint ? L"[BP] " : L"";
        lines.push_back(tag + wName + L" (1 slot)");
    }

    const int total = (int)lines.size();
    const int totalPages = (total + g_list_page_size - 1) / g_list_page_size;
    if (page > totalPages) page = totalPages;

    const int start = (page - 1) * g_list_page_size;
    const int end = (start + g_list_page_size < total) ? start + g_list_page_size : total;

    for (int i = start; i < end; ++i)
        Notify(pc, lines[i]);

    const long long used = GetTotalSlots(eosId) + GetInstanceCount(eosId);
    const int allowance = ResolveSlots(eosId);

    std::wstring footer = L"Page " + std::to_wstring(page) + L"/" + std::to_wstring(totalPages) +
        L" - " + std::to_wstring(used) + L"/" + std::to_wstring(allowance) + L" slots";
    if (page < totalPages)
        footer += L" - type " + g_reg_list + L" " + std::to_wstring(page + 1) + L" for more";
    Notify(pc, footer);
}

// =============================================================================
// Command - /uploadall
// =============================================================================

static void UploadAllCommand(AShooterPlayerController* pc, FString* message, int, int)
{
    if (!pc) return;

    const std::string eosId = GetEosId(pc);
    if (eosId.empty()) { Notify(pc, L"Could not resolve your id."); return; }

    UPrimalInventoryComponent* inv = GetPlayerInventory(pc);
    if (!inv) { Notify(pc, L"No inventory found."); return; }

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!EnsureConnected()) { Notify(pc, L"Storage is temporarily unavailable."); return; }

    UWorld* world = AsaApi::GetApiUtils().GetWorld();

    std::set<UPrimalItem*> hotbar;
    {
        TArray<UPrimalItem*>& slots = inv->ItemSlotsField();
        for (int i = 0; i < slots.Num(); ++i)
            if (slots[i]) hotbar.insert(slots[i]);
    }

    const int allowance = ResolveSlots(eosId);
    long long remaining = (long long)allowance - (GetTotalSlots(eosId) + GetInstanceCount(eosId));
    if (remaining < 0) remaining = 0;

    struct FungAgg { std::string path; long long qty = 0; int stack = 1; std::vector<FItemNetID> ids; };
    std::map<std::string, FungAgg> fung;
    std::vector<UPrimalItem*> instanceItems;

    TArray<UPrimalItem*>& items = inv->InventoryItemsField();
    for (int i = 0; i < items.Num(); ++i)
    {
        UPrimalItem* it = items[i];
        if (!it) continue;
        if (it->bEquippedItem()()) continue;
        if (hotbar.count(it)) continue;
        if (it->IsItemSkin(true)) continue;
        if (it->bIsBlueprint()() && it->bIsEngram()()) continue;
        if (IsRealBlueprint(it)) { if (g_allow_blueprints) instanceItems.push_back(it); continue; }
        if (it->bIsEgg()()) continue;
        if (ItemHasNestedData(it)) continue;
        if (ItemIsGear(it)) { instanceItems.push_back(it); continue; }

        const std::string nm = FStr(it->DescriptiveNameBaseField());
        if (nm.empty()) continue;
        const std::string bpPath = FStr(AsaApi::GetApiUtils().GetBlueprint(it));
        if (g_blacklist_bp.count(bpPath)) continue;

        int stk = it->GetMaxItemQuantity(reinterpret_cast<UObject*>(world));
        if (stk <= 0) stk = 1;

        FungAgg& a = fung[nm];
        if (a.path.empty()) { a.path = bpPath; a.stack = stk; }
        a.qty += it->GetItemQuantity();
        a.ids.push_back(it->ItemIDField());
    }

    int bankedInstances = 0;
    int bankedTypes = 0;

    for (UPrimalItem* it : instanceItems)
    {
        if (remaining <= 0) break;
        if (!it) continue;
        const std::string instBp = FStr(AsaApi::GetApiUtils().GetBlueprint(it));
        if (g_blacklist_bp.count(instBp)) continue;

        const std::string js = GearToJson(it);
        if (js.empty()) continue;
        const std::string nm = FStr(it->DescriptiveNameBaseField());
        if (!WriteInstance(eosId, nm, js)) continue;

        FItemNetID id = it->ItemIDField();
        inv->RemoveItem(&id, false, false, true, false);
        --remaining;
        ++bankedInstances;
    }

    for (auto& [nm, a] : fung)
    {
        if (remaining <= 0) break;
        if (a.path.empty() || a.qty <= 0) continue;

        long long exQty = 0; int exSlots = 0, exStack = 0;
        std::string exN, exP;
        GetStoredItem(eosId, nm, exQty, exSlots, exStack, exN, exP);

        int stack = a.stack > 0 ? a.stack : 1;
        const long long newQty = exQty + a.qty;
        const int newSlots = SlotsFor(newQty, stack);
        const long long delta = (long long)newSlots - exSlots;
        if (delta > remaining) continue;

        if (!WriteStoredItem(eosId, nm, a.path, newQty, newSlots, stack)) continue;

        for (auto& id : a.ids)
        {
            FItemNetID tmp = id;
            inv->RemoveItem(&tmp, false, false, true, false);
        }
        remaining -= delta;
        ++bankedTypes;
    }

    const long long used = GetTotalSlots(eosId) + GetInstanceCount(eosId);

    if (bankedInstances == 0 && bankedTypes == 0)
    {
        Notify(pc, L"Nothing eligible to store (or no free slots).");
        return;
    }

    Notify(pc, L"Stored " + std::to_wstring(bankedTypes) + L" item type(s) and " +
        std::to_wstring(bankedInstances) + L" gear/blueprint(s). " +
        std::to_wstring(used) + L"/" + std::to_wstring(allowance) + L" slots used.");
}

// =============================================================================
// Command - /downloadall
// =============================================================================

static void DownloadAllCommand(AShooterPlayerController* pc, FString* message, int, int)
{
    if (!pc) return;

    const std::string eosId = GetEosId(pc);
    if (eosId.empty()) { Notify(pc, L"Could not resolve your id."); return; }

    UPrimalInventoryComponent* inv = GetPlayerInventory(pc);
    if (!inv) { Notify(pc, L"No inventory found."); return; }

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!EnsureConnected()) { Notify(pc, L"Storage is temporarily unavailable."); return; }

    TSubclassOf<UPrimalItem> noSkin{};
    long long movedItems = 0;
    bool capHit = false;

    std::vector<StoredEntry> entries = GetAllStored(eosId);
    for (auto& e : entries)
    {
        if (capHit) break;
        if (e.path.empty() || e.qty <= 0) continue;

        const std::wstring wPath(e.path.begin(), e.path.end());
        FString fPath(wPath.c_str());
        UClass* cls = UVictoryCore::BPLoadClass(fPath);
        if (!cls) continue;

        TSubclassOf<UPrimalItem> itemSub = cls;
        int stack = e.stack > 0 ? e.stack : 1;
        long long given = 0;
        long long remaining = e.qty;
        while (remaining > 0)
        {
            const int give = (int)((remaining < stack) ? remaining : stack);
            UPrimalItem* added = UPrimalItem::AddNewItem(
                itemSub, inv, false, false, 0.0f, false, give, false, 0.0f, false,
                noSkin, 0.0f, false, false, false, false, true, false, AsaApi::GetApiUtils().GetWorld());
            if (!added) { capHit = true; break; }
            given += give;
            remaining -= give;
        }

        if (given > 0)
        {
            const long long newQty = e.qty - given;
            const int newSlots = SlotsFor(newQty, stack);
            WriteStoredItem(eosId, e.name, e.path, newQty, newSlots, stack);
            movedItems += given;
        }
    }

    if (!capHit)
    {
        std::vector<InstanceRow> insts = GetInstanceList(eosId);
        for (auto& r : insts)
        {
            std::string instName, instBlob;
            if (!GetInstanceById(eosId, r.id, instName, instBlob)) continue;
            if (!DeleteInstance(eosId, r.id)) continue;
            if (!JsonToInventory(inv, instBlob)) { WriteInstance(eosId, instName, instBlob); capHit = true; break; }
            movedItems += 1;
        }
    }

    const long long used = GetTotalSlots(eosId) + GetInstanceCount(eosId);
    const int allowance = ResolveSlots(eosId);

    if (movedItems <= 0 && !capHit) { Notify(pc, L"You have nothing stored."); return; }
    if (capHit)
        Notify(pc, L"Inventory full - downloaded what fit. " +
            std::to_wstring(used) + L"/" + std::to_wstring(allowance) + L" slots still stored.");
    else
        Notify(pc, L"Downloaded everything. " +
            std::to_wstring(used) + L"/" + std::to_wstring(allowance) + L" slots used.");
}

// =============================================================================
// Tick - Config Hot-Reload
// =============================================================================

static bool GetFileInfo(const std::string& path, time_t& mtime, uintmax_t& fsize)
{
    struct _stat st {};
    if (_stat(path.c_str(), &st) != 0) return false;
    mtime = st.st_mtime;
    fsize = static_cast<uintmax_t>(st.st_size);
    return true;
}

static float g_config_check_accumulator = 0.0f;

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

// =============================================================================
// Plugin Entry Points
// =============================================================================

static void Plugin_Init_Impl()
{
    Log::Get().Init("CloudStorage");

    if (!LoadConfig())
    {
        Log::GetLog()->error("[CloudStorage] Config load failed, aborting init");
        return;
    }
    GetFileInfo(g_config_path, g_config_last_modified, g_config_last_size);

    if (!InitDatabase())
    {
        Log::GetLog()->error("[CloudStorage] Database init failed, aborting init");
        return;
    }

    g_reg_upload.assign(g_cmd_upload.begin(), g_cmd_upload.end());
    g_reg_download.assign(g_cmd_download.begin(), g_cmd_download.end());
    g_reg_list.assign(g_cmd_list.begin(), g_cmd_list.end());
    g_reg_uploadall.assign(g_cmd_uploadall.begin(), g_cmd_uploadall.end());
    g_reg_downloadall.assign(g_cmd_downloadall.begin(), g_cmd_downloadall.end());

    AsaApi::GetCommands().AddChatCommand(FString(g_reg_upload.c_str()), &UploadCommand);
    AsaApi::GetCommands().AddChatCommand(FString(g_reg_download.c_str()), &DownloadCommand);
    AsaApi::GetCommands().AddChatCommand(FString(g_reg_list.c_str()), &ListCommand);
    AsaApi::GetCommands().AddChatCommand(FString(g_reg_uploadall.c_str()), &UploadAllCommand);
    AsaApi::GetCommands().AddChatCommand(FString(g_reg_downloadall.c_str()), &DownloadAllCommand);
    AsaApi::GetCommands().AddOnTickCallback(FString(L"CloudStorage_Tick"), &OnTick);

    Log::GetLog()->info("[CloudStorage] Plugin loaded");
}

static void Plugin_Unload_Impl()
{
    AsaApi::GetCommands().RemoveChatCommand(FString(g_reg_upload.c_str()));
    AsaApi::GetCommands().RemoveChatCommand(FString(g_reg_download.c_str()));
    AsaApi::GetCommands().RemoveChatCommand(FString(g_reg_list.c_str()));
    AsaApi::GetCommands().RemoveChatCommand(FString(g_reg_uploadall.c_str()));
    AsaApi::GetCommands().RemoveChatCommand(FString(g_reg_downloadall.c_str()));
    AsaApi::GetCommands().RemoveOnTickCallback(FString(L"CloudStorage_Tick"));

    CloseDatabase();

    Log::GetLog()->info("[CloudStorage] Plugin unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { Plugin_Init_Impl(); }
    catch (const std::exception& ex) { Log::GetLog()->critical("[CloudStorage] Init exception: {}", ex.what()); }
    catch (...) { Log::GetLog()->critical("[CloudStorage] Init unknown exception"); }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { Plugin_Unload_Impl(); }
    catch (...) {}
}