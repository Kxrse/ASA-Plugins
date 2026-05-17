/*
TidyDams - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * TidyDams - ASA Plugin
 *
 * Hook category: Inventory
 *
 * Automatically destroys beaver dams when a player closes the inventory
 * and only wood (or nothing) remains inside.
 *
 * Hooks:
 *   AShooterPlayerController.ServerActorCloseRemoteInventory_Implementation(UPrimalInventoryComponent*)
 *     — checks dam inventory on close, queues destroy if wood-only or empty
 */

#include <API/ARK/Ark.h>
#include <vector>
#include <string>

#pragma comment(lib, "AsaApi.lib")
#pragma warning(disable: 4191)

// =============================================================================
// Constants
// =============================================================================

static const std::string DAM_BP_SUBSTRING = "BeaverDam";
static const std::string WOOD_ITEM_NAME   = "Wood";

// =============================================================================
// Helpers
// =============================================================================

static std::string FStr(const FString& f)
{
    const char* s = TCHAR_TO_UTF8(*f);
    return (s && s[0]) ? s : "";
}

static bool IsWoodOnly(UPrimalInventoryComponent* inv)
{
    if (!inv) return true;

    TArray<UPrimalItem*>& items = inv->InventoryItemsField();
    if (items.Num() == 0) return true;

    for (int i = 0; i < items.Num(); ++i)
    {
        UPrimalItem* item = items[i];
        if (!item) continue;

        if (FStr(item->DescriptiveNameBaseField()) != WOOD_ITEM_NAME)
            return false;
    }

    return true;
}

// =============================================================================
// Flagged Inventories (game thread only)
// =============================================================================

static std::vector<UPrimalInventoryComponent*> g_flagged;

// =============================================================================
// Hook
// =============================================================================

using ServerActorCloseRemoteInventory_t = void(*)(AShooterPlayerController*, UPrimalInventoryComponent*);
static ServerActorCloseRemoteInventory_t Original_ServerActorCloseRemoteInventory = nullptr;

static void Detour_ServerActorCloseRemoteInventory(AShooterPlayerController* pc,
    UPrimalInventoryComponent* inv)
{
    Original_ServerActorCloseRemoteInventory(pc, inv);

    if (!inv) return;

    FString bpStr = AsaApi::GetApiUtils().GetBlueprint(inv);
    const std::string bp = FStr(bpStr);
    Log::GetLog()->info("[TidyDams] {}", bp);

    if (bp.find(DAM_BP_SUBSTRING) == std::string::npos) return;

    if (!IsWoodOnly(inv)) return;

    g_flagged.push_back(inv);
}

// =============================================================================
// Tick Callback — Find Owner Container and Destroy
// =============================================================================

static void OnTick(float)
{
    if (g_flagged.empty()) return;

    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) { g_flagged.clear(); return; }

    TArray<AActor*> containers;
    UGameplayStatics::GetAllActorsOfClass(world,
        APrimalStructureItemContainer::StaticClass(), &containers);

    for (auto* flaggedInv : g_flagged)
    {
        for (int i = 0; i < containers.Num(); ++i)
        {
            APrimalStructureItemContainer* container =
                static_cast<APrimalStructureItemContainer*>(containers[i]);
            if (!container) continue;

            if (container->MyInventoryComponentField() == flaggedInv)
            {
                try { container->Destroy(false, false); }
                catch (...) {}
                break;
            }
        }
    }

    g_flagged.clear();
}

// =============================================================================
// Init / Unload
// =============================================================================

static void PluginInit()
{
    Log::Get().Init("TidyDams");

    AsaApi::GetHooks().SetHook(
        "AShooterPlayerController.ServerActorCloseRemoteInventory_Implementation(UPrimalInventoryComponent*)",
        (LPVOID)&Detour_ServerActorCloseRemoteInventory,
        (LPVOID*)&Original_ServerActorCloseRemoteInventory);

    AsaApi::GetCommands().AddOnTickCallback(FString(L"TidyDams_Tick"), &OnTick);

    Log::GetLog()->info("[TidyDams] Loaded");
}

static void PluginUnload()
{
    AsaApi::GetHooks().DisableHook(
        "AShooterPlayerController.ServerActorCloseRemoteInventory_Implementation(UPrimalInventoryComponent*)",
        (LPVOID)&Detour_ServerActorCloseRemoteInventory);

    AsaApi::GetCommands().RemoveOnTickCallback(FString(L"TidyDams_Tick"));

    g_flagged.clear();

    Log::GetLog()->info("[TidyDams] Unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { PluginInit(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->critical("[TidyDams] Init exception: {}", e.what());
    }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { PluginUnload(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->error("[TidyDams] Unload exception: {}", e.what());
    }
}