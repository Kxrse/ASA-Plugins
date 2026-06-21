/*
PrivateMessages - ASA Plugin

Author: Kxrse
Repository: https://github.com/Kxrse/ASA-Plugins

License: Kxrse ASA Plugins Non-Commercial License

You may use, modify, and redistribute this code with attribution.
Commercial use or resale is not permitted without explicit permission.
*/

/**
 * PrivateMessages - ASA Plugin
 *
 * Hook categories: Chat
 *
 * Chat commands:
 *   /pm {survivorname} {message}          - send a private message
 *   /pm {survivorname} {number} {message} - send PM when duplicate names exist
 *   /r {message}                          - reply to last PM conversation
 *
 * Config:
 *   ArkApi/Plugins/PrivateMessages/config.json
 *   MessageColor: RichColor RGBA string (default cyan)
 *   Prefix: display prefix for PM messages
 */

#include <API/ARK/Ark.h>
#include <json.hpp>
#include <fstream>
#include <string>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <cctype>
#include <sys/stat.h>

#pragma warning(disable: 4191)
#pragma comment(lib, "AsaApi")

static const std::string g_config_path = "ArkApi/Plugins/PrivateMessages/config.json";
static std::string g_message_color = "0.0,1.0,1.0,1.0";
static std::string g_prefix = "[PM]";
static std::mutex  g_config_mutex;

static time_t    g_config_last_modified = 0;
static uintmax_t g_config_last_size = 0;

static std::unordered_map<std::string, std::string> g_reply_map;
static std::mutex g_reply_mutex;

// =============================================================================
// Config
// =============================================================================

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
        Log::GetLog()->error("[PrivateMessages] Cannot open config: {}", g_config_path);
        return false;
    }

    try
    {
        nlohmann::json j;
        file >> j;

        std::lock_guard<std::mutex> lock(g_config_mutex);
        g_message_color = j.value("MessageColor", "0.0,1.0,1.0,1.0");
        g_prefix = j.value("Prefix", "[PM]");
    }
    catch (const std::exception& ex)
    {
        Log::GetLog()->error("[PrivateMessages] Config parse error: {}", ex.what());
        return false;
    }

    Log::GetLog()->info("[PrivateMessages] Config loaded");
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

static std::string ToLower(const std::string& s)
{
    std::string out = s;
    for (char& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

static std::string GetEosId(AShooterPlayerController* pc)
{
    if (!pc) return "";
    AShooterPlayerState* ps =
        static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
    if (!ps) return "";
    FString eos;
    ps->GetUniqueNetIdAsString(&eos);
    return FStr(eos);
}

static std::string GetSurvivorName(AShooterPlayerController* pc)
{
    if (!pc) return "";
    AActor* ch = pc->BaseGetPlayerCharacter();
    if (!ch) return "";
    AShooterCharacter* character = static_cast<AShooterCharacter*>(ch);
    return FStr(character->PlayerNameField());
}

static void SendPM(AShooterPlayerController* pc, const std::wstring& msg)
{
    std::wstring wColor;
    std::wstring wPrefix;
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        wColor.assign(g_message_color.begin(), g_message_color.end());
        wPrefix.assign(g_prefix.begin(), g_prefix.end());
    }

    const std::wstring wFull =
        L"<RichColor Color=\"" + wColor + L"\">" + wPrefix + L" " + msg + L"</>";

    FString fSender(L"");
    FString fMsg(wFull.c_str());
    AsaApi::GetApiUtils().SendChatMessage(pc, fSender, L"{}", std::wstring_view(*fMsg));
}

// =============================================================================
// Player Lookup
// =============================================================================

struct OnlinePlayer
{
    AShooterPlayerController* pc;
    std::string eosId;
    std::string survivorName;
    std::string tribeName;
};

static std::vector<OnlinePlayer> FindPlayersByName(const std::string& searchName)
{
    std::vector<OnlinePlayer> results;

    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return results;

    ULevel* level = world->PersistentLevelField();
    if (!level) return results;

    auto actors = level->ActorsField();
    const std::string searchLower = ToLower(searchName);

    for (int i = 0; i < actors.Num(); ++i)
    {
        AActor* actor = actors[i];
        if (!actor) continue;
        if (!actor->IsA(AShooterPlayerController::StaticClass())) continue;

        AShooterPlayerController* pc = static_cast<AShooterPlayerController*>(actor);

        AActor* ch = pc->BaseGetPlayerCharacter();
        if (!ch) continue;

        AShooterCharacter* character = static_cast<AShooterCharacter*>(ch);

        FString nameRaw = character->PlayerNameField();
        const std::string survivorName = FStr(nameRaw);
        if (survivorName.empty()) continue;

        if (ToLower(survivorName) != searchLower) continue;

        AShooterPlayerState* ps =
            static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());

        std::string tribeName;
        if (ps && ps->IsInTribe())
        {
            FString tribeRaw = character->TribeNameField();
            tribeName = FStr(tribeRaw);
        }

        OnlinePlayer op;
        op.pc = pc;
        op.eosId = GetEosId(pc);
        op.survivorName = survivorName;
        op.tribeName = tribeName;
        results.push_back(op);
    }

    return results;
}

static AShooterPlayerController* FindOnlineByEos(const std::string& eosId)
{
    UWorld* world = AsaApi::GetApiUtils().GetWorld();
    if (!world) return nullptr;

    ULevel* level = world->PersistentLevelField();
    if (!level) return nullptr;

    auto actors = level->ActorsField();

    for (int i = 0; i < actors.Num(); ++i)
    {
        AActor* actor = actors[i];
        if (!actor) continue;
        if (!actor->IsA(AShooterPlayerController::StaticClass())) continue;

        AShooterPlayerController* pc = static_cast<AShooterPlayerController*>(actor);
        if (GetEosId(pc) == eosId) return pc;
    }

    return nullptr;
}

// =============================================================================
// /pm Command
// =============================================================================

static void PmCommand(AShooterPlayerController* pc, FString* message, int, int)
{
    if (!pc || !message) return;

    const std::string raw = FStr(*message);
    const auto firstSpace = raw.find(' ');
    if (firstSpace == std::string::npos || firstSpace + 1 >= raw.size())
    {
        SendPM(pc, L"Usage: /pm <survivorname> <message>");
        return;
    }

    const std::string args = raw.substr(firstSpace + 1);
    const std::string senderEos = GetEosId(pc);
    if (senderEos.empty()) return;

    const auto nameEnd = args.find(' ');
    if (nameEnd == std::string::npos || nameEnd + 1 >= args.size())
    {
        SendPM(pc, L"Usage: /pm <survivorname> <message>");
        return;
    }

    const std::string nameToken = args.substr(0, nameEnd);
    const std::string afterName = args.substr(nameEnd + 1);

    auto matches = FindPlayersByName(nameToken);

    matches.erase(
        std::remove_if(matches.begin(), matches.end(),
            [&senderEos](const OnlinePlayer& p) { return p.eosId == senderEos; }),
        matches.end());

    if (matches.empty())
    {
        std::wstring wName(nameToken.begin(), nameToken.end());
        SendPM(pc, wName + L" is not online.");
        return;
    }

    const OnlinePlayer* target = nullptr;
    std::string messageBody;

    if (matches.size() == 1)
    {
        target = &matches[0];
        messageBody = afterName;
    }
    else
    {
        bool secondIsNumber = false;
        int selection = 0;
        std::string secondToken;
        std::string restAfterSecond;

        const auto secondSpace = afterName.find(' ');
        if (secondSpace != std::string::npos)
        {
            secondToken = afterName.substr(0, secondSpace);
            restAfterSecond = afterName.substr(secondSpace + 1);

            secondIsNumber = !secondToken.empty();
            for (char c : secondToken)
            {
                if (!std::isdigit((unsigned char)c)) { secondIsNumber = false; break; }
            }

            if (secondIsNumber)
                selection = std::atoi(secondToken.c_str());
        }

        if (secondIsNumber && selection >= 1 && selection <= (int)matches.size() && !restAfterSecond.empty())
        {
            target = &matches[selection - 1];
            messageBody = restAfterSecond;
        }
        else
        {
            std::wstring wName(nameToken.begin(), nameToken.end());
            SendPM(pc, L"Multiple " + wName + L" found:");

            for (size_t i = 0; i < matches.size(); ++i)
            {
                const auto& m = matches[i];
                std::wstring wN(m.survivorName.begin(), m.survivorName.end());
                std::wstring wTribe = m.tribeName.empty() ?
                    L"no tribe" :
                    std::wstring(m.tribeName.begin(), m.tribeName.end());

                std::wstring line = std::to_wstring(i + 1) + L" - " + wN +
                    L" (" + wTribe + L")";
                SendPM(pc, line);
            }
            return;
        }
    }

    if (!target || messageBody.empty()) return;

    const std::string senderName = GetSurvivorName(pc);
    const std::string targetName = target->survivorName;

    std::wstring wSender(senderName.begin(), senderName.end());
    std::wstring wTarget(targetName.begin(), targetName.end());
    std::wstring wBody(messageBody.begin(), messageBody.end());

    SendPM(pc, wTarget + L": " + wBody);
    SendPM(target->pc, wSender + L": " + wBody);

    {
        std::lock_guard<std::mutex> lock(g_reply_mutex);
        g_reply_map[senderEos] = target->eosId;
        g_reply_map[target->eosId] = senderEos;
    }

    Log::GetLog()->info("[PrivateMessages] {} -> {} : {}",
        senderEos, target->eosId, messageBody);
}

// =============================================================================
// /r Command
// =============================================================================

static void ReplyCommand(AShooterPlayerController* pc, FString* message, int, int)
{
    if (!pc || !message) return;

    const std::string raw = FStr(*message);
    const auto firstSpace = raw.find(' ');
    if (firstSpace == std::string::npos || firstSpace + 1 >= raw.size())
    {
        SendPM(pc, L"Usage: /r <message>");
        return;
    }

    const std::string messageBody = raw.substr(firstSpace + 1);
    const std::string senderEos = GetEosId(pc);
    if (senderEos.empty()) return;

    std::string targetEos;
    {
        std::lock_guard<std::mutex> lock(g_reply_mutex);
        auto it = g_reply_map.find(senderEos);
        if (it == g_reply_map.end())
        {
            SendPM(pc, L"No one to reply to.");
            return;
        }
        targetEos = it->second;
    }

    AShooterPlayerController* targetPc = FindOnlineByEos(targetEos);
    if (!targetPc)
    {
        SendPM(pc, L"That player is no longer online.");
        return;
    }

    const std::string senderName = GetSurvivorName(pc);
    const std::string targetName = GetSurvivorName(targetPc);

    std::wstring wSender(senderName.begin(), senderName.end());
    std::wstring wTarget(targetName.begin(), targetName.end());
    std::wstring wBody(messageBody.begin(), messageBody.end());

    SendPM(pc, wTarget + L": " + wBody);
    SendPM(targetPc, wSender + L": " + wBody);

    {
        std::lock_guard<std::mutex> lock(g_reply_mutex);
        g_reply_map[senderEos] = targetEos;
        g_reply_map[targetEos] = senderEos;
    }

    Log::GetLog()->info("[PrivateMessages] {} -> {} (reply): {}",
        senderEos, targetEos, messageBody);
}

// =============================================================================
// Tick — Config Hot-Reload
// =============================================================================

static float g_config_check_accumulator = 0.0f;

static void OnTick(float delta)
{
    g_config_check_accumulator += delta;
    if (g_config_check_accumulator < 10.0f) return;
    g_config_check_accumulator = 0.0f;

    time_t mtime = 0;
    uintmax_t fsize = 0;
    if (GetFileInfo(g_config_path, mtime, fsize) && fsize > 0 &&
        (mtime != g_config_last_modified || fsize != g_config_last_size))
    {
        if (LoadConfig())
        {
            g_config_last_modified = mtime;
            g_config_last_size = fsize;
        }
    }
}

// =============================================================================
// Plugin Entry Points
// =============================================================================

static void Plugin_Init_Impl()
{
    Log::Get().Init("PrivateMessages");

    LoadConfig();
    GetFileInfo(g_config_path, g_config_last_modified, g_config_last_size);

    AsaApi::GetCommands().AddChatCommand(FString(L"/pm"), &PmCommand);
    AsaApi::GetCommands().AddChatCommand(FString(L"/r"), &ReplyCommand);
    AsaApi::GetCommands().AddOnTickCallback(FString(L"PrivateMessages_Tick"), &OnTick);

    Log::GetLog()->info("[PrivateMessages] Plugin loaded");
}

static void Plugin_Unload_Impl()
{
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/pm"));
    AsaApi::GetCommands().RemoveChatCommand(FString(L"/r"));
    AsaApi::GetCommands().RemoveOnTickCallback(FString(L"PrivateMessages_Tick"));

    {
        std::lock_guard<std::mutex> lock(g_reply_mutex);
        g_reply_map.clear();
    }

    Log::GetLog()->info("[PrivateMessages] Plugin unloaded");
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
    try { Plugin_Init_Impl(); }
    catch (const std::exception& ex) { Log::GetLog()->critical("[PrivateMessages] Init exception: {}", ex.what()); }
    catch (...) { Log::GetLog()->critical("[PrivateMessages] Init unknown exception"); }
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    try { Plugin_Unload_Impl(); }
    catch (...) {}
}