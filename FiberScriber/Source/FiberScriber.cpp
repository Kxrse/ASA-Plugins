/*
FiberScriber - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * FiberScriber - ASA Plugin
 * Generator tool. On command, enumerates every craftable item on the server and writes
 * ConfigOverrideItemCraftingCosts lines into Game.ini so all crafting costs become fiber.
 * Items already overridden in Game.ini are skipped. Overrides load on next server boot.
 *
 * Hooks:
 *   None - console command only.
 *
 * Config (ArkApi/Plugins/FiberScriber/config.json):
 *   FiberResourceClass - resource class used as the crafting cost, e.g. PrimalItemResource_Fibers_C
 *   FiberQuantity      - amount of the fiber resource per craft
 *   GameIniPath        - path to the Game.ini to read and write
 *
 * Command:
 *   GenFiberCosts - server console command that reads config, enumerates items, writes Game.ini
 *
 * Item sources:
 *   UPrimalGameData.MasterItemList
 *   UPrimalGameData.EngramBlueprintClasses           - via each engram BluePrintEntry
 *   UPrimalGameData.AdditionalEngramBlueprintClasses - via each engram BluePrintEntry
 */

#include <API/ARK/Ark.h>
#include <json.hpp>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_set>

#pragma comment(lib, "AsaApi.lib")
#pragma warning(disable: 4191)

static std::string g_fiber_class;
static int g_fiber_qty = 1;
static std::string g_gameini_path;

static const std::string kSection = "[/script/shootergame.shootergamemode]";
static const std::string kOverridePrefix = "ConfigOverrideItemCraftingCosts=";
static const std::string kItemClassTag = "ItemClassString=\"";

static std::string FStr(const FString& f)
{
    const char* s = TCHAR_TO_UTF8(*f);
    return (s && s[0]) ? s : "";
}

static bool LoadConfig()
{
    const std::string path = "ArkApi/Plugins/FiberScriber/config.json";
    std::ifstream file(path);
    if (!file.is_open())
        return false;

    nlohmann::json j;
    try { file >> j; }
    catch (...) { return false; }

    g_fiber_class = j.value("FiberResourceClass", std::string());
    g_fiber_qty = j.value("FiberQuantity", 1);
    g_gameini_path = j.value("GameIniPath", std::string());
    return true;
}

static std::string ClassName(UClass* cls)
{
    if (!cls)
        return "";
    return FStr(UVictoryCore::GetClassFName(cls).ToString());
}

static std::string BuildLine(const std::string& itemClass)
{
    return kOverridePrefix + "(ItemClassString=\"" + itemClass +
        "\",BaseCraftingResourceRequirements=((ResourceItemTypeString=\"" + g_fiber_class +
        "\",BaseResourceRequirement=" + std::to_string(g_fiber_qty) +
        ".0,bCraftingRequireExactResourceType=false)))";
}

static std::string TrimLeft(const std::string& s)
{
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
        ++i;
    return s.substr(i);
}

static bool ExtractItemClass(const std::string& line, std::string& out)
{
    const size_t tag = line.find(kItemClassTag);
    if (tag == std::string::npos)
        return false;
    const size_t start = tag + kItemClassTag.size();
    const size_t end = line.find('"', start);
    if (end == std::string::npos)
        return false;
    out = line.substr(start, end - start);
    return true;
}

static void CollectMasterItems(UPrimalGameData* gd, std::unordered_set<std::string>& out)
{
    TArray<TSubclassOf<UPrimalItem>, TSizedDefaultAllocator<32>>& masterList =
        *GetNativePointerField<TArray<TSubclassOf<UPrimalItem>, TSizedDefaultAllocator<32>>*>(gd, "UPrimalGameData.MasterItemList");

    for (int i = 0; i < masterList.Num(); ++i)
    {
        const std::string name = ClassName(masterList[i].uClass);
        if (!name.empty())
            out.insert(name);
    }
}

static void CollectEngramItems(UPrimalGameData* gd, const char* field, std::unordered_set<std::string>& out)
{
    TArray<TSubclassOf<UPrimalEngramEntry>, TSizedDefaultAllocator<32>>& engrams =
        *GetNativePointerField<TArray<TSubclassOf<UPrimalEngramEntry>, TSizedDefaultAllocator<32>>*>(gd, field);

    for (int i = 0; i < engrams.Num(); ++i)
    {
        UClass* engramCls = engrams[i].uClass;
        if (!engramCls)
            continue;

        UObject* cdo = UVictoryCore::PureClassDefaultObject(engramCls);
        if (!cdo)
            continue;

        UPrimalEngramEntry* entry = static_cast<UPrimalEngramEntry*>(cdo);
        const std::string name = ClassName(entry->BluePrintEntryField().uClass);
        if (!name.empty())
            out.insert(name);
    }
}

static void Cmd_GenFiberCosts(APlayerController* pc, FString* cmd, bool boolean)
{
    if (!LoadConfig())
    {
        Log::GetLog()->error("[FiberScriber] Failed to read config.json");
        return;
    }

    if (g_fiber_class.empty())
    {
        Log::GetLog()->error("[FiberScriber] FiberResourceClass is empty");
        return;
    }

    if (g_gameini_path.empty())
    {
        Log::GetLog()->error("[FiberScriber] GameIniPath is empty");
        return;
    }

    std::vector<std::string> lines;
    std::unordered_set<std::string> existing;
    {
        std::ifstream in(g_gameini_path);
        if (!in.is_open())
        {
            Log::GetLog()->error("[FiberScriber] Could not open Game.ini at '{}'", g_gameini_path);
            return;
        }
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            lines.push_back(line);

            const std::string trimmed = TrimLeft(line);
            if (trimmed.rfind(kOverridePrefix, 0) == 0)
            {
                std::string itemClass;
                if (ExtractItemClass(trimmed, itemClass))
                    existing.insert(itemClass);
            }
        }
    }

    UPrimalGameData* gd = AsaApi::GetApiUtils().GetGameData();
    if (!gd)
    {
        Log::GetLog()->error("[FiberScriber] GetGameData returned null");
        return;
    }

    std::unordered_set<std::string> allItems;
    CollectMasterItems(gd, allItems);
    CollectEngramItems(gd, "UPrimalGameData.EngramBlueprintClasses", allItems);
    CollectEngramItems(gd, "UPrimalGameData.AdditionalEngramBlueprintClasses", allItems);

    std::vector<std::string> newLines;
    for (const std::string& name : allItems)
    {
        if (existing.count(name))
            continue;
        newLines.push_back(BuildLine(name));
    }

    if (newLines.empty())
    {
        Log::GetLog()->info("[FiberScriber] Nothing to add. Unique items {}, already present {}", (int)allItems.size(), (int)existing.size());
        return;
    }

    int sectionIdx = -1;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (TrimLeft(lines[i]) == kSection)
        {
            sectionIdx = (int)i;
            break;
        }
    }

    std::vector<std::string> out;
    if (sectionIdx < 0)
    {
        out = lines;
        if (!out.empty() && !out.back().empty())
            out.push_back("");
        out.push_back(kSection);
        for (const std::string& nl : newLines)
            out.push_back(nl);
    }
    else
    {
        for (size_t i = 0; i <= (size_t)sectionIdx; ++i)
            out.push_back(lines[i]);
        for (const std::string& nl : newLines)
            out.push_back(nl);
        for (size_t i = sectionIdx + 1; i < lines.size(); ++i)
            out.push_back(lines[i]);
    }

    std::ofstream ofs(g_gameini_path, std::ios::trunc);
    if (!ofs.is_open())
    {
        Log::GetLog()->error("[FiberScriber] Could not write Game.ini at '{}'", g_gameini_path);
        return;
    }
    for (size_t i = 0; i < out.size(); ++i)
    {
        ofs << out[i];
        if (i + 1 < out.size())
            ofs << "\n";
    }

    Log::GetLog()->info("[FiberScriber] Done. Unique items {}, skipped {}, added {}", (int)allItems.size(), (int)existing.size(), (int)newLines.size());
}

static void PluginInit()
{
    Log::Get().Init("FiberScriber");
    AsaApi::GetCommands().AddConsoleCommand(FString(L"GenFiberCosts"), &Cmd_GenFiberCosts);
    Log::GetLog()->info("[FiberScriber] Loaded");
}

static void PluginUnload()
{
    AsaApi::GetCommands().RemoveConsoleCommand(FString(L"GenFiberCosts"));
    Log::GetLog()->info("[FiberScriber] Unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { PluginInit(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->critical("[FiberScriber] Init exception: {}", e.what());
    }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { PluginUnload(); }
    catch (const std::exception& e)
    {
        Log::GetLog()->error("[FiberScriber] Unload exception: {}", e.what());
    }
}