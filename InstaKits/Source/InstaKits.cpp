/*
InstaKits - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * InstaKits - ASA Plugin
 * Gives a rank-based item kit to a player on spawn. Highest rank (lowest Order) wins.
 * Recurring kits deliver every spawn; one-time kits deliver once per EOS per group, tracked in MariaDB.
 *
 * Hooks:
 *   AShooterPlayerController.HandleRespawned_Implementation - queue kit delivery on spawn
 *   AShooterGameMode.Tick                                   - deliver queued kits, config hot-reload
 *
 * Config:
 *   MessageColor       - RichColor string for the delivery message
 *   DeliverMessage     - message shown on delivery, empty for silent
 *   UnlockTekEngrams   - unlock tek engrams before equipping so tek gear can be worn
 *   DbHost DbPort DbUser DbPass DbName - MariaDB connection for one-time claim tracking
 *   Kits               - array of Group, Order, Recurring, Items (BlueprintPath, Quantity, Quality, Equip, Slot, Tier)
 */

#include <API/ARK/Ark.h>
#include <json.hpp>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>
#include <mutex>
#include <ctime>
#include <sys/stat.h>
#include <cctype>

#pragma comment(lib, "AsaApi")
#pragma warning(disable: 4191)

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
            Log::GetLog()->info("[InstaKits] Loaded DB library: {}", candidates[i]);
            break;
        }
    }

    if (!g_mysql_module)
    {
        Log::GetLog()->error("[InstaKits] Could not find libmariadb.dll or libmysql.dll");
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
        Log::GetLog()->error("[InstaKits] Failed to resolve required DB functions");
        return false;
    }

    g_mysql_loaded = true;
    return true;
}

struct ItemEntry
{
    std::string BlueprintPath;
    int Quantity = 1;
    float Quality = 0.0f;
    bool Equip = false;
    int Slot = -1;
    int TierIndex = -1;
};

struct Kit
{
    std::string Group;
    int Order = 0;
    bool Recurring = true;
    std::vector<ItemEntry> Items;
};

struct PendingDelivery
{
    std::string Eos;
    std::chrono::steady_clock::time_point DeliverAt;
};

static const std::string g_config_path = "ArkApi/Plugins/InstaKits/config.json";
static const int g_deliver_delay_ms = 1000;

static std::string g_message_color = "1.0,1.0,1.0,1.0";
static std::string g_deliver_message;
static bool g_unlock_tek_engrams = false;
static std::vector<Kit> g_kits;
static std::vector<PendingDelivery> g_pending;

static std::string g_db_host = "127.0.0.1";
static unsigned int g_db_port = 3306;
static std::string g_db_user;
static std::string g_db_pass;
static std::string g_db_name;
static MYSQL* g_db = nullptr;
static std::mutex g_db_mutex;

static time_t g_config_last_modified = 0;
static long long g_config_last_size = 0;
static std::chrono::steady_clock::time_point g_last_config_check;

static std::string FStr(const FString& f)
{
    const char* s = TCHAR_TO_UTF8(*f);
    return (s && s[0]) ? s : "";
}

static std::string ToLower(const std::string& in)
{
    std::string out = in;
    for (char& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

static int TierToIndex(const std::string& tier)
{
    std::string t = ToLower(tier);
    if (t == "primitive") return 0;
    if (t == "ramshackle") return 1;
    if (t == "apprentice") return 2;
    if (t == "journeyman") return 3;
    if (t == "mastercraft") return 4;
    if (t == "ascendant") return 5;
    return -1;
}

static std::string GetEos(AShooterPlayerController* pc)
{
    if (!pc) return "";
    AShooterPlayerState* ps = static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
    if (!ps) return "";
    FString raw;
    ps->GetUniqueNetIdAsString(&raw);
    return FStr(raw);
}

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

static bool LoadConfig()
{
    std::ifstream file(g_config_path);
    if (!file.is_open())
    {
        Log::GetLog()->error("[InstaKits] Cannot open config: {}", g_config_path);
        return false;
    }

    try
    {
        nlohmann::json j;
        file >> j;

        g_message_color = j.value("MessageColor", std::string("1.0,1.0,1.0,1.0"));
        g_deliver_message = j.value("DeliverMessage", std::string(""));
        g_unlock_tek_engrams = j.value("UnlockTekEngrams", false);

        g_db_host = j.value("DbHost", std::string("127.0.0.1"));
        g_db_port = j.value("DbPort", 3306u);
        g_db_user = j.value("DbUser", std::string(""));
        g_db_pass = j.value("DbPass", std::string(""));
        g_db_name = j.value("DbName", std::string(""));

        std::vector<Kit> newKits;
        if (j.contains("Kits") && j["Kits"].is_array())
        {
            for (auto& kj : j["Kits"])
            {
                if (!kj.is_object()) continue;
                Kit k;
                k.Group = ToLower(kj.value("Group", std::string("")));
                k.Order = kj.value("Order", 0);
                k.Recurring = kj.value("Recurring", true);
                if (k.Group.empty()) continue;

                if (kj.contains("Items") && kj["Items"].is_array())
                {
                    for (auto& ij : kj["Items"])
                    {
                        if (!ij.is_object()) continue;
                        ItemEntry e;
                        e.BlueprintPath = ij.value("BlueprintPath", std::string(""));
                        e.Quantity = ij.value("Quantity", 1);
                        e.Quality = ij.value("Quality", 0.0f);
                        e.Equip = ij.value("Equip", false);
                        e.Slot = ij.value("Slot", -1);
                        e.TierIndex = TierToIndex(ij.value("Tier", std::string("")));
                        if (!e.BlueprintPath.empty()) k.Items.push_back(e);
                    }
                }

                newKits.push_back(std::move(k));
            }
        }

        g_kits = std::move(newKits);
        g_config_last_modified = GetFileModTime(g_config_path);
        g_config_last_size = GetFileSize(g_config_path);
        Log::GetLog()->info("[InstaKits] Config loaded, {} kits", g_kits.size());
        return true;
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[InstaKits] Config parse error: {}", ex.what());
        return false;
    }
}

static void CheckConfigReload()
{
    long long sz = GetFileSize(g_config_path);
    if (sz == 0) return;
    time_t mt = GetFileModTime(g_config_path);
    if (mt == g_config_last_modified && sz == g_config_last_size) return;
    LoadConfig();
}

static bool InitDb()
{
    if (!LoadMySQLLib()) return false;

    std::lock_guard<std::mutex> lock(g_db_mutex);

    if (g_db)
    {
        pmysql_close(g_db);
        g_db = nullptr;
    }

    g_db = pmysql_init(nullptr);
    if (!g_db)
    {
        Log::GetLog()->error("[InstaKits] mysql_init failed");
        return false;
    }

    if (!pmysql_real_connect(g_db, g_db_host.c_str(), g_db_user.c_str(), g_db_pass.c_str(),
        g_db_name.c_str(), g_db_port, nullptr, 0))
    {
        Log::GetLog()->error("[InstaKits] DB connect failed: {}", pmysql_error(g_db));
        pmysql_close(g_db);
        g_db = nullptr;
        return false;
    }

    const char* create =
        "CREATE TABLE IF NOT EXISTS instakits_claims ("
        "eos_id VARCHAR(64) NOT NULL,"
        "kit_group VARCHAR(128) NOT NULL,"
        "claimed_at BIGINT NOT NULL DEFAULT 0,"
        "PRIMARY KEY (eos_id, kit_group))";

    if (pmysql_query(g_db, create))
    {
        Log::GetLog()->error("[InstaKits] Create table failed: {}", pmysql_error(g_db));
        return false;
    }

    Log::GetLog()->info("[InstaKits] DB connected");
    return true;
}

static std::string EscapeUnsafe(const std::string& in)
{
    if (!g_db || !pmysql_real_escape_string) return in;
    std::vector<char> buf(in.size() * 2 + 1);
    unsigned long len = pmysql_real_escape_string(g_db, buf.data(), in.c_str(), (unsigned long)in.size());
    return std::string(buf.data(), len);
}

static bool GetClaimed(const std::string& eos, const std::string& group, bool& outClaimed)
{
    outClaimed = false;
    std::string safeEos = EscapeUnsafe(eos);
    std::string safeGroup = EscapeUnsafe(group);

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!g_db) return false;

    std::string q = "SELECT 1 FROM instakits_claims WHERE eos_id='" + safeEos +
        "' AND kit_group='" + safeGroup + "' LIMIT 1";
    if (pmysql_query(g_db, q.c_str()))
    {
        Log::GetLog()->error("[InstaKits] Claim query failed: {}", pmysql_error(g_db));
        return false;
    }

    MYSQL_RES* res = pmysql_store_result(g_db);
    if (!res) return false;

    MYSQL_ROW row = pmysql_fetch_row(res);
    outClaimed = (row != nullptr);
    pmysql_free_result(res);
    return true;
}

static void RecordClaim(const std::string& eos, const std::string& group, long long now)
{
    std::string safeEos = EscapeUnsafe(eos);
    std::string safeGroup = EscapeUnsafe(group);

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!g_db) return;

    std::string q =
        "INSERT IGNORE INTO instakits_claims (eos_id, kit_group, claimed_at) VALUES ('" +
        safeEos + "', '" + safeGroup + "', " + std::to_string(now) + ")";

    if (pmysql_query(g_db, q.c_str()))
        Log::GetLog()->error("[InstaKits] Record claim failed: {}", pmysql_error(g_db));
}

typedef TArray<FString>(*GetPlayerGroups_t)(const FString&);
static GetPlayerGroups_t pGetPlayerGroups = nullptr;
static bool g_permissions_loaded = false;

static void LoadPermissionsAPI()
{
    if (g_permissions_loaded) return;

    HMODULE hMod = GetModuleHandleA("Permissions");
    if (!hMod) return;

    pGetPlayerGroups = (GetPlayerGroups_t)GetProcAddress(hMod,
        "?GetPlayerGroups@Permissions@@YA?AV?$TArray@VFString@@V?$TSizedDefaultAllocator@$0CA@@@@@AEBVFString@@@Z");

    if (!pGetPlayerGroups)
    {
        Log::GetLog()->warn("[InstaKits] Failed to resolve Permissions functions");
        return;
    }

    g_permissions_loaded = true;
    Log::GetLog()->info("[InstaKits] Permissions API loaded");
}

static bool GetGroups(const std::string& eosId, std::vector<std::string>& out)
{
    out.clear();

    if (!g_permissions_loaded)
    {
        out.push_back("default");
        return true;
    }

    std::wstring wEos(eosId.begin(), eosId.end());
    FString fEos(wEos.c_str());
    TArray<FString> groups = pGetPlayerGroups(fEos);

    if (groups.Num() == 0)
        return false;

    for (int i = 0; i < groups.Num(); ++i)
        out.push_back(ToLower(FStr(groups[i])));
    return true;
}

static const Kit* ResolveKit(const std::vector<std::string>& groups)
{
    const Kit* best = nullptr;
    for (const auto& k : g_kits)
    {
        bool inGroup = false;
        for (const auto& g : groups)
            if (g == k.Group) { inGroup = true; break; }
        if (!inGroup) continue;
        if (!best || k.Order < best->Order)
            best = &k;
    }
    return best;
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

static void UnlockTekEngrams(AShooterPlayerController* pc)
{
    if (!pc) return;

    UClass* cmClass = UShooterCheatManager::StaticClass();
    if (!cmClass) return;

    FStaticConstructObjectParameters params{};
    params.Class = cmClass;
    params.Outer = pc;
    params.Name = FName();
    params.SetFlags = EObjectFlags::RF_NoFlags;
    params.InternalSetFlags = EInternalObjectFlags::None;
    params.bCopyTransientsFromClassDefaults = false;
    params.bAssumeTemplateIsArchetype = false;
    params.Template = nullptr;
    params.InstanceGraph = nullptr;
    params.ExternalPackage = nullptr;
    params.SubobjectOverrides = nullptr;

    UShooterCheatManager* cm = static_cast<UShooterCheatManager*>(
        NativeCall<UObject*, FStaticConstructObjectParameters&>(nullptr,
            "Global.StaticConstructObject_Internal(FStaticConstructObjectParameters&)", params));
    if (!cm) return;

    cm->MyPCField() = pc;
    cm->InitCheatManager();

    auto& cmFieldRef = pc->CheatManagerField();
    UPTRINT* cmRawPtr = reinterpret_cast<UPTRINT*>(&cmFieldRef);
    UPTRINT savedCMPtr = *cmRawPtr;
    *cmRawPtr = reinterpret_cast<UPTRINT>(cm);

    const bool wasAdmin = pc->bIsAdmin()();
    if (!wasAdmin)
        pc->bIsAdmin() = true;

    cm->GiveEngramsTekOnly();

    if (!wasAdmin)
        pc->bIsAdmin() = false;

    *cmRawPtr = savedCMPtr;
    cm->ConditionalBeginDestroy();
}

static void GiveItem(AShooterPlayerController* pc, UPrimalInventoryComponent* inv, const ItemEntry& it)
{
    if (!inv) return;

    std::wstring wPath(it.BlueprintPath.begin(), it.BlueprintPath.end());
    FString fPath(wPath.c_str());
    UClass* itemClass = UVictoryCore::BPLoadClass(fPath);
    if (!itemClass)
    {
        Log::GetLog()->warn("[InstaKits] Item BPLoadClass failed for '{}'", it.BlueprintPath);
        return;
    }

    TSubclassOf<UPrimalItem> itemSub = itemClass;
    TSubclassOf<UPrimalItem> noSkin{};
    UPrimalItem* added = UPrimalItem::AddNewItem(
        itemSub, inv, false, false, it.Quality, false, it.Quantity, false, 0.0f, true,
        noSkin, 0.0f, false, true, true, true, true);

    if (!added) return;

    if (it.TierIndex >= 0)
        added->ItemQualityIndexField() = (unsigned char)it.TierIndex;

    if (it.Equip)
    {
        FItemNetID id = added->ItemIDField();
        inv->ServerEquipItem(id, pc);
    }
    else if (it.Slot >= 0 && it.Slot <= 9)
    {
        FItemNetID id = added->ItemIDField();
        inv->ServerAddItemToSlot(id, it.Slot, true);
    }
}

static void GiveKit(AShooterPlayerController* pc, AShooterCharacter* ch, const Kit& kit)
{
    if (!ch) return;
    UPrimalInventoryComponent* inv = ch->MyInventoryComponentField();
    if (!inv) return;

    if (g_unlock_tek_engrams)
        UnlockTekEngrams(pc);

    for (const auto& it : kit.Items)
        GiveItem(pc, inv, it);

    if (!g_deliver_message.empty())
        Notify(pc, std::wstring(g_deliver_message.begin(), g_deliver_message.end()));
}

static void ProcessPending()
{
    if (g_pending.empty()) return;

    auto now = std::chrono::steady_clock::now();

    for (size_t i = 0; i < g_pending.size(); )
    {
        if (now < g_pending[i].DeliverAt)
        {
            ++i;
            continue;
        }

        std::string eos = g_pending[i].Eos;
        std::wstring wEos(eos.begin(), eos.end());
        FString fEos(wEos.c_str());
        AShooterPlayerController* pc = AsaApi::GetApiUtils().FindPlayerFromEOSID(fEos);

        if (pc)
        {
            std::vector<std::string> groups;
            if (GetGroups(eos, groups))
            {
                const Kit* kit = ResolveKit(groups);
                if (kit)
                {
                    bool deliver = true;
                    if (!kit->Recurring)
                    {
                        bool claimed = false;
                        if (!GetClaimed(eos, kit->Group, claimed))
                            deliver = false;
                        else if (claimed)
                            deliver = false;
                    }

                    if (deliver)
                    {
                        AActor* charActor = pc->BaseGetPlayerCharacter();
                        AShooterCharacter* ch = charActor ? static_cast<AShooterCharacter*>(charActor) : nullptr;
                        if (ch)
                        {
                            GiveKit(pc, ch, *kit);
                            if (!kit->Recurring)
                                RecordClaim(eos, kit->Group, (long long)std::time(nullptr));
                        }
                    }
                }
            }
        }

        g_pending.erase(g_pending.begin() + i);
    }
}

using HandleRespawned_t = void(*)(AShooterPlayerController*, APawn*, bool);
static HandleRespawned_t Original_HandleRespawned = nullptr;

void Detour_HandleRespawned(AShooterPlayerController* pc, APawn* pawn, bool bNewPlayer)
{
    Original_HandleRespawned(pc, pawn, bNewPlayer);

    if (!pc || !pawn) return;

    LoadPermissionsAPI();

    std::string eos = GetEos(pc);
    if (eos.empty()) return;

    std::vector<std::string> groups;
    if (!GetGroups(eos, groups)) return;
    if (!ResolveKit(groups)) return;

    PendingDelivery pd;
    pd.Eos = eos;
    pd.DeliverAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(g_deliver_delay_ms);
    g_pending.push_back(pd);
}

using Tick_t = void(*)(AShooterGameMode*, float);
static Tick_t Original_Tick = nullptr;

static void Detour_Tick(AShooterGameMode* gm, float dt)
{
    Original_Tick(gm, dt);

    ProcessPending();

    auto now = std::chrono::steady_clock::now();
    auto sinceCheck = std::chrono::duration_cast<std::chrono::seconds>(now - g_last_config_check).count();
    if (sinceCheck >= 10)
    {
        g_last_config_check = now;
        CheckConfigReload();
    }
}

static void PluginInit()
{
    Log::Get().Init("InstaKits");

    if (!LoadConfig())
        Log::GetLog()->error("[InstaKits] Failed to load config");

    InitDb();

    g_last_config_check = std::chrono::steady_clock::now();

    AsaApi::GetHooks().SetHook(
        "AShooterPlayerController.HandleRespawned_Implementation(APawn*,bool)",
        (LPVOID)&Detour_HandleRespawned,
        (LPVOID*)&Original_HandleRespawned);

    AsaApi::GetHooks().SetHook(
        "AShooterGameMode.Tick(float)",
        (LPVOID)&Detour_Tick,
        (LPVOID*)&Original_Tick);

    Log::GetLog()->info("[InstaKits] Loaded");
}

static void PluginUnload()
{
    AsaApi::GetHooks().DisableHook(
        "AShooterPlayerController.HandleRespawned_Implementation(APawn*,bool)",
        (LPVOID)&Detour_HandleRespawned);

    AsaApi::GetHooks().DisableHook(
        "AShooterGameMode.Tick(float)",
        (LPVOID)&Detour_Tick);

    if (g_db)
    {
        pmysql_close(g_db);
        g_db = nullptr;
    }

    Log::GetLog()->info("[InstaKits] Unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { PluginInit(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->critical("[InstaKits] Init exception: {}", e.what());
    }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { PluginUnload(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->error("[InstaKits] Unload exception: {}", e.what());
    }
}