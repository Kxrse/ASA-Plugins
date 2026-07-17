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
 *   MessageColor          - RichColor tuple used for all plugin chat output
 *   UploadCommand         - chat command for uploading (default "/upload")
 *   DownloadCommand       - chat command for downloading (default "/download")
 *   ListCommand           - chat command for listing storage (default "/ulist")
 *   UploadAllCommand      - chat command for bulk upload (default "/uploadall")
 *   DownloadAllCommand    - chat command for bulk download (default "/downloadall")
 *   ListPageSize          - rows shown per page by ListCommand
 *   DefaultSlots          - slots granted when a player matches no configured group
 *   AllowBlueprints       - whether blueprints may be stored at all
 *   Groups                - map of permission group name to slot count
 *   Abbreviations         - map of friendly item name to abbreviation
 *   BlacklistedBlueprints - array of exact blueprint paths blocked from upload
 *
 * Command names are read once at load. Changing them in config requires a
 * restart, since chat command registration happens in Plugin_Init.
 *
 * Config Example:
 * {
 *   "DbHost": "127.0.0.1",
 *   "DbPort": 3306,
 *   "DbUser": "DbUser",
 *   "DbPassword": "Password",
 *   "DbName": "DbName",
 *   "MessageColor": "0, 1, 0.65, 1",
 *   "UploadCommand": "/upload",
 *   "DownloadCommand": "/download",
 *   "ListCommand": "/ulist",
 *   "UploadAllCommand": "/uploadall",
 *   "DownloadAllCommand": "/downloadall",
 *   "ListPageSize": 5,
 *   "DefaultSlots": 5,
 *   "AllowBlueprints": false,
 *   "Groups": {
 *     "Admins": 100,
 *     "Vip": 25
 *   },
 *   "Abbreviations": {
 *     "Advanced Rifle Bullet": "arb",
 *     "Metal Ingot": "ingot"
 *   },
 *   "BlacklistedBlueprints": [
 *     "Blueprint'/Game/PrimalEarth/CoreBlueprints/Items/PrimalItem_Example.PrimalItem_Example'"
 *   ]
 * }
 *
 * Tables:
 *   cloudstorage_items - PK (eos_id, item_name)
 *     eos_id, item_name, item_bp_path, quantity, slots, stack, updated_at
 *   cloudstorage_instances - PK (id), INDEX (eos_id)
 *     id, eos_id, item_name, item_bp_path, is_blueprint, item_blob, updated_at
 *
 * Commands:
 *   /upload {item} [amount|all]   - moves items from inventory into storage
 *   /upload bp {item} [amount|all]
 *   /download {item} [amount|all] - moves items from storage into inventory
 *   /download bp {item} [amount|all]
 *   /ulist [page]                 - lists the caller's stored items, paginated
 *   /uploadall                    - stores everything eligible that fits
 *   /downloadall                  - withdraws everything that fits
 *
 * Slot model:
 *   A slot is one in-game stack. Slots used by a stackable item equal
 *   ceil(quantity / max stack size). Each stored gear piece or blueprint
 *   occupies exactly one slot. Capacity is the sum across both tables.
 *
 * Failure policy:
 *   Storage is never allowed to duplicate. Every mutation commits to the
 *   database before the inventory changes, so a database failure aborts
 *   cleanly with the player keeping what they had. Where an inventory
 *   operation fails after a successful commit, the row is corrected back
 *   and any unrecoverable discrepancy is logged with the EOS id.
 */

#include <API/ARK/Ark.h>
#include <json.hpp>
#include <Windows.h>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <set>
#include <map>
#include <sstream>
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
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
        !pmysql_free_result || !pmysql_error || !pmysql_real_escape_string)
    {
        Log::GetLog()->error("[CloudStorage] Failed to resolve required DB functions");
        return false;
    }

    g_mysql_loaded = true;
    return true;
}

// =============================================================================
// Text Helpers
// =============================================================================

static std::string ToLower(const std::string& s)
{
    std::string out = s;
    for (char& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

static std::string FStr(const FString& f)
{
    if (f.IsEmpty()) return std::string();
    return f.ToStringUTF8();
}

static FString ToFStr(const std::string& s)
{
    return FString::FromStringUTF8(s);
}

static std::vector<std::string> SplitWords(const std::string& s)
{
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string word;
    while (iss >> word) out.push_back(word);
    return out;
}

static std::string JoinList(const std::vector<std::string>& items)
{
    std::string out;
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (i) out += ", ";
        out += items[i];
    }
    return out;
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
static int         g_list_page_size = 5;
static bool        g_allow_blueprints = false;

static std::string g_cmd_upload = "/upload";
static std::string g_cmd_download = "/download";
static std::string g_cmd_list = "/ulist";
static std::string g_cmd_uploadall = "/uploadall";
static std::string g_cmd_downloadall = "/downloadall";

static std::string g_reg_upload;
static std::string g_reg_download;
static std::string g_reg_list;
static std::string g_reg_uploadall;
static std::string g_reg_downloadall;

static std::unordered_map<std::string, int>         g_groups;
static std::unordered_map<std::string, std::string> g_abbrev_to_name;
static std::set<std::string>                        g_blacklist_bp;

static time_t    g_config_last_modified = 0;
static uintmax_t g_config_last_size = 0;

static bool LoadConfig(bool first_load)
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
        g_list_page_size = j.value("ListPageSize", 5);
        g_allow_blueprints = j.value("AllowBlueprints", false);

        if (g_list_page_size < 1) g_list_page_size = 1;
        if (g_default_slots < 0) g_default_slots = 0;

        if (first_load)
        {
            g_cmd_upload = j.value("UploadCommand", "/upload");
            g_cmd_download = j.value("DownloadCommand", "/download");
            g_cmd_list = j.value("ListCommand", "/ulist");
            g_cmd_uploadall = j.value("UploadAllCommand", "/uploadall");
            g_cmd_downloadall = j.value("DownloadAllCommand", "/downloadall");
        }

        std::unordered_map<std::string, int> groups;
        if (j.contains("Groups") && j["Groups"].is_object())
        {
            for (auto& [key, val] : j["Groups"].items())
                if (val.is_number_integer()) groups[key] = val.get<int>();
        }
        g_groups = std::move(groups);

        std::unordered_map<std::string, std::string> abbrevs;
        if (j.contains("Abbreviations") && j["Abbreviations"].is_object())
        {
            for (auto& [name, abbrev] : j["Abbreviations"].items())
                if (abbrev.is_string()) abbrevs[ToLower(abbrev.get<std::string>())] = name;
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

static bool TableExists(const std::string& name)
{
    std::string eName;
    if (!Escape(name, eName)) return false;

    const std::string sql =
        "SELECT COUNT(*) FROM information_schema.tables "
        "WHERE table_schema=DATABASE() AND table_name='" + eName + "'";

    if (pmysql_query(g_mysql, sql.c_str()) != 0)
    {
        Log::GetLog()->error("[CloudStorage] TableExists query error: {}", pmysql_error(g_mysql));
        return false;
    }

    MYSQL_RES* res = pmysql_store_result(g_mysql);
    if (!res) return false;

    bool found = false;
    if (MYSQL_ROW row = pmysql_fetch_row(res))
        if (row[0]) found = (std::atoi(row[0]) > 0);

    pmysql_free_result(res);
    return found;
}

static void BackfillInstanceColumns()
{
    const std::string sel =
        "SELECT id, item_blob FROM cloudstorage_instances WHERE item_bp_path=''";

    if (pmysql_query(g_mysql, sel.c_str()) != 0)
    {
        Log::GetLog()->error("[CloudStorage] Backfill select error: {}", pmysql_error(g_mysql));
        return;
    }

    MYSQL_RES* res = pmysql_store_result(g_mysql);
    if (!res) return;

    struct Pending { long long id; std::string path; bool blueprint; };
    std::vector<Pending> pending;

    while (MYSQL_ROW row = pmysql_fetch_row(res))
    {
        if (!row[0] || !row[1]) continue;
        try
        {
            nlohmann::json j = nlohmann::json::parse(row[1]);
            Pending p;
            p.id = std::atoll(row[0]);
            p.path = j.value("bp", std::string());
            p.blueprint = j.value("blueprint", j.value("kind", std::string()) == "blueprint");
            if (!p.path.empty()) pending.push_back(p);
        }
        catch (...) {}
    }

    pmysql_free_result(res);
    if (pending.empty()) return;

    int done = 0;
    for (const auto& p : pending)
    {
        std::string ePath;
        if (!Escape(p.path, ePath)) continue;

        char buf[64];
        std::snprintf(buf, sizeof(buf), "%d WHERE id=%lld", p.blueprint ? 1 : 0, p.id);

        const std::string upd =
            "UPDATE cloudstorage_instances SET item_bp_path='" + ePath +
            "', is_blueprint=" + std::string(buf);

        if (ExecQuery(upd)) ++done;
    }

    Log::GetLog()->info("[CloudStorage] Backfilled {} instance row(s)", done);
}

static bool InitDatabase()
{
    if (!LoadMySQLLib()) return false;
    if (!OpenConnection()) return false;

    if (TableExists("cloudstorage") && !TableExists("cloudstorage_items"))
    {
        if (ExecQuery("RENAME TABLE cloudstorage TO cloudstorage_items"))
            Log::GetLog()->info("[CloudStorage] Migrated cloudstorage to cloudstorage_items");
        else
            Log::GetLog()->error("[CloudStorage] Failed to migrate cloudstorage to cloudstorage_items");
    }

    const bool okItems = ExecQuery(
        "CREATE TABLE IF NOT EXISTS cloudstorage_items ("
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

    if (!okItems)
    {
        Log::GetLog()->error("[CloudStorage] Failed to create cloudstorage_items table");
        pmysql_close(g_mysql);
        g_mysql = nullptr;
        return false;
    }

    ExecQuery("ALTER TABLE cloudstorage_items ADD COLUMN IF NOT EXISTS stack INT UNSIGNED NOT NULL DEFAULT 1");

    const bool okInstances = ExecQuery(
        "CREATE TABLE IF NOT EXISTS cloudstorage_instances ("
        "  id           BIGINT UNSIGNED  NOT NULL AUTO_INCREMENT,"
        "  eos_id       VARCHAR(64)      NOT NULL,"
        "  item_name    VARCHAR(128)     NOT NULL,"
        "  item_bp_path VARCHAR(512)     NOT NULL DEFAULT '',"
        "  is_blueprint TINYINT(1)       NOT NULL DEFAULT 0,"
        "  item_blob    MEDIUMTEXT       NOT NULL,"
        "  updated_at   TIMESTAMP        NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
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

    ExecQuery("ALTER TABLE cloudstorage_instances ADD COLUMN IF NOT EXISTS item_bp_path VARCHAR(512) NOT NULL DEFAULT ''");
    ExecQuery("ALTER TABLE cloudstorage_instances ADD COLUMN IF NOT EXISTS is_blueprint TINYINT(1) NOT NULL DEFAULT 0");

    BackfillInstanceColumns();

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

// =============================================================================
// Storage Model
// =============================================================================

struct FungibleRow
{
    std::string name;
    std::string path;
    long long   qty = 0;
    int         slots = 0;
    int         stack = 1;
    bool        dirty = false;
};

struct InstanceRow
{
    long long   id = 0;
    std::string name;
    std::string path;
    bool        blueprint = false;
};

struct PlayerStorage
{
    std::vector<FungibleRow> fungibles;
    std::vector<InstanceRow> instances;

    long long FungibleSlots() const
    {
        long long total = 0;
        for (const auto& f : fungibles) total += f.slots;
        return total;
    }

    long long UsedSlots() const
    {
        return FungibleSlots() + (long long)instances.size();
    }

    FungibleRow* Find(const std::string& name)
    {
        for (auto& f : fungibles)
            if (f.name == name) return &f;
        return nullptr;
    }
};

static int SlotsFor(long long quantity, int stack)
{
    if (stack <= 0) stack = 1;
    if (quantity <= 0) return 0;
    const long long slots = (quantity + stack - 1) / stack;
    if (slots > 0x7FFFFFFF) return 0x7FFFFFFF;
    return (int)slots;
}

static bool LoadStorage(const std::string& eosId, PlayerStorage& out)
{
    out.fungibles.clear();
    out.instances.clear();

    std::string eEos;
    if (!Escape(eosId, eEos)) return false;

    {
        const std::string sql =
            "SELECT item_name, item_bp_path, quantity, slots, stack FROM cloudstorage_items "
            "WHERE eos_id='" + eEos + "' ORDER BY item_name";

        if (pmysql_query(g_mysql, sql.c_str()) != 0)
        {
            Log::GetLog()->error("[CloudStorage] LoadStorage items error: {}", pmysql_error(g_mysql));
            return false;
        }

        MYSQL_RES* res = pmysql_store_result(g_mysql);
        if (!res) return false;

        while (MYSQL_ROW row = pmysql_fetch_row(res))
        {
            FungibleRow f;
            f.name = row[0] ? row[0] : "";
            f.path = row[1] ? row[1] : "";
            f.qty = row[2] ? std::atoll(row[2]) : 0;
            f.slots = row[3] ? std::atoi(row[3]) : 0;
            f.stack = row[4] ? std::atoi(row[4]) : 1;
            if (f.stack <= 0) f.stack = 1;
            if (!f.name.empty()) out.fungibles.push_back(std::move(f));
        }

        pmysql_free_result(res);
    }

    {
        const std::string sql =
            "SELECT id, item_name, item_bp_path, is_blueprint FROM cloudstorage_instances "
            "WHERE eos_id='" + eEos + "' ORDER BY id";

        if (pmysql_query(g_mysql, sql.c_str()) != 0)
        {
            Log::GetLog()->error("[CloudStorage] LoadStorage instances error: {}", pmysql_error(g_mysql));
            return false;
        }

        MYSQL_RES* res = pmysql_store_result(g_mysql);
        if (!res) return false;

        while (MYSQL_ROW row = pmysql_fetch_row(res))
        {
            InstanceRow r;
            r.id = row[0] ? std::atoll(row[0]) : 0;
            r.name = row[1] ? row[1] : "";
            r.path = row[2] ? row[2] : "";
            r.blueprint = row[3] ? (std::atoi(row[3]) != 0) : false;
            if (r.id > 0) out.instances.push_back(std::move(r));
        }

        pmysql_free_result(res);
    }

    return true;
}

static bool FlushFungibles(const std::string& eosId, PlayerStorage& storage)
{
    std::string eEos;
    if (!Escape(eosId, eEos)) return false;

    std::string insertValues;
    std::string deleteNames;
    int insertCount = 0;
    int deleteCount = 0;

    for (const auto& f : storage.fungibles)
    {
        if (!f.dirty) continue;

        std::string eName;
        if (!Escape(f.name, eName)) return false;

        if (f.qty <= 0)
        {
            if (deleteCount) deleteNames += ",";
            deleteNames += "'" + eName + "'";
            ++deleteCount;
            continue;
        }

        std::string ePath;
        if (!Escape(f.path, ePath)) return false;

        char tail[96];
        std::snprintf(tail, sizeof(tail), "%lld, %d, %d", f.qty, f.slots, f.stack);

        if (insertCount) insertValues += ",";
        insertValues += "('" + eEos + "', '" + eName + "', '" + ePath + "', " + std::string(tail) + ")";
        ++insertCount;
    }

    if (deleteCount)
    {
        const std::string del =
            "DELETE FROM cloudstorage_items WHERE eos_id='" + eEos +
            "' AND item_name IN (" + deleteNames + ")";
        if (!ExecQuery(del)) return false;
    }

    if (insertCount)
    {
        const std::string sql =
            "INSERT INTO cloudstorage_items (eos_id, item_name, item_bp_path, quantity, slots, stack) VALUES " +
            insertValues +
            " ON DUPLICATE KEY UPDATE item_bp_path=VALUES(item_bp_path), quantity=VALUES(quantity), "
            "slots=VALUES(slots), stack=IF(VALUES(stack)=0, stack, VALUES(stack))";
        if (!ExecQuery(sql)) return false;
    }

    for (auto& f : storage.fungibles) f.dirty = false;
    return true;
}

static bool WriteInstance(const std::string& eosId, const InstanceRow& row, const std::string& blob, long long& outId)
{
    outId = 0;

    std::string eEos, eName, ePath, eBlob;
    if (!Escape(eosId, eEos)) return false;
    if (!Escape(row.name, eName)) return false;
    if (!Escape(row.path, ePath)) return false;
    if (!Escape(blob, eBlob)) return false;

    const std::string sql =
        "INSERT INTO cloudstorage_instances (eos_id, item_name, item_bp_path, is_blueprint, item_blob) VALUES ('" +
        eEos + "', '" + eName + "', '" + ePath + "', " + (row.blueprint ? "1" : "0") + ", '" + eBlob + "')";

    if (!ExecQuery(sql)) return false;

    if (pmysql_query(g_mysql, "SELECT LAST_INSERT_ID()") == 0)
    {
        if (MYSQL_RES* res = pmysql_store_result(g_mysql))
        {
            if (MYSQL_ROW r = pmysql_fetch_row(res))
                if (r[0]) outId = std::atoll(r[0]);
            pmysql_free_result(res);
        }
    }

    return true;
}

static bool ReadInstanceBlob(const std::string& eosId, long long id, std::string& outBlob)
{
    outBlob.clear();

    std::string eEos;
    if (!Escape(eosId, eEos)) return false;

    char idbuf[32];
    std::snprintf(idbuf, sizeof(idbuf), "%lld", id);

    const std::string sql =
        "SELECT item_blob FROM cloudstorage_instances WHERE eos_id='" + eEos +
        "' AND id=" + idbuf + " LIMIT 1";

    if (pmysql_query(g_mysql, sql.c_str()) != 0)
    {
        Log::GetLog()->error("[CloudStorage] ReadInstanceBlob error: {}", pmysql_error(g_mysql));
        return false;
    }

    MYSQL_RES* res = pmysql_store_result(g_mysql);
    if (!res) return false;

    bool found = false;
    if (MYSQL_ROW row = pmysql_fetch_row(res))
    {
        if (row[0]) { outBlob = row[0]; found = true; }
    }

    pmysql_free_result(res);
    return found;
}

static bool DeleteInstance(const std::string& eosId, long long id)
{
    std::string eEos;
    if (!Escape(eosId, eEos)) return false;

    char idbuf[32];
    std::snprintf(idbuf, sizeof(idbuf), "%lld", id);

    return ExecQuery("DELETE FROM cloudstorage_instances WHERE eos_id='" + eEos + "' AND id=" + idbuf);
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
        FString fEos = ToFStr(eosId);
        TArray<FString> groups = pGetPlayerGroups(fEos);

        for (int i = 0; i < groups.Num(); ++i)
        {
            const std::string groupName = FStr(groups[i]);
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
// Player Helpers
// =============================================================================

static void Notify(AShooterPlayerController* pc, const std::string& msg)
{
    if (!pc) return;

    const std::string full = "<RichColor Color=\"" + g_message_color + "\">" + msg + "</>";
    FString fSender(L"");
    FString fMsg = ToFStr(full);
    AsaApi::GetApiUtils().SendChatMessage(pc, fSender, L"{}", std::wstring_view(*fMsg));
}

static std::string GetEosId(AShooterPlayerController* pc)
{
    if (!pc) return std::string();
    AShooterPlayerState* ps = static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
    if (!ps) return std::string();
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

// =============================================================================
// Item Classification
// =============================================================================

enum class ItemKind { Fungible, Instance, Blueprint, Blocked };

static ItemKind ClassifyItem(UPrimalItem* item)
{
    if (!item) return ItemKind::Blocked;
    if (item->bIsEngram()()) return ItemKind::Blocked;
    if (item->IsItemSkin(true)) return ItemKind::Blocked;
    if (item->bIsEgg()()) return ItemKind::Blocked;
    if (item->CustomItemDatasField().Num() > 0) return ItemKind::Blocked;
    if (item->bIsBlueprint()()) return ItemKind::Blueprint;
    if (item->bUseItemDurability()()) return ItemKind::Instance;
    return ItemKind::Fungible;
}

static std::string ItemDisplayName(UPrimalItem* item)
{
    return FStr(item->DescriptiveNameBaseField());
}

static std::string ItemPath(UPrimalItem* item)
{
    return FStr(AsaApi::GetApiUtils().GetBlueprint(item));
}

// =============================================================================
// Instance Blob
// =============================================================================

static std::string ItemToBlob(UPrimalItem* item)
{
    if (!item) return std::string();

    FItemNetInfo* info = AsaApi::IApiUtils::AllocateStruct<FItemNetInfo>();
    if (!info) return std::string();

    item->GetItemNetInfo(info, false);

    std::string result;
    try
    {
        nlohmann::json j;
        j["v"] = 1;
        j["kind"] = item->bIsBlueprint()() ? "blueprint" : "gear";
        j["bp"] = ItemPath(item);
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
        if (unsigned short* sv = info->ItemStatValuesField()())
            for (int i = 0; i < 8; ++i) stats.push_back((unsigned int)sv[i]);
        j["stats"] = stats;

        nlohmann::json colors = nlohmann::json::array();
        if (short* cv = info->ItemColorIDField()())
            for (int i = 0; i < 6; ++i) colors.push_back((int)cv[i]);
        j["colors"] = colors;

        result = j.dump();
    }
    catch (...)
    {
        result.clear();
    }

    AsaApi::IApiUtils::FreeStruct(info);
    return result;
}

static bool BlobToItem(UPrimalInventoryComponent* inv, const std::string& blob)
{
    if (!inv) return false;

    nlohmann::json j;
    try { j = nlohmann::json::parse(blob); }
    catch (...) { return false; }

    const std::string kind = j.value("kind", std::string("gear"));
    const bool isBp = (kind == "blueprint") || j.value("blueprint", false);
    if (kind != "gear" && kind != "blueprint") return false;

    const std::string bp = j.value("bp", std::string());
    if (bp.empty()) return false;

    FString fPath = ToFStr(bp);
    UClass* cls = UVictoryCore::BPLoadClass(fPath);
    if (!cls) return false;

    int qty = (int)j.value("qty", 1u);
    if (qty < 1) qty = 1;

    TSubclassOf<UPrimalItem> itemSub(cls);
    TSubclassOf<UPrimalItem> noSkin{};
    UPrimalItem* item = UPrimalItem::AddNewItem(
        itemSub, inv, false, false, 0.0f, false, qty, isBp, 0.0f, true,
        noSkin, 0.0f, false, false, true, true, true, false, AsaApi::GetApiUtils().GetWorld());
    if (!item) return false;

    item->ItemDurabilityField() = j.value("durability", 0.0f);
    item->ItemRatingField() = j.value("rating", 0.0f);
    item->ItemQualityIndexField() = (unsigned char)j.value("qualityIndex", j.value("quality", 0));
    item->CraftingSkillField() = j.value("craftingSkill", 0.0f);
    item->CraftedSkillBonusField() = j.value("craftedBonus", 0.0f);
    item->WeaponClipAmmoField() = (int)j.value("clipAmmo", 0u);

    item->CrafterCharacterNameField() = ToFStr(j.value("crafterChar", std::string()));
    item->CrafterTribeNameField() = ToFStr(j.value("crafterTribe", std::string()));
    item->CustomItemNameField() = ToFStr(j.value("customName", std::string()));
    item->CustomItemDescriptionField() = ToFStr(j.value("customDesc", std::string()));

    if (j.contains("stats") && j["stats"].is_array())
    {
        if (unsigned short* sv = item->ItemStatValuesField()())
            for (int i = 0; i < 8 && i < (int)j["stats"].size(); ++i)
                sv[i] = (unsigned short)j["stats"][i].get<unsigned int>();
    }
    if (j.contains("colors") && j["colors"].is_array())
    {
        if (short* cv = item->ItemColorIDField()())
            for (int i = 0; i < 6 && i < (int)j["colors"].size(); ++i)
                cv[i] = (short)j["colors"][i].get<int>();
    }

    item->UpdatedItem(true, false);
    return true;
}

// =============================================================================
// Name Resolution
// =============================================================================

enum class NameMatch { Resolved, Ambiguous, None };

typedef std::vector<std::pair<std::string, std::string>> NamePool;

static NameMatch ResolveName(const NamePool& pool, const std::string& phrase,
    std::string& out, std::vector<std::string>& candidates)
{
    const std::string pl = ToLower(phrase);
    std::map<std::string, std::string> exact;
    std::map<std::string, std::string> prefix;

    for (const auto& entry : pool)
    {
        if (entry.first == pl) exact[entry.first] = entry.second;
        else if (entry.first.rfind(pl, 0) == 0) prefix[entry.first] = entry.second;
    }

    std::map<std::string, std::string>& pick = !exact.empty() ? exact : prefix;
    if (pick.empty()) return NameMatch::None;
    if (pick.size() == 1) { out = pick.begin()->second; return NameMatch::Resolved; }
    for (const auto& kv : pick) candidates.push_back(kv.second);
    return NameMatch::Ambiguous;
}

static NameMatch ResolveWithAbbrev(const NamePool& pool, const std::string& phrase,
    std::string& out, std::vector<std::string>& candidates)
{
    NamePool abbrevPool;
    abbrevPool.reserve(g_abbrev_to_name.size());
    for (const auto& kv : g_abbrev_to_name) abbrevPool.emplace_back(kv.first, kv.second);

    std::string abbrevName;
    std::vector<std::string> abbrevCands;
    const NameMatch am = ResolveName(abbrevPool, phrase, abbrevName, abbrevCands);
    if (am == NameMatch::Ambiguous) { candidates = abbrevCands; return am; }

    std::string poolName;
    std::vector<std::string> poolCands;
    const NameMatch pm = ResolveName(pool, phrase, poolName, poolCands);

    if (am == NameMatch::Resolved)
    {
        if (pm == NameMatch::Resolved && ToLower(poolName) != ToLower(abbrevName))
        {
            candidates.push_back(abbrevName);
            candidates.push_back(poolName);
            return NameMatch::Ambiguous;
        }
        out = abbrevName;
        return NameMatch::Resolved;
    }

    if (pm == NameMatch::Ambiguous) { candidates = poolCands; return pm; }
    if (pm == NameMatch::Resolved) { out = poolName; return pm; }
    return NameMatch::None;
}

static bool ReportMatch(AShooterPlayerController* pc, NameMatch m,
    const std::vector<std::string>& candidates, const std::string& noneMessage)
{
    if (m == NameMatch::Resolved) return true;

    if (m == NameMatch::Ambiguous)
        Notify(pc, "Multiple matches: " + JoinList(candidates) + ". Be more specific.");
    else
        Notify(pc, noneMessage);

    return false;
}

// =============================================================================
// Command Parsing
// =============================================================================

struct ParsedCommand
{
    bool        ok = false;
    bool        blueprint = false;
    bool        badAmount = false;
    std::string phrase;
    long long   amount = -1;
};

static bool IsAmountToken(const std::string& s)
{
    if (s.empty()) return false;
    if (ToLower(s) == "all") return true;
    for (char c : s) if (!std::isdigit((unsigned char)c)) return false;
    return true;
}

static ParsedCommand ParseItemCommand(FString* message)
{
    ParsedCommand p;

    const std::vector<std::string> tokens = SplitWords(FStr(*message));
    if (tokens.size() < 2) return p;

    size_t phraseStart = 1;
    if (ToLower(tokens[1]) == "bp") { p.blueprint = true; phraseStart = 2; }

    size_t itemEnd = tokens.size();
    if (tokens.size() >= phraseStart + 2 && IsAmountToken(tokens.back()))
    {
        const std::string amountStr = ToLower(tokens.back());
        itemEnd = tokens.size() - 1;
        if (amountStr != "all")
        {
            errno = 0;
            const long long parsed = std::strtoll(amountStr.c_str(), nullptr, 10);
            if (errno != 0 || parsed <= 0) p.badAmount = true;
            else p.amount = parsed;
        }
    }

    for (size_t i = phraseStart; i < itemEnd; ++i)
    {
        if (!p.phrase.empty()) p.phrase += " ";
        p.phrase += tokens[i];
    }

    p.ok = !p.phrase.empty();
    return p;
}

static std::string SlotsSuffix(long long used, int allowance)
{
    return " (" + std::to_string(used) + "/" + std::to_string(allowance) + " slots)";
}

// =============================================================================
// Inventory Scanning
// =============================================================================

struct InventoryMatch
{
    std::vector<UPrimalItem*> items;
    std::string               name;
    std::string               path;
    long long                 quantity = 0;
    int                       stack = 1;
    bool                      sawBlocked = false;
};

static void BuildNamePool(UPrimalInventoryComponent* inv, bool wantBlueprint, NamePool& pool)
{
    std::set<std::string> seen;

    TArray<UPrimalItem*>& items = inv->InventoryItemsField();
    for (int i = 0; i < items.Num(); ++i)
    {
        UPrimalItem* item = items[i];
        if (!item) continue;

        const ItemKind kind = ClassifyItem(item);
        if (kind == ItemKind::Blocked) continue;
        if (wantBlueprint && kind != ItemKind::Blueprint) continue;
        if (!wantBlueprint && kind == ItemKind::Blueprint) continue;

        const std::string disp = ItemDisplayName(item);
        if (disp.empty()) continue;

        const std::string dl = ToLower(disp);
        if (seen.insert(dl).second) pool.emplace_back(dl, disp);
    }
}

static bool HoldsMatchingBlueprint(UPrimalInventoryComponent* inv, const std::string& phrase)
{
    const std::string pl = ToLower(phrase);

    TArray<UPrimalItem*>& items = inv->InventoryItemsField();
    for (int i = 0; i < items.Num(); ++i)
    {
        UPrimalItem* item = items[i];
        if (!item) continue;
        if (ClassifyItem(item) != ItemKind::Blueprint) continue;

        const std::string dl = ToLower(ItemDisplayName(item));
        if (dl.empty()) continue;
        if (dl == pl || dl.rfind(pl, 0) == 0) return true;
    }
    return false;
}

static void CollectMatches(UPrimalInventoryComponent* inv, const std::string& name,
    bool wantBlueprint, UWorld* world, InventoryMatch& out)
{
    const std::string nameLower = ToLower(name);

    TArray<UPrimalItem*>& items = inv->InventoryItemsField();
    for (int i = 0; i < items.Num(); ++i)
    {
        UPrimalItem* item = items[i];
        if (!item) continue;
        if (ToLower(ItemDisplayName(item)) != nameLower) continue;

        const ItemKind kind = ClassifyItem(item);
        if (kind == ItemKind::Blocked) { out.sawBlocked = true; continue; }

        if (wantBlueprint)
        {
            if (kind != ItemKind::Blueprint) continue;
        }
        else
        {
            if (kind == ItemKind::Blueprint) continue;
        }

        if (out.name.empty()) out.name = ItemDisplayName(item);
        if (out.path.empty()) out.path = ItemPath(item);

        if (kind == ItemKind::Fungible)
        {
            const long long q = item->GetItemQuantity();
            if (q <= 0) continue;
            out.quantity += q;
            if (out.stack <= 1)
            {
                const int s = item->GetMaxItemQuantity(reinterpret_cast<UObject*>(world));
                out.stack = (s > 0) ? s : 1;
            }
        }

        out.items.push_back(item);
    }
}

// =============================================================================
// Command - /upload
// =============================================================================

static void UploadInstances(AShooterPlayerController* pc, UPrimalInventoryComponent* inv,
    const std::string& eosId, PlayerStorage& storage, InventoryMatch& match,
    long long requested, bool wantBlueprint, int allowance)
{
    const long long free = (long long)allowance - storage.UsedSlots();
    if (free <= 0)
    {
        Notify(pc, "Not enough storage slots." + SlotsSuffix(storage.UsedSlots(), allowance));
        return;
    }

    long long take = (requested < 0) ? (long long)match.items.size() : requested;
    take = std::min<long long>(take, (long long)match.items.size());
    take = std::min<long long>(take, free);

    int banked = 0;
    for (long long i = 0; i < take; ++i)
    {
        UPrimalItem* item = match.items[(size_t)i];
        if (!item) continue;

        const std::string path = ItemPath(item);
        if (g_blacklist_bp.count(path)) continue;

        const std::string blob = ItemToBlob(item);
        if (blob.empty()) continue;

        InstanceRow row;
        row.name = ItemDisplayName(item);
        row.path = path;
        row.blueprint = wantBlueprint;

        long long newId = 0;
        if (!WriteInstance(eosId, row, blob, newId)) continue;

        FItemNetID id = item->ItemIDField();
        if (!inv->RemoveItem(&id, false, false, true, false))
        {
            if (!DeleteInstance(eosId, newId))
                Log::GetLog()->error("[CloudStorage] Orphaned instance row {} for {} after failed removal", newId, eosId);
            continue;
        }

        row.id = newId;
        storage.instances.push_back(row);
        ++banked;
    }

    if (banked == 0)
    {
        Notify(pc, "Nothing was stored.");
        return;
    }

    const std::string tag = wantBlueprint ? "[BP] " : "";
    Notify(pc, "Uploaded " + std::to_string(banked) + " " + tag + match.name +
        SlotsSuffix(storage.UsedSlots(), allowance));
}

static void UploadFungible(AShooterPlayerController* pc, UPrimalInventoryComponent* inv,
    const std::string& eosId, PlayerStorage& storage, InventoryMatch& match, long long requested, int allowance)
{
    if (g_blacklist_bp.count(match.path)) { Notify(pc, "This item can't be stored."); return; }
    if (match.path.empty()) { Notify(pc, "Could not resolve that item path."); return; }

    long long take = (requested < 0) ? match.quantity : std::min<long long>(requested, match.quantity);
    if (take <= 0) { Notify(pc, "You have none of that item."); return; }

    FungibleRow* row = storage.Find(match.name);
    const long long existingQty = row ? row->qty : 0;
    const int existingSlots = row ? row->slots : 0;

    const long long otherSlots = storage.UsedSlots() - existingSlots;
    const long long headroom = (long long)allowance - otherSlots;
    if (headroom <= 0)
    {
        Notify(pc, "Not enough storage slots." + SlotsSuffix(storage.UsedSlots(), allowance));
        return;
    }

    const long long maxQty = headroom * (long long)match.stack;
    if (existingQty + take > maxQty) take = maxQty - existingQty;
    if (take <= 0)
    {
        Notify(pc, "Not enough storage slots." + SlotsSuffix(storage.UsedSlots(), allowance));
        return;
    }

    const long long newQty = existingQty + take;
    const int newSlots = SlotsFor(newQty, match.stack);

    if (!row)
    {
        FungibleRow fresh;
        fresh.name = match.name;
        storage.fungibles.push_back(fresh);
        row = &storage.fungibles.back();
    }

    const long long prevQty = row->qty;
    const int prevSlots = row->slots;
    const std::string prevPath = row->path;
    const int prevStack = row->stack;

    row->path = match.path;
    row->qty = newQty;
    row->slots = newSlots;
    row->stack = match.stack;
    row->dirty = true;

    if (!FlushFungibles(eosId, storage))
    {
        row->path = prevPath;
        row->qty = prevQty;
        row->slots = prevSlots;
        row->stack = prevStack;
        row->dirty = false;
        Notify(pc, "Storage write failed. Nothing was stored.");
        return;
    }

    long long remaining = take;
    for (UPrimalItem* item : match.items)
    {
        if (remaining <= 0) break;
        if (!item) continue;

        const long long have = item->GetItemQuantity();
        if (have <= 0) continue;

        if (remaining >= have)
        {
            FItemNetID id = item->ItemIDField();
            if (inv->RemoveItem(&id, false, false, true, false)) remaining -= have;
        }
        else
        {
            item->IncrementItemQuantity(-(int)remaining, true, false, false, false, false, false);
            const long long now = item->GetItemQuantity();
            remaining -= (have - now);
        }
    }

    if (remaining > 0)
    {
        row->qty = newQty - remaining;
        row->slots = SlotsFor(row->qty, row->stack);
        row->dirty = true;
        if (!FlushFungibles(eosId, storage))
            Log::GetLog()->error("[CloudStorage] Failed to correct {} of {} for {} after partial removal",
                remaining, match.name, eosId);
        take -= remaining;
    }

    if (take <= 0) { Notify(pc, "Nothing was stored."); return; }

    Notify(pc, "Uploaded " + std::to_string(take) + " " + match.name +
        SlotsSuffix(storage.UsedSlots(), allowance));
}

static void UploadCommand(AShooterPlayerController* pc, FString* message, int, int)
{
    if (!pc || !message) return;

    const std::string eosId = GetEosId(pc);
    if (eosId.empty()) { Notify(pc, "Could not resolve your id."); return; }

    const ParsedCommand cmd = ParseItemCommand(message);
    if (!cmd.ok) { Notify(pc, "Usage: " + g_reg_upload + " {item} [amount or all]"); return; }
    if (cmd.badAmount) { Notify(pc, "Invalid amount."); return; }

    UPrimalInventoryComponent* inv = GetPlayerInventory(pc);
    if (!inv) { Notify(pc, "No inventory found."); return; }

    if (cmd.blueprint && !g_allow_blueprints) { Notify(pc, "Blueprints can't be stored."); return; }

    NamePool pool;
    BuildNamePool(inv, cmd.blueprint, pool);

    std::string itemName;
    std::vector<std::string> candidates;
    const NameMatch m = cmd.blueprint
        ? ResolveName(pool, cmd.phrase, itemName, candidates)
        : ResolveWithAbbrev(pool, cmd.phrase, itemName, candidates);

    if (m == NameMatch::None && !cmd.blueprint && HoldsMatchingBlueprint(inv, cmd.phrase))
    {
        if (g_allow_blueprints) Notify(pc, "That's a blueprint. Use the bp prefix to store it.");
        else Notify(pc, "Blueprints can't be stored.");
        return;
    }

    if (!ReportMatch(pc, m, candidates, cmd.blueprint ? "No matching blueprint." : "No matching item."))
        return;

    UWorld* world = AsaApi::GetApiUtils().GetWorld();

    InventoryMatch match;
    CollectMatches(inv, itemName, cmd.blueprint, world, match);

    if (match.items.empty())
    {
        Notify(pc, match.sawBlocked ? "This item can't be stored." : "You have none of that item.");
        return;
    }

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!EnsureConnected()) { Notify(pc, "Storage is temporarily unavailable."); return; }

    PlayerStorage storage;
    if (!LoadStorage(eosId, storage)) { Notify(pc, "Storage is temporarily unavailable."); return; }

    const int allowance = ResolveSlots(eosId);

    if (cmd.blueprint || match.quantity <= 0)
        UploadInstances(pc, inv, eosId, storage, match, cmd.amount, cmd.blueprint, allowance);
    else
        UploadFungible(pc, inv, eosId, storage, match, cmd.amount, allowance);
}

// =============================================================================
// Command - /download
// =============================================================================

static long long GiveFungible(UPrimalInventoryComponent* inv, UClass* cls, long long want, int stack)
{
    TSubclassOf<UPrimalItem> itemSub(cls);
    TSubclassOf<UPrimalItem> noSkin{};
    UWorld* world = AsaApi::GetApiUtils().GetWorld();

    long long given = 0;
    long long remaining = want;

    while (remaining > 0)
    {
        const int give = (int)std::min<long long>(remaining, stack);
        UPrimalItem* added = UPrimalItem::AddNewItem(
            itemSub, inv, false, false, 0.0f, false, give, false, 0.0f, false,
            noSkin, 0.0f, false, false, false, false, true, false, world);
        if (!added) break;
        given += give;
        remaining -= give;
    }

    return given;
}

static void DownloadInstance(AShooterPlayerController* pc, UPrimalInventoryComponent* inv,
    const std::string& eosId, PlayerStorage& storage, const std::string& name,
    bool wantBlueprint, long long requested, int allowance)
{
    std::vector<InstanceRow*> targets;
    for (auto& r : storage.instances)
        if (r.blueprint == wantBlueprint && ToLower(r.name) == ToLower(name))
            targets.push_back(&r);

    if (targets.empty()) { Notify(pc, "You have none of that item stored."); return; }

    long long take = (requested < 0) ? (long long)targets.size() : std::min<long long>(requested, (long long)targets.size());

    int given = 0;
    bool capHit = false;

    for (long long i = 0; i < take; ++i)
    {
        InstanceRow* r = targets[(size_t)i];

        std::string blob;
        if (!ReadInstanceBlob(eosId, r->id, blob)) continue;
        if (!DeleteInstance(eosId, r->id)) continue;

        if (!BlobToItem(inv, blob))
        {
            long long restored = 0;
            if (!WriteInstance(eosId, *r, blob, restored))
                Log::GetLog()->error("[CloudStorage] Lost instance {} for {} after failed rebuild", r->id, eosId);
            capHit = true;
            break;
        }

        r->id = 0;
        ++given;
    }

    storage.instances.erase(
        std::remove_if(storage.instances.begin(), storage.instances.end(),
            [](const InstanceRow& r) { return r.id == 0; }),
        storage.instances.end());

    if (given == 0)
    {
        Notify(pc, capHit ? "Failed to rebuild that item." : "You have none of that item stored.");
        return;
    }

    const std::string tag = wantBlueprint ? "[BP] " : "";
    Notify(pc, "Downloaded " + std::to_string(given) + " " + tag + name +
        SlotsSuffix(storage.UsedSlots(), allowance));
}

static void DownloadFungible(AShooterPlayerController* pc, UPrimalInventoryComponent* inv,
    const std::string& eosId, PlayerStorage& storage, const std::string& name,
    long long requested, int allowance)
{
    FungibleRow* row = storage.Find(name);
    if (!row || row->qty <= 0) { Notify(pc, "You have none of that item stored."); return; }
    if (row->path.empty()) { Notify(pc, "Stored item path is missing."); return; }

    const long long take = (requested < 0) ? row->qty : std::min<long long>(requested, row->qty);
    if (take <= 0) { Notify(pc, "Invalid amount."); return; }

    FString fPath = ToFStr(row->path);
    UClass* cls = UVictoryCore::BPLoadClass(fPath);
    if (!cls) { Notify(pc, "Failed to load that item."); return; }

    const long long prevQty = row->qty;
    const int prevSlots = row->slots;

    row->qty = prevQty - take;
    row->slots = SlotsFor(row->qty, row->stack);
    row->dirty = true;

    if (!FlushFungibles(eosId, storage))
    {
        row->qty = prevQty;
        row->slots = prevSlots;
        row->dirty = false;
        Notify(pc, "Storage write failed. Nothing was withdrawn.");
        return;
    }

    const long long given = GiveFungible(inv, cls, take, row->stack);

    if (given < take)
    {
        row->qty = prevQty - given;
        row->slots = SlotsFor(row->qty, row->stack);
        row->dirty = true;
        if (!FlushFungibles(eosId, storage))
            Log::GetLog()->error("[CloudStorage] Failed to restore {} of {} for {} after partial give",
                take - given, name, eosId);
    }

    if (given <= 0) { Notify(pc, "Your inventory is full."); return; }

    const std::string suffix = (given < take) ? " Inventory full." : "";
    Notify(pc, "Downloaded " + std::to_string(given) + " " + name +
        SlotsSuffix(storage.UsedSlots(), allowance) + suffix);
}

static void DownloadCommand(AShooterPlayerController* pc, FString* message, int, int)
{
    if (!pc || !message) return;

    const std::string eosId = GetEosId(pc);
    if (eosId.empty()) { Notify(pc, "Could not resolve your id."); return; }

    const ParsedCommand cmd = ParseItemCommand(message);
    if (!cmd.ok) { Notify(pc, "Usage: " + g_reg_download + " {item} [amount or all]"); return; }
    if (cmd.badAmount) { Notify(pc, "Invalid amount."); return; }

    UPrimalInventoryComponent* inv = GetPlayerInventory(pc);
    if (!inv) { Notify(pc, "No inventory found."); return; }

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!EnsureConnected()) { Notify(pc, "Storage is temporarily unavailable."); return; }

    PlayerStorage storage;
    if (!LoadStorage(eosId, storage)) { Notify(pc, "Storage is temporarily unavailable."); return; }

    const int allowance = ResolveSlots(eosId);

    NamePool instancePool;
    {
        std::set<std::string> seen;
        for (const auto& r : storage.instances)
        {
            if (r.blueprint != cmd.blueprint) continue;
            const std::string dl = ToLower(r.name);
            if (seen.insert(dl).second) instancePool.emplace_back(dl, r.name);
        }
    }

    {
        std::string name;
        std::vector<std::string> candidates;
        const NameMatch m = ResolveName(instancePool, cmd.phrase, name, candidates);

        if (m == NameMatch::Ambiguous)
        {
            Notify(pc, "Multiple matches: " + JoinList(candidates) + ". Be more specific.");
            return;
        }
        if (m == NameMatch::Resolved)
        {
            DownloadInstance(pc, inv, eosId, storage, name, cmd.blueprint, cmd.amount, allowance);
            return;
        }
    }

    if (cmd.blueprint) { Notify(pc, "No stored blueprint by that name."); return; }

    NamePool fungiblePool;
    for (const auto& f : storage.fungibles)
        fungiblePool.emplace_back(ToLower(f.name), f.name);

    std::string itemName;
    std::vector<std::string> candidates;
    const NameMatch m = ResolveWithAbbrev(fungiblePool, cmd.phrase, itemName, candidates);
    if (!ReportMatch(pc, m, candidates, "You have none of that item stored.")) return;

    DownloadFungible(pc, inv, eosId, storage, itemName, cmd.amount, allowance);
}

// =============================================================================
// Command - /ulist
// =============================================================================

static void ListCommand(AShooterPlayerController* pc, FString* message, int, int)
{
    if (!pc || !message) return;

    const std::string eosId = GetEosId(pc);
    if (eosId.empty()) { Notify(pc, "Could not resolve your id."); return; }

    int page = 1;
    const std::vector<std::string> tokens = SplitWords(FStr(*message));
    if (tokens.size() >= 2)
    {
        const int p = std::atoi(tokens[1].c_str());
        if (p > 0) page = p;
    }

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!EnsureConnected()) { Notify(pc, "Storage is temporarily unavailable."); return; }

    PlayerStorage storage;
    if (!LoadStorage(eosId, storage)) { Notify(pc, "Storage is temporarily unavailable."); return; }

    if (storage.fungibles.empty() && storage.instances.empty())
    {
        Notify(pc, "You have nothing stored.");
        return;
    }

    std::vector<std::string> lines;
    for (const auto& f : storage.fungibles)
        lines.push_back(f.name + " x" + std::to_string(f.qty) + " (" + std::to_string(f.slots) +
            (f.slots == 1 ? " slot)" : " slots)"));

    for (const auto& r : storage.instances)
        lines.push_back(std::string(r.blueprint ? "[BP] " : "") + r.name + " (1 slot)");

    const int total = (int)lines.size();
    const int totalPages = (total + g_list_page_size - 1) / g_list_page_size;
    if (page > totalPages) page = totalPages;

    const int start = (page - 1) * g_list_page_size;
    const int end = std::min<int>(start + g_list_page_size, total);

    for (int i = start; i < end; ++i)
        Notify(pc, lines[i]);

    const int allowance = ResolveSlots(eosId);

    std::string footer = "Page " + std::to_string(page) + "/" + std::to_string(totalPages) +
        ", " + std::to_string(storage.UsedSlots()) + "/" + std::to_string(allowance) + " slots";
    if (page < totalPages)
        footer += ", type " + g_reg_list + " " + std::to_string(page + 1) + " for more";

    Notify(pc, footer);
}

// =============================================================================
// Command - /uploadall
// =============================================================================

static void UploadAllCommand(AShooterPlayerController* pc, FString* message, int, int)
{
    if (!pc) return;

    const std::string eosId = GetEosId(pc);
    if (eosId.empty()) { Notify(pc, "Could not resolve your id."); return; }

    UPrimalInventoryComponent* inv = GetPlayerInventory(pc);
    if (!inv) { Notify(pc, "No inventory found."); return; }

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!EnsureConnected()) { Notify(pc, "Storage is temporarily unavailable."); return; }

    PlayerStorage storage;
    if (!LoadStorage(eosId, storage)) { Notify(pc, "Storage is temporarily unavailable."); return; }

    const int allowance = ResolveSlots(eosId);
    UWorld* world = AsaApi::GetApiUtils().GetWorld();

    std::set<UPrimalItem*> reserved;
    {
        TArray<UPrimalItem*>& slots = inv->ItemSlotsField();
        for (int i = 0; i < slots.Num(); ++i)
            if (slots[i]) reserved.insert(slots[i]);
    }

    struct Aggregate
    {
        std::string               path;
        long long                 qty = 0;
        int                       stack = 1;
        std::vector<UPrimalItem*> items;
    };

    std::map<std::string, Aggregate> aggregates;
    std::vector<UPrimalItem*> instanceItems;

    TArray<UPrimalItem*>& items = inv->InventoryItemsField();
    for (int i = 0; i < items.Num(); ++i)
    {
        UPrimalItem* item = items[i];
        if (!item) continue;
        if (item->bEquippedItem()()) continue;
        if (reserved.count(item)) continue;

        const ItemKind kind = ClassifyItem(item);
        if (kind == ItemKind::Blocked) continue;

        const std::string path = ItemPath(item);
        if (g_blacklist_bp.count(path)) continue;

        if (kind == ItemKind::Blueprint)
        {
            if (g_allow_blueprints) instanceItems.push_back(item);
            continue;
        }
        if (kind == ItemKind::Instance)
        {
            instanceItems.push_back(item);
            continue;
        }

        const std::string name = ItemDisplayName(item);
        if (name.empty()) continue;

        const long long q = item->GetItemQuantity();
        if (q <= 0) continue;

        Aggregate& a = aggregates[name];
        if (a.path.empty())
        {
            a.path = path;
            const int s = item->GetMaxItemQuantity(reinterpret_cast<UObject*>(world));
            a.stack = (s > 0) ? s : 1;
        }
        a.qty += q;
        a.items.push_back(item);
    }

    long long free = (long long)allowance - storage.UsedSlots();
    if (free < 0) free = 0;

    int bankedTypes = 0;
    int bankedInstances = 0;

    for (const auto& kv : aggregates)
    {
        if (free <= 0) break;

        const Aggregate& a = kv.second;
        FungibleRow* row = storage.Find(kv.first);
        const long long existingQty = row ? row->qty : 0;
        const int existingSlots = row ? row->slots : 0;

        long long take = a.qty;
        const long long maxQty = ((long long)existingSlots + free) * (long long)a.stack;
        if (existingQty + take > maxQty) take = maxQty - existingQty;
        if (take <= 0) continue;

        const long long newQty = existingQty + take;
        const int newSlots = SlotsFor(newQty, a.stack);
        const long long delta = (long long)newSlots - existingSlots;
        if (delta > free) continue;

        if (!row)
        {
            FungibleRow fresh;
            fresh.name = kv.first;
            storage.fungibles.push_back(fresh);
            row = &storage.fungibles.back();
        }

        row->path = a.path;
        row->qty = newQty;
        row->slots = newSlots;
        row->stack = a.stack;
        row->dirty = true;

        if (!FlushFungibles(eosId, storage))
        {
            Notify(pc, "Storage write failed. Stopped early.");
            break;
        }

        long long remaining = take;
        for (UPrimalItem* item : a.items)
        {
            if (remaining <= 0) break;
            if (!item) continue;

            const long long have = item->GetItemQuantity();
            if (have <= 0) continue;

            if (remaining >= have)
            {
                FItemNetID id = item->ItemIDField();
                if (inv->RemoveItem(&id, false, false, true, false)) remaining -= have;
            }
            else
            {
                item->IncrementItemQuantity(-(int)remaining, true, false, false, false, false, false);
                const long long now = item->GetItemQuantity();
                remaining -= (have - now);
            }
        }

        if (remaining > 0)
        {
            row->qty = newQty - remaining;
            row->slots = SlotsFor(row->qty, row->stack);
            row->dirty = true;
            if (!FlushFungibles(eosId, storage))
                Log::GetLog()->error("[CloudStorage] Failed to correct {} of {} for {} during uploadall",
                    remaining, kv.first, eosId);
        }

        free -= (long long)row->slots - existingSlots;
        ++bankedTypes;
    }

    for (UPrimalItem* item : instanceItems)
    {
        if (free <= 0) break;
        if (!item) continue;

        const std::string blob = ItemToBlob(item);
        if (blob.empty()) continue;

        InstanceRow row;
        row.name = ItemDisplayName(item);
        row.path = ItemPath(item);
        row.blueprint = (ClassifyItem(item) == ItemKind::Blueprint);

        long long newId = 0;
        if (!WriteInstance(eosId, row, blob, newId)) continue;

        FItemNetID id = item->ItemIDField();
        if (!inv->RemoveItem(&id, false, false, true, false))
        {
            if (!DeleteInstance(eosId, newId))
                Log::GetLog()->error("[CloudStorage] Orphaned instance row {} for {} during uploadall", newId, eosId);
            continue;
        }

        row.id = newId;
        storage.instances.push_back(row);
        --free;
        ++bankedInstances;
    }

    if (bankedTypes == 0 && bankedInstances == 0)
    {
        Notify(pc, "Nothing eligible to store, or no free slots.");
        return;
    }

    Notify(pc, "Stored " + std::to_string(bankedTypes) + " item type(s) and " +
        std::to_string(bankedInstances) + " gear/blueprint(s)." +
        SlotsSuffix(storage.UsedSlots(), allowance));
}

// =============================================================================
// Command - /downloadall
// =============================================================================

static void DownloadAllCommand(AShooterPlayerController* pc, FString* message, int, int)
{
    if (!pc) return;

    const std::string eosId = GetEosId(pc);
    if (eosId.empty()) { Notify(pc, "Could not resolve your id."); return; }

    UPrimalInventoryComponent* inv = GetPlayerInventory(pc);
    if (!inv) { Notify(pc, "No inventory found."); return; }

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!EnsureConnected()) { Notify(pc, "Storage is temporarily unavailable."); return; }

    PlayerStorage storage;
    if (!LoadStorage(eosId, storage)) { Notify(pc, "Storage is temporarily unavailable."); return; }

    const int allowance = ResolveSlots(eosId);

    long long movedItems = 0;
    bool capHit = false;

    for (auto& f : storage.fungibles)
    {
        if (capHit) break;
        if (f.path.empty() || f.qty <= 0) continue;

        FString fPath = ToFStr(f.path);
        UClass* cls = UVictoryCore::BPLoadClass(fPath);
        if (!cls) continue;

        const long long want = f.qty;
        const long long prevQty = f.qty;
        const int prevSlots = f.slots;

        f.qty = 0;
        f.slots = 0;
        f.dirty = true;

        if (!FlushFungibles(eosId, storage))
        {
            f.qty = prevQty;
            f.slots = prevSlots;
            f.dirty = false;
            Notify(pc, "Storage write failed. Stopped early.");
            capHit = true;
            break;
        }

        const long long given = GiveFungible(inv, cls, want, f.stack);

        if (given < want)
        {
            f.qty = prevQty - given;
            f.slots = SlotsFor(f.qty, f.stack);
            f.dirty = true;
            if (!FlushFungibles(eosId, storage))
                Log::GetLog()->error("[CloudStorage] Failed to restore {} of {} for {} during downloadall",
                    want - given, f.name, eosId);
            capHit = true;
        }

        movedItems += given;
    }

    storage.fungibles.erase(
        std::remove_if(storage.fungibles.begin(), storage.fungibles.end(),
            [](const FungibleRow& f) { return f.qty <= 0; }),
        storage.fungibles.end());

    if (!capHit)
    {
        for (auto& r : storage.instances)
        {
            std::string blob;
            if (!ReadInstanceBlob(eosId, r.id, blob)) continue;
            if (!DeleteInstance(eosId, r.id)) continue;

            if (!BlobToItem(inv, blob))
            {
                long long restored = 0;
                if (!WriteInstance(eosId, r, blob, restored))
                    Log::GetLog()->error("[CloudStorage] Lost instance {} for {} during downloadall", r.id, eosId);
                else
                    r.id = restored;
                capHit = true;
                break;
            }

            r.id = 0;
            movedItems += 1;
        }

        storage.instances.erase(
            std::remove_if(storage.instances.begin(), storage.instances.end(),
                [](const InstanceRow& r) { return r.id == 0; }),
            storage.instances.end());
    }

    if (movedItems <= 0 && !capHit) { Notify(pc, "You have nothing stored."); return; }

    if (capHit)
        Notify(pc, "Downloaded what fit." + SlotsSuffix(storage.UsedSlots(), allowance));
    else
        Notify(pc, "Downloaded everything." + SlotsSuffix(storage.UsedSlots(), allowance));
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
        if (LoadConfig(false))
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

    if (!LoadConfig(true))
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

    g_reg_upload = g_cmd_upload;
    g_reg_download = g_cmd_download;
    g_reg_list = g_cmd_list;
    g_reg_uploadall = g_cmd_uploadall;
    g_reg_downloadall = g_cmd_downloadall;

    AsaApi::GetCommands().AddChatCommand(ToFStr(g_reg_upload), &UploadCommand);
    AsaApi::GetCommands().AddChatCommand(ToFStr(g_reg_download), &DownloadCommand);
    AsaApi::GetCommands().AddChatCommand(ToFStr(g_reg_list), &ListCommand);
    AsaApi::GetCommands().AddChatCommand(ToFStr(g_reg_uploadall), &UploadAllCommand);
    AsaApi::GetCommands().AddChatCommand(ToFStr(g_reg_downloadall), &DownloadAllCommand);
    AsaApi::GetCommands().AddOnTickCallback(FString(L"CloudStorage_Tick"), &OnTick);

    Log::GetLog()->info("[CloudStorage] Plugin loaded");
}

static void Plugin_Unload_Impl()
{
    AsaApi::GetCommands().RemoveChatCommand(ToFStr(g_reg_upload));
    AsaApi::GetCommands().RemoveChatCommand(ToFStr(g_reg_download));
    AsaApi::GetCommands().RemoveChatCommand(ToFStr(g_reg_list));
    AsaApi::GetCommands().RemoveChatCommand(ToFStr(g_reg_uploadall));
    AsaApi::GetCommands().RemoveChatCommand(ToFStr(g_reg_downloadall));
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