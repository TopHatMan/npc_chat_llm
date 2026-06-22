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
#include "ObjectMgr.h"     // quests / templates
#include "QuestDef.h"      // Quest accessors
#include "DatabaseEnv.h"   // WorldDatabase query/cache
#include "Config.h"
#include "SharedDefines.h"   // CHAT_MSG_SAY, LANG_UNIVERSAL, CreatureType, ranks
#include "UnitDefines.h"     // UNIT_NPC_FLAG_*
#include "DBCStores.h"       // sAreaTableStore
#include "DBCStructure.h"    // AreaTableEntry
#include "WorldSession.h"    // session / bot check on playerbots builds
#include "World.h"           // active sessions for cached proximity barks
#include "Chat.h"            // CommandScript / ChatHandler

#include "npcchat_llm.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <ctime>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <queue>
#include <map>
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

    // Optional separate API/model for expensive generation jobs.
    // Normal live roleplay uses NpcChat.* above; one-time prompt/bark/quest
    // generation can use NpcChat.Generation.* and falls back to NpcChat.*.
    std::string g_GenerationBaseUrl;
    std::string g_GenerationApiKey;
    std::string g_GenerationModel;
    std::string g_GenerationExtraParams;
    int         g_GenerationTimeoutSec = 30;

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

    // Cached relationship barks: NPCs can speak first, but only from saved .barks files.
    // No automatic LLM call happens from proximity scanning unless a future feature explicitly enables it.
    bool        g_RelationshipBarksEnabled = false;
    bool        g_RelationshipBarksRealPlayersOnly = true;
    float       g_RelationshipBarksTriggerDistance = 12.0f;
    int         g_RelationshipBarksChancePct = 8;
    int         g_RelationshipBarksPlayerCooldownSec = 600;
    int         g_RelationshipBarksNpcCooldownSec = 300;
    int         g_RelationshipBarksPairCooldownSec = 900;
    int         g_RelationshipBarksScanIntervalMs = 2000;
    bool        g_RelationshipBarksGenerateMissing = false;

    // Cached hostile first-talk barks: elite/intelligent enemies can speak first
    // from saved shared .hostile_barks files. Cache-only by default; no proximity
    // scan will ever call the LLM in this safe pass.
    bool        g_HostileFirstTalkEnabled = false;
    bool        g_HostileFirstTalkRealPlayersOnly = true;
    float       g_HostileFirstTalkTriggerDistance = 35.0f;
    int         g_HostileFirstTalkChancePct = 20;
    int         g_HostileFirstTalkPlayerCooldownSec = 900;
    int         g_HostileFirstTalkNpcCooldownSec = 900;
    int         g_HostileFirstTalkPairCooldownSec = 1800;
    int         g_HostileFirstTalkScanIntervalMs = 2000;
    bool        g_HostileFirstTalkGenerateMissing = false;
    bool        g_HostileFirstTalkElitesOnly = true;
    bool        g_HostileFirstTalkAllowInCombat = false;

    // Cached trainer barks. Safe pass: cache-only and disabled by default.
    // Manual commands can generate/fire a trainer bark; proximity never calls the LLM.
    bool        g_TrainerBarksEnabled = false;
    bool        g_TrainerBarksRealPlayersOnly = true;
    float       g_TrainerBarksTriggerDistance = 12.0f;
    int         g_TrainerBarksChancePct = 10;
    int         g_TrainerBarksPlayerCooldownSec = 600;
    int         g_TrainerBarksNpcCooldownSec = 300;
    int         g_TrainerBarksPairCooldownSec = 900;
    int         g_TrainerBarksScanIntervalMs = 3000;
    bool        g_TrainerBarksGenerateMissing = false;

    // Cached quest intro barks. Safe first pass: selected questgiver only,
    // cache-first, and automatic generation disabled by default.
    bool        g_QuestBarksEnabled = false;
    bool        g_QuestBarksRealPlayersOnly = true;
    bool        g_QuestBarksGenerateMissing = false;
    bool        g_QuestBarksSelectedOnly = true;
    float       g_QuestBarksTriggerDistance = 12.0f;
    int         g_QuestBarksChancePct = 10;
    int         g_QuestBarksPlayerCooldownSec = 900;
    int         g_QuestBarksNpcCooldownSec = 300;
    int         g_QuestBarksPairCooldownSec = 1800;
    int         g_QuestBarksScanIntervalMs = 3000;
    int         g_QuestBarksMaxQuestsCheckedPerNpc = 8;

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

        // Backward-compatible generation settings. The old GeneratePrompt* keys still work,
        // while the new NpcChat.Generation.* block can point at a stronger model/API.
        g_GenerationBaseUrl = sConfigMgr->GetOption<std::string>("NpcChat.Generation.BaseUrl", g_BaseUrl);
        g_GenerationApiKey = sConfigMgr->GetOption<std::string>("NpcChat.Generation.ApiKey", g_ApiKey);
        g_GenerationModel = sConfigMgr->GetOption<std::string>("NpcChat.Generation.Model", g_Model);
        g_GenerationExtraParams = sConfigMgr->GetOption<std::string>("NpcChat.Generation.ModelExtraParameters", g_ExtraParams);
        g_GenerationTimeoutSec = sConfigMgr->GetOption<int32>("NpcChat.Generation.RequestTimeoutSec", g_TimeoutSec);
        g_GeneratePromptMaxTokens = sConfigMgr->GetOption<int32>("NpcChat.Generation.MaxTokens",
            sConfigMgr->GetOption<int32>("NpcChat.GeneratePromptMaxTokens", 700));
        g_GeneratePromptTemperature = sConfigMgr->GetOption<float>("NpcChat.Generation.Temperature",
            sConfigMgr->GetOption<float>("NpcChat.GeneratePromptTemperature", 0.75f));

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

        g_RelationshipBarksEnabled = sConfigMgr->GetOption<bool>("NpcChat.RelationshipBarks.Enabled", false);
        g_RelationshipBarksRealPlayersOnly = sConfigMgr->GetOption<bool>("NpcChat.RelationshipBarks.RealPlayersOnly", true);
        g_RelationshipBarksTriggerDistance = sConfigMgr->GetOption<float>("NpcChat.RelationshipBarks.TriggerDistance", 12.0f);
        g_RelationshipBarksChancePct = sConfigMgr->GetOption<int32>("NpcChat.RelationshipBarks.ChancePct", 8);
        g_RelationshipBarksPlayerCooldownSec = sConfigMgr->GetOption<int32>("NpcChat.RelationshipBarks.PlayerCooldownSec", 600);
        g_RelationshipBarksNpcCooldownSec = sConfigMgr->GetOption<int32>("NpcChat.RelationshipBarks.NpcCooldownSec", 300);
        g_RelationshipBarksPairCooldownSec = sConfigMgr->GetOption<int32>("NpcChat.RelationshipBarks.PairCooldownSec", 900);
        g_RelationshipBarksScanIntervalMs = sConfigMgr->GetOption<int32>("NpcChat.RelationshipBarks.ScanIntervalMs", 2000);
        g_RelationshipBarksGenerateMissing = sConfigMgr->GetOption<bool>("NpcChat.RelationshipBarks.GenerateMissing", false);

        if (g_RelationshipBarksChancePct < 0)
            g_RelationshipBarksChancePct = 0;
        if (g_RelationshipBarksChancePct > 100)
            g_RelationshipBarksChancePct = 100;
        if (g_RelationshipBarksScanIntervalMs < 500)
            g_RelationshipBarksScanIntervalMs = 500;

        g_HostileFirstTalkEnabled = sConfigMgr->GetOption<bool>("NpcChat.HostileFirstTalk.Enabled", false);
        g_HostileFirstTalkRealPlayersOnly = sConfigMgr->GetOption<bool>("NpcChat.HostileFirstTalk.RealPlayersOnly", true);
        g_HostileFirstTalkTriggerDistance = sConfigMgr->GetOption<float>("NpcChat.HostileFirstTalk.TriggerDistance", 35.0f);
        g_HostileFirstTalkChancePct = sConfigMgr->GetOption<int32>("NpcChat.HostileFirstTalk.ChancePct", 20);
        g_HostileFirstTalkPlayerCooldownSec = sConfigMgr->GetOption<int32>("NpcChat.HostileFirstTalk.PlayerCooldownSec", 900);
        g_HostileFirstTalkNpcCooldownSec = sConfigMgr->GetOption<int32>("NpcChat.HostileFirstTalk.NpcCooldownSec", 900);
        g_HostileFirstTalkPairCooldownSec = sConfigMgr->GetOption<int32>("NpcChat.HostileFirstTalk.PairCooldownSec", 1800);
        g_HostileFirstTalkScanIntervalMs = sConfigMgr->GetOption<int32>("NpcChat.HostileFirstTalk.ScanIntervalMs", 2000);
        g_HostileFirstTalkGenerateMissing = sConfigMgr->GetOption<bool>("NpcChat.HostileFirstTalk.GenerateMissing", false);
        g_HostileFirstTalkElitesOnly = sConfigMgr->GetOption<bool>("NpcChat.HostileFirstTalk.ElitesOnly", true);
        g_HostileFirstTalkAllowInCombat = sConfigMgr->GetOption<bool>("NpcChat.HostileFirstTalk.AllowInCombat", false);

        if (g_HostileFirstTalkChancePct < 0)
            g_HostileFirstTalkChancePct = 0;
        if (g_HostileFirstTalkChancePct > 100)
            g_HostileFirstTalkChancePct = 100;
        if (g_HostileFirstTalkScanIntervalMs < 500)
            g_HostileFirstTalkScanIntervalMs = 500;

        g_TrainerBarksEnabled = sConfigMgr->GetOption<bool>("NpcChat.TrainerBarks.Enabled", false);
        g_TrainerBarksRealPlayersOnly = sConfigMgr->GetOption<bool>("NpcChat.TrainerBarks.RealPlayersOnly", true);
        g_TrainerBarksTriggerDistance = sConfigMgr->GetOption<float>("NpcChat.TrainerBarks.TriggerDistance", 12.0f);
        g_TrainerBarksChancePct = sConfigMgr->GetOption<int32>("NpcChat.TrainerBarks.ChancePct", 10);
        g_TrainerBarksPlayerCooldownSec = sConfigMgr->GetOption<int32>("NpcChat.TrainerBarks.PlayerCooldownSec", 600);
        g_TrainerBarksNpcCooldownSec = sConfigMgr->GetOption<int32>("NpcChat.TrainerBarks.NpcCooldownSec", 300);
        g_TrainerBarksPairCooldownSec = sConfigMgr->GetOption<int32>("NpcChat.TrainerBarks.PairCooldownSec", 900);
        g_TrainerBarksScanIntervalMs = sConfigMgr->GetOption<int32>("NpcChat.TrainerBarks.ScanIntervalMs", 3000);
        g_TrainerBarksGenerateMissing = sConfigMgr->GetOption<bool>("NpcChat.TrainerBarks.GenerateMissing", false);
        if (g_TrainerBarksChancePct < 0)
            g_TrainerBarksChancePct = 0;
        if (g_TrainerBarksChancePct > 100)
            g_TrainerBarksChancePct = 100;
        if (g_TrainerBarksScanIntervalMs < 500)
            g_TrainerBarksScanIntervalMs = 500;

        g_QuestBarksEnabled = sConfigMgr->GetOption<bool>("NpcChat.QuestBarks.Enabled", false);
        g_QuestBarksRealPlayersOnly = sConfigMgr->GetOption<bool>("NpcChat.QuestBarks.RealPlayersOnly", true);
        g_QuestBarksGenerateMissing = sConfigMgr->GetOption<bool>("NpcChat.QuestBarks.GenerateMissing", false);
        g_QuestBarksSelectedOnly = sConfigMgr->GetOption<bool>("NpcChat.QuestBarks.SelectedOnly", true);
        g_QuestBarksTriggerDistance = sConfigMgr->GetOption<float>("NpcChat.QuestBarks.TriggerDistance", 12.0f);
        g_QuestBarksChancePct = sConfigMgr->GetOption<int32>("NpcChat.QuestBarks.ChancePct", 10);
        g_QuestBarksPlayerCooldownSec = sConfigMgr->GetOption<int32>("NpcChat.QuestBarks.PlayerCooldownSec", 900);
        g_QuestBarksNpcCooldownSec = sConfigMgr->GetOption<int32>("NpcChat.QuestBarks.NpcCooldownSec", 300);
        g_QuestBarksPairCooldownSec = sConfigMgr->GetOption<int32>("NpcChat.QuestBarks.PairCooldownSec", 1800);
        g_QuestBarksScanIntervalMs = sConfigMgr->GetOption<int32>("NpcChat.QuestBarks.ScanIntervalMs", 3000);
        g_QuestBarksMaxQuestsCheckedPerNpc = sConfigMgr->GetOption<int32>("NpcChat.QuestBarks.MaxQuestsCheckedPerNpc", 8);
        if (g_QuestBarksChancePct < 0)
            g_QuestBarksChancePct = 0;
        if (g_QuestBarksChancePct > 100)
            g_QuestBarksChancePct = 100;
        if (g_QuestBarksScanIntervalMs < 1000)
            g_QuestBarksScanIntervalMs = 1000;
        if (g_QuestBarksMaxQuestsCheckedPerNpc < 1)
            g_QuestBarksMaxQuestsCheckedPerNpc = 1;
    }

    NpcChat_ApiConfig BuildChatApiConfig()
    {
        NpcChat_ApiConfig cfg;
        cfg.baseUrl = g_BaseUrl;
        cfg.apiKey = g_ApiKey;
        cfg.model = g_Model;
        cfg.maxTokens = g_MaxTokens;
        cfg.temperature = g_Temperature;
        cfg.timeoutSec = g_TimeoutSec;
        cfg.extraParams = g_ExtraParams;
        return cfg;
    }

    NpcChat_ApiConfig BuildGenerationApiConfig(int maxTokenCap = 0)
    {
        NpcChat_ApiConfig cfg;
        cfg.baseUrl = g_GenerationBaseUrl.empty() ? g_BaseUrl : g_GenerationBaseUrl;
        cfg.apiKey = g_GenerationApiKey.empty() ? g_ApiKey : g_GenerationApiKey;
        cfg.model = g_GenerationModel.empty() ? g_Model : g_GenerationModel;
        cfg.maxTokens = maxTokenCap > 0 ? std::min(g_GeneratePromptMaxTokens, maxTokenCap) : g_GeneratePromptMaxTokens;
        cfg.temperature = g_GeneratePromptTemperature;
        cfg.timeoutSec = g_GenerationTimeoutSec > 0 ? g_GenerationTimeoutSec : g_TimeoutSec;
        cfg.extraParams = g_GenerationExtraParams.empty() ? g_ExtraParams : g_GenerationExtraParams;
        return cfg;
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

    struct GenBarkRequest
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

        std::string barkKind;         // relationship, hostile, trainer
        std::string extraInstruction;
        std::string outputPath;
    };

    struct QuestBarkQuestInfo
    {
        uint32_t questId = 0;
        int32_t  questLevel = 0;
        uint32_t minLevel = 0;
        std::string title;
        std::string details;
        std::string objectives;
        std::string requestItemsText;
        std::string offerRewardText;
    };

    struct GenQuestBarkRequest
    {
        uint64_t    playerGuidRaw = 0;
        uint64_t    npcGuidRaw = 0;
        uint32_t    npcEntry = 0;
        std::string npcName;
        std::string npcSubName;
        std::string roleStr;
        std::string zoneName;
        std::string playerName;
        uint8       playerRace = 0;
        uint8       playerClass = 0;
        uint8       playerLevel = 0;
        uint8       faction = 0;
        uint16      phase = 0;
        std::string questKey;
        std::vector<QuestBarkQuestInfo> quests;
        std::string extraInstruction;
        bool        notifyPlayer = true;
    };

    std::queue<ChatReply> g_ReplyQueue;
    std::mutex            g_ReplyMutex;
    std::queue<SystemMessage> g_SystemMessageQueue;
    std::mutex            g_SystemMessageMutex;

    // One global lock for all NPC history file IO. Good enough for a small
    // personal realm; prevents two workers from interleaving the same file.
    std::mutex            g_FileMutex;

    // Main-thread-only cooldowns for cached relationship barks.
    std::map<uint64_t, time_t> g_RelationshipBarkPlayerCooldownUntil;
    std::map<uint64_t, time_t> g_RelationshipBarkNpcCooldownUntil;
    std::map<std::string, time_t> g_RelationshipBarkPairCooldownUntil;

    // Main-thread-only cooldowns for cached hostile first-talk barks.
    std::map<uint64_t, time_t> g_HostileBarkPlayerCooldownUntil;
    std::map<uint64_t, time_t> g_HostileBarkNpcCooldownUntil;
    std::map<std::string, time_t> g_HostileBarkPairCooldownUntil;

    // Main-thread-only cooldowns for cached trainer barks.
    std::map<uint64_t, time_t> g_TrainerBarkPlayerCooldownUntil;
    std::map<uint64_t, time_t> g_TrainerBarkNpcCooldownUntil;
    std::map<std::string, time_t> g_TrainerBarkPairCooldownUntil;

    std::map<uint64_t, time_t> g_QuestBarkPlayerCooldownUntil;
    std::map<uint64_t, time_t> g_QuestBarkNpcCooldownUntil;
    std::map<std::string, time_t> g_QuestBarkPairCooldownUntil;
    std::map<std::string, time_t> g_QuestBarkGenerationCooldownUntil;
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

    std::string PersonalRelationshipFilePath(std::string const& playerName, uint64_t playerGuidRaw,
        std::string const& npcName, uint32_t entry)
    {
        return PersonalHistoryFolderPath(playerName, playerGuidRaw) + "/" +
            NpcHistoryBase(npcName, entry) + ".relationship";
    }

    std::string PersonalBarksFilePath(std::string const& playerName, uint64_t playerGuidRaw,
        std::string const& npcName, uint32_t entry)
    {
        return PersonalHistoryFolderPath(playerName, playerGuidRaw) + "/" +
            NpcHistoryBase(npcName, entry) + ".barks";
    }

    std::string SharedHostileBarksFilePath(std::string const& npcName, uint32_t entry)
    {
        return g_HistoryPath + "/shared/" + NpcHistoryBase(npcName, entry) + ".hostile_barks";
    }

    std::string SharedTrainerBarksFilePath(std::string const& npcName, uint32_t entry)
    {
        return g_HistoryPath + "/shared/" + NpcHistoryBase(npcName, entry) + ".trainer_barks";
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
            std::filesystem::create_directories(g_HistoryPath + "/quest_barks");

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

    std::map<std::string, std::string> LoadKeyValueFile(std::string const& path)
    {
        std::map<std::string, std::string> out;
        std::ifstream f(path);
        if (!f.is_open())
            return out;

        std::string line;
        while (std::getline(f, line))
        {
            line = TrimCopy(line);
            if (line.empty() || line[0] == '#')
                continue;

            size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;

            std::string key = ToLowerCopy(TrimCopy(line.substr(0, eq)));
            std::string value = TrimCopy(line.substr(eq + 1));
            if (!key.empty())
                out[key] = value;
        }

        return out;
    }

    bool WriteKeyValueFile(std::string const& path, std::map<std::string, std::string> const& kv)
    {
        std::ostringstream ss;

        auto writeIf = [&](char const* key)
            {
                auto it = kv.find(key);
                if (it != kv.end() && !TrimCopy(it->second).empty())
                    ss << key << "=" << TrimCopy(it->second) << "\n";
            };

        writeIf("score");
        writeIf("intimacy");
        writeIf("stance");
        writeIf("last_contact");
        writeIf("tags");
        writeIf("summary");

        for (auto const& pair : kv)
        {
            if (pair.first == "score" || pair.first == "intimacy" || pair.first == "stance" ||
                pair.first == "last_contact" || pair.first == "tags" || pair.first == "summary")
                continue;

            if (!TrimCopy(pair.second).empty())
                ss << pair.first << "=" << TrimCopy(pair.second) << "\n";
        }

        return WriteWholeTextFile(path, ss.str(), true);
    }

    std::vector<std::string> SplitCsvNames(std::string const& text)
    {
        std::vector<std::string> out;
        std::stringstream ss(text);
        std::string token;
        while (std::getline(ss, token, ','))
        {
            token = TrimCopy(token);
            if (!token.empty() && std::find(out.begin(), out.end(), token) == out.end())
                out.push_back(token);
        }
        return out;
    }

    std::string JoinCsvNames(std::vector<std::string> const& names)
    {
        std::string out;
        for (size_t i = 0; i < names.size(); ++i)
        {
            if (i)
                out += ",";
            out += names[i];
        }
        return out;
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

    int ToIntOrDefault(std::string const& text, int def = 0)
    {
        try
        {
            std::string t = TrimCopy(text);
            if (t.empty())
                return def;
            return std::stoi(t);
        }
        catch (std::exception const&)
        {
            return def;
        }
    }

    std::string MapGet(std::map<std::string, std::string> const& kv, std::string const& key)
    {
        auto it = kv.find(ToLowerCopy(key));
        if (it == kv.end())
            return "";
        return TrimCopy(it->second);
    }

    std::string PlayerRaceName(uint8 race)
    {
        switch (race)
        {
        case 1:  return "Human";
        case 2:  return "Orc";
        case 3:  return "Dwarf";
        case 4:  return "Night Elf";
        case 5:  return "Undead";
        case 6:  return "Tauren";
        case 7:  return "Gnome";
        case 8:  return "Troll";
        case 10: return "Blood Elf";
        case 11: return "Draenei";
        default: return "adventurer";
        }
    }

    std::string PlayerClassName(uint8 cls)
    {
        switch (cls)
        {
        case 1:  return "Warrior";
        case 2:  return "Paladin";
        case 3:  return "Hunter";
        case 4:  return "Rogue";
        case 5:  return "Priest";
        case 6:  return "Death Knight";
        case 7:  return "Shaman";
        case 8:  return "Mage";
        case 9:  return "Warlock";
        case 11: return "Druid";
        default: return "adventurer";
        }
    }

    std::string PlayerFactionName(Player const* player)
    {
        if (!player)
            return "Neutral";

        switch (player->getRace())
        {
        case 1: case 3: case 4: case 7: case 11:
            return "Alliance";
        case 2: case 5: case 6: case 8: case 10:
            return "Horde";
        default:
            return "Neutral";
        }
    }

    std::string GenderPronoun(Player const* player, char const* male, char const* female, char const* fallback)
    {
        if (!player)
            return fallback;
        return player->getGender() == 1 ? female : male;
    }

    void ReplaceAllInPlace(std::string& text, std::string const& from, std::string const& to)
    {
        if (from.empty())
            return;
        size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos)
        {
            text.replace(pos, from.length(), to);
            pos += to.length();
        }
    }

    std::string ApplyPlayerPlaceholders(std::string text, Player const* player, Creature const* npc)
    {
        std::string playerName = player ? player->GetName() : "traveler";
        std::string race = player ? PlayerRaceName(player->getRace()) : "adventurer";
        std::string cls = player ? PlayerClassName(player->getClass()) : "adventurer";
        std::string level = player ? std::to_string(player->GetLevel()) : "0";
        std::string faction = PlayerFactionName(player);
        std::string npcName = npc ? npc->GetName() : "the NPC";
        std::string zone = "this place";
        if (player)
        {
            if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(player->GetAreaId()))
                zone = area->area_name[0];
        }

        ReplaceAllInPlace(text, "{player}", playerName);
        ReplaceAllInPlace(text, "{name}", playerName);
        ReplaceAllInPlace(text, "{race}", race);
        ReplaceAllInPlace(text, "{class}", cls);
        ReplaceAllInPlace(text, "{level}", level);
        ReplaceAllInPlace(text, "{faction}", faction);
        ReplaceAllInPlace(text, "{npc}", npcName);
        ReplaceAllInPlace(text, "{zone}", zone);
        ReplaceAllInPlace(text, "{he_she}", GenderPronoun(player, "he", "she", "they"));
        ReplaceAllInPlace(text, "{him_her}", GenderPronoun(player, "him", "her", "them"));
        ReplaceAllInPlace(text, "{his_her}", GenderPronoun(player, "his", "her", "their"));
        ReplaceAllInPlace(text, "{sir_miss}", GenderPronoun(player, "sir", "miss", "traveler"));
        return text;
    }

    std::string FirstBark(std::map<std::string, std::string> const& barks, std::vector<std::string> const& keys, bool& forcePrivate)
    {
        for (std::string const& rawKey : keys)
        {
            std::string key = ToLowerCopy(rawKey);
            std::string text = MapGet(barks, key);
            if (text.empty())
                continue;

            forcePrivate = key.rfind("private_", 0) == 0;
            return text;
        }

        return "";
    }

    std::string SelectRelationshipBark(std::map<std::string, std::string> const& relationship,
        std::map<std::string, std::string> const& barks, bool& forcePrivate)
    {
        forcePrivate = false;

        std::string stance = ToLowerCopy(MapGet(relationship, "stance"));
        std::string tags = ToLowerCopy(MapGet(relationship, "tags"));
        int intimacy = ToIntOrDefault(MapGet(relationship, "intimacy"), 0);
        int score = ToIntOrDefault(MapGet(relationship, "score"), 0);

        bool hostile = stance.find("hostile") != std::string::npos || stance.find("enemy") != std::string::npos ||
            tags.find("hostile") != std::string::npos || tags.find("killed_player") != std::string::npos || score <= -30;

        bool friendly = stance.find("friendly") != std::string::npos || stance.find("friend") != std::string::npos ||
            stance.find("affection") != std::string::npos || stance.find("close") != std::string::npos || score >= 30;

        bool wary = stance.find("wary") != std::string::npos || stance.find("suspicious") != std::string::npos ||
            tags.find("suspicious") != std::string::npos || score < 0;

        if (hostile)
        {
            if (intimacy >= 2)
                return FirstBark(barks, { "private_hostile", "public_hostile", "hostile", "private_wary", "public_wary", "general" }, forcePrivate);
            return FirstBark(barks, { "public_hostile", "private_hostile", "hostile", "public_wary", "private_wary", "general" }, forcePrivate);
        }

        if (friendly)
        {
            if (intimacy >= 4)
                return FirstBark(barks, { "private_close", "private_friendly", "public_friendly", "friendly", "general" }, forcePrivate);
            if (intimacy >= 2)
                return FirstBark(barks, { "public_friendly", "private_friendly", "private_close", "friendly", "general" }, forcePrivate);
            return FirstBark(barks, { "public_friendly", "friendly", "public_neutral", "general" }, forcePrivate);
        }

        if (wary)
        {
            if (intimacy >= 3)
                return FirstBark(barks, { "private_wary", "public_wary", "private_neutral", "public_neutral", "general" }, forcePrivate);
            return FirstBark(barks, { "public_wary", "private_wary", "public_neutral", "general" }, forcePrivate);
        }

        if (intimacy >= 4)
            return FirstBark(barks, { "private_close", "private_neutral", "public_neutral", "general" }, forcePrivate);

        return FirstBark(barks, { "public_neutral", "neutral", "public_friendly", "general" }, forcePrivate);
    }

    std::string RelationshipPairKey(Player const* player, Creature const* npc)
    {
        if (!player || !npc)
            return "";

        return std::to_string(player->GetGUID().GetRawValue()) + ":" + std::to_string(npc->GetEntry());
    }

    bool SpeakCachedRelationshipBark(Player* player, Creature* npc, bool bypassChanceAndCooldown, std::string* reason = nullptr)
    {
        if (!player || !npc || !npc->IsAlive())
        {
            if (reason) *reason = "No valid living NPC target.";
            return false;
        }

        WorldSession* session = player->GetSession();
        if (!session)
        {
            if (reason) *reason = "No player session.";
            return false;
        }

        if (g_RelationshipBarksRealPlayersOnly && session->IsBot())
        {
            if (reason) *reason = "Bots do not trigger relationship barks.";
            return false;
        }

        if (!bypassChanceAndCooldown && player->IsInCombat())
        {
            if (reason) *reason = "Player is in combat.";
            return false;
        }

        if (!player->IsWithinDist(npc, g_RelationshipBarksTriggerDistance, true))
        {
            if (reason) *reason = "Target NPC is outside relationship bark range.";
            return false;
        }

        time_t now = std::time(nullptr);
        uint64_t playerKey = player->GetGUID().GetRawValue();
        uint64_t npcKey = npc->GetGUID().GetRawValue();
        std::string pairKey = RelationshipPairKey(player, npc);

        if (!bypassChanceAndCooldown)
        {
            if (g_RelationshipBarkPlayerCooldownUntil[playerKey] > now)
            {
                if (reason) *reason = "Player cooldown is still active.";
                return false;
            }

            if (g_RelationshipBarkNpcCooldownUntil[npcKey] > now)
            {
                if (reason) *reason = "NPC cooldown is still active.";
                return false;
            }

            if (!pairKey.empty() && g_RelationshipBarkPairCooldownUntil[pairKey] > now)
            {
                if (reason) *reason = "Player/NPC pair cooldown is still active.";
                return false;
            }

            if (g_RelationshipBarksChancePct <= 0)
            {
                if (reason) *reason = "Chance is set to 0.";
                return false;
            }

            if (g_RelationshipBarksChancePct < 100 && (std::rand() % 100) >= g_RelationshipBarksChancePct)
            {
                if (reason) *reason = "Chance roll did not fire.";
                return false;
            }
        }

        std::map<std::string, std::string> relationship;
        std::map<std::string, std::string> barks;
        std::string relationshipPath = PersonalRelationshipFilePath(player->GetName(), playerKey, npc->GetName(), npc->GetEntry());
        std::string barksPath = PersonalBarksFilePath(player->GetName(), playerKey, npc->GetName(), npc->GetEntry());

        {
            std::lock_guard<std::mutex> lock(g_FileMutex);
            relationship = LoadKeyValueFile(relationshipPath);
            barks = LoadKeyValueFile(barksPath);
        }

        if (relationship.empty())
        {
            if (reason) *reason = "No relationship memory file exists for this player/NPC.";
            return false;
        }

        if (barks.empty())
        {
            if (reason) *reason = g_RelationshipBarksGenerateMissing ?
                "No cached bark file exists. Automatic generation is not implemented in this safe pass." :
                "No cached bark file exists. Use .npcc gen bark relationship first.";
            return false;
        }

        bool forcePrivate = false;
        std::string bark = SelectRelationshipBark(relationship, barks, forcePrivate);
        if (bark.empty())
        {
            if (reason) *reason = "Cached bark file exists, but no suitable bark key matched relationship state.";
            return false;
        }

        bark = ApplyPlayerPlaceholders(bark, player, npc);

        if (forcePrivate)
            npc->Say(bark, LANG_UNIVERSAL, player);
        else
            npc->Say(bark, LANG_UNIVERSAL);

        if (!bypassChanceAndCooldown)
        {
            g_RelationshipBarkPlayerCooldownUntil[playerKey] = now + std::max(1, g_RelationshipBarksPlayerCooldownSec);
            g_RelationshipBarkNpcCooldownUntil[npcKey] = now + std::max(1, g_RelationshipBarksNpcCooldownSec);
            if (!pairKey.empty())
                g_RelationshipBarkPairCooldownUntil[pairKey] = now + std::max(1, g_RelationshipBarksPairCooldownSec);
        }

        if (reason) *reason = "Relationship bark spoken.";
        return true;
    }
}

namespace
{
    char const* CreatureTypeStr(uint32 t);

    bool CanSpeakCreatureType(std::string const& type)
    {
        std::string t = ToLowerCopy(type);
        return t == "humanoid" || t == "undead" || t == "demon" ||
            t == "dragonkin" || t == "giant" || t == "elemental";
    }

    bool IsEliteLikeRank(uint32 rank)
    {
        return rank == 1 || rank == 2 || rank == 3;
    }

    std::string HostilePairKey(Player const* player, Creature const* npc)
    {
        if (!player || !npc)
            return "";

        return std::to_string(player->GetGUID().GetRawValue()) + ":hostile:" + std::to_string(npc->GetEntry());
    }

    std::string SelectHostileBark(std::map<std::string, std::string> const& barks, Creature const* npc)
    {
        if (npc && npc->IsInCombat())
        {
            float hp = UnitHealthPct(npc);
            if (hp <= 30.0f)
            {
                bool ignoredPrivate = false;
                std::string low = FirstBark(barks, { "low_health", "wounded", "desperate", "combat_taunt", "aggro_intro", "general" }, ignoredPrivate);
                if (!low.empty())
                    return low;
            }

            bool ignoredPrivate = false;
            std::string combat = FirstBark(barks, { "combat_taunt", "aggro_intro", "threat", "general" }, ignoredPrivate);
            if (!combat.empty())
                return combat;
        }

        bool ignoredPrivate = false;
        return FirstBark(barks, { "aggro_intro", "public_hostile", "hostile", "threat", "warning", "general" }, ignoredPrivate);
    }

    bool SpeakCachedHostileBark(Player* player, Creature* npc, bool bypassChanceAndCooldown, std::string* reason = nullptr)
    {
        if (!player || !npc || !npc->IsAlive())
        {
            if (reason) *reason = "No valid living NPC target.";
            return false;
        }

        WorldSession* session = player->GetSession();
        if (!session)
        {
            if (reason) *reason = "No player session.";
            return false;
        }

        if (g_HostileFirstTalkRealPlayersOnly && session->IsBot())
        {
            if (reason) *reason = "Bots do not trigger hostile first-talk barks.";
            return false;
        }

        if (!npc->IsHostileTo(player))
        {
            if (reason) *reason = "Target NPC is not hostile to this player.";
            return false;
        }

        if (!g_HostileFirstTalkAllowInCombat && (player->IsInCombat() || npc->IsInCombat()) && !bypassChanceAndCooldown)
        {
            if (reason) *reason = "Hostile first-talk is disabled during combat.";
            return false;
        }

        if (!player->IsWithinDist(npc, g_HostileFirstTalkTriggerDistance, true))
        {
            if (reason) *reason = "Target NPC is outside hostile first-talk range.";
            return false;
        }

        uint32 rank = 0;
        std::string creatureType = CreatureTypeStr(npc->GetCreatureType());
        if (CreatureTemplate const* ct = npc->GetCreatureTemplate())
        {
            rank = ct->rank;
            if (creatureType.empty())
                creatureType = CreatureTypeStr(ct->type);
        }

        if (g_HostileFirstTalkElitesOnly && !IsEliteLikeRank(rank))
        {
            if (reason) *reason = "Hostile first-talk is configured for elite/rare elite/world boss NPCs only.";
            return false;
        }

        if (!CanSpeakCreatureType(creatureType))
        {
            if (reason) *reason = "Target creature type is not configured as naturally speaking.";
            return false;
        }

        time_t now = std::time(nullptr);
        uint64_t playerKey = player->GetGUID().GetRawValue();
        uint64_t npcKey = npc->GetGUID().GetRawValue();
        std::string pairKey = HostilePairKey(player, npc);

        if (!bypassChanceAndCooldown)
        {
            if (g_HostileBarkPlayerCooldownUntil[playerKey] > now)
            {
                if (reason) *reason = "Player hostile bark cooldown is still active.";
                return false;
            }
            if (g_HostileBarkNpcCooldownUntil[npcKey] > now)
            {
                if (reason) *reason = "NPC hostile bark cooldown is still active.";
                return false;
            }
            if (!pairKey.empty() && g_HostileBarkPairCooldownUntil[pairKey] > now)
            {
                if (reason) *reason = "Player/NPC hostile pair cooldown is still active.";
                return false;
            }
            if (g_HostileFirstTalkChancePct <= 0)
            {
                if (reason) *reason = "Chance is set to 0.";
                return false;
            }
            if (g_HostileFirstTalkChancePct < 100 && (std::rand() % 100) >= g_HostileFirstTalkChancePct)
            {
                if (reason) *reason = "Chance roll did not fire.";
                return false;
            }
        }

        std::map<std::string, std::string> barks;
        std::string barksPath = SharedHostileBarksFilePath(npc->GetName(), npc->GetEntry());
        {
            std::lock_guard<std::mutex> lock(g_FileMutex);
            barks = LoadKeyValueFile(barksPath);
        }

        if (barks.empty())
        {
            if (reason) *reason = g_HostileFirstTalkGenerateMissing ?
                "No cached hostile bark file exists. Automatic generation is not implemented in this safe pass." :
                "No cached hostile bark file exists. Use .npcc gen bark hostile first.";
            return false;
        }

        std::string bark = SelectHostileBark(barks, npc);
        if (bark.empty())
        {
            if (reason) *reason = "Cached hostile bark file exists, but no suitable bark key was found.";
            return false;
        }

        bark = ApplyPlayerPlaceholders(bark, player, npc);
        npc->Say(bark, LANG_UNIVERSAL);

        if (!bypassChanceAndCooldown)
        {
            g_HostileBarkPlayerCooldownUntil[playerKey] = now + std::max(1, g_HostileFirstTalkPlayerCooldownSec);
            g_HostileBarkNpcCooldownUntil[npcKey] = now + std::max(1, g_HostileFirstTalkNpcCooldownSec);
            if (!pairKey.empty())
                g_HostileBarkPairCooldownUntil[pairKey] = now + std::max(1, g_HostileFirstTalkPairCooldownSec);
        }

        if (reason) *reason = "Hostile bark spoken.";
        return true;
    }
}

namespace
{
    bool IsTrainerNpc(Creature const* npc)
    {
        if (!npc)
            return false;
        if (CreatureTemplate const* ct = npc->GetCreatureTemplate())
            return (ct->npcflag & UNIT_NPC_FLAG_TRAINER) != 0;
        return false;
    }

    std::string TrainerPairKey(Player const* player, Creature const* npc)
    {
        if (!player || !npc)
            return "";
        return std::to_string(player->GetGUID().GetRawValue()) + ":trainer:" + std::to_string(npc->GetEntry());
    }

    std::string SelectTrainerBark(std::map<std::string, std::string> const& barks)
    {
        bool ignoredPrivate = false;
        return FirstBark(barks, { "available", "trainer_available", "class_available", "profession_available", "general" }, ignoredPrivate);
    }

    bool SpeakCachedTrainerBark(Player* player, Creature* npc, bool bypassChanceAndCooldown, std::string* reason = nullptr)
    {
        if (!player || !npc || !npc->IsAlive())
        {
            if (reason) *reason = "No valid living NPC target.";
            return false;
        }
        WorldSession* session = player->GetSession();
        if (!session)
        {
            if (reason) *reason = "No player session.";
            return false;
        }
        if (g_TrainerBarksRealPlayersOnly && session->IsBot())
        {
            if (reason) *reason = "Bots do not trigger trainer barks.";
            return false;
        }
        if (!IsTrainerNpc(npc))
        {
            if (reason) *reason = "Target NPC is not a trainer.";
            return false;
        }
        if (!player->IsWithinDist(npc, g_TrainerBarksTriggerDistance, true))
        {
            if (reason) *reason = "Target trainer is outside trainer bark range.";
            return false;
        }

        time_t now = std::time(nullptr);
        uint64_t playerKey = player->GetGUID().GetRawValue();
        uint64_t npcKey = npc->GetGUID().GetRawValue();
        std::string pairKey = TrainerPairKey(player, npc);
        if (!bypassChanceAndCooldown)
        {
            if (player->IsInCombat())
            {
                if (reason) *reason = "Player is in combat.";
                return false;
            }
            if (g_TrainerBarkPlayerCooldownUntil[playerKey] > now)
            {
                if (reason) *reason = "Player trainer bark cooldown is still active.";
                return false;
            }
            if (g_TrainerBarkNpcCooldownUntil[npcKey] > now)
            {
                if (reason) *reason = "Trainer NPC cooldown is still active.";
                return false;
            }
            if (!pairKey.empty() && g_TrainerBarkPairCooldownUntil[pairKey] > now)
            {
                if (reason) *reason = "Player/trainer pair cooldown is still active.";
                return false;
            }
            if (g_TrainerBarksChancePct <= 0)
            {
                if (reason) *reason = "Chance is set to 0.";
                return false;
            }
            if (g_TrainerBarksChancePct < 100 && (std::rand() % 100) >= g_TrainerBarksChancePct)
            {
                if (reason) *reason = "Chance roll did not fire.";
                return false;
            }
        }

        std::map<std::string, std::string> barks;
        std::string barksPath = SharedTrainerBarksFilePath(npc->GetName(), npc->GetEntry());
        {
            std::lock_guard<std::mutex> lock(g_FileMutex);
            barks = LoadKeyValueFile(barksPath);
        }
        if (barks.empty())
        {
            if (reason) *reason = g_TrainerBarksGenerateMissing ?
                "No cached trainer bark file exists. Automatic generation is not implemented in this safe pass." :
                "No cached trainer bark file exists. Use .npcc gen bark trainer first.";
            return false;
        }
        std::string bark = SelectTrainerBark(barks);
        if (bark.empty())
        {
            if (reason) *reason = "Cached trainer bark file exists, but no suitable bark key was found.";
            return false;
        }

        bark = ApplyPlayerPlaceholders(bark, player, npc);
        npc->Say(bark, LANG_UNIVERSAL);
        if (!bypassChanceAndCooldown)
        {
            g_TrainerBarkPlayerCooldownUntil[playerKey] = now + std::max(1, g_TrainerBarksPlayerCooldownSec);
            g_TrainerBarkNpcCooldownUntil[npcKey] = now + std::max(1, g_TrainerBarksNpcCooldownSec);
            if (!pairKey.empty())
                g_TrainerBarkPairCooldownUntil[pairKey] = now + std::max(1, g_TrainerBarksPairCooldownSec);
        }
        if (reason) *reason = "Trainer bark spoken.";
        return true;
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
        std::string const& personalPrompt,
        std::string const& relationshipText)
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

        if (!relationshipText.empty())
        {
            ss << "\n\nPersonal relationship memory for " << req.playerName << ":\n";
            ss << relationshipText;
            ss << "\nUse this relationship memory naturally. Do not mention that it came from a file or memory system.";
            ss << "\nIf the relationship stance, score, tags, or summary indicate distrust, resentment, hostility, affection, familiarity, or fear, let that personal attitude noticeably color the NPC's tone while still preserving the NPC's core public identity.";
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

    NpcChat_ApiConfig cfg = BuildGenerationApiConfig();

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


std::string BuildGenerateRelationshipBarksSystemPrompt()
{
    return
        "You are creating reusable cached bark lines for a World of Warcraft NPC relationship system. "
        "Output ONLY key=value lines. Do not use markdown. Do not use quotes around the values. "
        "Each value must be one short in-character spoken line, suitable for an NPC to say in-game when the player walks nearby. "
        "Do not mention AI, files, prompts, players, servers, tokens, or game mechanics. "
        "Use the relationship context. Public lines can be overheard; private lines should feel more personal. "
        "Use reusable placeholders when useful: {player}, {race}, {class}, {level}, {faction}, {npc}, {zone}, {sir_miss}. "
        "Prefer placeholders over hardcoding the player name, race, or class. "
        "Use these exact keys when relevant: public_friendly, private_friendly, public_neutral, private_neutral, public_hostile, private_hostile, public_close, private_close.";
}

std::string BuildGenerateRelationshipBarksUserPrompt(GenBarkRequest const& req,
    std::string const& sharedPrompt,
    std::string const& personalPrompt,
    std::string const& relationshipText,
    std::deque<std::string> const& personalHistory,
    std::string const& existingBarks)
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
        ss << "- Role: " << req.roleStr << "\n";
    if (!req.zoneName.empty())
        ss << "- Zone: " << req.zoneName << "\n";
    if (!req.stance.empty())
        ss << "- Current stance toward player: " << req.stance << "\n";
    if (req.isHostile)
        ss << "- Hostile to this player: yes\n";

    if (!sharedPrompt.empty())
    {
        ss << "\nShared NPC character prompt:\n";
        ss << sharedPrompt << "\n";
    }

    if (!personalPrompt.empty())
    {
        ss << "\nPersonal NPC prompt for " << req.playerName << ":\n";
        ss << personalPrompt << "\n";
    }

    if (!relationshipText.empty())
    {
        ss << "\nRelationship memory with " << req.playerName << ":\n";
        ss << relationshipText << "\n";
    }

    if (!personalHistory.empty())
    {
        ss << "\nRecent one-on-one history:\n";
        for (std::string const& l : personalHistory)
            ss << l << "\n";
    }

    if (!existingBarks.empty())
    {
        ss << "\nExisting bark file, if improving/replacing it:\n";
        ss << existingBarks << "\n";
    }

    if (!req.extraInstruction.empty())
    {
        ss << "\nExtra direction from the player:\n";
        ss << req.extraInstruction << "\n";
    }

    ss << "\nGenerate a compact bark set now. Include only lines that fit this NPC and relationship. "
        "If a key does not fit, omit it. Keep each bark under about 22 words.";

    return ss.str();
}

void GenerateRelationshipBarksWorker(GenBarkRequest req)
{
    std::string sharedPrompt;
    std::string personalPrompt;
    std::string relationshipText;
    std::string existingBarks;
    std::deque<std::string> personalHistory;

    {
        std::lock_guard<std::mutex> lock(g_FileMutex);
        EnsureNpcChatDirectoriesAndDefaultPrompt();

        sharedPrompt = ReadWholeTextFile(SharedPromptFilePath(req.npcName, req.npcEntry));
        personalPrompt = ReadWholeTextFile(PersonalPromptFilePath(req.playerName, req.playerGuidRaw, req.npcName, req.npcEntry));
        relationshipText = ReadWholeTextFile(PersonalRelationshipFilePath(req.playerName, req.playerGuidRaw, req.npcName, req.npcEntry));
        existingBarks = ReadWholeTextFile(req.outputPath);
        personalHistory = LoadHistoryTail(PersonalHistoryFilePath(req.playerName, req.playerGuidRaw, req.npcName, req.npcEntry), std::min(g_PersonalHistoryTail, 10));
    }

    if (relationshipText.empty() && personalPrompt.empty() && personalHistory.empty() && req.extraInstruction.empty())
    {
        QueueSystemMessage(req.playerGuidRaw, "NPC Chat relationship bark generation needs relationship memory, personal prompt/history, or extra direction.");
        return;
    }

    NpcChat_ApiConfig cfg = BuildGenerationApiConfig(550);

    NpcChat_LLMResult res = NpcChat_CallLLM(
        cfg,
        BuildGenerateRelationshipBarksSystemPrompt(),
        BuildGenerateRelationshipBarksUserPrompt(req, sharedPrompt, personalPrompt, relationshipText, personalHistory, existingBarks));

    if (!res.success || TrimCopy(res.text).empty())
    {
        QueueSystemMessage(req.playerGuidRaw, "NPC Chat relationship bark generation failed or returned empty text.");
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

    {
        std::lock_guard<std::mutex> lock(g_FileMutex);
        EnsureNpcChatDirectoriesAndDefaultPrompt();
        if (!WriteWholeTextFile(req.outputPath, generated, true))
        {
            QueueSystemMessage(req.playerGuidRaw, "NPC Chat generated relationship barks but failed to save the file.");
            return;
        }
    }

    QueueSystemMessage(req.playerGuidRaw, "NPC Chat relationship barks saved: " + req.outputPath);
}

std::string BuildGenerateHostileBarksSystemPrompt()
{
    return
        "You are creating reusable cached hostile first-talk bark lines for an intelligent elite World of Warcraft enemy. "
        "Output ONLY key=value lines. Do not use markdown. Do not use quotes around the values. "
        "Each value must be one short in-character spoken line suitable for the enemy to say when a real player approaches. "
        "Do not mention AI, files, prompts, players, servers, tokens, aggro tables, or game mechanics. "
        "Use reusable placeholders when useful: {player}, {race}, {class}, {level}, {faction}, {npc}, {zone}, {sir_miss}. "
        "Prefer placeholders over hardcoding the player name, race, or class. "
        "Use these exact keys when relevant: aggro_intro, warning, threat, combat_taunt, low_health, victory, general.";
}

std::string BuildGenerateHostileBarksUserPrompt(GenBarkRequest const& req, std::string const& sharedPrompt, std::string const& sharedSubPrompts, std::string const& existingBarks)
{
    std::ostringstream ss;
    ss << "Hostile NPC facts:\n";
    ss << "- Name: " << req.npcName << "\n";
    ss << "- Entry ID: " << req.npcEntry << "\n";
    if (!req.npcSubName.empty()) ss << "- Subname/title: " << req.npcSubName << "\n";
    if (req.npcLevel) ss << "- Level: " << req.npcLevel << "\n";
    if (!req.gender.empty()) ss << "- Gender: " << req.gender << "\n";
    if (!req.creatureType.empty()) ss << "- Creature type: " << req.creatureType << "\n";
    if (!req.rankStr.empty()) ss << "- Rank: " << req.rankStr << "\n";
    if (!req.roleStr.empty()) ss << "- Role: " << req.roleStr << "\n";
    if (!req.zoneName.empty()) ss << "- Zone: " << req.zoneName << "\n";
    ss << "- Hostile to the player: " << (req.isHostile ? "yes" : "no") << "\n";
    if (!sharedSubPrompts.empty()) ss << "\nAttached shared sub-prompts:\n" << sharedSubPrompts << "\n";
    if (!sharedPrompt.empty()) ss << "\nShared NPC character prompt:\n" << sharedPrompt << "\n";
    if (!existingBarks.empty()) ss << "\nExisting hostile bark file, if improving/replacing it:\n" << existingBarks << "\n";
    if (!req.extraInstruction.empty()) ss << "\nExtra direction from the player/worldbuilder:\n" << req.extraInstruction << "\n";
    ss << "\nGenerate a compact hostile bark set now. Keep each value under about 25 words. The aggro_intro line is the main pre-combat approach line.";
    return ss.str();
}

void GenerateHostileBarksWorker(GenBarkRequest req)
{
    std::string sharedPrompt;
    std::string sharedSubPrompts;
    std::string existingBarks;
    {
        std::lock_guard<std::mutex> lock(g_FileMutex);
        EnsureNpcChatDirectoriesAndDefaultPrompt();
        sharedPrompt = ReadWholeTextFile(SharedPromptFilePath(req.npcName, req.npcEntry));
        sharedSubPrompts = LoadSubPromptBlocks(LoadSubPromptNameList(SharedSubPromptListFilePath(req.npcName, req.npcEntry)));
        existingBarks = ReadWholeTextFile(req.outputPath);
    }
    if (sharedPrompt.empty() && sharedSubPrompts.empty() && req.extraInstruction.empty())
    {
        QueueSystemMessage(req.playerGuidRaw, "NPC Chat hostile bark generation needs a shared prompt, shared sub-prompts, or extra direction.");
        return;
    }
    NpcChat_ApiConfig cfg = BuildGenerationApiConfig(450);
    NpcChat_LLMResult res = NpcChat_CallLLM(cfg, BuildGenerateHostileBarksSystemPrompt(), BuildGenerateHostileBarksUserPrompt(req, sharedPrompt, sharedSubPrompts, existingBarks));
    if (!res.success || TrimCopy(res.text).empty())
    {
        QueueSystemMessage(req.playerGuidRaw, "NPC Chat hostile bark generation failed or returned empty text.");
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
    {
        std::lock_guard<std::mutex> lock(g_FileMutex);
        EnsureNpcChatDirectoriesAndDefaultPrompt();
        if (!WriteWholeTextFile(req.outputPath, generated, true))
        {
            QueueSystemMessage(req.playerGuidRaw, "NPC Chat generated hostile barks but failed to save the file.");
            return;
        }
    }
    QueueSystemMessage(req.playerGuidRaw, "NPC Chat hostile barks saved: " + req.outputPath);
}


// ===========================================================================
// Quest bark cache / generation
// ===========================================================================
namespace
{
    std::string SqlEscape(std::string s)
    {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s)
        {
            if (c == '\\' || c == '\'')
                out += '\\';
            out += c;
        }
        return out;
    }

    void EnsureQuestBarkCacheTable()
    {
        WorldDatabase.Execute(
            "CREATE TABLE IF NOT EXISTS `npcchat_quest_bark_cache` ("
            "`id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
            "`npc_entry` INT UNSIGNED NOT NULL,"
            "`quest_key` VARCHAR(128) NOT NULL,"
            "`quest_ids` VARCHAR(128) NOT NULL,"
            "`faction` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "`race_id` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "`class_id` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "`phase` SMALLINT UNSIGNED NOT NULL DEFAULT 0,"
            "`bark_type` VARCHAR(32) NOT NULL DEFAULT 'quest_available',"
            "`text` TEXT NOT NULL,"
            "`created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            "`updated_at` TIMESTAMP NULL DEFAULT NULL ON UPDATE CURRENT_TIMESTAMP,"
            "PRIMARY KEY (`id`),"
            "UNIQUE KEY `uq_npcchat_quest_bark` (`npc_entry`, `quest_key`, `faction`, `race_id`, `class_id`, `phase`, `bark_type`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    }

    uint8 PlayerFactionId(Player const* player)
    {
        if (!player)
            return 0;
        std::string f = PlayerFactionName(player);
        if (f == "Alliance") return 1;
        if (f == "Horde") return 2;
        return 0;
    }

    std::string TruncateText(std::string text, size_t maxLen)
    {
        text = TrimCopy(text);
        if (text.size() <= maxLen)
            return text;
        if (maxLen <= 3)
            return text.substr(0, maxLen);
        return TrimCopy(text.substr(0, maxLen - 3)) + "...";
    }

    bool IsQuestGiverNpc(Creature const* npc)
    {
        if (!npc)
            return false;
        if (CreatureTemplate const* ct = npc->GetCreatureTemplate())
            return (ct->npcflag & UNIT_NPC_FLAG_QUESTGIVER) != 0;
        return false;
    }

    std::string QuestPairKey(Player const* player, Creature const* npc)
    {
        if (!player || !npc)
            return "";
        return std::to_string(player->GetGUID().GetRawValue()) + ":quest:" + std::to_string(npc->GetEntry());
    }

    std::vector<uint32> GetNpcStarterQuestIds(uint32 npcEntry)
    {
        std::vector<uint32> ids;
        QueryResult result = WorldDatabase.Query("SELECT `quest` FROM `creature_queststarter` WHERE `id` = {} ORDER BY `quest`", npcEntry);
        if (!result)
            return ids;
        do
        {
            Field* fields = result->Fetch();
            uint32 q = fields[0].Get<uint32>();
            if (q && std::find(ids.begin(), ids.end(), q) == ids.end())
                ids.push_back(q);
        } while (result->NextRow());
        return ids;
    }

    std::vector<uint32> GetNearbyQuestgiverEntriesFromDb(Player const* player, float range, uint32 maxRows)
    {
        std::vector<uint32> entries;
        if (!player || maxRows == 0)
            return entries;

        // Low-risk first pass for "walk near a questgiver":
        // use the creature spawn table to find questgiver entries near the player's current map/position,
        // then resolve those entries back to live creatures with FindNearestCreature().
        //
        // This avoids sWorld->GetAllSessions() and avoids whole-grid creature iteration on branches
        // where the public APIs differ. For a private/single-player server this DB-assisted scan is cheap
        // enough at a multi-second interval.
        float x = player->GetPositionX();
        float y = player->GetPositionY();
        float z = player->GetPositionZ();
        float zRange = std::max(10.0f, range * 2.0f);

        std::ostringstream sql;
        sql << "SELECT c.`id`, MIN(((c.`position_x` - " << x << ") * (c.`position_x` - " << x << ") + "
            << "(c.`position_y` - " << y << ") * (c.`position_y` - " << y << ") + "
            << "(c.`position_z` - " << z << ") * (c.`position_z` - " << z << "))) AS `dist2` "
            << "FROM `creature` c "
            << "INNER JOIN `creature_queststarter` qs ON qs.`id` = c.`id` "
            << "WHERE c.`map` = " << player->GetMapId() << " "
            << "AND c.`position_x` BETWEEN " << (x - range) << " AND " << (x + range) << " "
            << "AND c.`position_y` BETWEEN " << (y - range) << " AND " << (y + range) << " "
            << "AND c.`position_z` BETWEEN " << (z - zRange) << " AND " << (z + zRange) << " "
            << "GROUP BY c.`id` "
            << "ORDER BY `dist2` ASC "
            << "LIMIT " << maxRows;

        QueryResult result = WorldDatabase.Query(sql.str().c_str());
        if (!result)
            return entries;

        do
        {
            Field* fields = result->Fetch();
            uint32 entry = fields[0].Get<uint32>();
            if (entry && std::find(entries.begin(), entries.end(), entry) == entries.end())
                entries.push_back(entry);
        } while (result->NextRow());

        return entries;
    }

    bool PlayerCanTakeQuestNow(Player* player, Quest const* quest)
    {
        if (!player || !quest)
            return false;
        uint32 questId = quest->GetQuestId();
        if (player->GetQuestRewardStatus(questId) && !quest->IsRepeatable())
            return false;
        QuestStatus status = player->GetQuestStatus(questId);
        if (status != QUEST_STATUS_NONE && status != QUEST_STATUS_FAILED)
            return false;
        if (!player->CanSeeStartQuest(quest))
            return false;
        return true;
    }

    QuestBarkQuestInfo BuildQuestInfo(Quest const* quest)
    {
        QuestBarkQuestInfo info;
        if (!quest)
            return info;
        info.questId = quest->GetQuestId();
        info.questLevel = quest->GetQuestLevel();
        info.minLevel = quest->GetMinLevel();
        info.title = TruncateText(quest->GetTitle(), 120);
        info.details = TruncateText(quest->GetDetails(), 500);
        info.objectives = TruncateText(quest->GetObjectives(), 350);
        info.requestItemsText = TruncateText(quest->GetRequestItemsText(), 220);
        info.offerRewardText = TruncateText(quest->GetOfferRewardText(), 220);
        return info;
    }

    std::vector<QuestBarkQuestInfo> GetAcceptableQuestInfos(Player* player, Creature* npc, uint32 onlyQuestId = 0)
    {
        std::vector<QuestBarkQuestInfo> out;
        if (!player || !npc || !IsQuestGiverNpc(npc))
            return out;
        std::vector<uint32> starterIds = GetNpcStarterQuestIds(npc->GetEntry());
        for (uint32 questId : starterIds)
        {
            if (onlyQuestId && questId != onlyQuestId)
                continue;
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!PlayerCanTakeQuestNow(player, quest))
                continue;
            out.push_back(BuildQuestInfo(quest));
            if (static_cast<int>(out.size()) >= g_QuestBarksMaxQuestsCheckedPerNpc)
                break;
        }
        return out;
    }

    std::string BuildQuestKey(std::vector<QuestBarkQuestInfo> quests)
    {
        std::vector<uint32> ids;
        for (QuestBarkQuestInfo const& q : quests)
            if (q.questId)
                ids.push_back(q.questId);
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        std::ostringstream ss;
        for (size_t i = 0; i < ids.size(); ++i)
        {
            if (i)
                ss << "_";
            ss << ids[i];
        }
        return ss.str();
    }

    std::string QuestIdsString(std::vector<QuestBarkQuestInfo> const& quests)
    {
        std::ostringstream ss;
        for (size_t i = 0; i < quests.size(); ++i)
        {
            if (i)
                ss << ",";
            ss << quests[i].questId;
        }
        return ss.str();
    }

    std::string LookupQuestBarkCache(uint32 npcEntry, std::string const& questKey, uint8 faction, uint8 race, uint8 cls, uint16 phase)
    {
        EnsureQuestBarkCacheTable();
        std::ostringstream sql;
        sql << "SELECT `text` FROM `npcchat_quest_bark_cache` "
            << "WHERE `npc_entry`=" << npcEntry
            << " AND `quest_key`='" << SqlEscape(questKey) << "'"
            << " AND `phase` IN (" << phase << ",0)"
            << " AND `bark_type`='quest_available'"
            << " AND `faction` IN (" << uint32(faction) << ",0)"
            << " AND `race_id` IN (" << uint32(race) << ",0)"
            << " AND `class_id` IN (" << uint32(cls) << ",0)"
            << " ORDER BY ((`faction`=" << uint32(faction) << ") + (`race_id`=" << uint32(race)
            << ") + (`class_id`=" << uint32(cls) << ") + (`phase`=" << phase << ")) DESC, `updated_at` DESC, `id` DESC LIMIT 1";
        QueryResult result = WorldDatabase.Query(sql.str().c_str());
        if (!result)
            return "";
        Field* fields = result->Fetch();
        return TrimCopy(fields[0].Get<std::string>());
    }

    bool SaveQuestBarkCache(GenQuestBarkRequest const& req, std::string const& text)
    {
        if (req.questKey.empty() || text.empty())
            return false;
        EnsureQuestBarkCacheTable();
        std::ostringstream sql;
        sql << "REPLACE INTO `npcchat_quest_bark_cache` "
            << "(`npc_entry`,`quest_key`,`quest_ids`,`faction`,`race_id`,`class_id`,`phase`,`bark_type`,`text`) VALUES ("
            << req.npcEntry << ","
            << "'" << SqlEscape(req.questKey) << "',"
            << "'" << SqlEscape(QuestIdsString(req.quests)) << "',"
            << uint32(req.faction) << ","
            << uint32(req.playerRace) << ","
            << uint32(req.playerClass) << ","
            << uint32(req.phase) << ","
            << "'quest_available',"
            << "'" << SqlEscape(text) << "')";
        WorldDatabase.Execute(sql.str().c_str());
        return true;
    }

    std::string BuildGenerateQuestBarkSystemPrompt()
    {
        return
            "You are creating one reusable cached quest-intro bark for a World of Warcraft questgiver. "
            "The bark will be reused by the server when an eligible real player approaches this NPC. "
            "Return ONLY the spoken line. No markdown, no quotes, no key names. "
            "Write one or two short in-character sentences. "
            "Do not mention AI, files, prompts, servers, tokens, quest IDs, gossip windows, SQL, or game mechanics. "
            "Do not summarize the full quest text; hook the player's attention naturally. "
            "If there are multiple available quests, make the bark imply the NPC has several urgent matters without listing them mechanically. "
            "Use reusable placeholders when useful: {player}, {race}, {class}, {level}, {faction}, {npc}, {zone}, {sir_miss}. "
            "Prefer placeholders over hardcoding the player name, race, or class.";
    }

    std::string BuildGenerateQuestBarkUserPrompt(GenQuestBarkRequest const& req, std::string const& sharedPrompt, std::string const& existingBark)
    {
        std::ostringstream ss;
        ss << "NPC facts:\n";
        ss << "- Name: " << req.npcName << "\n";
        ss << "- Entry ID: " << req.npcEntry << "\n";
        if (!req.npcSubName.empty()) ss << "- Subname/title: " << req.npcSubName << "\n";
        if (!req.roleStr.empty()) ss << "- Role: " << req.roleStr << "\n";
        if (!req.zoneName.empty()) ss << "- Zone: " << req.zoneName << "\n";
        ss << "\nPlayer context for placeholder planning:\n";
        ss << "- Player placeholder: {player}\n";
        ss << "- Race: " << PlayerRaceName(req.playerRace) << " ({race})\n";
        ss << "- Class: " << PlayerClassName(req.playerClass) << " ({class})\n";
        ss << "- Level: " << uint32(req.playerLevel) << " ({level})\n";
        ss << "- Faction: " << (req.faction == 1 ? "Alliance" : req.faction == 2 ? "Horde" : "Neutral") << " ({faction})\n";
        if (!sharedPrompt.empty())
            ss << "\nShared NPC character prompt:\n" << TruncateText(sharedPrompt, 900) << "\n";
        ss << "\nAvailable quest" << (req.quests.size() == 1 ? "" : "s") << " the player can accept right now:\n";
        for (QuestBarkQuestInfo const& q : req.quests)
        {
            ss << "\nQuest " << q.questId << ": " << q.title << "\n";
            ss << "- Quest level: " << q.questLevel << ", min level: " << q.minLevel << "\n";
            if (!q.details.empty()) ss << "- Details: " << q.details << "\n";
            if (!q.objectives.empty()) ss << "- Objectives: " << q.objectives << "\n";
            if (!q.requestItemsText.empty()) ss << "- Request/completion flavor: " << q.requestItemsText << "\n";
            if (!q.offerRewardText.empty()) ss << "- Reward/ending flavor: " << q.offerRewardText << "\n";
        }
        if (!existingBark.empty())
            ss << "\nExisting cached bark, if improving/replacing it:\n" << existingBark << "\n";
        if (!req.extraInstruction.empty())
            ss << "\nExtra direction from the player/worldbuilder:\n" << req.extraInstruction << "\n";
        ss << "\nWrite the final reusable quest approach bark now. Keep it under about 35 words.";
        return ss.str();
    }

    void GenerateQuestBarkWorker(GenQuestBarkRequest req)
    {
        if (req.quests.empty() || req.questKey.empty())
        {
            if (req.notifyPlayer)
                QueueSystemMessage(req.playerGuidRaw, "NPC Chat quest bark generation failed: no eligible quest context was captured.");
            return;
        }
        std::string sharedPrompt;
        std::string existingBark;
        {
            std::lock_guard<std::mutex> lock(g_FileMutex);
            EnsureNpcChatDirectoriesAndDefaultPrompt();
            sharedPrompt = ReadWholeTextFile(SharedPromptFilePath(req.npcName, req.npcEntry));
        }
        existingBark = LookupQuestBarkCache(req.npcEntry, req.questKey, req.faction, req.playerRace, req.playerClass, req.phase);
        NpcChat_ApiConfig cfg = BuildGenerationApiConfig(350);
        NpcChat_LLMResult res = NpcChat_CallLLM(cfg, BuildGenerateQuestBarkSystemPrompt(), BuildGenerateQuestBarkUserPrompt(req, sharedPrompt, existingBark));
        if (!res.success || TrimCopy(res.text).empty())
        {
            if (req.notifyPlayer)
                QueueSystemMessage(req.playerGuidRaw, "NPC Chat quest bark generation failed or returned empty text.");
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
        size_t eq = generated.find('=');
        if (eq != std::string::npos && eq < 32)
            generated = TrimCopy(generated.substr(eq + 1));
        generated = StripWrappingQuotes(generated);
        generated = TruncateText(generated, 360);
        if (!SaveQuestBarkCache(req, generated))
        {
            if (req.notifyPlayer)
                QueueSystemMessage(req.playerGuidRaw, "NPC Chat generated a quest bark but failed to save it to the DB cache.");
            return;
        }
        if (req.notifyPlayer)
            QueueSystemMessage(req.playerGuidRaw, "NPC Chat quest bark saved to DB cache for quest key " + req.questKey + ".");
    }

    GenQuestBarkRequest BuildQuestBarkRequest(Player* player, Creature* npc, std::vector<QuestBarkQuestInfo> quests, std::string const& extraInstruction, bool notify)
    {
        GenQuestBarkRequest req;
        req.playerGuidRaw = player->GetGUID().GetRawValue();
        req.npcGuidRaw = npc->GetGUID().GetRawValue();
        req.npcEntry = npc->GetEntry();
        req.npcName = npc->GetName();
        req.playerName = player->GetName();
        req.playerRace = player->getRace();
        req.playerClass = player->getClass();
        req.playerLevel = player->GetLevel();
        req.faction = PlayerFactionId(player);
        req.phase = 0;
        req.quests = std::move(quests);
        req.questKey = BuildQuestKey(req.quests);
        req.extraInstruction = extraInstruction;
        req.notifyPlayer = notify;
        if (CreatureTemplate const* ct = npc->GetCreatureTemplate())
        {
            req.npcSubName = ct->SubName;
            req.roleStr = RolesFromNpcFlags(ct->npcflag);
        }
        if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(npc->GetZoneId()))
            req.zoneName = zone->area_name[0];
        return req;
    }

    void MaybeGenerateMissingQuestBark(Player* player, Creature* npc, std::vector<QuestBarkQuestInfo> quests)
    {
        if (!g_QuestBarksGenerateMissing || quests.empty())
            return;
        GenQuestBarkRequest req = BuildQuestBarkRequest(player, npc, quests, "", false);
        if (req.questKey.empty())
            return;
        std::string genKey = std::to_string(req.npcEntry) + ":" + req.questKey + ":" + std::to_string(req.faction) + ":" + std::to_string(req.playerRace) + ":" + std::to_string(req.playerClass);
        time_t now = std::time(nullptr);
        if (g_QuestBarkGenerationCooldownUntil[genKey] > now)
            return;
        g_QuestBarkGenerationCooldownUntil[genKey] = now + 3600;
        std::thread(GenerateQuestBarkWorker, std::move(req)).detach();
    }

    Creature* FindNearbyQuestgiverForBark(Player* player)
    {
        if (!player || !player->IsInWorld() || player->IsInCombat())
            return nullptr;

        // Prefer the selected questgiver when it is valid; this keeps manual targeting predictable.
        if (Unit* selected = player->GetSelectedUnit())
        {
            if (Creature* selectedNpc = selected->ToCreature())
            {
                if (selectedNpc->IsInWorld() && selectedNpc->IsAlive() &&
                    player->IsWithinDist(selectedNpc, g_QuestBarksTriggerDistance, true) &&
                    IsQuestGiverNpc(selectedNpc) &&
                    !GetAcceptableQuestInfos(player, selectedNpc).empty())
                    return selectedNpc;
            }
        }

        if (g_QuestBarksSelectedOnly)
            return nullptr;

        std::vector<uint32> entries = GetNearbyQuestgiverEntriesFromDb(player, g_QuestBarksTriggerDistance, 32);
        Creature* best = nullptr;
        float bestDist = g_QuestBarksTriggerDistance + 1.0f;

        for (uint32 entry : entries)
        {
            Creature* candidate = player->FindNearestCreature(entry, g_QuestBarksTriggerDistance, true);
            if (!candidate || !candidate->IsInWorld() || !candidate->IsAlive())
                continue;
            if (!IsQuestGiverNpc(candidate))
                continue;
            if (!player->IsWithinDist(candidate, g_QuestBarksTriggerDistance, true))
                continue;
            if (GetAcceptableQuestInfos(player, candidate).empty())
                continue;

            float dist = player->GetDistance(candidate);
            if (!best || dist < bestDist)
            {
                best = candidate;
                bestDist = dist;
            }
        }

        return best;
    }

    bool SpeakCachedQuestBark(Player* player, Creature* npc, bool bypassChanceAndCooldown, uint32 onlyQuestId = 0, std::string* reason = nullptr)
    {
        if (!player || !npc || !npc->IsAlive())
        {
            if (reason) *reason = "No valid living NPC target.";
            return false;
        }
        WorldSession* session = player->GetSession();
        if (!session)
        {
            if (reason) *reason = "No player session.";
            return false;
        }
        if (g_QuestBarksRealPlayersOnly && session->IsBot())
        {
            if (reason) *reason = "Bots do not trigger quest barks.";
            return false;
        }
        if (!IsQuestGiverNpc(npc))
        {
            if (reason) *reason = "Target NPC is not a questgiver.";
            return false;
        }
        if (!player->IsWithinDist(npc, g_QuestBarksTriggerDistance, true))
        {
            if (reason) *reason = "Questgiver is outside quest bark range.";
            return false;
        }
        uint64 playerKey = player->GetGUID().GetRawValue();
        uint64 npcKey = npc->GetGUID().GetRawValue();
        std::string pairKey = QuestPairKey(player, npc);
        time_t now = std::time(nullptr);
        if (!bypassChanceAndCooldown)
        {
            if (player->IsInCombat()) { if (reason) *reason = "Player is in combat."; return false; }
            if (g_QuestBarkPlayerCooldownUntil[playerKey] > now) { if (reason) *reason = "Player quest bark cooldown is still active."; return false; }
            if (g_QuestBarkNpcCooldownUntil[npcKey] > now) { if (reason) *reason = "Questgiver cooldown is still active."; return false; }
            if (!pairKey.empty() && g_QuestBarkPairCooldownUntil[pairKey] > now) { if (reason) *reason = "Player/questgiver pair cooldown is still active."; return false; }
            if (g_QuestBarksChancePct <= 0) { if (reason) *reason = "Quest bark chance is 0."; return false; }
            if (g_QuestBarksChancePct < 100 && (std::rand() % 100) >= g_QuestBarksChancePct) { if (reason) *reason = "Quest bark chance roll did not pass."; return false; }
        }
        std::vector<QuestBarkQuestInfo> quests = GetAcceptableQuestInfos(player, npc, onlyQuestId);
        if (quests.empty())
        {
            if (reason) *reason = onlyQuestId ? "That quest is not currently acceptable for this player." : "No acceptable quests found for this player.";
            return false;
        }
        std::string questKey = BuildQuestKey(quests);
        std::string bark = LookupQuestBarkCache(npc->GetEntry(), questKey, PlayerFactionId(player), player->getRace(), player->getClass(), 0);
        if (bark.empty())
        {
            MaybeGenerateMissingQuestBark(player, npc, quests);
            if (reason) *reason = g_QuestBarksGenerateMissing ? "No cached quest bark existed. Generation was queued; try again after it saves." : "No cached quest bark exists. Use .npcc gen quest first.";
            return false;
        }
        bark = ApplyPlayerPlaceholders(bark, player, npc);
        npc->Say(bark, LANG_UNIVERSAL);
        if (!bypassChanceAndCooldown)
        {
            g_QuestBarkPlayerCooldownUntil[playerKey] = now + std::max(1, g_QuestBarksPlayerCooldownSec);
            g_QuestBarkNpcCooldownUntil[npcKey] = now + std::max(1, g_QuestBarksNpcCooldownSec);
            if (!pairKey.empty())
                g_QuestBarkPairCooldownUntil[pairKey] = now + std::max(1, g_QuestBarksPairCooldownSec);
        }
        if (reason) *reason = "Quest bark spoken.";
        return true;
    }
}

// ===========================================================================
// Worker
// ===========================================================================
namespace
{
    std::string BuildGenerateTrainerBarksSystemPrompt()
    {
        return
            "You are creating reusable cached trainer bark lines for a World of Warcraft NPC trainer. "
            "Output ONLY key=value lines. Do not use markdown. Do not use quotes around the values. "
            "Each value must be one short in-character spoken line suitable for a trainer to say when a real player approaches. "
            "Do not mention AI, files, prompts, players, servers, tokens, trainer windows, menus, or game mechanics. "
            "Use reusable placeholders when useful: {player}, {race}, {class}, {level}, {faction}, {npc}, {zone}, {sir_miss}. "
            "Prefer placeholders over hardcoding the player name, race, or class. "
            "Use these exact keys when relevant: available, trainer_available, class_available, profession_available, general.";
    }

    std::string BuildGenerateTrainerBarksUserPrompt(GenBarkRequest const& req, std::string const& sharedPrompt, std::string const& existingBarks)
    {
        std::ostringstream ss;
        ss << "Trainer NPC facts:\n";
        ss << "- Name: " << req.npcName << "\n";
        ss << "- Entry ID: " << req.npcEntry << "\n";
        if (!req.npcSubName.empty()) ss << "- Subname/title: " << req.npcSubName << "\n";
        if (req.npcLevel) ss << "- Level: " << req.npcLevel << "\n";
        if (!req.creatureType.empty()) ss << "- Creature type: " << req.creatureType << "\n";
        if (!req.rankStr.empty()) ss << "- Rank: " << req.rankStr << "\n";
        if (!req.roleStr.empty()) ss << "- Role: " << req.roleStr << "\n";
        if (!req.zoneName.empty()) ss << "- Zone: " << req.zoneName << "\n";
        if (!sharedPrompt.empty())
        {
            ss << "\nShared NPC prompt:\n" << sharedPrompt << "\n";
        }
        if (!existingBarks.empty())
        {
            ss << "\nExisting trainer bark file, if improving/replacing it:\n" << existingBarks << "\n";
        }
        if (!req.extraInstruction.empty())
        {
            ss << "\nExtra direction from the player:\n" << req.extraInstruction << "\n";
        }
        ss << "\nGenerate reusable trainer bark templates as key=value lines. "
            "Use placeholders instead of hardcoding player identity. Supported placeholders: "
            "{player}, {race}, {class}, {level}, {faction}, {npc}, {zone}, {sir_miss}. "
            "Useful keys: available, trainer_available, class_available, profession_available, general. "
            "Each line should be one short in-character sentence.";
        return ss.str();
    }

    void GenerateTrainerBarksWorker(GenBarkRequest req)
    {
        std::string sharedPrompt;
        std::string existingBarks;
        {
            std::lock_guard<std::mutex> lock(g_FileMutex);
            EnsureNpcChatDirectoriesAndDefaultPrompt();
            sharedPrompt = ReadWholeTextFile(SharedPromptFilePath(req.npcName, req.npcEntry));
            existingBarks = ReadWholeTextFile(req.outputPath);
        }
        NpcChat_ApiConfig cfg = BuildGenerationApiConfig(500);
        NpcChat_LLMResult res = NpcChat_CallLLM(cfg, BuildGenerateTrainerBarksSystemPrompt(), BuildGenerateTrainerBarksUserPrompt(req, sharedPrompt, existingBarks));
        if (!res.success || TrimCopy(res.text).empty())
        {
            QueueSystemMessage(req.playerGuidRaw, "NPC Chat trainer bark generation failed or returned empty text.");
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
        {
            std::lock_guard<std::mutex> lock(g_FileMutex);
            EnsureNpcChatDirectoriesAndDefaultPrompt();
            if (!WriteWholeTextFile(req.outputPath, generated, true))
            {
                QueueSystemMessage(req.playerGuidRaw, "NPC Chat generated trainer barks but failed to save the file.");
                return;
            }
        }
        QueueSystemMessage(req.playerGuidRaw, "NPC Chat trainer barks saved: " + req.outputPath);
    }

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
        std::string relationshipText;

        // Read prompts + prior context, then record what the player just said in both layers.
        {
            std::lock_guard<std::mutex> lock(g_FileMutex);

            EnsureNpcChatDirectoriesAndDefaultPrompt();

            defaultPrompt = ReadWholeTextFile(defaultPromptPath);
            sharedSubPrompts = LoadSubPromptBlocks(LoadSubPromptNameList(sharedSubPromptListPath));
            sharedPrompt = ReadWholeTextFile(sharedPromptPath);
            personalSubPrompts = LoadSubPromptBlocks(LoadSubPromptNameList(personalSubPromptListPath));
            personalPrompt = ReadWholeTextFile(personalPromptPath);
            relationshipText = ReadWholeTextFile(PersonalRelationshipFilePath(
                req.playerName, req.playerGuidRaw, req.npcName, req.npcEntry));

            sharedHistory = LoadHistoryTail(sharedPath, g_SharedHistoryTail);
            personalHistory = LoadHistoryTail(personalPath, g_PersonalHistoryTail);

            AppendHistoryLine(sharedPath, "[" + req.playerName + "] " + req.playerName + ": " + req.message);
            AppendHistoryLine(personalPath, req.playerName + ": " + req.message);
        }

        NpcChat_ApiConfig cfg = BuildChatApiConfig();

        NpcChat_LLMResult res = NpcChat_CallLLM(
            cfg,
            BuildSystemPrompt(req, defaultPrompt, sharedSubPrompts, sharedPrompt, personalSubPrompts, personalPrompt, relationshipText),
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
            PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT,
            PLAYERHOOK_ON_AFTER_UPDATE,
            PLAYERHOOK_ON_LOGOUT
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

    void OnPlayerAfterUpdate(Player* player, uint32 diff) override
    {
        if (!g_Enable || !player || !player->IsInWorld() || !player->IsAlive())
            return;

        WorldSession* session = player->GetSession();
        if (!session || session->IsBot())
            return;

        ProcessCachedBarksForPlayer(player, diff);
    }

    void OnPlayerLogout(Player* player) override
    {
        if (!player)
            return;

        m_PlayerBarkTimers.erase(player->GetGUID().GetCounter());
    }

private:
    struct PlayerBarkTimers
    {
        uint32 relationshipMs = 0;
        uint32 hostileMs = 0;
        uint32 trainerMs = 0;
        uint32 questMs = 0;
    };

    std::map<ObjectGuid::LowType, PlayerBarkTimers> m_PlayerBarkTimers;

    static bool ShouldRunTimer(uint32& accumulator, uint32 diff, int intervalMs)
    {
        uint32 interval = intervalMs > 0 ? static_cast<uint32>(intervalMs) : 1000u;
        accumulator += diff;
        if (accumulator < interval)
            return false;

        accumulator = 0;
        return true;
    }

    void ProcessCachedBarksForPlayer(Player* player, uint32 diff)
    {
        if (!player)
            return;

        Unit* selected = player->GetSelectedUnit();
        Creature* selectedNpc = selected ? selected->ToCreature() : nullptr;

        PlayerBarkTimers& timers = m_PlayerBarkTimers[player->GetGUID().GetCounter()];

        // Relationship/hostile/trainer barks remain selected-NPC-only for now.
        // Quest barks can optionally use the nearby questgiver scan when
        // NpcChat.QuestBarks.SelectedOnly = 0.
        if (selectedNpc && selectedNpc->IsInWorld())
        {
            if (g_RelationshipBarksEnabled && ShouldRunTimer(timers.relationshipMs, diff, g_RelationshipBarksScanIntervalMs))
                SpeakCachedRelationshipBark(player, selectedNpc, false);

            if (g_HostileFirstTalkEnabled && ShouldRunTimer(timers.hostileMs, diff, g_HostileFirstTalkScanIntervalMs))
                SpeakCachedHostileBark(player, selectedNpc, false);

            if (g_TrainerBarksEnabled && ShouldRunTimer(timers.trainerMs, diff, g_TrainerBarksScanIntervalMs))
                SpeakCachedTrainerBark(player, selectedNpc, false);
        }

        if (g_QuestBarksEnabled && ShouldRunTimer(timers.questMs, diff, g_QuestBarksScanIntervalMs))
        {
            if (Creature* questNpc = FindNearbyQuestgiverForBark(player))
                SpeakCachedQuestBark(player, questNpc, false);
        }
    }

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
        EnsureQuestBarkCacheTable();
    }

    void OnUpdate(uint32 diff) override
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

        // Cached proximity bark checks run from PlayerScript::OnPlayerAfterUpdate.
        // This branch exposes no sWorld->GetAllSessions() on IWorld, and player hooks
        // are a safer fit for selected-NPC-only checks anyway.
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

    static void FillBarkRequestCommon(GenBarkRequest& req, Player* player, Creature* npc)
    {
        req.playerGuidRaw = player->GetGUID().GetRawValue();
        req.npcGuidRaw = npc->GetGUID().GetRawValue();
        req.npcEntry = npc->GetEntry();
        req.playerName = player->GetName();
        req.npcName = npc->GetName();

        if (CreatureTemplate const* ct = npc->GetCreatureTemplate())
        {
            req.npcSubName = ct->SubName;
            req.creatureType = CreatureTypeStr(ct->type);
            req.rankStr = RankStr(ct->rank);
            req.roleStr = RolesFromNpcFlags(ct->npcflag);
        }

        req.npcLevel = npc->GetLevel();
        req.gender = GenderStr(npc->getGender());
        req.isHostile = npc->IsHostileTo(player);

        if (req.isHostile)
            req.stance = "an enemy";
        else if (npc->IsFriendlyTo(player))
            req.stance = "a friend";
        else
            req.stance = "a stranger";

        if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(npc->GetZoneId()))
            req.zoneName = zone->area_name[0];
    }

    static bool HandleNpcChatCommand(ChatHandler* handler, char const* args)
    {
        std::string arg = TrimCopy(args ? args : "");
        std::string rest;

        if (arg.empty() || StartsWithWord(arg, "help", rest))
        {
            handler->PSendSysMessage("NPC Chat commands:");
            handler->PSendSysMessage(".npcc key");
            handler->PSendSysMessage(".npcc reload");
            handler->PSendSysMessage(".npcc reset");
            handler->PSendSysMessage(".npcc rel");
            handler->PSendSysMessage(".npcc rel set <score|intimacy|stance|summary> <value>");
            handler->PSendSysMessage(".npcc rel tag <name>");
            handler->PSendSysMessage(".npcc prompt [quoted personal prompt]");
            handler->PSendSysMessage(".npcc gen shared [quoted extra direction]");
            handler->PSendSysMessage(".npcc gen personal [quoted extra direction]");
            handler->PSendSysMessage(".npcc gen preview [quoted extra direction]");
            handler->PSendSysMessage(".npcc gen bark relationship [quoted extra direction]");
            handler->PSendSysMessage(".npcc gen bark hostile [quoted extra direction]");
            handler->PSendSysMessage(".npcc bark relationship");
            handler->PSendSysMessage(".npcc bark hostile");
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

        if (StartsWithWord(arg, "key", rest) || StartsWithWord(arg, "paths", rest))
        {
            Player* player = GetCommandPlayer(handler);
            Creature* npc = GetSelectedCreature(handler);
            if (!player)
            {
                handler->PSendSysMessage("This command must be used in game.");
                return true;
            }
            if (!npc)
            {
                handler->PSendSysMessage("Target an NPC first.");
                return true;
            }

            std::string key = NpcHistoryBase(npc->GetName(), npc->GetEntry());
            std::string creatureType;
            std::string rankStr;
            std::string roleStr;
            if (CreatureTemplate const* ct = npc->GetCreatureTemplate())
            {
                creatureType = CreatureTypeStr(ct->type);
                rankStr = RankStr(ct->rank);
                roleStr = RolesFromNpcFlags(ct->npcflag);
            }

            handler->PSendSysMessage("NPC Chat key: {}", key.c_str());
            handler->PSendSysMessage("Entry: {}  Name: {}", npc->GetEntry(), npc->GetName());
            handler->PSendSysMessage("Type: {}  Rank: {}  Role: {}",
                creatureType.empty() ? "(unknown)" : creatureType.c_str(),
                rankStr.empty() ? "normal" : rankStr.c_str(),
                roleStr.empty() ? "(none)" : roleStr.c_str());
            handler->PSendSysMessage("Hostile to you: {}  Can speak by type: {}",
                npc->IsHostileTo(player) ? "yes" : "no",
                (creatureType == "humanoid" || creatureType == "undead" || creatureType == "demon" ||
                    creatureType == "dragonkin" || creatureType == "giant" || creatureType == "elemental") ? "yes" : "maybe/no");
            handler->PSendSysMessage("Shared prompt: {}", SharedPromptFilePath(npc->GetName(), npc->GetEntry()).c_str());
            handler->PSendSysMessage("Shared subprompts: {}", SharedSubPromptListFilePath(npc->GetName(), npc->GetEntry()).c_str());
            handler->PSendSysMessage("Personal prompt: {}", PersonalPromptFilePath(player->GetName(), player->GetGUID().GetRawValue(), npc->GetName(), npc->GetEntry()).c_str());
            handler->PSendSysMessage("Personal relationship: {}", PersonalRelationshipFilePath(player->GetName(), player->GetGUID().GetRawValue(), npc->GetName(), npc->GetEntry()).c_str());
            handler->PSendSysMessage("Personal barks: {}", PersonalBarksFilePath(player->GetName(), player->GetGUID().GetRawValue(), npc->GetName(), npc->GetEntry()).c_str());
            handler->PSendSysMessage("Shared hostile barks: {}", SharedHostileBarksFilePath(npc->GetName(), npc->GetEntry()).c_str());
            return true;
        }

        if (StartsWithWord(arg, "rel", rest) || StartsWithWord(arg, "relationship", rest))
        {
            Player* player = GetCommandPlayer(handler);
            Creature* npc = GetSelectedCreature(handler);
            if (!player)
            {
                handler->PSendSysMessage("This command must be used in game.");
                return true;
            }
            if (!npc)
            {
                handler->PSendSysMessage("Target an NPC first.");
                return true;
            }

            std::string path = PersonalRelationshipFilePath(player->GetName(), player->GetGUID().GetRawValue(), npc->GetName(), npc->GetEntry());
            std::string relRest;

            std::lock_guard<std::mutex> lock(g_FileMutex);
            EnsureNpcChatDirectoriesAndDefaultPrompt();

            if (rest.empty() || StartsWithWord(rest, "show", relRest))
            {
                std::string current = ReadWholeTextFile(path);
                handler->PSendSysMessage("NPC Chat relationship file: {}", path.c_str());
                if (current.empty())
                    handler->PSendSysMessage("No relationship memory exists yet. Use .npcc rel set summary \"...\" or .npcc rel tag <name>.");
                else
                    handler->PSendSysMessage("{}", current.c_str());
                return true;
            }

            if (StartsWithWord(rest, "clear", relRest))
            {
                bool removed = RemoveFileIfExists(path);
                handler->PSendSysMessage(removed ? "NPC Chat relationship memory cleared." : "No relationship memory file existed.");
                return true;
            }

            if (StartsWithWord(rest, "tag", relRest))
            {
                std::string tag = NormalizeSubPromptName(relRest);
                if (tag.empty())
                {
                    handler->PSendSysMessage("Usage: .npcc rel tag <name>");
                    return true;
                }

                std::map<std::string, std::string> kv = LoadKeyValueFile(path);
                std::vector<std::string> tags = SplitCsvNames(kv["tags"]);
                if (std::find(tags.begin(), tags.end(), tag) == tags.end())
                    tags.push_back(tag);
                kv["tags"] = JoinCsvNames(tags);
                if (kv.find("score") == kv.end())
                    kv["score"] = "0";
                if (kv.find("intimacy") == kv.end())
                    kv["intimacy"] = "0";
                if (kv.find("stance") == kv.end())
                    kv["stance"] = npc->IsHostileTo(player) ? "hostile" : "neutral";

                if (!WriteKeyValueFile(path, kv))
                    handler->PSendSysMessage("Failed to write NPC Chat relationship memory.");
                else
                    handler->PSendSysMessage("NPC Chat relationship tag added: {}", tag.c_str());
                return true;
            }

            if (StartsWithWord(rest, "summary", relRest) || StartsWithWord(rest, "remember", relRest))
            {
                std::string text = StripWrappingQuotes(relRest);
                if (text.empty())
                {
                    handler->PSendSysMessage("Usage: .npcc rel summary \"short memory text\"");
                    return true;
                }

                std::map<std::string, std::string> kv = LoadKeyValueFile(path);
                kv["summary"] = text;
                if (kv.find("score") == kv.end())
                    kv["score"] = "0";
                if (kv.find("intimacy") == kv.end())
                    kv["intimacy"] = "0";
                if (kv.find("stance") == kv.end())
                    kv["stance"] = npc->IsHostileTo(player) ? "hostile" : "neutral";

                if (!WriteKeyValueFile(path, kv))
                    handler->PSendSysMessage("Failed to write NPC Chat relationship memory.");
                else
                    handler->PSendSysMessage("NPC Chat relationship summary saved: {}", path.c_str());
                return true;
            }

            if (StartsWithWord(rest, "set", relRest))
            {
                std::string key;
                std::string value;
                size_t space = relRest.find_first_of(" \t");
                if (space != std::string::npos)
                {
                    key = ToLowerCopy(TrimCopy(relRest.substr(0, space)));
                    value = StripWrappingQuotes(TrimCopy(relRest.substr(space + 1)));
                }

                if (key.empty() || value.empty())
                {
                    handler->PSendSysMessage("Usage: .npcc rel set <score|intimacy|stance|summary|tags> <value>");
                    return true;
                }

                if (key != "score" && key != "intimacy" && key != "stance" && key != "summary" && key != "tags")
                {
                    handler->PSendSysMessage("Allowed relationship keys: score, intimacy, stance, summary, tags");
                    return true;
                }

                std::map<std::string, std::string> kv = LoadKeyValueFile(path);
                kv[key] = value;
                if (!WriteKeyValueFile(path, kv))
                    handler->PSendSysMessage("Failed to write NPC Chat relationship memory.");
                else
                    handler->PSendSysMessage("NPC Chat relationship {} saved: {}", key.c_str(), path.c_str());
                return true;
            }

            handler->PSendSysMessage("NPC Chat relationship commands: .npcc rel, .npcc rel set <key> <value>, .npcc rel tag <name>, .npcc rel summary \"...\", .npcc rel clear");
            return true;
        }

        if (StartsWithWord(arg, "bark", rest) || StartsWithWord(arg, "barks", rest))
        {
            Player* player = GetCommandPlayer(handler);
            Creature* npc = GetSelectedCreature(handler);
            if (!player)
            {
                handler->PSendSysMessage("This command must be used in game.");
                return true;
            }
            if (!npc)
            {
                handler->PSendSysMessage("Target an NPC first.");
                return true;
            }

            std::string barkRest;
            std::string barkMode = "relationship";
            if (StartsWithWord(rest, "hostile", barkRest) || StartsWithWord(rest, "enemy", barkRest))
                barkMode = "hostile";
            else if (StartsWithWord(rest, "trainer", barkRest) || StartsWithWord(rest, "train", barkRest))
                barkMode = "trainer";
            else if (StartsWithWord(rest, "quest", barkRest) || StartsWithWord(rest, "quests", barkRest))
                barkMode = "quest";
            else if (rest.empty() || StartsWithWord(rest, "relationship", barkRest) || StartsWithWord(rest, "rel", barkRest))
                barkMode = "relationship";
            else
            {
                handler->PSendSysMessage("Usage: .npcc bark <relationship|hostile|trainer|quest> [questId]");
                return true;
            }
            std::string reason;
            if (barkMode == "hostile")
            {
                if (SpeakCachedHostileBark(player, npc, true, &reason))
                    handler->PSendSysMessage("NPC Chat cached hostile bark fired.");
                else
                    handler->PSendSysMessage("NPC Chat hostile bark did not fire: {}", reason.c_str());
            }
            else if (barkMode == "trainer")
            {
                if (SpeakCachedTrainerBark(player, npc, true, &reason))
                    handler->PSendSysMessage("NPC Chat cached trainer bark fired.");
                else
                    handler->PSendSysMessage("NPC Chat trainer bark did not fire: {}", reason.c_str());
            }
            else if (barkMode == "quest")
            {
                uint32 questId = 0;
                std::string qText = TrimCopy(barkRest);
                if (!qText.empty())
                {
                    try { questId = static_cast<uint32>(std::stoul(qText)); }
                    catch (std::exception const&) { questId = 0; }
                }
                if (SpeakCachedQuestBark(player, npc, true, questId, &reason))
                    handler->PSendSysMessage("NPC Chat cached quest bark fired.");
                else
                    handler->PSendSysMessage("NPC Chat quest bark did not fire: {}", reason.c_str());
            }
            else
            {
                if (SpeakCachedRelationshipBark(player, npc, true, &reason))
                    handler->PSendSysMessage("NPC Chat cached relationship bark fired.");
                else
                    handler->PSendSysMessage("NPC Chat relationship bark did not fire: {}", reason.c_str());
            }
            return true;
        }

        if (StartsWithWord(arg, "placeholders", rest) || StartsWithWord(arg, "tokens", rest))
        {
            handler->PSendSysMessage("NPC Chat placeholders: {{player}}, {{name}}, {{race}}, {{class}}, {{level}}, {{faction}}, {{npc}}, {{zone}}");
            handler->PSendSysMessage("Pronoun placeholders: {{he_she}}, {{him_her}}, {{his_her}}, {{sir_miss}}");
            return true;
        }

        if (StartsWithWord(arg, "account", rest) || StartsWithWord(arg, "whoami", rest))
        {
            handler->PSendSysMessage("NPC Chat account ID: {}", GetCommandAccountId(handler));
            handler->PSendSysMessage("NPC Chat GM: {}", IsGm(handler) ? "yes" : "no");
            handler->PSendSysMessage("NPC Chat sub-prompt creator: {}", CanCreateSubPrompts(handler) ? "yes" : "no");
            handler->PSendSysMessage("NPC Chat loaded creator account IDs: {}", JoinAccountIds(g_SubPromptCreatorAccounts));
            handler->PSendSysMessage("NPC Chat relationship barks enabled: {} chance={} range={} scanMs={}",
                g_RelationshipBarksEnabled ? "yes" : "no",
                g_RelationshipBarksChancePct,
                g_RelationshipBarksTriggerDistance,
                g_RelationshipBarksScanIntervalMs);
            handler->PSendSysMessage("NPC Chat hostile first-talk enabled: {} chance={} range={} scanMs={} elitesOnly={}",
                g_HostileFirstTalkEnabled ? "yes" : "no",
                g_HostileFirstTalkChancePct,
                g_HostileFirstTalkTriggerDistance,
                g_HostileFirstTalkScanIntervalMs,
                g_HostileFirstTalkElitesOnly ? "yes" : "no");
            handler->PSendSysMessage("NPC Chat trainer barks enabled: {} chance={} range={} scanMs={}",
                g_TrainerBarksEnabled ? "yes" : "no",
                g_TrainerBarksChancePct,
                g_TrainerBarksTriggerDistance,
                g_TrainerBarksScanIntervalMs);
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

        if (StartsWithWord(arg, "quest", rest) || StartsWithWord(arg, "quests", rest))
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
                handler->PSendSysMessage("Target a questgiver first.");
                return true;
            }
            std::string qRest;
            if (rest.empty() || StartsWithWord(rest, "list", qRest))
            {
                std::vector<QuestBarkQuestInfo> quests = GetAcceptableQuestInfos(player, npc);
                if (quests.empty())
                {
                    handler->PSendSysMessage("NPC Chat quest list: no quests this player can accept from {} right now.", npc->GetName());
                    return true;
                }
                handler->PSendSysMessage("NPC Chat acceptable quests from {}:", npc->GetName());
                for (QuestBarkQuestInfo const& q : quests)
                    handler->PSendSysMessage("{} - {} (level {}, min {})", q.questId, q.title.c_str(), q.questLevel, q.minLevel);
                return true;
            }
            handler->PSendSysMessage("Usage: .npcc quest list");
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

            std::string questGenRest;
            if (StartsWithWord(rest, "quest", questGenRest) || StartsWithWord(rest, "quests", questGenRest))
            {
                uint32 onlyQuestId = 0;
                std::string extraQuest = questGenRest;
                std::string trimmedQuest = TrimCopy(questGenRest);
                if (!trimmedQuest.empty())
                {
                    size_t firstSpace = trimmedQuest.find_first_of(" \t");
                    std::string maybeId = firstSpace == std::string::npos ? trimmedQuest : trimmedQuest.substr(0, firstSpace);
                    bool digitsOnly = !maybeId.empty() && std::all_of(maybeId.begin(), maybeId.end(), [](unsigned char c) { return std::isdigit(c); });
                    if (digitsOnly)
                    {
                        onlyQuestId = static_cast<uint32>(std::stoul(maybeId));
                        extraQuest = firstSpace == std::string::npos ? "" : TrimCopy(trimmedQuest.substr(firstSpace + 1));
                    }
                }
                std::vector<QuestBarkQuestInfo> quests = GetAcceptableQuestInfos(player, npc, onlyQuestId);
                if (quests.empty())
                {
                    handler->PSendSysMessage(onlyQuestId ?
                        "NPC Chat quest bark generation failed: that quest is not currently acceptable from this NPC for this player." :
                        "NPC Chat quest bark generation failed: no currently acceptable quests found on this NPC for this player.");
                    return true;
                }
                std::string qKey = BuildQuestKey(quests);
                GenQuestBarkRequest qReq = BuildQuestBarkRequest(player, npc, quests, StripWrappingQuotes(extraQuest), true);
                std::thread(GenerateQuestBarkWorker, std::move(qReq)).detach();
                handler->PSendSysMessage("NPC Chat quest bark generation started for {} quest key {}.", npc->GetName(), qKey.c_str());
                handler->PSendSysMessage("You will get a system message when the DB cache is saved.");
                return true;
            }

            std::string genKindRest;
            if (StartsWithWord(rest, "bark", genKindRest) || StartsWithWord(rest, "barks", genKindRest))
            {
                std::string barkRest;
                std::string extraBark = genKindRest;
                std::string barkMode = "relationship";
                if (StartsWithWord(genKindRest, "relationship", barkRest) || StartsWithWord(genKindRest, "rel", barkRest))
                {
                    barkMode = "relationship";
                    extraBark = barkRest;
                }
                else if (StartsWithWord(genKindRest, "hostile", barkRest) || StartsWithWord(genKindRest, "enemy", barkRest))
                {
                    barkMode = "hostile";
                    extraBark = barkRest;
                }
                else if (StartsWithWord(genKindRest, "trainer", barkRest) || StartsWithWord(genKindRest, "train", barkRest))
                {
                    barkMode = "trainer";
                    extraBark = barkRest;
                }
                else if (!genKindRest.empty())
                {
                    handler->PSendSysMessage("Usage: .npcc gen bark <relationship|hostile|trainer> [quoted extra direction]");
                    return true;
                }
                GenBarkRequest barkReq;
                FillBarkRequestCommon(barkReq, player, npc);
                barkReq.barkKind = barkMode;
                barkReq.extraInstruction = StripWrappingQuotes(TrimCopy(extraBark));
                if (barkMode == "hostile")
                {
                    if (!CanManageSharedSubPrompts(handler))
                    {
                        handler->PSendSysMessage("Only GMs or configured NPC Chat creator accounts may generate shared hostile barks.");
                        return true;
                    }
                    barkReq.outputPath = SharedHostileBarksFilePath(npc->GetName(), npc->GetEntry());
                    std::thread(GenerateHostileBarksWorker, std::move(barkReq)).detach();
                    handler->PSendSysMessage("NPC Chat hostile bark generation started for {}.", npc->GetName());
                    handler->PSendSysMessage("You will get a system message when the .hostile_barks file is saved.");
                }
                else if (barkMode == "trainer")
                {
                    if (!CanManageSharedSubPrompts(handler))
                    {
                        handler->PSendSysMessage("Only GMs or configured NPC Chat creator accounts may generate shared trainer barks.");
                        return true;
                    }
                    if (!IsTrainerNpc(npc))
                    {
                        handler->PSendSysMessage("Target NPC is not a trainer.");
                        return true;
                    }
                    barkReq.outputPath = SharedTrainerBarksFilePath(npc->GetName(), npc->GetEntry());
                    std::thread(GenerateTrainerBarksWorker, std::move(barkReq)).detach();
                    handler->PSendSysMessage("NPC Chat trainer bark generation started for {}.", npc->GetName());
                    handler->PSendSysMessage("You will get a system message when the .trainer_barks file is saved.");
                }
                else
                {
                    barkReq.outputPath = PersonalBarksFilePath(player->GetName(), player->GetGUID().GetRawValue(), npc->GetName(), npc->GetEntry());
                    std::thread(GenerateRelationshipBarksWorker, std::move(barkReq)).detach();
                    handler->PSendSysMessage("NPC Chat relationship bark generation started for {}.", npc->GetName());
                    handler->PSendSysMessage("You will get a system message when the .barks file is saved.");
                }
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
                req.creatureType = CreatureTypeStr(ct->type);
                req.rankStr = RankStr(ct->rank);
                req.roleStr = RolesFromNpcFlags(ct->npcflag);
            }

            req.npcLevel = npc->GetLevel();
            req.gender = GenderStr(npc->getGender());
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
