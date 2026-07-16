/*
TurretSlotCap - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * TurretSlotCap - ASA Plugin
 *
 * Hooks:
 *   APrimalStructureTurret.BeginPlay()  apply the configured slot cap on placement and on server load
 *
 * Config:
 *   ArkApi/Plugins/TurretSlotCap/config.json
 *   TurretSlotMap: array of entries, each with a full TurretBP path matched exactly to a slot count
 *
 * Config Example:
 * {
 *     "TurretSlotMap": [
 *         {
 *             "TurretBP": "Blueprint'/Game/PrimalEarth/Structures/BuildingBases/StructureTurretBaseBP.StructureTurretBaseBP'",
 *             "Slots": 5
 *         },
 *         {
 *             "TurretBP": "Blueprint'/Game/PrimalEarth/Structures/BuildingBases/StructureTurretBaseBP_Heavy.StructureTurretBaseBP_Heavy'",
 *             "Slots": 5
 *         },
 *         {
 *             "TurretBP": "Blueprint'/Game/PrimalEarth/Structures/BuildingBases/StructureTurretTek.StructureTurretTek'",
 *             "Slots": 2
 *         }
 *     ]
 * }
 *
 * Behavior:
 *   On BeginPlay the turret blueprint is matched exactly against the configured TurretBP paths,
 *   first match wins, and MaxInventoryItems and AbsoluteMaxInventoryItems are written so usable
 *   slots equal the configured cap with no ghost slots and hand placement respects the cap.
 *   Turrets whose blueprint matches no configured entry are left untouched. The client grid is
 *   drawn from the turret type default and is not affected, only the enforced capacity changes.
 *   Config is rescanned every 10 seconds on a one second timer using size and last write time.
 *   Changes affect turrets placed after the reload, existing turrets keep their cap until
 *   re-placed or the next server start. TurretBP paths are game assets, confirm them against the
 *   server log on first load.
 */

#include <API/ARK/Ark.h>

#pragma warning(disable: 4191)
#pragma comment(lib, "AsaApi.lib")

#include <json.hpp>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>

// =============================================================================
// Configuration
// =============================================================================

static const std::string g_config_path = "ArkApi/Plugins/TurretSlotCap/config.json";

struct TurretSlotEntry
{
    std::string turretBp;
    int         slots = 0;
};

static std::vector<TurretSlotEntry> g_turret_slots;

static std::uintmax_t                  g_cfg_size = 0;
static std::filesystem::file_time_type g_cfg_mtime{};

static std::string FStr(const FString& f)
{
    const char* s = TCHAR_TO_UTF8(*f);
    return (s && s[0]) ? s : "";
}

static bool LoadConfig()
{
    std::ifstream file(g_config_path);
    if (!file.is_open())
    {
        Log::GetLog()->error("[TurretSlotCap] Cannot open config: {}", g_config_path);
        return false;
    }

    try
    {
        nlohmann::json j;
        file >> j;

        std::vector<TurretSlotEntry> newMap;
        if (j.contains("TurretSlotMap") && j["TurretSlotMap"].is_array())
        {
            for (auto& val : j["TurretSlotMap"])
            {
                if (!val.is_object()) continue;

                TurretSlotEntry e;
                e.turretBp = val.value("TurretBP", "");
                e.slots = val.value("Slots", 0);

                if (!e.turretBp.empty() && e.slots > 0)
                    newMap.push_back(std::move(e));
            }
        }

        g_turret_slots = std::move(newMap);
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[TurretSlotCap] Config parse error: {}", ex.what());
        return false;
    }

    try
    {
        g_cfg_size = std::filesystem::file_size(g_config_path);
        g_cfg_mtime = std::filesystem::last_write_time(g_config_path);
    }
    catch (...) {}

    Log::GetLog()->info("[TurretSlotCap] Config loaded: {} turret mapping(s)", g_turret_slots.size());
    return true;
}

static void ReloadConfigIfChanged()
{
    std::uintmax_t size = 0;
    std::filesystem::file_time_type mtime{};
    try
    {
        size = std::filesystem::file_size(g_config_path);
        mtime = std::filesystem::last_write_time(g_config_path);
    }
    catch (...) { return; }

    if (size == 0) return;
    if (size == g_cfg_size && mtime == g_cfg_mtime) return;

    Log::GetLog()->info("[TurretSlotCap] Config change detected, reloading...");
    LoadConfig();
}

// =============================================================================
// Hook Type Aliases
// =============================================================================

using BeginPlay_t = void(*)(APrimalStructureTurret*);
static BeginPlay_t Original_BeginPlay = nullptr;

// =============================================================================
// BeginPlay Hook - Apply Slot Cap
// =============================================================================

static void Detour_BeginPlay(APrimalStructureTurret* turret)
{
    Original_BeginPlay(turret);
    if (!turret) return;

    const std::string bp = FStr(AsaApi::GetApiUtils().GetBlueprint(turret));

    int cap = -1;
    for (const auto& e : g_turret_slots)
    {
        if (bp == e.turretBp)
        {
            cap = e.slots;
            break;
        }
    }
    if (cap <= 0) return;

    UPrimalInventoryComponent* inv = turret->MyInventoryComponentField();
    if (!inv) return;

    inv->MaxInventoryItemsField()         = cap;
    inv->AbsoluteMaxInventoryItemsField() = cap;
}

// =============================================================================
// Timer - Config Hot-Reload
// =============================================================================

static int g_tick_counter = 0;

static void OnTimerTick()
{
    g_tick_counter++;
    if (g_tick_counter % 10 == 0)
        ReloadConfigIfChanged();
}

// =============================================================================
// Plugin Entry Points
// =============================================================================

static void PluginInit()
{
    Log::Get().Init("TurretSlotCap");

    if (!LoadConfig())
    {
        Log::GetLog()->error("[TurretSlotCap] Halted - config error");
        return;
    }

    AsaApi::GetHooks().SetHook(
        "APrimalStructureTurret.BeginPlay()",
        (LPVOID)&Detour_BeginPlay,
        (LPVOID*)&Original_BeginPlay
    );

    AsaApi::GetCommands().AddOnTimerCallback(FString(L"TurretSlotCap_Timer"), &OnTimerTick);

    Log::GetLog()->info("[TurretSlotCap] Plugin loaded");
}

static void PluginUnload()
{
    AsaApi::GetCommands().RemoveOnTimerCallback(FString(L"TurretSlotCap_Timer"));

    AsaApi::GetHooks().DisableHook(
        "APrimalStructureTurret.BeginPlay()",
        (LPVOID)&Detour_BeginPlay
    );

    Log::GetLog()->info("[TurretSlotCap] Plugin unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { PluginInit(); }
    catch (const std::exception& ex)
    {
        Log::GetLog()->critical("[TurretSlotCap] Init exception: {}", ex.what());
    }
    catch (...)
    {
        Log::GetLog()->critical("[TurretSlotCap] Init unknown exception");
    }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { PluginUnload(); }
    catch (const std::exception& ex)
    {
        Log::GetLog()->critical("[TurretSlotCap] Unload exception: {}", ex.what());
    }
    catch (...)
    {
        Log::GetLog()->critical("[TurretSlotCap] Unload unknown exception");
    }
}