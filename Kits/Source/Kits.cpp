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

#pragma comment(lib, "AsaApi.lib")
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
            Log::GetLog()->info("[Kits] Loaded DB library: {}", candidates[i]);
            break;
        }
    }

    if (!g_mysql_module)
    {
        Log::GetLog()->error("[Kits] Could not find libmariadb.dll or libmysql.dll");
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
        Log::GetLog()->error("[Kits] Failed to resolve required DB functions");
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
};

struct DinoEntry
{
    std::string BlueprintPath;
    int Level = 1;
    bool Neutered = false;
};

struct RankAccess
{
    std::string Group;
    long long CooldownSeconds = 0;
    int MaxUses = 0;
};

struct Kit
{
    std::string Name;
    std::vector<ItemEntry> Items;
    std::vector<DinoEntry> Dinos;
    std::vector<RankAccess> Ranks;
};

static const std::string g_config_path = "ArkApi/Plugins/Kits/config.json";

static std::string g_db_host = "127.0.0.1";
static unsigned int g_db_port = 3306;
static std::string g_db_user;
static std::string g_db_pass;
static std::string g_db_name;
static std::string g_cryopod_bp;
static std::string g_pelayori_cryopod_bp;
static bool g_use_pelayori_cryo = false;
static std::string g_message_color = "1.0,1.0,1.0,1.0";
static std::vector<Kit> g_kits;

static time_t g_config_last_modified = 0;
static long long g_config_last_size = 0;
static std::chrono::steady_clock::time_point g_last_config_check;

static MYSQL* g_db = nullptr;
static std::mutex g_db_mutex;

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
        Log::GetLog()->error("[Kits] Cannot open config: {}", g_config_path);
        return false;
    }

    try
    {
        nlohmann::json j;
        file >> j;

        g_db_host = j.value("DbHost", std::string("127.0.0.1"));
        g_db_port = j.value("DbPort", 3306u);
        g_db_user = j.value("DbUser", std::string(""));
        g_db_pass = j.value("DbPass", std::string(""));
        g_db_name = j.value("DbName", std::string(""));
        g_cryopod_bp = j.value("CryopodBlueprint", std::string(""));
        g_pelayori_cryopod_bp = j.value("PelayoriCryopodBlueprint", std::string("Blueprint'/Cryopods/Cryopods/PrimalItem_WeaponEmptyCryopod_Mod.PrimalItem_WeaponEmptyCryopod_Mod'"));
        g_use_pelayori_cryo = j.value("UsePelayoriCryo", false);
        g_message_color = j.value("MessageColor", std::string("1.0,1.0,1.0,1.0"));

        std::vector<Kit> newKits;
        if (j.contains("Kits") && j["Kits"].is_array())
        {
            for (auto& kj : j["Kits"])
            {
                if (!kj.is_object()) continue;
                Kit k;
                k.Name = kj.value("Name", std::string(""));
                if (k.Name.empty()) continue;

                if (kj.contains("Items") && kj["Items"].is_array())
                {
                    for (auto& ij : kj["Items"])
                    {
                        if (!ij.is_object()) continue;
                        ItemEntry e;
                        e.BlueprintPath = ij.value("BlueprintPath", std::string(""));
                        e.Quantity = ij.value("Quantity", 1);
                        e.Quality = ij.value("Quality", 0.0f);
                        if (!e.BlueprintPath.empty()) k.Items.push_back(e);
                    }
                }

                if (kj.contains("Dinos") && kj["Dinos"].is_array())
                {
                    for (auto& dj : kj["Dinos"])
                    {
                        if (!dj.is_object()) continue;
                        DinoEntry e;
                        e.BlueprintPath = dj.value("BlueprintPath", std::string(""));
                        e.Level = dj.value("Level", 1);
                        e.Neutered = dj.value("Neutered", false);
                        if (!e.BlueprintPath.empty()) k.Dinos.push_back(e);
                    }
                }

                if (kj.contains("Ranks") && kj["Ranks"].is_array())
                {
                    for (auto& rj : kj["Ranks"])
                    {
                        if (!rj.is_object()) continue;
                        RankAccess r;
                        r.Group = ToLower(rj.value("Group", std::string("")));
                        r.CooldownSeconds = rj.value("CooldownSeconds", (long long)0);
                        r.MaxUses = rj.value("MaxUses", 0);
                        if (!r.Group.empty()) k.Ranks.push_back(r);
                    }
                }

                newKits.push_back(std::move(k));
            }
        }

        g_kits = std::move(newKits);
        g_config_last_modified = GetFileModTime(g_config_path);
        g_config_last_size = GetFileSize(g_config_path);
        Log::GetLog()->info("[Kits] Config loaded, {} kits", g_kits.size());
        return true;
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[Kits] Config parse error: {}", ex.what());
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
        Log::GetLog()->error("[Kits] mysql_init failed");
        return false;
    }

    if (!pmysql_real_connect(g_db, g_db_host.c_str(), g_db_user.c_str(), g_db_pass.c_str(),
        g_db_name.c_str(), g_db_port, nullptr, 0))
    {
        Log::GetLog()->error("[Kits] DB connect failed: {}", pmysql_error(g_db));
        pmysql_close(g_db);
        g_db = nullptr;
        return false;
    }

    const char* create =
        "CREATE TABLE IF NOT EXISTS kits_usage ("
        "eos_id VARCHAR(64) NOT NULL,"
        "kit_name VARCHAR(128) NOT NULL,"
        "uses INT NOT NULL DEFAULT 0,"
        "last_redeem BIGINT NOT NULL DEFAULT 0,"
        "PRIMARY KEY (eos_id, kit_name))";

    if (pmysql_query(g_db, create))
    {
        Log::GetLog()->error("[Kits] Create table failed: {}", pmysql_error(g_db));
        return false;
    }

    Log::GetLog()->info("[Kits] DB connected");
    return true;
}

static std::string EscapeUnsafe(const std::string& in)
{
    if (!g_db || !pmysql_real_escape_string) return in;
    std::vector<char> buf(in.size() * 2 + 1);
    unsigned long len = pmysql_real_escape_string(g_db, buf.data(), in.c_str(), (unsigned long)in.size());
    return std::string(buf.data(), len);
}

static bool GetUsage(const std::string& eos, const std::string& kitName, int& outUses, long long& outLast)
{
    outUses = 0;
    outLast = 0;
    std::string safeEos = EscapeUnsafe(eos);
    std::string safeKit = EscapeUnsafe(kitName);

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!g_db) return false;

    std::string q = "SELECT uses, last_redeem FROM kits_usage WHERE eos_id='" + safeEos +
        "' AND kit_name='" + safeKit + "' LIMIT 1";
    if (pmysql_query(g_db, q.c_str()))
    {
        Log::GetLog()->error("[Kits] Usage query failed: {}", pmysql_error(g_db));
        return false;
    }

    MYSQL_RES* res = pmysql_store_result(g_db);
    if (!res) return false;

    MYSQL_ROW row = pmysql_fetch_row(res);
    if (row)
    {
        outUses = row[0] ? std::atoi(row[0]) : 0;
        outLast = row[1] ? std::atoll(row[1]) : 0;
    }
    pmysql_free_result(res);
    return true;
}

static void RecordUse(const std::string& eos, const std::string& kitName, long long now)
{
    std::string safeEos = EscapeUnsafe(eos);
    std::string safeKit = EscapeUnsafe(kitName);

    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!g_db) return;

    std::string q =
        "INSERT INTO kits_usage (eos_id, kit_name, uses, last_redeem) VALUES ('" +
        safeEos + "', '" + safeKit + "', 1, " + std::to_string(now) + ") "
        "ON DUPLICATE KEY UPDATE uses = uses + 1, last_redeem = " + std::to_string(now);

    if (pmysql_query(g_db, q.c_str()))
        Log::GetLog()->error("[Kits] Record use failed: {}", pmysql_error(g_db));
}

typedef TArray<FString>(*GetPlayerGroups_t)(const FString&);
static GetPlayerGroups_t pGetPlayerGroups = nullptr;
static bool g_permissions_loaded = false;
static bool g_permissions_attempted = false;

static void LoadPermissionsAPI()
{
    if (g_permissions_attempted) return;
    g_permissions_attempted = true;

    HMODULE hMod = GetModuleHandleA("Permissions");
    if (!hMod)
    {
        Log::GetLog()->warn("[Kits] Permissions.dll not found, using Default group");
        return;
    }

    pGetPlayerGroups = (GetPlayerGroups_t)GetProcAddress(hMod,
        "?GetPlayerGroups@Permissions@@YA?AV?$TArray@VFString@@V?$TSizedDefaultAllocator@$0CA@@@@@AEBVFString@@@Z");

    if (!pGetPlayerGroups)
    {
        Log::GetLog()->warn("[Kits] Failed to resolve Permissions functions");
        return;
    }

    g_permissions_loaded = true;
    Log::GetLog()->info("[Kits] Permissions API loaded");
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

static const RankAccess* ResolveRank(const Kit& k, const std::vector<std::string>& groups)
{
    const RankAccess* best = nullptr;
    for (const auto& r : k.Ranks)
        for (const auto& g : groups)
            if (g == r.Group)
            {
                if (!best || r.CooldownSeconds < best->CooldownSeconds)
                    best = &r;
            }
    return best;
}

static const Kit* FindKit(const std::string& name)
{
    std::string lname = ToLower(name);
    for (const auto& k : g_kits)
        if (ToLower(k.Name) == lname)
            return &k;
    return nullptr;
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

static UClass* LoadBP(const std::string& path)
{
    std::wstring w(path.begin(), path.end());
    FString f(w.c_str());
    return UVictoryCore::BPLoadClass(f);
}

static bool ValidateKit(AShooterPlayerController* pc, const Kit& k)
{
    AActor* charActor = pc->BaseGetPlayerCharacter();
    if (!charActor) return false;
    AShooterCharacter* ch = static_cast<AShooterCharacter*>(charActor);
    UPrimalInventoryComponent* inv = ch->MyInventoryComponentField();
    if (!inv) return false;

    for (const auto& it : k.Items)
        if (!LoadBP(it.BlueprintPath))
        {
            Log::GetLog()->warn("[Kits] Validate failed, item BP '{}'", it.BlueprintPath);
            return false;
        }

    for (const auto& d : k.Dinos)
        if (!LoadBP(d.BlueprintPath))
        {
            Log::GetLog()->warn("[Kits] Validate failed, dino BP '{}'", d.BlueprintPath);
            return false;
        }

    if (!k.Dinos.empty())
    {
        const std::string& podPath = g_use_pelayori_cryo ? g_pelayori_cryopod_bp : g_cryopod_bp;
        if (!LoadBP(podPath))
        {
            Log::GetLog()->warn("[Kits] Validate failed, cryopod BP '{}'", podPath);
            return false;
        }
    }

    return true;
}

static TArray<float> GetStatPoints(APrimalDinoCharacter* dino)
{
    TArray<float> floats;
    UPrimalCharacterStatusComponent* comp = dino->GetCharacterStatusComponent();
    int NumEntries = EPrimalCharacterStatusValue::MAX - 1;
    for (int i = 0; i < NumEntries; i++)
        floats.Add(
            comp->NumberOfLevelUpPointsAppliedField()()[(EPrimalCharacterStatusValue::Type)i]
            +
            comp->NumberOfMutationsAppliedTamedField()()[(EPrimalCharacterStatusValue::Type)i]
        );
    for (int i = 0; i < NumEntries; i++)
        floats.Add(comp->NumberOfLevelUpPointsAppliedTamedField()()[(EPrimalCharacterStatusValue::Type)i]);
    return floats;
}

static TArray<float> GetCharacterStatsAsFloats(APrimalDinoCharacter* dino)
{
    TArray<float> floats;
    UPrimalCharacterStatusComponent* comp = dino->GetCharacterStatusComponent();
    int NumEntries = EPrimalCharacterStatusValue::MAX - 1;
    for (int i = 0; i < NumEntries; i++)
        floats.Add(comp->CurrentStatusValuesField()()[(EPrimalCharacterStatusValue::Type)i]);
    for (int i = 0; i < NumEntries; i++)
        floats.Add(comp->MaxStatusValuesField()()[(EPrimalCharacterStatusValue::Type)i]);
    for (int i = 0; i < NumEntries; i++)
        floats.Add(comp->GetStatusValueRecoveryRate((EPrimalCharacterStatusValue::Type)i));
    floats.Add((float)dino->bIsFemale()());
    int len = floats.Num();
    floats.Append(GetStatPoints(dino));
    floats.Add(len);
    return floats;
}

static TArray<FString> GetDinoDataStrings(APrimalDinoCharacter* dino, const FString& dinoNameInMAP, const FString& dinoName)
{
    TArray<FString> strings;
    strings.Reserve(11);
    strings.Add(dinoNameInMAP);
    strings.Add(dinoName);
    FString tmp;
    dino->GetColorSetInidcesAsString(&tmp);
    strings.Add(tmp);
    strings.Add(dino->bNeutered()() ? "NEUTERED" : "");
    tmp = "";
    if (dino->bUsesGender()())
        if (dino->bIsFemale()())
            tmp = "FEMALE";
        else
            tmp = "MALE";
    strings.Add(tmp);
    strings.Add("");
    strings.Add("0");
    strings.Add("");
    strings.Add("");
    if (!dino->DinoNameTagField().IsNone())
        strings.Add(dino->DinoNameTagField().ToString());
    else
        strings.Add("");
    strings.Add("");
    return strings;
}

static FCustomItemData GetDinoCustomItemData(APrimalDinoCharacter* dino)
{
    FCustomItemData customItemData;

    FARKDinoData dinoData;
    dino->GetDinoData(&dinoData);

    customItemData.CustomDataName = FName("Dino", EFindName::FNAME_Add);

    TArray<FName> names;
    dino->GetColorSetNamesAsArray(&names);
    customItemData.CustomDataNames = names;
    customItemData.CustomDataNames.Add(FName("MissionTemporary", EFindName::FNAME_Add));
    customItemData.CustomDataNames.Add(FName("None", EFindName::FNAME_Find));

    customItemData.CustomDataFloats = GetCharacterStatsAsFloats(dino);

    auto t1 = AsaApi::GetApiUtils().GetShooterGameMode()->GetWorld()->TimeSecondsField();
    customItemData.CustomDataDoubles.Doubles.Add(t1);
    customItemData.CustomDataDoubles.Doubles.Add(dino->BabyNextCuddleTimeField() - t1);
    customItemData.CustomDataDoubles.Doubles.Add(dino->NextAllowedMatingTimeField());

    const float d1 = static_cast<float>(dino->RandomMutationsMaleField());
    customItemData.CustomDataDoubles.Doubles.Add(static_cast<double>(d1));

    const float d3 = static_cast<float>(dino->RandomMutationsFemaleField());
    customItemData.CustomDataDoubles.Doubles.Add(static_cast<double>(d3));

    auto stat = dino->MyCharacterStatusComponentField();
    if (stat)
        customItemData.CustomDataDoubles.Doubles.Add(static_cast<double>(stat->DinoImprintingQualityField()));

    customItemData.CustomDataDoubles.Doubles.Add(static_cast<double>(std::time(nullptr)));

    customItemData.CustomDataStrings = GetDinoDataStrings(dino, dinoData.DinoNameInMap, dinoData.DinoName);

    customItemData.CustomDataClasses.Add(dinoData.DinoClass);

    FCustomItemByteArray dinoBytes;
    dinoBytes.Bytes = dinoData.DinoData;
    customItemData.CustomDataBytes.ByteArrays.Add(dinoBytes);

    customItemData.CustomDataBytes.ByteArrays.Add(FCustomItemByteArray());

    FCustomItemByteArray arr = FCustomItemByteArray();
    arr.Bytes.Add(dino->TamedAggressionLevelField());
    customItemData.CustomDataBytes.ByteArrays.Add(arr);

    return customItemData;
}

static bool GiveDinoCryo(AShooterPlayerController* pc, const DinoEntry& d)
{
    AActor* charActor = pc->BaseGetPlayerCharacter();
    if (!charActor) return false;

    std::wstring wPath(d.BlueprintPath.begin(), d.BlueprintPath.end());
    FString fPath(wPath.c_str());

    APrimalDinoCharacter* dino = AsaApi::GetApiUtils().SpawnDino(pc, fPath, nullptr, d.Level, true, d.Neutered);
    if (!dino)
    {
        Log::GetLog()->warn("[Kits] SpawnDino returned null for '{}'", d.BlueprintPath);
        return false;
    }

    FCustomItemData cryoData = GetDinoCustomItemData(dino);

    AShooterCharacter* ch = static_cast<AShooterCharacter*>(charActor);
    UPrimalInventoryComponent* inv = ch->MyInventoryComponentField();
    if (!inv)
    {
        dino->Destroy(false, false);
        return false;
    }

    const std::string& podPath = g_use_pelayori_cryo ? g_pelayori_cryopod_bp : g_cryopod_bp;
    std::wstring wCryo(podPath.begin(), podPath.end());
    FString fCryoPath(wCryo.c_str());
    UClass* cryoClass = UVictoryCore::BPLoadClass(fCryoPath);
    if (!cryoClass)
    {
        Log::GetLog()->warn("[Kits] Cryopod BPLoadClass failed for '{}'", podPath);
        dino->Destroy(false, false);
        return false;
    }

    TSubclassOf<UPrimalItem> cryoSub = cryoClass;
    TSubclassOf<UPrimalItem> noSkin{};
    UPrimalItem* cryoItem = UPrimalItem::AddNewItem(
        cryoSub, nullptr, false, true, 0.0f, true, 1, false, 0.0f, false,
        noSkin, 0.0f, false, true, true, false, true);
    if (!cryoItem)
    {
        dino->Destroy(false, false);
        return false;
    }

    cryoItem->SetCustomItemData(&cryoData);
    cryoItem->UpdatedItem(true, false);
    inv->AddItemObject(cryoItem);

    dino->Destroy(false, false);
    return true;
}

static void GiveItem(AShooterPlayerController* pc, const ItemEntry& it)
{
    AActor* charActor = pc->BaseGetPlayerCharacter();
    if (!charActor) return;
    AShooterCharacter* ch = static_cast<AShooterCharacter*>(charActor);
    UPrimalInventoryComponent* inv = ch->MyInventoryComponentField();
    if (!inv) return;

    std::wstring wPath(it.BlueprintPath.begin(), it.BlueprintPath.end());
    FString fPath(wPath.c_str());
    UClass* itemClass = UVictoryCore::BPLoadClass(fPath);
    if (!itemClass)
    {
        Log::GetLog()->warn("[Kits] Item BPLoadClass failed for '{}'", it.BlueprintPath);
        return;
    }

    TSubclassOf<UPrimalItem> itemSub = itemClass;
    TSubclassOf<UPrimalItem> noSkin{};
    UPrimalItem::AddNewItem(
        itemSub, inv, false, false, it.Quality, false, it.Quantity, false, 0.0f, true,
        noSkin, 0.0f, false, true, true, true, true);
}

static void GiveKit(AShooterPlayerController* pc, const Kit& k)
{
    for (const auto& it : k.Items)
        GiveItem(pc, it);
    for (const auto& d : k.Dinos)
        GiveDinoCryo(pc, d);
}

static void ListKitsCommand(AShooterPlayerController* pc, FString* message, int, int)
{
    if (!pc) return;
    std::string eos = GetEos(pc);
    if (eos.empty()) return;

    std::vector<std::string> groups;
    if (!GetGroups(eos, groups))
    {
        Notify(pc, L"Kit access could not be verified right now. Please contact an admin.");
        return;
    }

    std::string list;
    for (const auto& k : g_kits)
    {
        if (!ResolveRank(k, groups)) continue;
        if (!list.empty()) list += ", ";
        list += k.Name;
    }

    if (list.empty())
    {
        Notify(pc, L"You have no kits available.");
        return;
    }

    std::string out = "Kits available to you: " + list;
    Notify(pc, std::wstring(out.begin(), out.end()));
}

static void RedeemKitCommand(AShooterPlayerController* pc, FString* message, int, int)
{
    if (!pc || !message) return;
    std::string eos = GetEos(pc);
    if (eos.empty()) return;

    std::string msg = FStr(*message);
    size_t sp = msg.find(' ');
    if (sp == std::string::npos)
    {
        Notify(pc, L"Usage: /kit <name>");
        return;
    }
    std::string kitName = msg.substr(sp + 1);
    while (!kitName.empty() && kitName.front() == ' ') kitName.erase(kitName.begin());
    while (!kitName.empty() && (kitName.back() == ' ' || kitName.back() == '\r' || kitName.back() == '\n')) kitName.pop_back();

    if (kitName.empty())
    {
        Notify(pc, L"Usage: /kit <name>");
        return;
    }

    const Kit* k = FindKit(kitName);
    if (!k)
    {
        Notify(pc, L"That kit does not exist.");
        return;
    }

    std::vector<std::string> groups;
    if (!GetGroups(eos, groups))
    {
        Notify(pc, L"Kit access could not be verified right now. Please contact an admin.");
        return;
    }

    const RankAccess* rank = ResolveRank(*k, groups);
    if (!rank)
    {
        Notify(pc, L"You do not have access to that kit.");
        return;
    }

    int uses = 0;
    long long last = 0;
    if (!GetUsage(eos, k->Name, uses, last))
    {
        Notify(pc, L"Kit usage could not be verified right now. Please contact an admin.");
        return;
    }

    long long now = (long long)std::time(nullptr);

    if (rank->MaxUses > 0 && uses >= rank->MaxUses)
    {
        Notify(pc, L"You have used all your redemptions for that kit.");
        return;
    }

    if (rank->CooldownSeconds > 0 && last > 0)
    {
        long long elapsed = now - last;
        if (elapsed < rank->CooldownSeconds)
        {
            long long remain = rank->CooldownSeconds - elapsed;
            long long mins = remain / 60;
            long long secs = remain % 60;
            std::string out = "That kit is on cooldown: " + std::to_string(mins) + "m " + std::to_string(secs) + "s remaining.";
            Notify(pc, std::wstring(out.begin(), out.end()));
            return;
        }
    }

    if (!ValidateKit(pc, *k))
    {
        Notify(pc, L"That kit could not be delivered. Please contact an admin.");
        return;
    }

    GiveKit(pc, *k);
    RecordUse(eos, k->Name, now);

    std::string out = "Redeemed kit: " + k->Name;
    Notify(pc, std::wstring(out.begin(), out.end()));
}

using Tick_t = void(*)(AShooterGameMode*, float);
static Tick_t Original_Tick = nullptr;

static void Detour_Tick(AShooterGameMode* gm, float dt)
{
    Original_Tick(gm, dt);
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
    Log::Get().Init("Kits");

    if (!LoadConfig())
        Log::GetLog()->error("[Kits] Failed to load config");

    LoadPermissionsAPI();
    InitDb();

    g_last_config_check = std::chrono::steady_clock::now();

    AsaApi::GetCommands().AddChatCommand(FString(L"/kits"), &ListKitsCommand);
    AsaApi::GetCommands().AddChatCommand(FString(L"/kit"), &RedeemKitCommand);

    AsaApi::GetHooks().SetHook(
        "AShooterGameMode.Tick(float)",
        (LPVOID)&Detour_Tick,
        (LPVOID*)&Original_Tick);

    Log::GetLog()->info("[Kits] Loaded");
}

static void PluginUnload()
{
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/kits"));
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/kit"));

    AsaApi::GetHooks().DisableHook(
        "AShooterGameMode.Tick(float)",
        (LPVOID)&Detour_Tick);

    if (g_db)
    {
        pmysql_close(g_db);
        g_db = nullptr;
    }

    Log::GetLog()->info("[Kits] Unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { PluginInit(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->critical("[Kits] Init exception: {}", e.what());
    }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { PluginUnload(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->error("[Kits] Unload exception: {}", e.what());
    }
}