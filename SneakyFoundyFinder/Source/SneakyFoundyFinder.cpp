/*
SneakyFoundyFinder - ASA Plugin
Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins
License: Kxrse ASA Plugins Non-Commercial License
You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/
/**
 * SneakyFoundyFinder - ASA Plugin
 *
 * Hooks:
 *   None. Registers the /sff chat command and a 10s config hot-reload timer.
 *
 * Config:
 *   ArkApi/Plugins/SneakyFoundyFinder/config.json
 *   ScanRadius: scan radius in Unreal units (default 5000 = 50m, capped at 10000)
 *
 * Foundation finding:
 *   /sff runs a server octree range query for structures within ScanRadius, skips the caller's own
 *   tribe, and team-pings the nearest enemy foundation, floor or pillar via Client_AddTeamPing,
 *   attached to the structure so the marker tracks it. Helps players locate meshed foundations when
 *   raiding or rebuilding after a raid. Each player is limited to one scan every 5 seconds.
 */

#include <API/ARK/Ark.h>
#include <json.hpp>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sys/stat.h>

#pragma warning(disable: 4191)
#pragma comment(lib, "AsaApi")

static const std::string g_config_path = "ArkApi/Plugins/SneakyFoundyFinder/config.json";

static const int g_max_pings = 1;
static const unsigned char g_ping_type = 5;
static const double g_cooldown_seconds = 5.0;

static double g_scan_radius = 5000.0;

static std::unordered_map<std::string, long double> g_last_use;

static time_t    g_config_last_modified = 0;
static uintmax_t g_config_last_size = 0;
static int       g_config_check_counter = 0;

static unsigned char g_ping_id = 0;

static std::string FStr(const FString& f)
{
    const char* s = TCHAR_TO_UTF8(*f);
    return (s && s[0]) ? s : "";
}

static std::string ToLower(const std::string& in)
{
    std::string out = in;
    std::transform(out.begin(), out.end(), out.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

static void Notify(AShooterPlayerController* pc, const std::wstring& msg)
{
    if (!pc) return;
    const std::wstring rich =
        L"<RichColor Color=\"1.0,1.0,1.0,1.0\">" + msg + L"</>";
    FString fSender(L"");
    FString fMsg(rich.c_str());
    AsaApi::GetApiUtils().SendChatMessage(pc, fSender, L"{}", std::wstring_view(*fMsg));
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
    if (!file.is_open())
    {
        Log::GetLog()->error("[SneakyFoundyFinder] Cannot open config: {}", g_config_path);
        return false;
    }

    try
    {
        nlohmann::json j;
        file >> j;
        g_scan_radius = j.value("ScanRadius", 5000.0);
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[SneakyFoundyFinder] Config parse error: {}", ex.what());
        return false;
    }

    if (g_scan_radius <= 0.0) g_scan_radius = 5000.0;
    if (g_scan_radius > 10000.0) g_scan_radius = 10000.0;

    return true;
}

static bool GetActorLocation(AActor* a, UE::Math::TVector<double>& out)
{
    if (!a) return false;
    USceneComponent* root = a->RootComponentField();
    if (!root) return false;
    out = root->RelativeLocationField();
    return true;
}

struct FoundCandidate
{
    AActor* actor;
    UE::Math::TVector<double> location;
    double distSq;
};

static void Cmd_Sff(AShooterPlayerController* pc, FString* /*message*/,
    int /*mode*/, int /*platform*/)
{
    if (!pc) return;

    AActor* charActor = pc->BaseGetPlayerCharacter();
    if (!charActor)
    {
        Notify(pc, L"You must be in-game to use this.");
        return;
    }

    const std::string playerKey = FStr(AsaApi::GetApiUtils().GetEOSIDFromController(pc));
    const long double now =
        static_cast<long double>(UVictoryCore::GetTimeSeconds(static_cast<APawn*>(charActor)));

    auto cd = g_last_use.find(playerKey);
    if (cd != g_last_use.end() && (now - cd->second) < g_cooldown_seconds)
    {
        Notify(pc, L"Wait a moment before scanning again.");
        return;
    }
    g_last_use[playerKey] = now;

    const int playerTeam = charActor->TargetingTeamField();

    UE::Math::TVector<double> playerLoc;
    if (!GetActorLocation(charActor, playerLoc))
    {
        Notify(pc, L"Could not resolve your location.");
        return;
    }

    TArray<AActor*> nearby = AsaApi::GetApiUtils().GetAllActorsInRange(
        playerLoc, static_cast<float>(g_scan_radius), EServerOctreeGroup::STRUCTURES);

    std::vector<FoundCandidate> candidates;

    for (int i = 0; i < nearby.Num(); ++i)
    {
        AActor* a = nearby[i];
        if (!a) continue;

        if (a->TargetingTeamField() == playerTeam) continue;

        const std::string bp = ToLower(FStr(AsaApi::GetApiUtils().GetBlueprint(a)));
        if (bp.find("foundation") == std::string::npos &&
            bp.find("floor") == std::string::npos &&
            bp.find("pillar") == std::string::npos)
            continue;

        UE::Math::TVector<double> aLoc;
        if (!GetActorLocation(a, aLoc)) continue;

        const double dx = playerLoc.X - aLoc.X;
        const double dy = playerLoc.Y - aLoc.Y;
        const double dz = playerLoc.Z - aLoc.Z;
        const double distSq = dx * dx + dy * dy + dz * dz;

        candidates.push_back(FoundCandidate{ a, aLoc, distSq });
    }

    if (candidates.empty())
    {
        Notify(pc, L"No enemy foundations found nearby.");
        return;
    }

    std::sort(candidates.begin(), candidates.end(),
        [](const FoundCandidate& a, const FoundCandidate& b) { return a.distSq < b.distSq; });

    const int limit = (static_cast<int>(candidates.size()) < g_max_pings)
        ? static_cast<int>(candidates.size()) : g_max_pings;

    const long double creationTime = now;

    for (int i = 0; i < limit; ++i)
    {
        FTeamPingData data;
        data.PingID = g_ping_id++;
        data.Location = candidates[i].location;
        data.ToActor = candidates[i].actor;
        data.ByPlayerID = 0;
        data.TargetingTeam = playerTeam;
        data.CreationTime = creationTime;
        *reinterpret_cast<unsigned char*>(&data.PingType) = g_ping_type;

        pc->Client_AddTeamPing(&data);
    }

    Notify(pc, L"Enemy foundation found.");
}

static void OnTimer()
{
    if (++g_config_check_counter < 10) return;
    g_config_check_counter = 0;

    time_t mtime = 0;
    uintmax_t fsize = 0;
    if (GetFileInfo(g_config_path, mtime, fsize) && fsize > 0 &&
        (mtime != g_config_last_modified || fsize != g_config_last_size))
    {
        if (LoadConfig())
        {
            g_config_last_modified = mtime;
            g_config_last_size     = fsize;
            Log::GetLog()->info("[SneakyFoundyFinder] Config reloaded");
        }
    }
}

static void Plugin_Init_Impl()
{
    Log::Get().Init("SneakyFoundyFinder");

    if (!LoadConfig())
    {
        Log::GetLog()->error("[SneakyFoundyFinder] Config load failed, plugin will not function");
        return;
    }

    GetFileInfo(g_config_path, g_config_last_modified, g_config_last_size);

    AsaApi::GetCommands().AddOnTimerCallback(FString(L"SneakyFoundyFinder_Timer"), &OnTimer);
    AsaApi::GetCommands().AddChatCommand(FString(L"/sff"), &Cmd_Sff);

    Log::GetLog()->info("[SneakyFoundyFinder] Plugin loaded");
}

static void Plugin_Unload_Impl()
{
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/sff"));
    AsaApi::GetCommands().RemoveOnTimerCallback(FString(L"SneakyFoundyFinder_Timer"));

    Log::GetLog()->info("[SneakyFoundyFinder] Plugin unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { Plugin_Init_Impl(); }
    catch (const std::exception& ex) { Log::GetLog()->critical("[SneakyFoundyFinder] Init exception: {}", ex.what()); }
    catch (...) { Log::GetLog()->critical("[SneakyFoundyFinder] Init unknown exception"); }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { Plugin_Unload_Impl(); }
    catch (...) {}
}