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

    std::string g_HistoryPath = "./AI_RP/npc_history";
    int         g_HistoryTail = 20;          // legacy/default tail if new split values are absent
    int         g_SharedHistoryTail = 12;    // shared NPC memory lines fed to model
    int         g_PersonalHistoryTail = 20;  // private player+NPC memory lines fed to model
    bool        g_NameByEntry = true;

    float       g_TriggerRange = 25.0f;
    bool        g_RequirePrefix = false;
    std::string g_Prefix;

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

        g_HistoryPath = sConfigMgr->GetOption<std::string>("NpcChat.HistoryPath", "./AI_RP/npc_history");
        g_HistoryTail = sConfigMgr->GetOption<int32>("NpcChat.HistoryMaxLines", 20);
        g_SharedHistoryTail = sConfigMgr->GetOption<int32>("NpcChat.SharedHistoryMaxLines", std::max(6, g_HistoryTail / 2));
        g_PersonalHistoryTail = sConfigMgr->GetOption<int32>("NpcChat.PersonalHistoryMaxLines", g_HistoryTail);
        g_NameByEntry = sConfigMgr->GetOption<bool>("NpcChat.NameByEntry", true);

        g_TriggerRange = sConfigMgr->GetOption<float>("NpcChat.TriggerRange", 25.0f);
        g_RequirePrefix = sConfigMgr->GetOption<bool>("NpcChat.RequirePrefix", false);
        g_Prefix = sConfigMgr->GetOption<std::string>("NpcChat.Prefix", "");
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

        std::string message;
    };

    struct ChatReply
    {
        uint64_t    playerGuidRaw = 0;
        uint64_t    npcGuidRaw = 0;
        std::string text;
    };

    std::queue<ChatReply> g_ReplyQueue;
    std::mutex            g_ReplyMutex;

    // One global lock for all NPC history file IO. Good enough for a small
    // personal realm; prevents two workers from interleaving the same file.
    std::mutex            g_FileMutex;
}

// ===========================================================================
// Text helpers
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

    std::string SharedHistoryFilePath(std::string const& npcName, uint32_t entry)
    {
        return g_HistoryPath + "/shared/" + NpcHistoryBase(npcName, entry) + ".history";
    }

    std::string PersonalHistoryFilePath(std::string const& playerName, uint64_t playerGuidRaw,
        std::string const& npcName, uint32_t entry)
    {
        return g_HistoryPath + "/personal/" + PlayerHistoryBase(playerName, playerGuidRaw) + "/" +
            NpcHistoryBase(npcName, entry) + ".history";
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
    std::string BuildSystemPrompt(ChatRequest const& req)
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

        ss << " Stay fully in character as " << req.npcName << ". ";
        ss << "Do not mention being an AI, a language model, a game script, a prompt, or a memory file. ";
        ss << "Use only your own spoken words: no narration, no asterisks, no emotes, no out-of-character text. ";
        ss << "Be more than a generic vendor line: react to what the player says, show a bit of personality, and keep the conversation open. ";
        ss << "When it fits your nature and attitude, ask a small follow-up question, offer a rumor, give practical advice, tease, warn, bargain, or invite the player to return. ";
        ss << "Let familiarity grow over time if this player has spoken with you before. Acknowledge remembered details naturally, without saying the word memory. ";
        ss << "If the speaker is an enemy, you may be hostile, suspicious, threatening, or mocking instead of friendly. ";
        ss << "Reply in one to three short sentences suitable for in-game NPC speech.";

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

        ss << req.playerName << " says to you: \"" << req.message << "\"\n\n";
        ss << "Reply as " << req.npcName << ". Engage naturally and leave room for the conversation to continue:";

        return ss.str();
    }
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

        std::deque<std::string> sharedHistory;
        std::deque<std::string> personalHistory;

        // Read prior context, then record what the player just said in both layers.
        {
            std::lock_guard<std::mutex> lock(g_FileMutex);

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
            BuildSystemPrompt(req),
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

        if (g_RequirePrefix)
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

        if (!player->IsWithinDist(npc, g_TriggerRange, true))
            return true;

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

        if (npc->IsHostileTo(player))
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
        if (!g_Enable)
            return;

        try
        {
            std::filesystem::create_directories(g_HistoryPath);
            std::filesystem::create_directories(g_HistoryPath + "/shared");
            std::filesystem::create_directories(g_HistoryPath + "/personal");
        }
        catch (std::exception const&)
        {
            // non-fatal
        }
    }

    void OnUpdate(uint32 /*diff*/) override
    {
        if (!g_Enable)
            return;

        std::queue<ChatReply> local;
        {
            std::lock_guard<std::mutex> lock(g_ReplyMutex);
            if (g_ReplyQueue.empty())
                return;

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
                    // Public creature speech. No target arg means it should behave like
                    // normal NPC /say with a visible chat bubble for nearby players.
                    npc->Say(r.text, LANG_UNIVERSAL);
                }
            }

            local.pop();
        }
    }
};

// ===========================================================================
// Registration
// ===========================================================================
void Addmod_npc_chat_llmScripts()
{
    new NpcChat_WorldScript();
    new NpcChat_PlayerScript();
}
