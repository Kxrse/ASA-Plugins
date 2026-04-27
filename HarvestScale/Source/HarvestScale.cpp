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
 * Hook category: Inventory
 *
 * Scales harvest yields based on total tribe members on the current map (online + offline).
 * Rates are config-driven: an array where index 0 = solo, index 1 = 2 members, etc.
 * If tribe size exceeds the array length, the last value is used as a floor.
 *
 * Tribe data is per-map: only members whose characters exist on this map are counted.
 * A 4-man tribe where only 1 has transferred to Scorched: Island = tier 4, Scorched = tier 1.
 *
 * Post-process approach: lets original GiveHarvestResource run unmodified,
 * then diffs inventory and reduces gained quantities by the rate factor.
 *
 * Config:
 *   ArkApi/Plugins/HarvestScale/config.json
 *   Rates: [1.0, 0.75, 0.5, 0.25]
 *
 * Hooks:
 *   AShooterGameMode.PostLogin                          — cache tribe size on connect
 *   AShooterPlayerController.HandleRespawned_Implementation — refresh on spawn
 *   AShooterPlayerState.NotifyPlayerJoinedTribe         — refresh on join
 *   AShooterPlayerState.NotifyPlayerLeftTribe           — reset to solo on leave
 *   AShooterGameMode.Logout                             — clear cache
 *   UPrimalHarvestingComponent.GiveHarvestResource      — post-process reduce yields
 */

#include <API/ARK/Ark.h>
#include <json.hpp>
#include <fstream>
#include <string>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <cmath>

 // =============================================================================
 // Configuration
 // =============================================================================

static std::vector<float> g_rates;

static bool LoadConfig()
{
    const std::string path = "ArkApi/Plugins/HarvestScale/config.json";
    std::ifstream file(path);
    if (!file.is_open())
    {
        Log::GetLog()->error("[HarvestScale] Cannot open config: {}", path);
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

        g_rates.clear();
        for (const auto& val : j["Rates"])
        {
            if (val.is_number())
                g_rates.push_back(val.get<float>());
        }

        if (g_rates.empty())
        {
            Log::GetLog()->error("[HarvestScale] Rates array has no valid numbers");
            return false;
        }

        Log::GetLog()->info("[HarvestScale] Loaded {} rate tier(s):", g_rates.size());
        for (size_t i = 0; i < g_rates.size(); ++i)
            Log::GetLog()->info("[HarvestScale]   {} member(s) = {}x", i + 1, g_rates[i]);
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[HarvestScale] Config parse error: {}", ex.what());
        return false;
    }

    return true;
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

static std::unordered_map<std::string, int> g_cache;  // eos_id -> member count
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

// Sweeps all online players with the given tribe ID and updates their cached count.
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

    Log::GetLog()->info("[HarvestScale] REFRESH tribe_id={} new_count={}", tribeId, newCount);
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

    Log::GetLog()->info("[HarvestScale] CACHE eos_id={} tribe_members={}", eosId, memberCount);
}

// =============================================================================
// Inventory Snapshot
// =============================================================================

static std::unordered_map<std::string, int> SnapshotInventory(UPrimalInventoryComponent* inv)
{
    std::unordered_map<std::string, int> snap;
    if (!inv) return snap;

    TArray<UPrimalItem*>& items = inv->InventoryItemsField();
    for (int i = 0; i < items.Num(); ++i)
    {
        UPrimalItem* item = items[i];
        if (!item) continue;
        const std::string name = FStr(item->DescriptiveNameBaseField());
        if (name.empty()) continue;
        snap[name] += item->GetItemQuantity();
    }
    return snap;
}

// =============================================================================
// Fractional Remainder Accumulator
// =============================================================================

// Carries fractional reduction remainders across harvest swings so the total
// converges to the exact target over many events.
// Key: eos_id + "|" + item_name -> remainder (always < 1.0)

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

static PostLogin_t             Original_PostLogin = nullptr;
static HandleRespawned_t       Original_HandleRespawned = nullptr;
static NotifyJoined_t          Original_NotifyJoined = nullptr;
static NotifyLeft_t            Original_NotifyLeft = nullptr;
static Logout_t                Original_Logout = nullptr;
static GiveHarvestResource_t   Original_GiveHarvestResource = nullptr;

// =============================================================================
// Detours — Cache Management
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

        Log::GetLog()->info("[HarvestScale] TRIBE_LEAVE eos_id={} tribe_members=1", eosId);
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
// Detour — Harvest (Post-Process)
// =============================================================================

void Detour_GiveHarvestResource(UPrimalHarvestingComponent* comp,
    UPrimalInventoryComponent* destInv,
    float                      harvestMultiplier,
    TSubclassOf<UDamageType>   damageType,
    AActor* harvester,
    TArray<FHarvestResourceEntry, TSizedDefaultAllocator<32>>* overrideResources)
{
    UPrimalInventoryComponent* inv = nullptr;
    std::string eosId;
    float       rate = 1.0f;

    if (harvester)
    {
        // Check if harvester is a dino with a rider.
        if (harvester->IsA(APrimalDinoCharacter::StaticClass()))
        {
            APrimalDinoCharacter* dino = static_cast<APrimalDinoCharacter*>(harvester);
            inv = dino->MyInventoryComponentField();

            AShooterCharacter* rider = dino->RiderField();
            if (rider)
            {
                AShooterPlayerController* pc =
                    static_cast<AShooterPlayerController*>(rider->GetOwnerController());
                if (pc)
                {
                    AShooterPlayerState* ps =
                        static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
                    if (ps)
                    {
                        FString eosRaw;
                        ps->GetUniqueNetIdAsString(&eosRaw);
                        const std::string raw = FStr(eosRaw);
                        if (!raw.empty() && raw != "unknown")
                            eosId = raw;
                    }
                }
            }
        }
        // Otherwise assume player character.
        else if (harvester->IsA(AShooterCharacter::StaticClass()))
        {
            AShooterCharacter* ch = static_cast<AShooterCharacter*>(harvester);
            inv = ch->MyInventoryComponentField();

            AShooterPlayerController* pc =
                static_cast<AShooterPlayerController*>(ch->GetOwnerController());
            if (pc)
            {
                AShooterPlayerState* ps =
                    static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
                if (ps)
                {
                    FString eosRaw;
                    ps->GetUniqueNetIdAsString(&eosRaw);
                    const std::string raw = FStr(eosRaw);
                    if (!raw.empty() && raw != "unknown")
                        eosId = raw;
                }
            }
        }

        if (!eosId.empty())
            rate = GetRateForEos(eosId);
    }

    // If rate is 1.0, no reduction needed — skip snapshot overhead.
    if (rate >= 1.0f || !inv || eosId.empty())
    {
        Original_GiveHarvestResource(comp, destInv, harvestMultiplier, damageType,
            harvester, overrideResources);
        return;
    }

    // Snapshot before (aggregated by item name).
    std::unordered_map<std::string, int> before = SnapshotInventory(inv);

    // Let original run unmodified.
    Original_GiveHarvestResource(comp, destInv, harvestMultiplier, damageType,
        harvester, overrideResources);

    // Snapshot after (aggregated by item name).
    std::unordered_map<std::string, int> after = SnapshotInventory(inv);

    // Compute per-name reductions with fractional remainder carry.
    std::unordered_map<std::string, int> reductions;
    {
        std::lock_guard<std::mutex> lock(g_remainder_mutex);
        for (const auto& [name, qtyAfter] : after)
        {
            const int qtyBefore = [&]() -> int {
                auto it = before.find(name);
                return it != before.end() ? it->second : 0;
                }();

            const int delta = qtyAfter - qtyBefore;
            if (delta <= 0) continue;

            const std::string remKey = eosId + "|" + name;
            const float carry = g_remainder.count(remKey) ? g_remainder[remKey] : 0.0f;
            const float exactReduction = static_cast<float>(delta) * (1.0f - rate) + carry;
            const int intReduction = static_cast<int>(std::floor(exactReduction));
            g_remainder[remKey] = exactReduction - static_cast<float>(intReduction);

            if (intReduction > 0)
                reductions[name] = intReduction;
        }
    }

    if (reductions.empty()) return;

    // Apply reductions across item stacks.
    TArray<UPrimalItem*>& items = inv->InventoryItemsField();
    for (int i = items.Num() - 1; i >= 0 && !reductions.empty(); --i)
    {
        UPrimalItem* item = items[i];
        if (!item) continue;

        const std::string name = FStr(item->DescriptiveNameBaseField());
        if (name.empty()) continue;

        auto it = reductions.find(name);
        if (it == reductions.end()) continue;

        int& remaining = it->second;
        const int qty = item->GetItemQuantity();
        const int take = (remaining >= qty) ? qty - 1 : remaining;

        if (take > 0)
        {
            item->SetQuantity(qty - take, true);
            remaining -= take;
        }

        if (remaining <= 0)
            reductions.erase(it);
    }
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

    Log::GetLog()->info("[HarvestScale] Plugin loaded");
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
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