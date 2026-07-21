/*
HarvestScale - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * HarvestScale - ASA Plugin
 *
 * Hooks:
 *   AShooterGameMode.PostLogin                                 cache tribe size on connect
 *   AShooterPlayerController.HandleRespawned_Implementation    refresh on spawn
 *   AShooterPlayerState.NotifyPlayerJoinedTribe                refresh on join
 *   AShooterPlayerState.NotifyPlayerLeftTribe                  reset to solo on leave
 *   AShooterGameMode.Logout                                    clear cache
 *   UPrimalHarvestingComponent.GiveHarvestResource             open harvest scope for tool and dino yields
 *   UPrimalHarvestingComponent.TryMultiUse                     open harvest scope for hand pickup yields
 *   UPrimalInventoryComponent.IncrementItemTemplateQuantity    scale the granted amount inside harvest scope
 *
 * Config:
 *   ArkApi/Plugins/HarvestScale/config.json
 *   Rates   array of multipliers by tribe size, index 0 = solo, index 1 = 2 members, etc.
 *           If tribe size exceeds the array length, the last value is used as a floor.
 *
 * Config Example:
 * {
 *   "Rates": [ 1.0, 0.75, 0.5, 0.25 ]
 * }
 *
 * Scales harvest yields based on total tribe members on the current map (online + offline).
 * Tribe data is per map: only members whose characters exist on this map are counted. A 4 man
 * tribe where only 1 has transferred to Scorched gives Island tier 4, Scorched tier 1.
 *
 * Grant time scaling: the two harvest entry points open a scope carrying the player rate, and
 * the inventory grant call inside that scope receives a pre scaled amount. The engine only ever
 * sees the scaled quantity, so the pickup toast, stack, and server state all agree. Fractional
 * remainders are carried per player and item class so totals converge exactly over many swings.
 * If a scaled grant rounds to zero the grant is skipped and the fraction carries forward.
 * Grants outside harvest scope (crafting, commands, kits) are untouched.
 *
 * Tribe sizes are cached per EOS ID on login and respawn, refreshed on tribe join and leave,
 * and cleared on logout.
 *
 * Config is hot reloadable: checked every 10 seconds via file size and last write time. A failed
 * reload keeps the previous rates.
 */

#include <API/ARK/Ark.h>
#include <json.hpp>
#include <fstream>
#include <filesystem>
#include <string>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <cmath>

// =============================================================================
// Configuration
// =============================================================================

static const std::string g_config_path = "ArkApi/Plugins/HarvestScale/config.json";

static std::vector<float> g_rates;

static time_t    g_config_last_modified = 0;
static uintmax_t g_config_last_size = 0;

static bool GetFileInfo(const std::string& path, time_t& modified, uintmax_t& size)
{
    std::error_code ec;
    const auto ftime = std::filesystem::last_write_time(path, ec);
    if (ec) return false;
    const uintmax_t fsize = std::filesystem::file_size(path, ec);
    if (ec) return false;
    modified = std::chrono::system_clock::to_time_t(
        std::chrono::clock_cast<std::chrono::system_clock>(ftime));
    size = fsize;
    return true;
}

static bool LoadConfig()
{
    std::ifstream file(g_config_path);
    if (!file.is_open())
    {
        Log::GetLog()->error("[HarvestScale] Cannot open config: {}", g_config_path);
        return false;
    }

    try
    {
        nlohmann::json j;
        file >> j;

        if (!j.contains("Rates") || !j["Rates"].is_array() || j["Rates"].empty())
        {
            Log::GetLog()->error("[HarvestScale] Config requires non-empty Rates array");
            return false;
        }

        std::vector<float> rates;
        for (const auto& val : j["Rates"])
        {
            if (val.is_number())
                rates.push_back(val.get<float>());
        }

        if (rates.empty())
        {
            Log::GetLog()->error("[HarvestScale] Rates array has no valid numbers");
            return false;
        }

        g_rates = std::move(rates);

        Log::GetLog()->info("[HarvestScale] Loaded {} rate tier(s):", g_rates.size());
        for (size_t i = 0; i < g_rates.size(); ++i)
            Log::GetLog()->info("[HarvestScale]   {} member(s) = {}x", i + 1, g_rates[i]);
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[HarvestScale] Config parse error: {}", ex.what());
        return false;
    }

    GetFileInfo(g_config_path, g_config_last_modified, g_config_last_size);
    return true;
}

static void ReloadConfigIfChanged()
{
    time_t    mtime = 0;
    uintmax_t size = 0;
    if (!GetFileInfo(g_config_path, mtime, size)) return;
    if (size == 0) return;
    if (mtime == g_config_last_modified && size == g_config_last_size) return;

    Log::GetLog()->info("[HarvestScale] Config change detected, reloading...");
    if (!LoadConfig())
    {
        g_config_last_modified = mtime;
        g_config_last_size = size;
        Log::GetLog()->error("[HarvestScale] Reload failed, keeping previous rates");
    }
}

static int g_tick_counter = 0;

static void OnTimerTick()
{
    g_tick_counter++;
    if (g_tick_counter % 10 == 0)
        ReloadConfigIfChanged();
}

// =============================================================================
// Helpers
// =============================================================================

static std::string FStr(const FString& f)
{
    const char* s = TCHAR_TO_UTF8(*f);
    return (s && s[0]) ? s : "";
}

// =============================================================================
// Tribe Size Cache
// =============================================================================

static std::unordered_map<std::string, int> g_cache;
static std::mutex                            g_cache_mutex;

static int GetTribeMemberCount(AShooterPlayerState* ps)
{
    if (!ps || !ps->IsInTribe()) return 1;

    FTribeData& tribeData = ps->MyTribeDataField();
    const int count = tribeData.MembersPlayerDataIDField().Num();
    return count > 0 ? count : 1;
}

static float GetRateForEos(const std::string& eosId)
{
    int memberCount = 1;
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        auto it = g_cache.find(eosId);
        if (it != g_cache.end())
            memberCount = it->second;
    }

    const int index = memberCount - 1;
    if (index < 0) return g_rates[0];
    if (index >= static_cast<int>(g_rates.size()))
        return g_rates.back();
    return g_rates[index];
}

static void RefreshCacheForTribe(int tribeId, int newCount)
{
    if (tribeId == 0) return;

    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return;

    AShooterGameMode* gm = static_cast<AShooterGameMode*>(world->AuthorityGameModeField().Get());
    if (!gm) return;

    TArray<AActor*> controllers;
    UGameplayStatics::GetAllActorsOfClass(world,
        AShooterPlayerController::StaticClass(), &controllers);

    std::lock_guard<std::mutex> lock(g_cache_mutex);

    for (int i = 0; i < controllers.Num(); ++i)
    {
        AShooterPlayerController* pc =
            static_cast<AShooterPlayerController*>(controllers[i]);
        if (!pc) continue;

        AShooterPlayerState* ps =
            static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
        if (!ps) continue;

        if (ps->GetTribeId() != tribeId) continue;

        FString eosRaw;
        ps->GetUniqueNetIdAsString(&eosRaw);
        const std::string eos = FStr(eosRaw);
        if (!eos.empty() && eos != "unknown")
            g_cache[eos] = newCount;
    }
}

static void CacheTribeSize(APlayerController* pc)
{
    if (!pc) return;

    AShooterPlayerState* ps =
        static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
    if (!ps) return;

    FString eosRaw;
    ps->GetUniqueNetIdAsString(&eosRaw);
    const std::string eosId = FStr(eosRaw);
    if (eosId.empty() || eosId == "unknown") return;

    const int memberCount = GetTribeMemberCount(ps);

    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        g_cache[eosId] = memberCount;
    }
}

// =============================================================================
// Harvest Scope and Fractional Remainder Accumulator
// =============================================================================

struct HarvestScope
{
    bool        active = false;
    float       rate = 1.0f;
    std::string eosId;
};

static HarvestScope g_harvest_scope;

static std::unordered_map<std::string, float> g_remainder;
static std::mutex                              g_remainder_mutex;

// =============================================================================
// Hook Type Aliases
// =============================================================================

using PostLogin_t = void(*)(AShooterGameMode*, APlayerController*);
using HandleRespawned_t = void(*)(AShooterPlayerController*, APawn*, bool);
using NotifyJoined_t = void(*)(AShooterPlayerState*, FString&, FString&, bool);
using NotifyLeft_t = void(*)(AShooterPlayerState*, FString&, FString&, bool);
using Logout_t = void(*)(AShooterGameMode*, AController*);
using GiveHarvestResource_t = void(*)(UPrimalHarvestingComponent*, UPrimalInventoryComponent*,
    float, TSubclassOf<UDamageType>, AActor*,
    TArray<FHarvestResourceEntry, TSizedDefaultAllocator<32>>*);
using HarvestTryMultiUse_t = bool(*)(UPrimalHarvestingComponent*, APlayerController*, int, int);
using IncrementItemTemplateQuantity_t = int(*)(UPrimalInventoryComponent*,
    TSubclassOf<UPrimalItem>, int, bool, bool, UPrimalItem**, UPrimalItem**,
    bool, bool, bool, bool, bool, bool, bool, bool, bool, bool, TSubclassOf<UPrimalItem>);

static PostLogin_t                      Original_PostLogin = nullptr;
static HandleRespawned_t                Original_HandleRespawned = nullptr;
static NotifyJoined_t                   Original_NotifyJoined = nullptr;
static NotifyLeft_t                     Original_NotifyLeft = nullptr;
static Logout_t                         Original_Logout = nullptr;
static GiveHarvestResource_t            Original_GiveHarvestResource = nullptr;
static HarvestTryMultiUse_t             Original_HarvestTryMultiUse = nullptr;
static IncrementItemTemplateQuantity_t  Original_IncrementItemTemplateQuantity = nullptr;

// =============================================================================
// Player Resolution
// =============================================================================

static void ResolveEosAndRate(AShooterPlayerController* pc, std::string& eosId, float& rate)
{
    if (!pc) return;

    AShooterPlayerState* ps =
        static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
    if (!ps) return;

    FString eosRaw;
    ps->GetUniqueNetIdAsString(&eosRaw);
    const std::string raw = FStr(eosRaw);
    if (raw.empty() || raw == "unknown") return;

    eosId = raw;
    rate = GetRateForEos(eosId);
}

// =============================================================================
// Detours - Cache Management
// =============================================================================

void Detour_PostLogin(AShooterGameMode* gm, APlayerController* pc)
{
    Original_PostLogin(gm, pc);
    CacheTribeSize(pc);
}

void Detour_HandleRespawned(AShooterPlayerController* pc, APawn* pawn, bool newPlayer)
{
    Original_HandleRespawned(pc, pawn, newPlayer);
    CacheTribeSize(pc);
}

void Detour_NotifyJoinedTribe(AShooterPlayerState* ps,
    FString& playerName, FString& tribeName, bool joinee)
{
    Original_NotifyJoined(ps, playerName, tribeName, joinee);

    if (!ps) return;

    const int tribeId = ps->GetTribeId();
    const int newCount = GetTribeMemberCount(ps);

    RefreshCacheForTribe(tribeId, newCount);
}

void Detour_NotifyLeftTribe(AShooterPlayerState* ps,
    FString& playerName, FString& tribeName, bool joinee)
{
    const int oldTribeId = ps ? ps->GetTribeId() : 0;

    Original_NotifyLeft(ps, playerName, tribeName, joinee);

    if (!ps) return;

    FString eosRaw;
    ps->GetUniqueNetIdAsString(&eosRaw);
    const std::string eosId = FStr(eosRaw);

    if (ps->IsInTribe())
    {
        const int newCount = GetTribeMemberCount(ps);
        RefreshCacheForTribe(ps->GetTribeId(), newCount);
    }
    else
    {
        if (!eosId.empty() && eosId != "unknown")
        {
            std::lock_guard<std::mutex> lock(g_cache_mutex);
            g_cache[eosId] = 1;
        }

        if (oldTribeId != 0)
        {
            UWorld* world = AsaApi::GetApiUtils().GetWorld();
            if (world)
            {
                TArray<AActor*> controllers;
                UGameplayStatics::GetAllActorsOfClass(world,
                    AShooterPlayerController::StaticClass(), &controllers);

                for (int i = 0; i < controllers.Num(); ++i)
                {
                    AShooterPlayerController* pc =
                        static_cast<AShooterPlayerController*>(controllers[i]);
                    if (!pc) continue;

                    AShooterPlayerState* memberPs =
                        static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
                    if (!memberPs || memberPs->GetTribeId() != oldTribeId) continue;

                    const int newCount = GetTribeMemberCount(memberPs);
                    RefreshCacheForTribe(oldTribeId, newCount);
                    break;
                }
            }
        }
    }
}

void Detour_Logout(AShooterGameMode* gm, AController* controller)
{
    if (controller)
    {
        AShooterPlayerState* ps =
            static_cast<AShooterPlayerState*>(controller->PlayerStateField().Get());
        if (ps)
        {
            FString eosRaw;
            ps->GetUniqueNetIdAsString(&eosRaw);
            const std::string eosId = FStr(eosRaw);
            if (!eosId.empty() && eosId != "unknown")
            {
                {
                    std::lock_guard<std::mutex> lock(g_cache_mutex);
                    g_cache.erase(eosId);
                }
                {
                    std::lock_guard<std::mutex> lock(g_remainder_mutex);
                    const std::string prefix = eosId + "|";
                    for (auto it = g_remainder.begin(); it != g_remainder.end(); )
                    {
                        if (it->first.rfind(prefix, 0) == 0)
                            it = g_remainder.erase(it);
                        else
                            ++it;
                    }
                }
            }
        }
    }

    Original_Logout(gm, controller);
}

// =============================================================================
// Detours - Harvest Scope
// =============================================================================

void Detour_GiveHarvestResource(UPrimalHarvestingComponent* comp,
    UPrimalInventoryComponent* destInv,
    float                      harvestMultiplier,
    TSubclassOf<UDamageType>   damageType,
    AActor* harvester,
    TArray<FHarvestResourceEntry, TSizedDefaultAllocator<32>>* overrideResources)
{
    if (g_harvest_scope.active)
    {
        Original_GiveHarvestResource(comp, destInv, harvestMultiplier, damageType,
            harvester, overrideResources);
        return;
    }

    std::string eosId;
    float       rate = 1.0f;

    if (harvester)
    {
        if (harvester->IsA(APrimalDinoCharacter::StaticClass()))
        {
            APrimalDinoCharacter* dino = static_cast<APrimalDinoCharacter*>(harvester);
            AShooterCharacter* rider = dino->RiderField();
            if (rider)
                ResolveEosAndRate(
                    static_cast<AShooterPlayerController*>(rider->GetOwnerController()),
                    eosId, rate);
        }
        else if (harvester->IsA(AShooterCharacter::StaticClass()))
        {
            AShooterCharacter* ch = static_cast<AShooterCharacter*>(harvester);
            ResolveEosAndRate(
                static_cast<AShooterPlayerController*>(ch->GetOwnerController()),
                eosId, rate);
        }
    }

    if (rate >= 1.0f || eosId.empty())
    {
        Original_GiveHarvestResource(comp, destInv, harvestMultiplier, damageType,
            harvester, overrideResources);
        return;
    }

    g_harvest_scope.active = true;
    g_harvest_scope.rate = rate;
    g_harvest_scope.eosId = eosId;

    Original_GiveHarvestResource(comp, destInv, harvestMultiplier, damageType,
        harvester, overrideResources);

    g_harvest_scope.active = false;
}

bool Detour_HarvestTryMultiUse(UPrimalHarvestingComponent* comp,
    APlayerController* forPC, int useIndex, int hitBodyIndex)
{
    if (g_harvest_scope.active)
        return Original_HarvestTryMultiUse(comp, forPC, useIndex, hitBodyIndex);

    std::string eosId;
    float       rate = 1.0f;

    if (forPC && forPC->IsA(AShooterPlayerController::GetPrivateStaticClass()))
        ResolveEosAndRate(static_cast<AShooterPlayerController*>(forPC), eosId, rate);

    if (rate >= 1.0f || eosId.empty())
        return Original_HarvestTryMultiUse(comp, forPC, useIndex, hitBodyIndex);

    g_harvest_scope.active = true;
    g_harvest_scope.rate = rate;
    g_harvest_scope.eosId = eosId;

    const bool result = Original_HarvestTryMultiUse(comp, forPC, useIndex, hitBodyIndex);

    g_harvest_scope.active = false;

    return result;
}

// =============================================================================
// Detour - Grant Time Scaling
// =============================================================================

int Detour_IncrementItemTemplateQuantity(UPrimalInventoryComponent* inv,
    TSubclassOf<UPrimalItem> itemTemplate, int amount, bool bReplicateToClient,
    bool bIsBlueprint, UPrimalItem** useSpecificItem, UPrimalItem** incrementedItem,
    bool bRequireExactClassMatch, bool bIsCraftingResourceConsumption,
    bool bIsFromUseConsumption, bool bIsArkTributeItem, bool showHUDNotification,
    bool bDontRecalcSpoilingTime, bool bDontExceedMaxItems, bool bDontProgressMilestones,
    bool bDontAddToSlot, bool bDontBroadcastItemUpdateEvents,
    TSubclassOf<UPrimalItem> ignoreClass)
{
    if (!g_harvest_scope.active || amount <= 0 || g_harvest_scope.rate >= 1.0f)
        return Original_IncrementItemTemplateQuantity(inv, itemTemplate, amount,
            bReplicateToClient, bIsBlueprint, useSpecificItem, incrementedItem,
            bRequireExactClassMatch, bIsCraftingResourceConsumption, bIsFromUseConsumption,
            bIsArkTributeItem, showHUDNotification, bDontRecalcSpoilingTime,
            bDontExceedMaxItems, bDontProgressMilestones, bDontAddToSlot,
            bDontBroadcastItemUpdateEvents, ignoreClass);

    int scaled = 0;
    {
        const std::string remKey = g_harvest_scope.eosId + "|"
            + std::to_string(reinterpret_cast<uintptr_t>(itemTemplate.uClass));

        std::lock_guard<std::mutex> lock(g_remainder_mutex);
        const float carry = g_remainder.count(remKey) ? g_remainder[remKey] : 0.0f;
        const float exact = static_cast<float>(amount) * g_harvest_scope.rate + carry;
        scaled = static_cast<int>(std::floor(exact));
        g_remainder[remKey] = exact - static_cast<float>(scaled);
    }

    if (scaled <= 0)
        return 0;

    return Original_IncrementItemTemplateQuantity(inv, itemTemplate, scaled,
        bReplicateToClient, bIsBlueprint, useSpecificItem, incrementedItem,
        bRequireExactClassMatch, bIsCraftingResourceConsumption, bIsFromUseConsumption,
        bIsArkTributeItem, showHUDNotification, bDontRecalcSpoilingTime,
        bDontExceedMaxItems, bDontProgressMilestones, bDontAddToSlot,
        bDontBroadcastItemUpdateEvents, ignoreClass);
}

// =============================================================================
// Plugin Entry Points
// =============================================================================

extern "C" __declspec(dllexport) void Plugin_Init()
{
    Log::Get().Init("HarvestScale");

    if (!LoadConfig())
    {
        Log::GetLog()->error("[HarvestScale] Halted - config error");
        return;
    }

    AsaApi::GetCommands().AddOnTimerCallback(FString(L"HarvestScale_Timer"), &OnTimerTick);

    AsaApi::GetHooks().SetHook(
        "AShooterGameMode.PostLogin(APlayerController*)",
        (LPVOID)&Detour_PostLogin,
        (LPVOID*)&Original_PostLogin
    );

    AsaApi::GetHooks().SetHook(
        "AShooterPlayerController.HandleRespawned_Implementation(APawn*,bool)",
        (LPVOID)&Detour_HandleRespawned,
        (LPVOID*)&Original_HandleRespawned
    );

    AsaApi::GetHooks().SetHook(
        "AShooterPlayerState.NotifyPlayerJoinedTribe(FString&,FString&,bool)",
        (LPVOID)&Detour_NotifyJoinedTribe,
        (LPVOID*)&Original_NotifyJoined
    );

    AsaApi::GetHooks().SetHook(
        "AShooterPlayerState.NotifyPlayerLeftTribe(FString&,FString&,bool)",
        (LPVOID)&Detour_NotifyLeftTribe,
        (LPVOID*)&Original_NotifyLeft
    );

    AsaApi::GetHooks().SetHook(
        "AShooterGameMode.Logout(AController*)",
        (LPVOID)&Detour_Logout,
        (LPVOID*)&Original_Logout
    );

    AsaApi::GetHooks().SetHook(
        "UPrimalHarvestingComponent.GiveHarvestResource(UPrimalInventoryComponent*,float,TSubclassOf<UDamageType>,AActor*,TArray<FHarvestResourceEntry,TSizedDefaultAllocator<32>>*)",
        (LPVOID)&Detour_GiveHarvestResource,
        (LPVOID*)&Original_GiveHarvestResource
    );

    AsaApi::GetHooks().SetHook(
        "UPrimalHarvestingComponent.TryMultiUse(APlayerController*,int,int)",
        (LPVOID)&Detour_HarvestTryMultiUse,
        (LPVOID*)&Original_HarvestTryMultiUse
    );

    AsaApi::GetHooks().SetHook(
        "UPrimalInventoryComponent.IncrementItemTemplateQuantity(TSubclassOf<UPrimalItem>,int,bool,bool,UPrimalItem**,UPrimalItem**,bool,bool,bool,bool,bool,bool,bool,bool,bool,bool,TSubclassOf<UPrimalItem>)",
        (LPVOID)&Detour_IncrementItemTemplateQuantity,
        (LPVOID*)&Original_IncrementItemTemplateQuantity
    );

    Log::GetLog()->info("[HarvestScale] Plugin loaded");
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    AsaApi::GetCommands().RemoveOnTimerCallback(FString(L"HarvestScale_Timer"));

    AsaApi::GetHooks().DisableHook(
        "AShooterGameMode.PostLogin(APlayerController*)",
        (LPVOID)&Detour_PostLogin
    );

    AsaApi::GetHooks().DisableHook(
        "AShooterPlayerController.HandleRespawned_Implementation(APawn*,bool)",
        (LPVOID)&Detour_HandleRespawned
    );

    AsaApi::GetHooks().DisableHook(
        "AShooterPlayerState.NotifyPlayerJoinedTribe(FString&,FString&,bool)",
        (LPVOID)&Detour_NotifyJoinedTribe
    );

    AsaApi::GetHooks().DisableHook(
        "AShooterPlayerState.NotifyPlayerLeftTribe(FString&,FString&,bool)",
        (LPVOID)&Detour_NotifyLeftTribe
    );

    AsaApi::GetHooks().DisableHook(
        "AShooterGameMode.Logout(AController*)",
        (LPVOID)&Detour_Logout
    );

    AsaApi::GetHooks().DisableHook(
        "UPrimalHarvestingComponent.GiveHarvestResource(UPrimalInventoryComponent*,float,TSubclassOf<UDamageType>,AActor*,TArray<FHarvestResourceEntry,TSizedDefaultAllocator<32>>*)",
        (LPVOID)&Detour_GiveHarvestResource
    );

    AsaApi::GetHooks().DisableHook(
        "UPrimalHarvestingComponent.TryMultiUse(APlayerController*,int,int)",
        (LPVOID)&Detour_HarvestTryMultiUse
    );

    AsaApi::GetHooks().DisableHook(
        "UPrimalInventoryComponent.IncrementItemTemplateQuantity(TSubclassOf<UPrimalItem>,int,bool,bool,UPrimalItem**,UPrimalItem**,bool,bool,bool,bool,bool,bool,bool,bool,bool,bool,TSubclassOf<UPrimalItem>)",
        (LPVOID)&Detour_IncrementItemTemplateQuantity
    );

    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        g_cache.clear();
    }

    {
        std::lock_guard<std::mutex> lock(g_remainder_mutex);
        g_remainder.clear();
    }

    Log::GetLog()->info("[HarvestScale] Plugin unloaded");
}