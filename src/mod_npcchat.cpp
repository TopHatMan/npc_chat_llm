/*
 * mod-npc-chat-llm -- talk to any targeted world NPC and get an AI reply in a
 * public NPC chat bubble.
 *
 * This version uses two history layers:
 *   1) shared NPC memory: everyone who has talked to this NPC entry
 *   2) personal memory: one-on-one memory between this player and this NPC entry
 *
 * Threading contract:
 *   PlayerScript hook        [main thread]  capture primitives, spawn worker
 *   worker                   [off thread]   file IO + LLM call, queue reply
 *   WorldScript::OnUpdate    [main thread]  find live objects, emit NPC Say
 *
 * Game object pointers are never touched off the main thread. The worker only
 * receives copied strings and raw GUID values.
 */

#include "ScriptMgr.h"
#include "Player.h"
#include "Creature.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Config.h"
#include "SharedDefines.h"   // CHAT_MSG_SAY, LANG_UNIVERSAL, CreatureType, ranks
#include "UnitDefines.h"     // UNIT_NPC_FLAG_*
#include "DBCStores.h"       // sAreaTableStore
#include "DBCStructure.h"    // AreaTableEntry
#include "WorldSession.h"    // session / bot check on playerbots builds
#include "Chat.h"            // CommandScript / ChatHandler

#include "npcchat_llm.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

 // ===========================================================================
 // Config
 // ===========================================================================
namespace
{
    bool        g_Enable = false;
    std::string g_BaseUrl = "https://openrouter.ai/api/v1";
    std::string g_ApiKey;
    std::string g_Model;
    int         g_MaxTokens = 180;
    double      g_Temperature = 0.85;
    int         g_TimeoutSec = 30;
    std::string g_ExtraParams;

    int         g_GeneratePromptMaxTokens = 700;
    double      g_GeneratePromptTemperature = 0.75;

    std::string g_HistoryPath = "./AI_RP/npc_history";
    int         g_HistoryTail = 20;          // legacy/default tail if new split values are absent
    int         g_SharedHistoryTail = 12;    // shared NPC memory lines fed to model
    int         g_PersonalHistoryTail = 20;  // private player+NPC memory lines fed to model
    bool        g_NameByEntry = true;

    float       g_TriggerRange = 25.0f;          // friendly / neutral NPC max chat range
    bool        g_AllowHostileChat = true;       // hostile NPC parley / combat talk mode
    bool        g_HostileAllowCloseChat = true;  // allow hostile NPC chat even inside normal aggro range
    bool        g_HostileAllowCombatChat = true; // allow hostile NPC chat while player/NPC are in combat
    float       g_HostileMinDistance = 30.0f;    // used only when close hostile chat is disabled
    float       g_HostileMaxDistance = 100.0f;   // still close enough to "shout"
    bool        g_HostileForcePrivateReply = true;

    bool        g_RequirePrefix = false;
    std::string g_Prefix;

    // Comma-separated account IDs allowed to create/edit global sub-prompt files
    // without requiring GM security. Example: NpcChat.SubPromptCreatorAccounts = 1,7,42
    std::vector<uint32> g_SubPromptCreatorAccounts;

    std::vector<uint32> ParseAccountIdList(std::string const& text)
    {
        std::vector<uint32> out;
        std::stringstream ss(text);
        std::string token;

        while (std::getline(ss, token, ','))
        {
            token.erase(token.begin(), std::find_if(token.begin(), token.end(), [](unsigned char c)
                {
                    return !std::isspace(c);
                }));
            token.erase(std::find_if(token.rbegin(), token.rend(), [](unsigned char c)
                {
                    return !std::isspace(c);
                }).base(), token.end());

            if (token.empty())
                continue;

            try
            {
                uint32 id = static_cast<uint32>(std::stoul(token));
                if (id && std::find(out.begin(), out.end(), id) == out.end())
                    out.push_back(id);
            }
            catch (std::exception const&)
            {
                // Ignore bad tokens so one typo does not disable the module.
            }
        }

        return out;
    }

    void LoadConfig()
    {
        g_Enable = sConfigMgr->GetOption<bool>("NpcChat.Enable", false);
        g_BaseUrl = sConfigMgr->GetOption<std::string>("NpcChat.BaseUrl", "https://openrouter.ai/api/v1");
        g_ApiKey = sConfigMgr->GetOption<std::string>("NpcChat.ApiKey", "");
        g_Model = sConfigMgr->GetOption<std::string>("NpcChat.Model", "");
        g_MaxTokens = sConfigMgr->GetOption<int32>("NpcChat.MaxResponseTokens", 180);
        g_Temperature = sConfigMgr->GetOption<float>("NpcChat.Temperature", 0.85f);
        g_TimeoutSec = sConfigMgr->GetOption<int32>("NpcChat.RequestTimeoutSec", 30);
        g_ExtraParams = sConfigMgr->GetOption<std::string>("NpcChat.ModelExtraParameters", "");
        g_GeneratePromptMaxTokens = sConfigMgr->GetOption<int32>("NpcChat.GeneratePromptMaxTokens", 700);
        g_GeneratePromptTemperature = sConfigMgr->GetOption<float>("NpcChat.GeneratePromptTemperature", 0.75f);

        g_HistoryPath = sConfigMgr->GetOption<std::string>("NpcChat.HistoryPath", "./AI_RP/npc_history");
        g_HistoryTail = sConfigMgr->GetOption<int32>("NpcChat.HistoryMaxLines", 20);
        g_SharedHistoryTail = sConfigMgr->GetOption<int32>("NpcChat.SharedHistoryMaxLines", std::max(6, g_HistoryTail / 2));
        g_PersonalHistoryTail = sConfigMgr->GetOption<int32>("NpcChat.PersonalHistoryMaxLines", g_HistoryTail);
        g_NameByEntry = sConfigMgr->GetOption<bool>("NpcChat.NameByEntry", true);

        g_TriggerRange = sConfigMgr->GetOption<float>("NpcChat.TriggerRange", 25.0f);
        g_AllowHostileChat = sConfigMgr->GetOption<bool>("NpcChat.AllowHostileChat", true);
        g_HostileAllowCloseChat = sConfigMgr->GetOption<bool>("NpcChat.HostileAllowCloseChat", true);
        g_HostileAllowCombatChat = sConfigMgr->GetOption<bool>("NpcChat.HostileAllowCombatChat", true);
        g_HostileMinDistance = sConfigMgr->GetOption<float>("NpcChat.HostileMinDistance", 30.0f);
        g_HostileMaxDistance = sConfigMgr->GetOption<float>("NpcChat.HostileMaxDistance", 100.0f);
        g_HostileForcePrivateReply = sConfigMgr->GetOption<bool>("NpcChat.HostileForcePrivateReply", true);

        g_RequirePrefix = sConfigMgr->GetOption<bool>("NpcChat.RequirePrefix", false);
        g_Prefix = sConfigMgr->GetOption<std::string>("NpcChat.Prefix", "");

        g_SubPromptCreatorAccounts = ParseAccountIdList(
            sConfigMgr->GetOption<std::string>("NpcChat.SubPromptCreatorAccounts", ""));

        // Accept a couple of aliases too, so a typo/name preference in the conf
        // does not leave a trusted play account locked out.
        auto mergeAccountIds = [](std::vector<uint32>& dst, std::vector<uint32> const& src)
            {
                for (uint32 id : src)
                {
                    if (id && std::find(dst.begin(), dst.end(), id) == dst.end())
                        dst.push_back(id);
                }
            };

        mergeAccountIds(g_SubPromptCreatorAccounts, ParseAccountIdList(
            sConfigMgr->GetOption<std::string>("NpcChat.SubPromptCreatorAccountIds", "")));
        mergeAccountIds(g_SubPromptCreatorAccounts, ParseAccountIdList(
            sConfigMgr->GetOption<std::string>("NpcChat.SubPromptCreatorAccountIDs", "")));
    }
}

// ===========================================================================
// Cross-thread message structs
// ===========================================================================
namespace
{
    struct ChatRequest
    {
        uint64_t    playerGuidRaw = 0;
        uint64_t    npcGuidRaw = 0;
        uint32_t    npcEntry = 0;

        std::string playerName;
        std::string npcName;
        std::string npcSubName;

        uint32_t    npcLevel = 0;
        std::string gender;
        std::string creatureType;
        std::string rankStr;
        std::string roleStr;
        std::string stance;
        std::string zoneName;

        bool        isHostile = false;
        bool        npcInCombat = false;
        bool        playerInCombat = false;
        bool        npcTargetingPlayer = false;
        float       npcHealthPct = 100.0f;
        float       playerHealthPct = 100.0f;
        float       distance = 0.0f;
        std::string fightState;
        bool        forcePrivateReply = false;

        std::string message;
    };

    struct ChatReply
    {
        uint64_t    playerGuidRaw = 0;
        uint64_t    npcGuidRaw = 0;
        std::string text;
        bool        forcePrivateReply = false;
    };

    struct SystemMessage
    {
        uint64_t    playerGuidRaw = 0;
        std::string text;
    };

    struct GenPromptRequest
    {
        uint64_t    playerGuidRaw = 0;
        uint64_t    npcGuidRaw = 0;
        uint32_t    npcEntry = 0;

        std::string playerName;
        std::string npcName;
        std::string npcSubName;
        uint32_t    npcLevel = 0;
        std::string gender;
        std::string creatureType;
        std::string rankStr;
        std::string roleStr;
        std::string stance;
        std::string zoneName;
        bool        isHostile = false;
        bool        npcInCombat = false;
        bool        playerInCombat = false;
        bool        npcTargetingPlayer = false;
        float       npcHealthPct = 100.0f;
        float       playerHealthPct = 100.0f;
        float       distance = 0.0f;
        std::string fightState;

        std::string mode;             // shared, personal, preview
        std::string extraInstruction;
        std::string outputPath;
    };

    std::queue<ChatReply> g_ReplyQueue;
    std::mutex            g_ReplyMutex;
    std::queue<SystemMessage> g_SystemMessageQueue;
    std::mutex            g_SystemMessageMutex;

    // One global lock for all NPC history file IO. Good enough for a small
    // personal realm; prevents two workers from interleaving the same file.
    std::mutex            g_FileMutex;
}

// ===========================================================================
// Text / file helpers
// ===========================================================================
namespace
{
    std::string TrimCopy(std::string s)
    {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos)
            return "";
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    std::string ToLowerCopy(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c)
            {
                return static_cast<char>(std::tolower(c));
            });
        return s;
    }

    float UnitHealthPct(Unit const* unit)
    {
        if (!unit)
            return 0.0f;

        uint64 maxHealth = unit->GetMaxHealth();
        if (!maxHealth)
            return 0.0f;

        return (float(unit->GetHealth()) * 100.0f) / float(maxHealth);
    }

    std::string BuildFightState(float npcHp, float playerHp, bool npcInCombat, bool playerInCombat, bool npcTargetingPlayer)
    {
        if (!npcInCombat && !playerInCombat)
            return "No active fight has started yet.";

        std::ostringstream ss;
        if (npcTargetingPlayer)
            ss << "The NPC is actively fighting the speaker. ";
        else if (npcInCombat || playerInCombat)
            ss << "Combat is nearby or already underway. ";

        if (npcHp <= 20.0f)
            ss << "The NPC is badly wounded. ";
        else if (npcHp <= 45.0f)
            ss << "The NPC is losing ground. ";
        else if (npcHp >= 85.0f)
            ss << "The NPC still looks strong. ";

        if (playerHp <= 20.0f)
            ss << "The speaker is badly hurt. ";
        else if (playerHp <= 45.0f)
            ss << "The speaker is under real pressure. ";
        else if (playerHp >= 85.0f)
            ss << "The speaker still looks strong. ";

        if (npcHp + 20.0f < playerHp)
            ss << "Overall, the speaker appears to be winning.";
        else if (playerHp + 20.0f < npcHp)
            ss << "Overall, the NPC appears to be winning.";
        else
            ss << "Overall, the fight looks close.";

        return TrimCopy(ss.str());
    }

    std::string StripWrappingQuotes(std::string s)
    {
        s = TrimCopy(s);
        if (s.size() >= 2)
        {
            char first = s.front();
            char last = s.back();
            if ((first == '"' && last == '"') || (first == '\'' && last == '\''))
                s = s.substr(1, s.size() - 2);
        }
        return TrimCopy(s);
    }

    bool StartsWithWord(std::string const& text, std::string const& word, std::string& rest)
    {
        std::string low = ToLowerCopy(TrimCopy(text));
        if (low == word)
        {
            rest.clear();
            return true;
        }

        if (low.rfind(word + " ", 0) == 0)
        {
            rest = TrimCopy(text.substr(word.size()));
            return true;
        }

        return false;
    }

    std::string SanitizeName(std::string const& in)
    {
        std::string out;
        out.reserve(in.size());

        for (unsigned char c : in)
            out += (std::isalnum(c) ? static_cast<char>(c) : '_');

        if (out.empty())
            out = "unknown";

        return out;
    }

    std::string NpcHistoryBase(std::string const& npcName, uint32_t entry)
    {
        std::string base = SanitizeName(npcName);
        if (g_NameByEntry)
            base += "_" + std::to_string(entry);
        return base;
    }

    std::string PlayerHistoryBase(std::string const& playerName, uint64_t playerGuidRaw)
    {
        return SanitizeName(playerName) + "_" + std::to_string(playerGuidRaw);
    }

    std::string DefaultPromptFilePath()
    {
        return g_HistoryPath + "/default.prompt";
    }

    std::string SubPromptRootPath()
    {
        return g_HistoryPath + "/subprompts";
    }

    std::string NormalizeSubPromptName(std::string name)
    {
        name = TrimCopy(name);
        std::string lower = ToLowerCopy(name);
        if (lower.size() > 7 && lower.substr(lower.size() - 7) == ".prompt")
            name = name.substr(0, name.size() - 7);

        std::string out;
        out.reserve(name.size());
        for (unsigned char c : name)
        {
            if (std::isalnum(c))
                out += static_cast<char>(std::tolower(c));
            else if (c == '_' || c == '-')
                out += static_cast<char>(c);
            else if (std::isspace(c))
                out += '_';
            else
                out += '_';
        }

        while (!out.empty() && out.front() == '_')
            out.erase(out.begin());
        while (!out.empty() && out.back() == '_')
            out.pop_back();
        return out;
    }

    std::string SubPromptFilePath(std::string const& name)
    {
        return SubPromptRootPath() + "/" + NormalizeSubPromptName(name) + ".prompt";
    }

    std::string SharedHistoryFilePath(std::string const& npcName, uint32_t entry)
    {
        return g_HistoryPath + "/shared/" + NpcHistoryBase(npcName, entry) + ".history";
    }

    std::string SharedPromptFilePath(std::string const& npcName, uint32_t entry)
    {
        return g_HistoryPath + "/shared/" + NpcHistoryBase(npcName, entry) + ".prompt";
    }

    std::string SharedSubPromptListFilePath(std::string const& npcName, uint32_t entry)
    {
        return g_HistoryPath + "/shared/" + NpcHistoryBase(npcName, entry) + ".subprompts";
    }

    std::string PersonalHistoryFolderPath(std::string const& playerName, uint64_t playerGuidRaw)
    {
        return g_HistoryPath + "/personal/" + PlayerHistoryBase(playerName, playerGuidRaw);
    }

    std::string PersonalHistoryFilePath(std::string const& playerName, uint64_t playerGuidRaw,
        std::string const& npcName, uint32_t entry)
    {
        return PersonalHistoryFolderPath(playerName, playerGuidRaw) + "/" +
            NpcHistoryBase(npcName, entry) + ".history";
    }

    std::string PersonalPromptFilePath(std::string const& playerName, uint64_t playerGuidRaw,
        std::string const& npcName, uint32_t entry)
    {
        return PersonalHistoryFolderPath(playerName, playerGuidRaw) + "/" +
            NpcHistoryBase(npcName, entry) + ".prompt";
    }

    std::string PersonalSubPromptListFilePath(std::string const& playerName, uint64_t playerGuidRaw,
        std::string const& npcName, uint32_t entry)
    {
        return PersonalHistoryFolderPath(playerName, playerGuidRaw) + "/" +
            NpcHistoryBase(npcName, entry) + ".subprompts";
    }

    std::string BuiltInDefaultPromptText()
    {
        return
            "Stay fully in character as this NPC in Azeroth.\n"
            "Speak naturally as a living person or creature in the world.\n"
            "Do not mention AI, prompts, files, scripts, players, servers, or game mechanics unless the NPC would naturally know.\n"
            "Use only your own spoken words: no narration, no asterisks, no emotes, no out-of-character text.\n"
            "Keep replies short, usually one to three sentences suitable for in-game NPC speech.\n"
            "React to what the player says instead of giving generic vendor lines.\n"
            "When appropriate, ask a small follow-up question, offer a rumor, give practical advice, tease, warn, bargain, or invite the player to return.\n"
            "Let familiarity grow over time if this player has spoken with you before. Acknowledge remembered details naturally, without saying the word memory.\n"
            "If the speaker is an enemy, you may be hostile, suspicious, threatening, mocking, or unwilling to answer instead of friendly.\n";
    }

    void EnsureNpcChatDirectoriesAndDefaultPrompt()
    {
        try
        {
            std::filesystem::create_directories(g_HistoryPath);
            std::filesystem::create_directories(g_HistoryPath + "/shared");
            std::filesystem::create_directories(g_HistoryPath + "/personal");
            std::filesystem::create_directories(SubPromptRootPath());

            std::filesystem::path defaultPrompt(DefaultPromptFilePath());
            if (!std::filesystem::exists(defaultPrompt))
            {
                std::ofstream f(defaultPrompt, std::ios::out | std::ios::trunc);
                if (f.is_open())
                    f << BuiltInDefaultPromptText();
            }
        }
        catch (std::exception const&)
        {
            // non-fatal; later file opens will simply fail if the path is bad
        }
    }

    std::string ReadWholeTextFile(std::string const& path)
    {
        std::ifstream f(path);
        if (!f.is_open())
            return "";

        std::ostringstream ss;
        ss << f.rdbuf();
        return TrimCopy(ss.str());
    }

    bool WriteWholeTextFile(std::string const& path, std::string const& text, bool overwrite)
    {
        try
        {
            std::filesystem::path p(path);
            std::filesystem::path parent = p.parent_path();
            if (!parent.empty())
                std::filesystem::create_directories(parent);

            if (!overwrite && std::filesystem::exists(p))
                return true;
        }
        catch (std::exception const&)
        {
            return false;
        }

        std::ofstream f(path, std::ios::out | std::ios::trunc);
        if (!f.is_open())
            return false;

        if (!text.empty())
            f << text << "\n";

        return true;
    }

    std::vector<std::string> LoadSubPromptNameList(std::string const& path)
    {
        std::vector<std::string> out;
        std::ifstream f(path);
        if (!f.is_open())
            return out;

        std::string line;
        while (std::getline(f, line))
        {
            line = TrimCopy(line);
            if (line.empty() || line[0] == '#')
                continue;
            line = NormalizeSubPromptName(line);
            if (!line.empty() && std::find(out.begin(), out.end(), line) == out.end())
                out.push_back(line);
        }
        return out;
    }

    bool WriteSubPromptNameList(std::string const& path, std::vector<std::string> names)
    {
        std::vector<std::string> clean;
        for (std::string const& n : names)
        {
            std::string key = NormalizeSubPromptName(n);
            if (!key.empty() && std::find(clean.begin(), clean.end(), key) == clean.end())
                clean.push_back(key);
        }

        std::sort(clean.begin(), clean.end());
        std::ostringstream ss;
        for (std::string const& n : clean)
            ss << n << "\n";
        return WriteWholeTextFile(path, ss.str(), true);
    }

    bool AddSubPromptName(std::string const& listPath, std::string const& name)
    {
        std::string key = NormalizeSubPromptName(name);
        if (key.empty())
            return false;

        std::vector<std::string> names = LoadSubPromptNameList(listPath);
        if (std::find(names.begin(), names.end(), key) == names.end())
            names.push_back(key);
        return WriteSubPromptNameList(listPath, names);
    }

    bool RemoveSubPromptName(std::string const& listPath, std::string const& name)
    {
        std::string key = NormalizeSubPromptName(name);
        if (key.empty())
            return false;

        std::vector<std::string> names = LoadSubPromptNameList(listPath);
        names.erase(std::remove(names.begin(), names.end(), key), names.end());
        return WriteSubPromptNameList(listPath, names);
    }

    std::string LoadSubPromptBlocks(std::vector<std::string> const& names)
    {
        std::ostringstream ss;
        for (std::string const& raw : names)
        {
            std::string key = NormalizeSubPromptName(raw);
            if (key.empty())
                continue;

            std::string text = ReadWholeTextFile(SubPromptFilePath(key));
            if (text.empty())
                continue;

            ss << "[" << key << "]\n" << text << "\n\n";
        }
        return TrimCopy(ss.str());
    }

    std::string JoinNames(std::vector<std::string> const& names)
    {
        if (names.empty())
            return "(none)";
        std::string out;
        for (size_t i = 0; i < names.size(); ++i)
        {
            if (i)
                out += ", ";
            out += names[i];
        }
        return out;
    }

    bool RemoveFileIfExists(std::string const& path)
    {
        try
        {
            std::filesystem::path p(path);
            if (!std::filesystem::exists(p))
                return false;
            return std::filesystem::remove(p);
        }
        catch (std::exception const&)
        {
            return false;
        }
    }

    std::size_t RemoveAllPersonalHistoryForNpc(std::string const& npcName, uint32_t entry)
    {
        std::size_t removed = 0;
        std::string wanted = NpcHistoryBase(npcName, entry) + ".history";

        try
        {
            std::filesystem::path root(g_HistoryPath + "/personal");
            if (!std::filesystem::exists(root))
                return 0;

            for (auto const& it : std::filesystem::recursive_directory_iterator(root))
            {
                if (!it.is_regular_file())
                    continue;

                if (it.path().filename().string() == wanted)
                {
                    if (std::filesystem::remove(it.path()))
                        ++removed;
                }
            }
        }
        catch (std::exception const&)
        {
            // best effort
        }

        return removed;
    }

    std::deque<std::string> LoadHistoryTail(std::string const& path, int tail)
    {
        std::deque<std::string> lines;
        if (tail <= 0)
            return lines;

        std::ifstream f(path);
        if (!f.is_open())
            return lines;

        std::string line;
        while (std::getline(f, line))
        {
            line = TrimCopy(line);
            if (line.empty())
                continue;

            lines.push_back(line);
            if (static_cast<int>(lines.size()) > tail)
                lines.pop_front();
        }

        return lines;
    }

    void AppendHistoryLine(std::string const& path, std::string const& line)
    {
        try
        {
            std::filesystem::path p(path);
            std::filesystem::path parent = p.parent_path();
            if (!parent.empty())
                std::filesystem::create_directories(parent);
        }
        catch (std::exception const&)
        {
            // non-fatal; opening the file below will simply fail if the path is bad
        }

        std::ofstream f(path, std::ios::app);
        if (f.is_open())
            f << line << "\n";
    }

    void QueueSystemMessage(uint64_t playerGuidRaw, std::string text)
    {
        SystemMessage msg;
        msg.playerGuidRaw = playerGuidRaw;
        msg.text = std::move(text);

        std::lock_guard<std::mutex> lock(g_SystemMessageMutex);
        g_SystemMessageQueue.push(std::move(msg));
    }
}

// ===========================================================================
// Creature attribute -> readable text
// ===========================================================================
namespace
{
    char const* GenderStr(uint8 g)
    {
        switch (g)
        {
        case 0: return "male";
        case 1: return "female";
        default: return "";
        }
    }

    char const* CreatureTypeStr(uint32 t)
    {
        switch (t)
        {
        case 1:  return "beast";
        case 2:  return "dragonkin";
        case 3:  return "demon";
        case 4:  return "elemental";
        case 5:  return "giant";
        case 6:  return "undead";
        case 7:  return "humanoid";
        case 8:  return "critter";
        case 9:  return "mechanical";
        case 11: return "totem";
        case 12: return "non-combat pet";
        case 13: return "gas cloud";
        default: return "";
        }
    }

    char const* RankStr(uint32 r)
    {
        switch (r)
        {
        case 1: return "an elite";
        case 2: return "a rare elite";
        case 3: return "a powerful world boss";
        case 4: return "a rare creature";
        default: return "";
        }
    }

    std::string RolesFromNpcFlags(uint32 f)
    {
        std::vector<std::string> roles;

        if (f & UNIT_NPC_FLAG_QUESTGIVER)   roles.push_back("a quest giver");
        if (f & UNIT_NPC_FLAG_VENDOR)       roles.push_back("a merchant");
        if (f & UNIT_NPC_FLAG_TRAINER)      roles.push_back("a trainer");
        if (f & UNIT_NPC_FLAG_INNKEEPER)    roles.push_back("an innkeeper");
        if (f & UNIT_NPC_FLAG_FLIGHTMASTER) roles.push_back("a flight master");
        if (f & UNIT_NPC_FLAG_BANKER)       roles.push_back("a banker");
        if (f & UNIT_NPC_FLAG_AUCTIONEER)   roles.push_back("an auctioneer");
        if (f & UNIT_NPC_FLAG_STABLEMASTER) roles.push_back("a stable master");
        if (f & UNIT_NPC_FLAG_BATTLEMASTER) roles.push_back("a battlemaster");
        if (f & UNIT_NPC_FLAG_REPAIR)       roles.push_back("an armorer");
        if (f & UNIT_NPC_FLAG_SPIRITHEALER) roles.push_back("a spirit healer");

        std::string out;
        for (size_t i = 0; i < roles.size(); ++i)
        {
            if (i == 0)
                out = roles[i];
            else if (i + 1 == roles.size())
                out += (roles.size() == 2 ? " and " : ", and ") + roles[i];
            else
                out += ", " + roles[i];
        }

        return out;
    }
}

// ===========================================================================
// Prompt assembly
// ===========================================================================
namespace
{
    std::string BuildSystemPrompt(ChatRequest const& req,
        std::string const& defaultPrompt,
        std::string const& sharedSubPrompts,
        std::string const& sharedPrompt,
        std::string const& personalSubPrompts,
        std::string const& personalPrompt)
    {
        std::ostringstream ss;

        ss << "You are " << req.npcName;
        if (!req.npcSubName.empty())
            ss << " <" << req.npcSubName << ">";
        ss << ", a living character in Azeroth, the world of World of Warcraft.";

        if (req.npcLevel > 0 || !req.gender.empty() || !req.creatureType.empty())
        {
            ss << " You are a";
            if (req.npcLevel > 0)
                ss << " level " << req.npcLevel;
            if (!req.gender.empty())
                ss << " " << req.gender;
            if (!req.creatureType.empty())
                ss << " " << req.creatureType;
            ss << ".";
        }

        if (!req.rankStr.empty())
            ss << " You are " << req.rankStr << ".";

        if (!req.roleStr.empty())
            ss << " You serve as " << req.roleStr << ".";

        if (!req.zoneName.empty())
            ss << " You are currently in " << req.zoneName << ".";

        if (!req.stance.empty())
            ss << " You regard the speaker as " << req.stance << ".";

        if (req.isHostile)
        {
            ss << " This is hostile enemy communication. It may be a distant parley, a close threat, or active fight banter.";
            ss << " Do not become friendly just because the enemy speaks to you.";
            ss << " If combat is underway, react like an enemy under pressure: taunt, threaten, bargain, panic, boast, rage, or refuse as fits your nature.";
            ss << " Do not mention exact health percentages unless the speaker directly asks about wounds or weakness.";
        }

        if (!defaultPrompt.empty())
        {
            ss << "\n\nGlobal NPC behavior rules:\n";
            ss << defaultPrompt;
        }

        if (!sharedSubPrompts.empty())
        {
            ss << "\n\nShared archetype/sub-prompts attached to this NPC:\n";
            ss << sharedSubPrompts;
        }

        if (!sharedPrompt.empty())
        {
            ss << "\n\nShared prompt for this NPC:\n";
            ss << sharedPrompt;
        }

        if (!personalSubPrompts.empty())
        {
            ss << "\n\nPersonal archetype/sub-prompts attached to this NPC for " << req.playerName << ":\n";
            ss << personalSubPrompts;
        }

        if (!personalPrompt.empty())
        {
            ss << "\n\nPersonal prompt for this NPC when speaking with " << req.playerName << ":\n";
            ss << personalPrompt;
        }

        return ss.str();
    }

    std::string BuildUserPrompt(ChatRequest const& req,
        std::deque<std::string> const& sharedHistory,
        std::deque<std::string> const& personalHistory)
    {
        std::ostringstream ss;

        if (!sharedHistory.empty())
        {
            ss << "Things you have recently heard or said with adventurers in general:\n";
            for (std::string const& l : sharedHistory)
                ss << l << "\n";
            ss << "\n";
        }

        if (!personalHistory.empty())
        {
            ss << "Your recent one-on-one history with " << req.playerName << ":\n";
            for (std::string const& l : personalHistory)
                ss << l << "\n";
            ss << "\n";
        }

        if (req.isHostile)
        {
            ss << "Hostile/combat context:\n";
            ss << "- Distance from speaker: " << req.distance << " yards.\n";
            ss << "- Your health: about " << req.npcHealthPct << "%.\n";
            ss << "- Speaker health: about " << req.playerHealthPct << "%.\n";
            ss << "- You are in combat: " << (req.npcInCombat ? "yes" : "no") << ".\n";
            ss << "- Speaker is in combat: " << (req.playerInCombat ? "yes" : "no") << ".\n";
            ss << "- You are directly targeting/fighting the speaker: " << (req.npcTargetingPlayer ? "yes" : "no") << ".\n";
            if (!req.fightState.empty())
                ss << "- Fight read: " << req.fightState << "\n";
            ss << "\n";
            ss << req.playerName << " says to you as an enemy: \"" << req.message << "\"\n\n";
        }
        else
            ss << req.playerName << " says to you: \"" << req.message << "\"\n\n";

        ss << "Reply as " << req.npcName << ". Engage naturally and leave room for the conversation to continue:";

        return ss.str();
    }
}

std::string BuildGenerateCharacterSystemPrompt(GenPromptRequest const& req)
{
    std::ostringstream ss;
    ss << "You are creating a reusable character prompt for a World of Warcraft non-playable character.";
    ss << " The prompt will be used by an NPC chat roleplay module as the shared or personal identity layer for that specific NPC.";
    ss << " Output ONLY the prompt text to save. Do not use markdown. Do not include headings. Do not include quotes around the answer.";
    ss << " Do not mention AI, LLMs, files, commands, players, prompts, or game mechanics.";
    ss << " Do not write sample dialogue. Write the NPC's identity, motives, worldview, speech style, loyalties, grudges, boundaries, and combat attitude.";
    ss << " Keep it lore-grounded, vivid, and concise enough to reuse during live chat.";

    if (req.mode == "personal")
        ss << " This is a personal relationship prompt for how this NPC specifically regards the named player; keep it compatible with the public identity.";
    else
        ss << " This is a shared public character prompt for everyone who talks to this NPC.";

    return ss.str();
}

std::string BuildGenerateCharacterUserPrompt(GenPromptRequest const& req,
    std::vector<std::string> const& sharedSubPromptNames,
    std::string const& sharedSubPrompts,
    std::string const& existingSharedPrompt,
    std::string const& existingPersonalPrompt)
{
    std::ostringstream ss;

    ss << "NPC facts:\n";
    ss << "- Name: " << req.npcName << "\n";
    ss << "- Entry ID: " << req.npcEntry << "\n";
    if (!req.npcSubName.empty())
        ss << "- Subname/title: " << req.npcSubName << "\n";
    if (req.npcLevel)
        ss << "- Level: " << req.npcLevel << "\n";
    if (!req.gender.empty())
        ss << "- Gender: " << req.gender << "\n";
    if (!req.creatureType.empty())
        ss << "- Creature type: " << req.creatureType << "\n";
    if (!req.rankStr.empty())
        ss << "- Rank: " << req.rankStr << "\n";
    if (!req.roleStr.empty())
        ss << "- NPC role flags: " << req.roleStr << "\n";
    if (!req.zoneName.empty())
        ss << "- Current zone: " << req.zoneName << "\n";
    if (!req.stance.empty())
        ss << "- Current reaction to player: " << req.stance << "\n";
    if (req.isHostile)
        ss << "- Hostile to the player: yes\n";
    if (req.npcInCombat || req.playerInCombat)
    {
        ss << "- Combat context: " << req.fightState << "\n";
        ss << "- NPC health now: about " << req.npcHealthPct << "%\n";
        ss << "- Speaker health now: about " << req.playerHealthPct << "%\n";
    }

    if (!sharedSubPromptNames.empty())
        ss << "\nAttached shared subprompt names: " << JoinNames(sharedSubPromptNames) << "\n";

    if (!sharedSubPrompts.empty())
    {
        ss << "\nAttached shared subprompt contents for context:\n";
        ss << sharedSubPrompts << "\n";
    }

    if (!existingSharedPrompt.empty())
    {
        ss << "\nExisting shared prompt, if improving/replacing it:\n";
        ss << existingSharedPrompt << "\n";
    }

    if (req.mode == "personal" && !existingPersonalPrompt.empty())
    {
        ss << "\nExisting personal prompt, if improving/replacing it:\n";
        ss << existingPersonalPrompt << "\n";
    }

    if (!req.extraInstruction.empty())
    {
        ss << "\nExtra direction from the player:\n";
        ss << req.extraInstruction << "\n";
    }

    ss << "\nWrite the final " << (req.mode == "personal" ? "personal relationship" : "shared character") << " prompt now.";
    ss << " Make it specific to " << req.npcName << ", not a generic archetype.";
    ss << " Aim for 180-450 words unless the extra direction requires less.";

    return ss.str();
}

void GeneratePromptWorker(GenPromptRequest req)
{
    std::vector<std::string> sharedSubPromptNames;
    std::string sharedSubPrompts;
    std::string existingSharedPrompt;
    std::string existingPersonalPrompt;

    {
        std::lock_guard<std::mutex> lock(g_FileMutex);
        EnsureNpcChatDirectoriesAndDefaultPrompt();

        sharedSubPromptNames = LoadSubPromptNameList(SharedSubPromptListFilePath(req.npcName, req.npcEntry));
        sharedSubPrompts = LoadSubPromptBlocks(sharedSubPromptNames);
        existingSharedPrompt = ReadWholeTextFile(SharedPromptFilePath(req.npcName, req.npcEntry));
        existingPersonalPrompt = ReadWholeTextFile(req.outputPath);
    }

    NpcChat_ApiConfig cfg;
    cfg.baseUrl = g_BaseUrl;
    cfg.apiKey = g_ApiKey;
    cfg.model = g_Model;
    cfg.maxTokens = g_GeneratePromptMaxTokens;
    cfg.temperature = g_GeneratePromptTemperature;
    cfg.timeoutSec = g_TimeoutSec;
    cfg.extraParams = g_ExtraParams;

    NpcChat_LLMResult res = NpcChat_CallLLM(
        cfg,
        BuildGenerateCharacterSystemPrompt(req),
        BuildGenerateCharacterUserPrompt(req, sharedSubPromptNames, sharedSubPrompts, existingSharedPrompt, existingPersonalPrompt));

    if (!res.success || TrimCopy(res.text).empty())
    {
        QueueSystemMessage(req.playerGuidRaw, "NPC Chat prompt generation failed or returned empty text.");
        return;
    }

    std::string generated = TrimCopy(res.text);

    if (generated.rfind("```", 0) == 0)
    {
        size_t firstNl = generated.find('\n');
        size_t lastFence = generated.rfind("```");
        if (firstNl != std::string::npos && lastFence != std::string::npos && lastFence > firstNl)
            generated = TrimCopy(generated.substr(firstNl + 1, lastFence - firstNl - 1));
    }

    std::string savedPath = req.outputPath;
    if (req.mode == "preview")
        savedPath = SharedPromptFilePath(req.npcName, req.npcEntry) + ".preview";

    {
        std::lock_guard<std::mutex> lock(g_FileMutex);
        EnsureNpcChatDirectoriesAndDefaultPrompt();
        if (!WriteWholeTextFile(savedPath, generated, true))
        {
            QueueSystemMessage(req.playerGuidRaw, "NPC Chat generated the prompt but failed to save the file.");
            return;
        }
    }

    std::ostringstream done;
    if (req.mode == "preview")
        done << "NPC Chat generated preview prompt saved: " << savedPath;
    else
        done << "NPC Chat generated " << req.mode << " prompt saved: " << savedPath;
    QueueSystemMessage(req.playerGuidRaw, done.str());
}

// ===========================================================================
// Worker
// ===========================================================================
namespace
{
    void WorkerRun(ChatRequest req)
    {
        std::string const sharedPath = SharedHistoryFilePath(req.npcName, req.npcEntry);
        std::string const personalPath = PersonalHistoryFilePath(
            req.playerName, req.playerGuidRaw, req.npcName, req.npcEntry);

        std::string const defaultPromptPath = DefaultPromptFilePath();
        std::string const sharedPromptPath = SharedPromptFilePath(req.npcName, req.npcEntry);
        std::string const personalPromptPath = PersonalPromptFilePath(
            req.playerName, req.playerGuidRaw, req.npcName, req.npcEntry);
        std::string const sharedSubPromptListPath = SharedSubPromptListFilePath(req.npcName, req.npcEntry);
        std::string const personalSubPromptListPath = PersonalSubPromptListFilePath(
            req.playerName, req.playerGuidRaw, req.npcName, req.npcEntry);

        std::deque<std::string> sharedHistory;
        std::deque<std::string> personalHistory;
        std::string defaultPrompt;
        std::string sharedSubPrompts;
        std::string sharedPrompt;
        std::string personalSubPrompts;
        std::string personalPrompt;

        // Read prompts + prior context, then record what the player just said in both layers.
        {
            std::lock_guard<std::mutex> lock(g_FileMutex);

            EnsureNpcChatDirectoriesAndDefaultPrompt();

            defaultPrompt = ReadWholeTextFile(defaultPromptPath);
            sharedSubPrompts = LoadSubPromptBlocks(LoadSubPromptNameList(sharedSubPromptListPath));
            sharedPrompt = ReadWholeTextFile(sharedPromptPath);
            personalSubPrompts = LoadSubPromptBlocks(LoadSubPromptNameList(personalSubPromptListPath));
            personalPrompt = ReadWholeTextFile(personalPromptPath);

            sharedHistory = LoadHistoryTail(sharedPath, g_SharedHistoryTail);
            personalHistory = LoadHistoryTail(personalPath, g_PersonalHistoryTail);

            AppendHistoryLine(sharedPath, "[" + req.playerName + "] " + req.playerName + ": " + req.message);
            AppendHistoryLine(personalPath, req.playerName + ": " + req.message);
        }

        NpcChat_ApiConfig cfg;
        cfg.baseUrl = g_BaseUrl;
        cfg.apiKey = g_ApiKey;
        cfg.model = g_Model;
        cfg.maxTokens = g_MaxTokens;
        cfg.temperature = g_Temperature;
        cfg.timeoutSec = g_TimeoutSec;
        cfg.extraParams = g_ExtraParams;

        NpcChat_LLMResult res = NpcChat_CallLLM(
            cfg,
            BuildSystemPrompt(req, defaultPrompt, sharedSubPrompts, sharedPrompt, personalSubPrompts, personalPrompt),
            BuildUserPrompt(req, sharedHistory, personalHistory));

        if (!res.success || res.text.empty())
            return;

        {
            std::lock_guard<std::mutex> lock(g_FileMutex);

            AppendHistoryLine(sharedPath, req.npcName + " to [" + req.playerName + "]: " + res.text);
            AppendHistoryLine(personalPath, req.npcName + ": " + res.text);
        }

        ChatReply reply;
        reply.playerGuidRaw = req.playerGuidRaw;
        reply.npcGuidRaw = req.npcGuidRaw;
        reply.text = res.text;
        reply.forcePrivateReply = req.forcePrivateReply;

        std::lock_guard<std::mutex> lock(g_ReplyMutex);
        g_ReplyQueue.push(std::move(reply));
    }
}

// ===========================================================================
// PlayerScript
// ===========================================================================
class NpcChat_PlayerScript : public PlayerScript
{
public:
    NpcChat_PlayerScript() : PlayerScript("NpcChat_PlayerScript",
        {
            PLAYERHOOK_CAN_PLAYER_USE_CHAT,
            PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
            PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT
        }) {}

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg) override
    {
        return HandleNpcChat(player, type, msg);
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Player* /*receiver*/) override
    {
        return HandleNpcChat(player, type, msg);
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Group* /*group*/) override
    {
        return HandleNpcChat(player, type, msg);
    }

private:
    bool HandleNpcChat(Player* player, uint32 type, std::string& msg)
    {
        // Returning true lets normal chat continue. This module listens; it does not block.
        if (!g_Enable || !player)
            return true;

        if (type != CHAT_MSG_SAY)
            return true;

        WorldSession* session = player->GetSession();
        if (!session || session->IsBot())
            return true;

        std::string text = TrimCopy(msg);
        if (text.empty())
            return true;

        // Ignore GM/dot commands.
        if (text[0] == '.')
            return true;

        bool forcePrivateReply = false;

        // Private NPC chat shortcut.
        // Normal NPC chat can still use NpcChat.Prefix, usually "!".
        // Private NPC chat now uses "!p message" directly. This is checked
        // before generic prefix stripping so it works even when NpcChat.Prefix = "!".
        if ((text.rfind("!p", 0) == 0 || text.rfind("!P", 0) == 0) &&
            (text.size() == 2 || std::isspace(static_cast<unsigned char>(text[2])) || text[2] == ':'))
        {
            forcePrivateReply = true;
            text = TrimCopy(text.substr(2));
            if (!text.empty() && text[0] == ':')
                text = TrimCopy(text.substr(1));
            if (text.empty())
                return true;
        }
        else if (g_RequirePrefix)
        {
            if (g_Prefix.empty() || text.rfind(g_Prefix, 0) != 0)
                return true;

            text = TrimCopy(text.substr(g_Prefix.size()));
            if (text.empty())
                return true;
        }

        Unit* sel = player->GetSelectedUnit();
        Creature* npc = sel ? sel->ToCreature() : nullptr;
        if (!npc || !npc->IsAlive())
            return true;

        bool const isHostile = npc->IsHostileTo(player);
        float const distance = player->GetDistance(npc);

        bool const npcInCombat = npc->IsInCombat();
        bool const playerInCombat = player->IsInCombat();
        bool const npcTargetingPlayer = npc->GetVictim() == player;
        bool const hostileCombatTalk = isHostile && (npcInCombat || playerInCombat);

        if (isHostile)
        {
            if (!g_AllowHostileChat)
                return true;

            if (hostileCombatTalk && !g_HostileAllowCombatChat)
                return true;

            // Hostile chat can now work close and far. If close hostile chat is disabled,
            // keep the old parley minimum range behavior.
            if (!g_HostileAllowCloseChat && distance < g_HostileMinDistance)
                return true;

            if (distance > g_HostileMaxDistance)
                return true;

            // Far hostile speech may not be visible as normal /say, so force private at range.
            if (g_HostileForcePrivateReply || distance > g_TriggerRange)
                forcePrivateReply = true;
        }
        else
        {
            if (!player->IsWithinDist(npc, g_TriggerRange, true))
                return true;
        }

        ChatRequest req;
        req.playerGuidRaw = player->GetGUID().GetRawValue();
        req.npcGuidRaw = npc->GetGUID().GetRawValue();
        req.npcEntry = npc->GetEntry();
        req.playerName = player->GetName();
        req.npcName = npc->GetName();
        req.npcLevel = npc->GetLevel();

        req.gender = GenderStr(npc->getGender());
        req.creatureType = CreatureTypeStr(npc->GetCreatureType());

        if (CreatureTemplate const* tmpl = npc->GetCreatureTemplate())
        {
            req.npcSubName = tmpl->SubName;
            req.rankStr = RankStr(tmpl->rank);
            req.roleStr = RolesFromNpcFlags(tmpl->npcflag);
        }

        req.isHostile = isHostile;
        req.npcInCombat = npcInCombat;
        req.playerInCombat = playerInCombat;
        req.npcTargetingPlayer = npcTargetingPlayer;
        req.npcHealthPct = UnitHealthPct(npc);
        req.playerHealthPct = UnitHealthPct(player);
        req.distance = distance;
        req.fightState = BuildFightState(req.npcHealthPct, req.playerHealthPct, npcInCombat, playerInCombat, npcTargetingPlayer);
        req.forcePrivateReply = forcePrivateReply;

        if (isHostile)
            req.stance = "an enemy";
        else if (npc->IsFriendlyTo(player))
            req.stance = "a friend";
        else
            req.stance = "a stranger";

        if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(npc->GetZoneId()))
            req.zoneName = zone->area_name[0];

        req.message = text;

        std::thread(WorkerRun, std::move(req)).detach();
        return true;
    }
};

// ===========================================================================
// WorldScript
// ===========================================================================
class NpcChat_WorldScript : public WorldScript
{
public:
    NpcChat_WorldScript() : WorldScript("NpcChat_WorldScript") {}

    void OnStartup() override
    {
        LoadConfig();
        EnsureNpcChatDirectoriesAndDefaultPrompt();
    }

    void OnUpdate(uint32 /*diff*/) override
    {
        if (!g_Enable)
            return;

        std::queue<SystemMessage> localSystem;
        {
            std::lock_guard<std::mutex> lock(g_SystemMessageMutex);
            std::swap(localSystem, g_SystemMessageQueue);
        }

        while (!localSystem.empty())
        {
            SystemMessage& m = localSystem.front();
            if (Player* player = ObjectAccessor::FindPlayer(ObjectGuid(m.playerGuidRaw)))
            {
                if (player->IsInWorld() && player->GetSession())
                {
                    ChatHandler chat(player->GetSession());
                    chat.PSendSysMessage("{}", m.text);
                }
            }
            localSystem.pop();
        }

        std::queue<ChatReply> local;
        {
            std::lock_guard<std::mutex> lock(g_ReplyMutex);
            std::swap(local, g_ReplyQueue);
        }

        while (!local.empty())
        {
            ChatReply& r = local.front();

            Player* anchor = ObjectAccessor::FindPlayer(ObjectGuid(r.playerGuidRaw));
            if (anchor && anchor->IsInWorld())
            {
                Creature* npc = ObjectAccessor::GetCreature(*anchor, ObjectGuid(r.npcGuidRaw));
                if (npc && npc->IsInWorld())
                {
                    if (r.forcePrivateReply)
                    {
                        // Targeted creature say: useful for !p private chat and hostile parley at long range.
                        npc->Say(r.text, LANG_UNIVERSAL, anchor);
                    }
                    else
                    {
                        // Public creature speech. No target arg means it should behave like
                        // normal NPC /say with a visible chat bubble for nearby players.
                        npc->Say(r.text, LANG_UNIVERSAL);
                    }
                }
            }

            local.pop();
        }
    }
};


// ===========================================================================
// Commands
// ===========================================================================
using namespace Acore::ChatCommands;

class NpcChat_CommandScript : public CommandScript
{
public:
    NpcChat_CommandScript() : CommandScript("NpcChat_CommandScript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "npcc", HandleNpcChatCommand, SEC_PLAYER, Console::No }
        };
        return commandTable;
    }

private:
    static bool IsGm(ChatHandler* handler)
    {
        if (!handler || !handler->GetSession())
            return true;

        return handler->GetSession()->GetSecurity() >= SEC_GAMEMASTER;
    }

    static uint32 GetCommandAccountId(ChatHandler* handler)
    {
        if (!handler || !handler->GetSession())
            return 0;

        return handler->GetSession()->GetAccountId();
    }

    static bool IsSubPromptCreatorAccount(ChatHandler* handler)
    {
        uint32 accountId = GetCommandAccountId(handler);
        if (!accountId)
            return false;

        return std::find(g_SubPromptCreatorAccounts.begin(),
            g_SubPromptCreatorAccounts.end(),
            accountId) != g_SubPromptCreatorAccounts.end();
    }

    static bool CanCreateSubPrompts(ChatHandler* handler)
    {
        return IsGm(handler) || IsSubPromptCreatorAccount(handler);
    }

    static bool CanManageSharedSubPrompts(ChatHandler* handler)
    {
        return IsGm(handler) || IsSubPromptCreatorAccount(handler);
    }

    static std::string JoinAccountIds(std::vector<uint32> const& ids)
    {
        if (ids.empty())
            return "(none)";

        std::ostringstream ss;
        for (size_t i = 0; i < ids.size(); ++i)
        {
            if (i)
                ss << ",";
            ss << ids[i];
        }
        return ss.str();
    }

    static Player* GetCommandPlayer(ChatHandler* handler)
    {
        if (!handler || !handler->GetSession())
            return nullptr;
        return handler->GetSession()->GetPlayer();
    }

    static Creature* GetSelectedCreature(ChatHandler* handler)
    {
        Player* player = GetCommandPlayer(handler);
        if (!player)
            return nullptr;

        Unit* sel = player->GetSelectedUnit();
        return sel ? sel->ToCreature() : nullptr;
    }

    static bool HandleNpcChatCommand(ChatHandler* handler, char const* args)
    {
        std::string arg = TrimCopy(args ? args : "");
        std::string rest;

        if (arg.empty() || StartsWithWord(arg, "help", rest))
        {
            handler->PSendSysMessage("NPC Chat commands:");
            handler->PSendSysMessage(".npcc reload");
            handler->PSendSysMessage(".npcc reset");
            handler->PSendSysMessage(".npcc prompt [quoted personal prompt]");
            handler->PSendSysMessage(".npcc gen shared [quoted extra direction]");
            handler->PSendSysMessage(".npcc gen personal [quoted extra direction]");
            handler->PSendSysMessage(".npcc gen preview [quoted extra direction]");
            handler->PSendSysMessage(".npcc sub list");
            handler->PSendSysMessage(".npcc sub show");
            handler->PSendSysMessage(".npcc sub attach <name>");
            handler->PSendSysMessage(".npcc sub detach <name>");
            handler->PSendSysMessage(".npcc sub clear");
            handler->PSendSysMessage(".npcc account");
            handler->PSendSysMessage("GM: .npcc prompt shared [quoted shared prompt]");
            handler->PSendSysMessage("GM: .npcc prompt default [quoted default prompt]");
            handler->PSendSysMessage("GM/Allowed: .npcc sub create <name> [quoted prompt text]");
            handler->PSendSysMessage("GM/Allowed: .npcc sub attach shared <name>");
            return true;
        }

        if (StartsWithWord(arg, "account", rest) || StartsWithWord(arg, "whoami", rest))
        {
            handler->PSendSysMessage("NPC Chat account ID: {}", GetCommandAccountId(handler));
            handler->PSendSysMessage("NPC Chat GM: {}", IsGm(handler) ? "yes" : "no");
            handler->PSendSysMessage("NPC Chat sub-prompt creator: {}", CanCreateSubPrompts(handler) ? "yes" : "no");
            handler->PSendSysMessage("NPC Chat loaded creator account IDs: {}", JoinAccountIds(g_SubPromptCreatorAccounts));
            return true;
        }

        if (StartsWithWord(arg, "reload", rest))
        {
            LoadConfig();
            std::lock_guard<std::mutex> lock(g_FileMutex);
            EnsureNpcChatDirectoriesAndDefaultPrompt();
            handler->PSendSysMessage("NPC Chat config and prompt paths reloaded.");
            return true;
        }

        if (StartsWithWord(arg, "reset", rest))
        {
            Creature* npc = GetSelectedCreature(handler);
            if (!npc)
            {
                handler->PSendSysMessage("Target an NPC first.");
                return true;
            }

            Player* player = GetCommandPlayer(handler);
            if (!player)
            {
                handler->PSendSysMessage("This command must be used in game.");
                return true;
            }

            std::lock_guard<std::mutex> lock(g_FileMutex);
            EnsureNpcChatDirectoriesAndDefaultPrompt();

            if (IsGm(handler))
            {
                bool removedShared = RemoveFileIfExists(SharedHistoryFilePath(npc->GetName(), npc->GetEntry()));
                std::size_t removedPersonal = RemoveAllPersonalHistoryForNpc(npc->GetName(), npc->GetEntry());
                handler->PSendSysMessage("NPC Chat GM reset complete for target NPC.");
                handler->PSendSysMessage("Shared history removed: {}. Personal histories removed: {}.",
                    removedShared ? "yes" : "no", static_cast<uint32>(removedPersonal));
            }
            else
            {
                bool removed = RemoveFileIfExists(PersonalHistoryFilePath(
                    player->GetName(), player->GetGUID().GetRawValue(), npc->GetName(), npc->GetEntry()));

                handler->PSendSysMessage(removed ?
                    "Your personal NPC Chat history with this NPC was reset." :
                    "No personal NPC Chat history existed for this NPC.");
            }

            return true;
        }

        if (StartsWithWord(arg, "gen", rest) || StartsWithWord(arg, "generate", rest))
        {
            Player* player = GetCommandPlayer(handler);
            if (!player)
            {
                handler->PSendSysMessage("This command must be used in game.");
                return true;
            }

            Creature* npc = GetSelectedCreature(handler);
            if (!npc)
            {
                handler->PSendSysMessage("Target an NPC first.");
                return true;
            }

            std::string mode = "preview";
            std::string extra = rest;
            std::string maybeRest;

            if (StartsWithWord(rest, "shared", maybeRest))
            {
                mode = "shared";
                extra = maybeRest;
            }
            else if (StartsWithWord(rest, "personal", maybeRest))
            {
                mode = "personal";
                extra = maybeRest;
            }
            else if (StartsWithWord(rest, "preview", maybeRest))
            {
                mode = "preview";
                extra = maybeRest;
            }

            if (mode == "shared" && !CanManageSharedSubPrompts(handler))
            {
                handler->PSendSysMessage("Only GMs or configured NPC Chat sub-prompt creator accounts may generate shared NPC prompts.");
                handler->PSendSysMessage("Use .npcc account to see your account ID and loaded allowlist.");
                return true;
            }

            extra = StripWrappingQuotes(TrimCopy(extra));

            GenPromptRequest req;
            req.playerGuidRaw = player->GetGUID().GetRawValue();
            req.npcGuidRaw = npc->GetGUID().GetRawValue();
            req.npcEntry = npc->GetEntry();
            req.playerName = player->GetName();
            req.npcName = npc->GetName();

            if (CreatureTemplate const* ct = npc->GetCreatureTemplate())
            {
                req.npcSubName = ct->SubName;
                req.gender = GenderStr(ct->gender);
                req.creatureType = CreatureTypeStr(ct->type);
                req.rankStr = RankStr(ct->rank);
                req.roleStr = RolesFromNpcFlags(ct->npcflag);
            }

            req.npcLevel = npc->GetLevel();
            req.isHostile = npc->IsHostileTo(player);
            req.npcInCombat = npc->IsInCombat();
            req.playerInCombat = player->IsInCombat();
            req.npcTargetingPlayer = npc->GetVictim() == player;
            req.npcHealthPct = UnitHealthPct(npc);
            req.playerHealthPct = UnitHealthPct(player);
            req.distance = npc->GetDistance(player);
            req.fightState = BuildFightState(req.npcHealthPct, req.playerHealthPct, req.npcInCombat, req.playerInCombat, req.npcTargetingPlayer);

            if (req.isHostile)
                req.stance = "an enemy";
            else if (npc->IsFriendlyTo(player))
                req.stance = "a friend";
            else
                req.stance = "a stranger";

            if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(npc->GetZoneId()))
                req.zoneName = zone->area_name[0];

            req.mode = mode;
            req.extraInstruction = extra;
            if (mode == "personal")
                req.outputPath = PersonalPromptFilePath(player->GetName(), player->GetGUID().GetRawValue(), npc->GetName(), npc->GetEntry());
            else
                req.outputPath = SharedPromptFilePath(npc->GetName(), npc->GetEntry());

            std::thread(GeneratePromptWorker, std::move(req)).detach();

            handler->PSendSysMessage("NPC Chat character prompt generation started for {} ({}).", npc->GetName(), mode.c_str());
            handler->PSendSysMessage("You will get a system message when the file is saved.");
            return true;
        }

        if (StartsWithWord(arg, "sub", rest))
        {
            Player* player = GetCommandPlayer(handler);
            if (!player)
            {
                handler->PSendSysMessage("This command must be used in game.");
                return true;
            }

            std::string subRest;

            if (rest.empty() || StartsWithWord(rest, "help", subRest))
            {
                handler->PSendSysMessage("NPC Chat sub-prompt commands:");
                handler->PSendSysMessage(".npcc sub list");
                handler->PSendSysMessage(".npcc sub show");
                handler->PSendSysMessage(".npcc sub attach <name>");
                handler->PSendSysMessage(".npcc sub detach <name>");
                handler->PSendSysMessage(".npcc sub clear");
                handler->PSendSysMessage("GM/Allowed: .npcc sub create <name> [quoted prompt text]");
                handler->PSendSysMessage("GM/Allowed: .npcc sub attach shared <name>");
                handler->PSendSysMessage("GM: .npcc sub detach shared <name>");
                handler->PSendSysMessage("GM: .npcc sub clear shared");
                return true;
            }

            if (StartsWithWord(rest, "list", subRest))
            {
                std::lock_guard<std::mutex> lock(g_FileMutex);
                EnsureNpcChatDirectoriesAndDefaultPrompt();

                std::vector<std::string> names;
                try
                {
                    std::filesystem::path root(SubPromptRootPath());
                    if (std::filesystem::exists(root))
                    {
                        for (auto const& it : std::filesystem::directory_iterator(root))
                        {
                            if (!it.is_regular_file())
                                continue;
                            if (it.path().extension().string() != ".prompt")
                                continue;

                            std::string key = NormalizeSubPromptName(it.path().stem().string());
                            if (!key.empty())
                                names.push_back(key);
                        }
                    }
                }
                catch (std::exception const&)
                {
                    // best effort
                }

                std::sort(names.begin(), names.end());
                handler->PSendSysMessage("NPC Chat available sub-prompts: {}", JoinNames(names).c_str());
                return true;
            }

            if (StartsWithWord(rest, "create", subRest))
            {
                if (!CanCreateSubPrompts(handler))
                {
                    handler->PSendSysMessage("Only GMs or configured NPC Chat sub-prompt creator accounts may create global sub-prompts.");
                    handler->PSendSysMessage("Use .npcc account to see your account ID, then add it to NpcChat.SubPromptCreatorAccounts.");
                    return true;
                }

                std::string name = subRest;
                std::string text;
                size_t space = subRest.find_first_of(" \t");
                if (space != std::string::npos)
                {
                    name = subRest.substr(0, space);
                    text = TrimCopy(subRest.substr(space + 1));
                }

                name = NormalizeSubPromptName(name);
                text = StripWrappingQuotes(text);

                if (name.empty())
                {
                    handler->PSendSysMessage("Usage: .npcc sub create <name> [quoted prompt text]");
                    return true;
                }

                std::string path = SubPromptFilePath(name);
                bool overwrite = !text.empty();

                std::lock_guard<std::mutex> lock(g_FileMutex);
                EnsureNpcChatDirectoriesAndDefaultPrompt();

                if (!WriteWholeTextFile(path, text, overwrite))
                {
                    handler->PSendSysMessage("Failed to write NPC Chat sub-prompt file.");
                    return true;
                }

                if (overwrite)
                    handler->PSendSysMessage("NPC Chat sub-prompt saved: {}", path.c_str());
                else
                    handler->PSendSysMessage("NPC Chat sub-prompt file exists/created: {}", path.c_str());
                return true;
            }

            if (StartsWithWord(rest, "show", subRest))
            {
                Creature* npc = GetSelectedCreature(handler);
                if (!npc)
                {
                    handler->PSendSysMessage("Target an NPC first.");
                    return true;
                }

                std::lock_guard<std::mutex> lock(g_FileMutex);
                EnsureNpcChatDirectoriesAndDefaultPrompt();

                std::vector<std::string> shared = LoadSubPromptNameList(SharedSubPromptListFilePath(npc->GetName(), npc->GetEntry()));
                std::vector<std::string> personal = LoadSubPromptNameList(PersonalSubPromptListFilePath(
                    player->GetName(), player->GetGUID().GetRawValue(), npc->GetName(), npc->GetEntry()));

                handler->PSendSysMessage("NPC Chat shared sub-prompts: {}", JoinNames(shared).c_str());
                handler->PSendSysMessage("NPC Chat personal sub-prompts: {}", JoinNames(personal).c_str());
                return true;
            }

            bool attachMatched = StartsWithWord(rest, "attach", subRest);
            bool detachMatched = StartsWithWord(rest, "detach", subRest);
            bool clearMatched = StartsWithWord(rest, "clear", subRest);
            if (attachMatched || detachMatched || clearMatched)
            {
                Creature* npc = GetSelectedCreature(handler);
                if (!npc)
                {
                    handler->PSendSysMessage("Target an NPC first.");
                    return true;
                }

                std::string mode;
                std::string name = subRest;
                std::string maybeRest;

                if (StartsWithWord(subRest, "shared", maybeRest))
                {
                    mode = "shared";
                    name = maybeRest;
                }
                else if (StartsWithWord(subRest, "personal", maybeRest))
                {
                    mode = "personal";
                    name = maybeRest;
                }
                else
                {
                    // GMs and configured creator accounts default to shared/global attachments.
                    // Regular players still default to personal-only attachments.
                    mode = CanManageSharedSubPrompts(handler) ? "shared" : "personal";
                }

                if (mode == "shared" && !CanManageSharedSubPrompts(handler))
                {
                    handler->PSendSysMessage("Only GMs or configured NPC Chat sub-prompt creator accounts may edit shared NPC Chat sub-prompt attachments.");
                    handler->PSendSysMessage("Use .npcc account to see your account ID and loaded allowlist.");
                    return true;
                }

                std::string listPath = (mode == "shared")
                    ? SharedSubPromptListFilePath(npc->GetName(), npc->GetEntry())
                    : PersonalSubPromptListFilePath(player->GetName(), player->GetGUID().GetRawValue(), npc->GetName(), npc->GetEntry());

                name = NormalizeSubPromptName(name);

                std::lock_guard<std::mutex> lock(g_FileMutex);
                EnsureNpcChatDirectoriesAndDefaultPrompt();

                if (clearMatched)
                {
                    if (!WriteSubPromptNameList(listPath, {}))
                        handler->PSendSysMessage("Failed to clear NPC Chat {} sub-prompt attachments.", mode.c_str());
                    else
                        handler->PSendSysMessage("NPC Chat {} sub-prompt attachments cleared for target NPC.", mode.c_str());
                    return true;
                }

                if (name.empty())
                {
                    handler->PSendSysMessage("Usage: .npcc sub {} [shared|personal] <name>", attachMatched ? "attach" : "detach");
                    return true;
                }

                if (attachMatched && !std::filesystem::exists(SubPromptFilePath(name)))
                {
                    handler->PSendSysMessage("NPC Chat sub-prompt does not exist yet: {}", SubPromptFilePath(name).c_str());
                    return true;
                }

                bool ok = attachMatched ? AddSubPromptName(listPath, name) : RemoveSubPromptName(listPath, name);
                if (!ok)
                {
                    handler->PSendSysMessage("Failed to update NPC Chat sub-prompt attachments.");
                    return true;
                }

                handler->PSendSysMessage("NPC Chat {} sub-prompt {} for target NPC: {}",
                    mode.c_str(), attachMatched ? "attached" : "detached", name.c_str());
                return true;
            }

            handler->PSendSysMessage("Unknown NPC Chat sub-prompt command. Use .npcc sub help");
            return true;
        }

        if (StartsWithWord(arg, "prompt", rest))
        {
            Player* player = GetCommandPlayer(handler);
            if (!player)
            {
                handler->PSendSysMessage("This command must be used in game.");
                return true;
            }

            std::string mode;
            std::string promptText = rest;
            std::string maybeRest;

            if (StartsWithWord(rest, "shared", maybeRest))
            {
                mode = "shared";
                promptText = maybeRest;
            }
            else if (StartsWithWord(rest, "default", maybeRest))
            {
                mode = "default";
                promptText = maybeRest;
            }
            else
            {
                mode = "personal";
            }

            if ((mode == "shared" || mode == "default") && !IsGm(handler))
            {
                handler->PSendSysMessage("Only GMs may edit shared/default NPC Chat prompts.");
                return true;
            }

            std::string path;
            if (mode == "default")
            {
                path = DefaultPromptFilePath();
            }
            else
            {
                Creature* npc = GetSelectedCreature(handler);
                if (!npc)
                {
                    handler->PSendSysMessage("Target an NPC first.");
                    return true;
                }

                if (mode == "shared")
                    path = SharedPromptFilePath(npc->GetName(), npc->GetEntry());
                else
                    path = PersonalPromptFilePath(player->GetName(), player->GetGUID().GetRawValue(), npc->GetName(), npc->GetEntry());
            }

            promptText = StripWrappingQuotes(promptText);
            bool overwrite = !promptText.empty();

            std::lock_guard<std::mutex> lock(g_FileMutex);
            EnsureNpcChatDirectoriesAndDefaultPrompt();

            if (!WriteWholeTextFile(path, promptText, overwrite))
            {
                handler->PSendSysMessage("Failed to write NPC Chat prompt file.");
                return true;
            }

            if (overwrite)
                handler->PSendSysMessage("NPC Chat {} prompt saved: {}", mode.c_str(), path.c_str());
            else
                handler->PSendSysMessage("NPC Chat {} prompt file exists/created: {}", mode.c_str(), path.c_str());

            return true;
        }

        handler->PSendSysMessage("Unknown NPC Chat command. Use .npcc help");
        return true;
    }
};

// ===========================================================================
// Registration
// ===========================================================================
void Addmod_npc_chat_llmScripts()
{
    new NpcChat_WorldScript();
    new NpcChat_PlayerScript();
    new NpcChat_CommandScript();
}
