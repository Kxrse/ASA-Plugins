/*
FoundationParity - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * FoundationParity - ASA Plugin
 *
 * Hook category: Structures
 *
 * Purpose:
 *   Closes the enemy-foundation proximity exploit. ARK blocks foundations
 *   from being placed near an enemy foundation, but walls and ceilings skip
 *   that check entirely. This gives walls and ceilings parity with
 *   foundations: a foundation, wall, or ceiling cannot be placed within
 *   ARK's enemy-foundation prevention radius of an enemy foundation, wall,
 *   or ceiling.
 *
 * Hooks:
 *   APrimalStructure.FinalStructurePlacement(...) — reject before commit
 *
 * Behavior:
 *   When a policed structure (foundation/wall/ceiling) is finalized, scans
 *   all structures for an enemy policed structure within
 *   EnemyFoundationPreventionRadius (3D) of the placement location. If found,
 *   placement is rejected and the placer is notified.
 */

#include <API/ARK/Ark.h>
#include <string>
#include <algorithm>
#include <cctype>
#include <cmath>

#pragma warning(disable: 4191)
#pragma comment(lib, "AsaApi")

using FinalStructurePlacement_t = char(*)(APrimalStructure*, APlayerController*,
    UE::Math::TVector<double>*, UE::Math::TRotator<double>*, UE::Math::TRotator<double>*,
    APawn*, FName, bool, FPlacementData*);
static FinalStructurePlacement_t Original_FinalStructurePlacement = nullptr;

static std::string FStr(const FString& f)
{
    const char* s = TCHAR_TO_UTF8(*f);
    return (s && s[0]) ? s : "";
}

static void Notify(AShooterPlayerController* pc, const std::wstring& msg,
    const std::wstring& color = L"1.0,0.2,0.2,1.0")
{
    if (!pc) return;
    const std::wstring rich =
        L"<RichColor Color=\"" + color + L"\">" + msg + L"</>";
    FString fSender(L"");
    FString fMsg(rich.c_str());
    AsaApi::GetApiUtils().SendChatMessage(pc, fSender, L"{}", std::wstring_view(*fMsg));
}

static bool GetStructureLocation(APrimalStructure* s, double& x, double& y, double& z)
{
    if (!s) return false;
    USceneComponent* root = s->RootComponentField();
    if (!root) return false;
    auto loc = root->RelativeLocationField();
    x = loc.X;
    y = loc.Y;
    z = loc.Z;
    return true;
}

static bool IsPoliced(APrimalStructure* s)
{
    if (!s) return false;
    if (s->bIsFoundation()()) return true;

    FString bp = AsaApi::GetApiUtils().GetBlueprint(s);
    std::string path = FStr(bp);
    std::transform(path.begin(), path.end(), path.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return path.find("wall") != std::string::npos
        || path.find("ceiling") != std::string::npos;
}

static int GetPlacerTeam(APlayerController* pc)
{
    AShooterPlayerController* spc = static_cast<AShooterPlayerController*>(pc);
    if (!spc) return 0;
    AActor* baseChar = spc->BaseGetPlayerCharacter();
    if (!baseChar) return 0;
    AShooterCharacter* character = static_cast<AShooterCharacter*>(baseChar);
    return character->TargetingTeamField();
}

static bool IsNearEnemyPoliced(APrimalStructure* structure, APlayerController* pc,
    const UE::Math::TVector<double>& loc)
{
    UPrimalGameData* gameData = AsaApi::GetApiUtils().GetGameData();
    if (!gameData) return false;

    double radiusUE = static_cast<double>(gameData->EnemyFoundationPreventionRadiusField());
    if (radiusUE <= 0.0) return false;

    int placerTeam = GetPlacerTeam(pc);

    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return false;

    TArray<AActor*> allStructures;
    UGameplayStatics::GetAllActorsOfClass(world,
        APrimalStructure::StaticClass(), &allStructures);

    for (int i = 0; i < allStructures.Num(); ++i)
    {
        APrimalStructure* cand = static_cast<APrimalStructure*>(allStructures[i]);
        if (!cand || cand == structure) continue;
        if (cand->TargetingTeamField() == placerTeam) continue;
        if (!IsPoliced(cand)) continue;

        double cx, cy, cz;
        if (!GetStructureLocation(cand, cx, cy, cz)) continue;

        double dx = loc.X - cx, dy = loc.Y - cy, dz = loc.Z - cz;
        if (std::sqrt(dx * dx + dy * dy + dz * dz) <= radiusUE) return true;
    }
    return false;
}

char Detour_FinalStructurePlacement(APrimalStructure* structure, APlayerController* pc,
    UE::Math::TVector<double>* atLocation, UE::Math::TRotator<double>* atRotation,
    UE::Math::TRotator<double>* playerViewRotation, APawn* attachToPawn,
    FName boneName, bool bFlipped, FPlacementData* placementData)
{
    if (structure && pc && atLocation && IsPoliced(structure))
    {
        if (IsNearEnemyPoliced(structure, pc, *atLocation))
        {
            Notify(static_cast<AShooterPlayerController*>(pc),
                L"You can't build this close to an enemy structure.");
            return 0;
        }
    }

    return Original_FinalStructurePlacement(structure, pc, atLocation, atRotation,
        playerViewRotation, attachToPawn, boneName, bFlipped, placementData);
}

static void InitImpl()
{
    try
    {
        AsaApi::GetHooks().SetHook(
            "APrimalStructure.FinalStructurePlacement(APlayerController*,UE::Math::TVector<double>,UE::Math::TRotator<double>,UE::Math::TRotator<double>,APawn*,FName,bool,FPlacementData&)",
            (LPVOID)&Detour_FinalStructurePlacement,
            (LPVOID*)&Original_FinalStructurePlacement
        );
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[FoundationParity] Hook registration failed: {}", ex.what());
    }

    Log::GetLog()->info("[FoundationParity] Plugin loaded");
}

static void UnloadImpl()
{
    AsaApi::GetHooks().DisableHook(
        "APrimalStructure.FinalStructurePlacement(APlayerController*,UE::Math::TVector<double>,UE::Math::TRotator<double>,UE::Math::TRotator<double>,APawn*,FName,bool,FPlacementData&)",
        (LPVOID)&Detour_FinalStructurePlacement
    );
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    Log::Get().Init("FoundationParity");
    try
    {
        InitImpl();
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[FoundationParity] Init failed: {}", ex.what());
    }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try
    {
        UnloadImpl();
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[FoundationParity] Unload failed: {}", ex.what());
    }
}