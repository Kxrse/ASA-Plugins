/*
Blockade - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * Blockade - ASA Plugin
 *
 * Hooks:
 *   APrimalCharacter.TakeDamage(float,FDamageEvent&,AController*,AActor*)  combat detection, gated on real victim health loss
 *   APrimalStructure.TakeDamage(float,FDamageEvent&,AController*,AActor*)  raid zone creation on real enemy structure damage
 *   AShooterGameMode.Tick(float)                                           config reload (10s), combat and raid sweep (1s)
 *
 * Exports:
 *   Blockade_IsCombatBlocked(const FString& eosId, int& outRemainingSeconds)  true if combat blocked, remaining seconds out
 *   Blockade_IsRaidBlocked(const FString& eosId)                             true if standing in an active raid zone
 *   Read only, resolved by other plugins via GetProcAddress. Both honor the enable toggles.
 *
 * Config:
 *   ArkApi/Plugins/Blockade/config.json
 *   RaidBlockRadius: raid zone radius in world units
 *   ImmuneEosIds: EOS IDs exempt from both combat and raid block
 *   CombatBlockEnabled, CombatBlockSeconds, CombatBlockMessage ({seconds} token), CombatBlockClearedMessage, CombatBlockNotifyInterval
 *   RaidBlockEnabled, RaidBlockSeconds, RaidBlockMessage, RaidBlockClearedMessage, RaidBlockNotifyInterval
 *
 * Config Example:
 * {
 *   "RaidBlockRadius": 10000.0,
 *   "ImmuneEosIds": [ "EOS_ID" ],
 *   "CombatBlockEnabled": true,
 *   "CombatBlockSeconds": 30,
 *   "CombatBlockMessage": "Combat Blocked: {seconds}s",
 *   "CombatBlockClearedMessage": "No longer combat blocked",
 *   "CombatBlockNotifyInterval": 10,
 *   "RaidBlockEnabled": true,
 *   "RaidBlockSeconds": 300,
 *   "RaidBlockMessage": "Raid Block Active",
 *   "RaidBlockClearedMessage": "Raid Block Removed",
 *   "RaidBlockNotifyInterval": 30
 * }
 *
 * Owns combat and raid block state for the cluster, applied globally except for ImmuneEosIds.
 * Combat fires only on real victim health loss from an enemy player or tamed dino. Structure
 * sourced damage to a player instead creates a raid zone at the victim. Raid zones form on real
 * enemy structure damage, merge within RaidBlockRadius, and expire RaidBlockSeconds after their
 * last hit. Players standing in an active zone are raid blocked. Both states show an on screen
 * marker and are held in memory only, queryable live through the exports above.
 *
 * Raid expiry is anchored to the zone, not the player. A blocked player's expiry is the latest
 * lastHit plus RaidBlockSeconds across every zone containing them, so it holds still while no
 * further structure damage lands and survives a player leaving and re-entering. Only a fresh hit
 * on the zone pushes it forward. State is empty at load and rebuilt from live activity, so a
 * restart clears all blocks. Config rescanned every 10 seconds, size plus last write time,
 * reloaded only on a confirmed stable change.
 */

#include <API/ARK/Ark.h>

#pragma warning(disable: 4191)
#pragma comment(lib, "AsaApi.lib")

#include <json.hpp>
#include <Windows.h>
#include <fstream>
#include <string>
#include <vector>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <mutex>
#include <ctime>
#include <cmath>
#include <sys/stat.h>

static const std::string g_config_path = "ArkApi/Plugins/Blockade/config.json";

static double g_raid_block_radius = 10000.0;

static std::unordered_set<std::string> g_immune_eos;

static bool g_combat_block_enabled = true;
static int g_combat_block_seconds = 30;
static std::wstring g_combat_msg = L"Combat Blocked: {seconds}s";
static std::wstring g_combat_cleared_msg = L"No longer combat blocked";
static int g_combat_notify_interval = 10;

static bool g_raid_block_enabled = true;
static int g_raid_block_seconds = 300;
static std::wstring g_raid_msg = L"Raid Block Active";
static std::wstring g_raid_cleared_msg = L"Raid Block Removed";
static int g_raid_notify_interval = 30;

static time_t g_config_last_modified = 0;
static long long g_config_last_size = 0;
static std::chrono::steady_clock::time_point g_last_config_check;
static std::chrono::steady_clock::time_point g_last_sweep;

struct CombatState
{
    std::chrono::steady_clock::time_point lastHit;
    int blockSeconds = 0;
    std::chrono::steady_clock::time_point lastNotify;
};

static std::unordered_map<std::string, CombatState> g_combat_times;
static std::mutex g_combat_mutex;

struct RaidZone
{
    double x, y;
    std::chrono::steady_clock::time_point lastHit;
    long long lastHitUnix;
};

struct RaidState
{
    std::chrono::steady_clock::time_point lastNotify;
    long long writtenExpiry = 0;
};

static std::vector<RaidZone> g_raid_zones;
static std::unordered_map<std::string, RaidState> g_raid_blocked_players;
static std::mutex g_raid_mutex;

static std::string FStr(const FString& f)
{
    const char* s = TCHAR_TO_UTF8(*f);
    return (s && s[0]) ? s : "";
}

static std::wstring Widen(const std::string& s)
{
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring out((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), len);
    return out;
}

static std::wstring FormatSeconds(const std::wstring& tmpl, int seconds)
{
    std::wstring out = tmpl;
    const std::wstring token = L"{seconds}";
    const std::wstring val = std::to_wstring(seconds);
    size_t pos = 0;
    while ((pos = out.find(token, pos)) != std::wstring::npos)
    {
        out.replace(pos, token.size(), val);
        pos += val.size();
    }
    return out;
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

static bool IsImmune(const std::string& eos)
{
    if (eos.empty()) return false;
    return g_immune_eos.count(eos) > 0;
}

static float GetCurrentHealth(APrimalCharacter* c)
{
    if (!c) return 0.0f;
    UPrimalCharacterStatusComponent* comp = c->MyCharacterStatusComponentField();
    if (!comp) return 0.0f;
    return comp->BPGetCurrentStatusValue(EPrimalCharacterStatusValue::Health);
}

static void OnScreen(AShooterPlayerController* pc, const std::wstring& text, const FLinearColor& color, float displayTime)
{
    if (!pc) return;
    AsaApi::GetApiUtils().SendNotification(pc, color, 1.3f, displayTime, nullptr, text.c_str());
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
        Log::GetLog()->error("[Blockade] Cannot open config: {}", g_config_path);
        return false;
    }

    try
    {
        nlohmann::json j;
        file >> j;

        g_raid_block_radius = j.value("RaidBlockRadius", 10000.0);

        std::unordered_set<std::string> newImmune;
        if (j.contains("ImmuneEosIds") && j["ImmuneEosIds"].is_array())
        {
            for (auto& e : j["ImmuneEosIds"])
                if (e.is_string()) newImmune.insert(e.get<std::string>());
        }
        g_immune_eos = std::move(newImmune);

        g_combat_block_enabled = j.value("CombatBlockEnabled", true);
        g_combat_block_seconds = j.value("CombatBlockSeconds", 30);
        g_combat_msg = Widen(j.value("CombatBlockMessage", std::string("Combat Blocked: {seconds}s")));
        g_combat_cleared_msg = Widen(j.value("CombatBlockClearedMessage", std::string("No longer combat blocked")));
        g_combat_notify_interval = j.value("CombatBlockNotifyInterval", 10);

        g_raid_block_enabled = j.value("RaidBlockEnabled", true);
        g_raid_block_seconds = j.value("RaidBlockSeconds", 300);
        g_raid_msg = Widen(j.value("RaidBlockMessage", std::string("Raid Block Active")));
        g_raid_cleared_msg = Widen(j.value("RaidBlockClearedMessage", std::string("Raid Block Removed")));
        g_raid_notify_interval = j.value("RaidBlockNotifyInterval", 30);

        if (g_combat_notify_interval < 1) g_combat_notify_interval = 10;
        if (g_raid_notify_interval < 1) g_raid_notify_interval = 30;

        g_config_last_modified = GetFileModTime(g_config_path);
        g_config_last_size = GetFileSize(g_config_path);
        Log::GetLog()->info("[Blockade] Config loaded, {} immune", g_immune_eos.size());
        return true;
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[Blockade] Config parse error: {}", ex.what());
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

static void RegisterCombat(AShooterPlayerController* pc, const std::string& eos, int blockSeconds)
{
    if (eos.empty() || blockSeconds <= 0) return;
    if (IsImmune(eos)) return;

    auto now = std::chrono::steady_clock::now();
    bool isNew = false;
    {
        std::lock_guard<std::mutex> lock(g_combat_mutex);
        auto it = g_combat_times.find(eos);
        if (it == g_combat_times.end())
        {
            CombatState st;
            st.lastHit = now;
            st.blockSeconds = blockSeconds;
            st.lastNotify = now;
            g_combat_times[eos] = st;
            isNew = true;
        }
        else
        {
            it->second.lastHit = now;
            it->second.blockSeconds = blockSeconds;
        }
    }

    if (isNew)
    {
        FLinearColor red{ 1.0f, 0.2f, 0.2f, 1.0f };
        OnScreen(pc, FormatSeconds(g_combat_msg, blockSeconds), red, 6.0f);
    }
}

static void CombatTick()
{
    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return;

    auto now = std::chrono::steady_clock::now();
    FLinearColor red{ 1.0f, 0.2f, 0.2f, 1.0f };
    FLinearColor green{ 0.2f, 1.0f, 0.2f, 1.0f };

    {
        std::lock_guard<std::mutex> lock(g_combat_mutex);
        if (g_combat_times.empty()) return;

        auto& controllers = world->PlayerControllerListField();
        for (int i = 0; i < controllers.Num(); ++i)
        {
            AShooterPlayerController* pc = static_cast<AShooterPlayerController*>(controllers[i].Get());
            if (!pc) continue;

            std::string eos = GetEosId(pc);
            if (eos.empty()) continue;

            auto it = g_combat_times.find(eos);
            if (it == g_combat_times.end()) continue;

            int elapsed = (int)std::chrono::duration_cast<std::chrono::seconds>(now - it->second.lastHit).count();
            int remaining = it->second.blockSeconds - elapsed;

            if (remaining <= 0)
            {
                OnScreen(pc, g_combat_cleared_msg, green, 5.0f);
                continue;
            }

            int sinceNotify = (int)std::chrono::duration_cast<std::chrono::seconds>(now - it->second.lastNotify).count();
            if (sinceNotify >= g_combat_notify_interval)
            {
                it->second.lastNotify = now;
                OnScreen(pc, FormatSeconds(g_combat_msg, remaining), red, 6.0f);
            }
        }

        for (auto it = g_combat_times.begin(); it != g_combat_times.end(); )
        {
            int elapsed = (int)std::chrono::duration_cast<std::chrono::seconds>(now - it->second.lastHit).count();
            if (elapsed >= it->second.blockSeconds)
                it = g_combat_times.erase(it);
            else
                ++it;
        }
    }
}

static void UpdateRaidZone(double sx, double sy)
{
    std::lock_guard<std::mutex> lock(g_raid_mutex);
    auto now = std::chrono::steady_clock::now();
    long long nowUnix = (long long)time(nullptr);

    for (auto& zone : g_raid_zones)
    {
        double dx = sx - zone.x;
        double dy = sy - zone.y;
        if (std::sqrt(dx * dx + dy * dy) < g_raid_block_radius)
        {
            zone.lastHit = now;
            zone.lastHitUnix = nowUnix;
            return;
        }
    }

    g_raid_zones.push_back({ sx, sy, now, nowUnix });
}

static void RaidTick()
{
    auto now = std::chrono::steady_clock::now();
    int zoneLife = g_raid_block_seconds;

    FLinearColor red{ 1.0f, 0.2f, 0.2f, 1.0f };
    FLinearColor green{ 0.2f, 1.0f, 0.2f, 1.0f };

    {
        std::lock_guard<std::mutex> lock(g_raid_mutex);

        for (auto it = g_raid_zones.begin(); it != g_raid_zones.end(); )
        {
            int elapsed = (int)std::chrono::duration_cast<std::chrono::seconds>(now - it->lastHit).count();
            if (zoneLife > 0 && elapsed >= zoneLife)
                it = g_raid_zones.erase(it);
            else
                ++it;
        }

        std::unordered_set<std::string> currentlyBlocked;
        if (g_raid_block_enabled && !g_raid_zones.empty())
        {
            UWorld* world = AsaApi::GetApiUtils().GetWorld();
            if (world)
            {
                auto& controllers = world->PlayerControllerListField();
                for (int i = 0; i < controllers.Num(); ++i)
                {
                    AShooterPlayerController* pc = static_cast<AShooterPlayerController*>(controllers[i].Get());
                    if (!pc) continue;

                    APawn* pawn = pc->PawnField().Get();
                    if (!pawn) continue;

                    USceneComponent* root = pawn->RootComponentField().Get();
                    if (!root) continue;

                    auto loc = root->RelativeLocationField();
                    std::string eos = GetEosId(pc);
                    if (eos.empty()) continue;
                    if (IsImmune(eos)) continue;

                    bool inZone = false;
                    long long newExpiry = 0;
                    for (const auto& zone : g_raid_zones)
                    {
                        double dx = zone.x - loc.X;
                        double dy = zone.y - loc.Y;
                        if (std::sqrt(dx * dx + dy * dy) <= g_raid_block_radius)
                        {
                            inZone = true;
                            long long zoneExpiry = zone.lastHitUnix + g_raid_block_seconds;
                            if (zoneExpiry > newExpiry) newExpiry = zoneExpiry;
                        }
                    }
                    if (!inZone) continue;

                    currentlyBlocked.insert(eos);

                    auto bit = g_raid_blocked_players.find(eos);
                    if (bit == g_raid_blocked_players.end())
                    {
                        RaidState st;
                        st.lastNotify = now;
                        st.writtenExpiry = newExpiry;
                        g_raid_blocked_players[eos] = st;
                        OnScreen(pc, g_raid_msg, red, 6.0f);
                    }
                    else
                    {
                        int sinceNotify = (int)std::chrono::duration_cast<std::chrono::seconds>(now - bit->second.lastNotify).count();
                        if (sinceNotify >= g_raid_notify_interval)
                        {
                            bit->second.lastNotify = now;
                            OnScreen(pc, g_raid_msg, red, 6.0f);
                        }
                        if (newExpiry != bit->second.writtenExpiry)
                            bit->second.writtenExpiry = newExpiry;
                    }
                }
            }
        }

        for (auto it = g_raid_blocked_players.begin(); it != g_raid_blocked_players.end(); )
        {
            if (currentlyBlocked.count(it->first) == 0)
            {
                FString fEos(it->first.c_str());
                AShooterPlayerController* pc = AsaApi::GetApiUtils().FindPlayerFromEOSID(fEos);
                if (pc) OnScreen(pc, g_raid_cleared_msg, green, 5.0f);
                it = g_raid_blocked_players.erase(it);
            }
            else
                ++it;
        }
    }
}

DECLARE_HOOK(APrimalCharacter_TakeDamage, float, APrimalCharacter*, float, FDamageEvent*, AController*, AActor*);
DECLARE_HOOK(APrimalStructure_TakeDamage, float, APrimalStructure*, float, FDamageEvent*, AController*, AActor*);

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

static bool IsAttackerWild(AController* instigator, AActor* damageCauser)
{
    if (instigator && instigator->IsA(AShooterPlayerController::GetPrivateStaticClass()))
        return false;

    if (damageCauser && damageCauser->IsA(APrimalDinoCharacter::GetPrivateStaticClass()))
    {
        auto* dino = static_cast<APrimalDinoCharacter*>(damageCauser);
        return dino->TamingTeamIDField() == 0;
    }

    if (damageCauser && damageCauser->IsA(APrimalStructure::GetPrivateStaticClass()))
        return false;

    return true;
}

float Hook_APrimalCharacter_TakeDamage(APrimalCharacter* _this, float Damage, FDamageEvent* DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
    float before = GetCurrentHealth(_this);
    float result = APrimalCharacter_TakeDamage_original(_this, Damage, DamageEvent, EventInstigator, DamageCauser);
    float after = GetCurrentHealth(_this);

    if (!_this) return result;
    if (after >= before) return result;
    if (!EventInstigator && !DamageCauser) return result;

    int victimTeam = _this->TargetingTeamField();
    int attackerTeam = GetAttackerTeam(EventInstigator, DamageCauser);

    if (victimTeam == attackerTeam || attackerTeam == 0) return result;
    if (IsAttackerWild(EventInstigator, DamageCauser)) return result;
    if (_this->IsA(APrimalDinoCharacter::GetPrivateStaticClass())
        && static_cast<APrimalDinoCharacter*>(_this)->TamingTeamIDField() == 0)
        return result;

    bool attackerIsPlayer = EventInstigator && EventInstigator->IsA(AShooterPlayerController::GetPrivateStaticClass());
    bool attackerIsDino = DamageCauser && DamageCauser->IsA(APrimalDinoCharacter::GetPrivateStaticClass());

    if (!attackerIsPlayer && !attackerIsDino)
    {
        if (g_raid_block_enabled)
        {
            USceneComponent* root = _this->RootComponentField().Get();
            if (root)
            {
                auto loc = root->RelativeLocationField();
                UpdateRaidZone(loc.X, loc.Y);
            }
        }
        return result;
    }

    if (!g_combat_block_enabled) return result;

    if (_this->IsA(AShooterCharacter::StaticClass()))
    {
        AShooterCharacter* sc = static_cast<AShooterCharacter*>(_this);
        AController* ctrl = sc->ControllerField().Get();
        if (ctrl && ctrl->IsA(AShooterPlayerController::GetPrivateStaticClass()))
        {
            auto* vpc = static_cast<AShooterPlayerController*>(ctrl);
            RegisterCombat(vpc, GetEosId(vpc), g_combat_block_seconds);
        }
    }

    if (EventInstigator && EventInstigator->IsA(AShooterPlayerController::GetPrivateStaticClass()))
    {
        auto* apc = static_cast<AShooterPlayerController*>(EventInstigator);
        RegisterCombat(apc, GetEosId(apc), g_combat_block_seconds);
    }

    return result;
}

float Hook_APrimalStructure_TakeDamage(APrimalStructure* _this, float Damage, FDamageEvent* DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
    float before = _this ? _this->HealthField() : 0.0f;
    float result = APrimalStructure_TakeDamage_original(_this, Damage, DamageEvent, EventInstigator, DamageCauser);
    float after = _this ? _this->HealthField() : 0.0f;

    if (!_this) return result;
    if (!g_raid_block_enabled) return result;
    if (!EventInstigator && !DamageCauser) return result;

    int structTeam = _this->TargetingTeamField();
    int attackerTeam = GetAttackerTeam(EventInstigator, DamageCauser);

    if (after >= before) return result;
    if (structTeam == 0) return result;
    if (attackerTeam != 0 && attackerTeam == structTeam) return result;

    bool wildDino = DamageCauser && DamageCauser->IsA(APrimalDinoCharacter::GetPrivateStaticClass())
        && static_cast<APrimalDinoCharacter*>(DamageCauser)->TamingTeamIDField() == 0
        && !(EventInstigator && EventInstigator->IsA(AShooterPlayerController::GetPrivateStaticClass()));
    if (wildDino) return result;

    USceneComponent* sRoot = _this->RootComponentField().Get();
    if (!sRoot) return result;

    auto sLoc = sRoot->RelativeLocationField();
    UpdateRaidZone(sLoc.X, sLoc.Y);
    return result;
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

    auto sinceSweep = std::chrono::duration_cast<std::chrono::seconds>(now - g_last_sweep).count();
    if (sinceSweep >= 1)
    {
        g_last_sweep = now;
        CombatTick();
        RaidTick();
    }
}

static void PluginInit()
{
    Log::Get().Init("Blockade");

    if (!LoadConfig())
        Log::GetLog()->error("[Blockade] Failed to load config");

    g_last_config_check = std::chrono::steady_clock::now();
    g_last_sweep = std::chrono::steady_clock::now();

    AsaApi::GetHooks().SetHook(
        "APrimalCharacter.TakeDamage(float,FDamageEvent&,AController*,AActor*)",
        (LPVOID)&Hook_APrimalCharacter_TakeDamage,
        (LPVOID*)&APrimalCharacter_TakeDamage_original);

    AsaApi::GetHooks().SetHook(
        "APrimalStructure.TakeDamage(float,FDamageEvent&,AController*,AActor*)",
        (LPVOID)&Hook_APrimalStructure_TakeDamage,
        (LPVOID*)&APrimalStructure_TakeDamage_original);

    AsaApi::GetHooks().SetHook(
        "AShooterGameMode.Tick(float)",
        (LPVOID)&Detour_Tick,
        (LPVOID*)&Original_Tick);

    Log::GetLog()->info("[Blockade] Loaded");
}

static void PluginUnload()
{
    AsaApi::GetHooks().DisableHook(
        "APrimalCharacter.TakeDamage(float,FDamageEvent&,AController*,AActor*)",
        (LPVOID)&Hook_APrimalCharacter_TakeDamage);

    AsaApi::GetHooks().DisableHook(
        "APrimalStructure.TakeDamage(float,FDamageEvent&,AController*,AActor*)",
        (LPVOID)&Hook_APrimalStructure_TakeDamage);

    AsaApi::GetHooks().DisableHook(
        "AShooterGameMode.Tick(float)",
        (LPVOID)&Detour_Tick);

    Log::GetLog()->info("[Blockade] Unloaded");
}

extern "C" __declspec(dllexport) bool Blockade_IsCombatBlocked(const FString& eosId, int& outRemainingSeconds)
{
    outRemainingSeconds = 0;
    if (!g_combat_block_enabled) return false;

    const std::string eos = FStr(eosId);
    if (eos.empty()) return false;

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_combat_mutex);

    auto it = g_combat_times.find(eos);
    if (it == g_combat_times.end()) return false;

    const int elapsed = (int)std::chrono::duration_cast<std::chrono::seconds>(now - it->second.lastHit).count();
    const int remaining = it->second.blockSeconds - elapsed;
    if (remaining <= 0) return false;

    outRemainingSeconds = remaining;
    return true;
}

extern "C" __declspec(dllexport) bool Blockade_IsRaidBlocked(const FString& eosId)
{
    if (!g_raid_block_enabled) return false;

    const std::string eos = FStr(eosId);
    if (eos.empty()) return false;

    const long long now = (long long)time(nullptr);
    std::lock_guard<std::mutex> lock(g_raid_mutex);

    auto it = g_raid_blocked_players.find(eos);
    if (it == g_raid_blocked_players.end()) return false;

    return it->second.writtenExpiry > now;
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { PluginInit(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->critical("[Blockade] Init exception: {}", e.what());
    }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { PluginUnload(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->error("[Blockade] Unload exception: {}", e.what());
    }
}