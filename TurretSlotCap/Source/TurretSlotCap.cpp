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
 * Hook categories: Structures
 *
 * Hooks:
 *   APrimalStructureTurret.BeginPlay() - applies the configured slot cap on turret placement and on server load
 *   AShooterGameMode.Tick(float)       - hot-reloads config every 10 seconds
 *
 * Config:
 *   TurretSlots - map of turret blueprint substring to usable ammo slot count
 *
 * Behavior:
 *   Writes MaxInventoryItems and AbsoluteMaxInventoryItems on matched turrets so usable
 *   slots equal the configured cap with no ghost slots and hand placement respects the cap.
 *   Turrets whose blueprint matches no configured key are left untouched. The client grid is
 *   drawn from the turret type default and is not affected, only the enforced capacity changes.
 */

#include <API/ARK/Ark.h>
#include <json.hpp>
#include <fstream>
#include <string>
#include <unordered_map>
#include <sys/stat.h>

#pragma warning(disable: 4191)

// =============================================================================
// Configuration
// =============================================================================

static const std::string g_config_path = "ArkApi/Plugins/TurretSlotCap/config.json";

static std::unordered_map<std::string, int> g_turret_slots;

static time_t    g_config_mtime = 0;
static uintmax_t g_config_size  = 0;

static time_t GetFileMTime(const std::string& path)
{
    struct _stat st {};
    if (_stat(path.c_str(), &st) == 0) return st.st_mtime;
    return 0;
}

static uintmax_t GetFileSize(const std::string& path)
{
    struct _stat st {};
    if (_stat(path.c_str(), &st) == 0) return static_cast<uintmax_t>(st.st_size);
    return 0;
}

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

        std::unordered_map<std::string, int> newMap;
        if (j.contains("TurretSlots") && j["TurretSlots"].is_object())
        {
            for (auto& [key, val] : j["TurretSlots"].items())
            {
                if (!val.is_number_integer()) continue;
                int slots = val.get<int>();
                if (slots > 0) newMap[key] = slots;
            }
        }

        g_turret_slots = std::move(newMap);
        g_config_mtime = GetFileMTime(g_config_path);
        g_config_size  = GetFileSize(g_config_path);
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[TurretSlotCap] Config parse error: {}", ex.what());
        return false;
    }

    Log::GetLog()->info("[TurretSlotCap] Config loaded: {} turret mapping(s)", g_turret_slots.size());
    return true;
}

// =============================================================================
// Hook Type Aliases
// =============================================================================

using BeginPlay_t = void(*)(APrimalStructureTurret*);
using Tick_t      = void(*)(AShooterGameMode*, float);

static BeginPlay_t Original_BeginPlay = nullptr;
static Tick_t      Original_Tick      = nullptr;

// =============================================================================
// BeginPlay Hook - Apply Slot Cap
// =============================================================================

static void Detour_BeginPlay(APrimalStructureTurret* turret)
{
    Original_BeginPlay(turret);
    if (!turret) return;

    const std::string bp = FStr(AsaApi::GetApiUtils().GetBlueprint(turret));

    int cap = -1;
    for (auto& [sub, slots] : g_turret_slots)
    {
        if (!sub.empty() && bp.find(sub) != std::string::npos)
        {
            cap = slots;
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
// Tick Hook - Config Hot-Reload
// =============================================================================

static float g_config_check_accumulator = 0.0f;

static void Detour_Tick(AShooterGameMode* gm, float delta)
{
    Original_Tick(gm, delta);

    g_config_check_accumulator += delta;
    if (g_config_check_accumulator >= 10.0f)
    {
        g_config_check_accumulator = 0.0f;

        const uintmax_t newSize  = GetFileSize(g_config_path);
        const time_t    newMtime = GetFileMTime(g_config_path);

        if (newSize == 0 || newMtime == 0) return;

        if (newSize != g_config_size || newMtime != g_config_mtime)
        {
            Log::GetLog()->info("[TurretSlotCap] Config change detected, reloading...");
            LoadConfig();
        }
    }
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

    AsaApi::GetHooks().SetHook(
        "AShooterGameMode.Tick(float)",
        (LPVOID)&Detour_Tick,
        (LPVOID*)&Original_Tick
    );

    Log::GetLog()->info("[TurretSlotCap] Plugin loaded");
}

static void PluginUnload()
{
    AsaApi::GetHooks().DisableHook(
        "APrimalStructureTurret.BeginPlay()",
        (LPVOID)&Detour_BeginPlay
    );

    AsaApi::GetHooks().DisableHook(
        "AShooterGameMode.Tick(float)",
        (LPVOID)&Detour_Tick
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
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { PluginUnload(); }
    catch (const std::exception& ex)
    {
        Log::GetLog()->critical("[TurretSlotCap] Unload exception: {}", ex.what());
    }
}