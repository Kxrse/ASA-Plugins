/*
TurretFPS - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * TurretFPS - ASA Plugin
 *
 * Hook categories: Items (ClientsFireProjectile)
 *
 * Purpose:
 *   Suppresses the tek turret server->client fire-visual RPC to reduce client
 *   particle load. Auto and heavy turrets do not route through this RPC and are
 *   unaffected. The damage path (DoFire / DoFireProjectile) is untouched.
 *
 * Hooks:
 *   APrimalStructureTurret.ClientsFireProjectile — skipped for tek turrets
 */

#include <API/ARK/Ark.h>
#include <string>
#include <algorithm>

#pragma warning(disable: 4191)
#pragma comment(lib, "AsaApi")

static std::string FStr(const FString& f)
{
    const char* s = TCHAR_TO_UTF8(*f);
    return (s && s[0]) ? s : "";
}

static bool IsTekTurret(const std::string& bp)
{
    std::string lowered = bp;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
        [](unsigned char c) { return static_cast<char>(::tolower(c)); });
    return lowered.find("structureturrettek") != std::string::npos;
}

using ClientsFireProjectile_t = void(*)(APrimalStructureTurret*, FVector, FVector);

static ClientsFireProjectile_t Original_ClientsFireProjectile = nullptr;

static void Detour_ClientsFireProjectile(APrimalStructureTurret* turret, FVector origin, FVector shootDir)
{
    if (turret)
    {
        const std::string bp = FStr(AsaApi::GetApiUtils().GetBlueprint(turret));
        if (IsTekTurret(bp)) return;
    }

    Original_ClientsFireProjectile(turret, origin, shootDir);
}

static void PluginInit()
{
    Log::Get().Init("TurretFPS");

    try
    {
        AsaApi::GetHooks().SetHook(
            "APrimalStructureTurret.ClientsFireProjectile(UE::Math::TVector<double>,FVector_NetQuantizeNormal)",
            (LPVOID)&Detour_ClientsFireProjectile,
            (LPVOID*)&Original_ClientsFireProjectile
        );
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[TurretFPS] Failed to hook ClientsFireProjectile: {}", ex.what());
    }

    Log::GetLog()->info("[TurretFPS] Plugin loaded");
}

static void PluginUnload()
{
    AsaApi::GetHooks().DisableHook(
        "APrimalStructureTurret.ClientsFireProjectile(UE::Math::TVector<double>,FVector_NetQuantizeNormal)",
        (LPVOID)&Detour_ClientsFireProjectile
    );

    Log::GetLog()->info("[TurretFPS] Plugin unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { PluginInit(); }
    catch (const std::exception& ex)
    {
        Log::GetLog()->critical("[TurretFPS] Init exception: {}", ex.what());
    }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { PluginUnload(); }
    catch (const std::exception& ex)
    {
        Log::GetLog()->critical("[TurretFPS] Unload exception: {}", ex.what());
    }
}