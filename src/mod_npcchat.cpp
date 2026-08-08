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
#include "Trainer.h"       // Trainer::Trainer / Trainer::Spell (spell-aware trainer barks)
#include "SpellMgr.h"      // sSpellMgr
#include "SpellInfo.h"     // SpellInfo / spell names
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
#include "Playerbots.h"     // PlayerbotAI, RandomPlayerbotMgr, GET_PLAYERBOT_AI
#include "AiFactory.h"      // GetPlayerSpecTab
#include "ChatHelper.h"     // FormatClass (readable spec)
#include "Guild.h"          // BroadcastToGuild
#include "Channel.h"        // Channel::Say / IsOn
#include <set>

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
#include <list>
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
    bool        g_VerifyCert = true;     // NpcChat.Api.VerifyCert (https only; set 0 for self-signed/local proxy)
    int         g_MaxConcurrent = 4;     // NpcChat.Api.MaxConcurrentRequests (0 = unlimited)

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

    // Untargeted chat: with no NPC selected, a /say or /yell can be answered by a nearby NPC.
    // A yell (or a hostile-sounding line) near enemies lets the nearest enemy answer back.
    bool        g_UntargetedChatEnabled = false;
    float       g_UntargetedYellRange = 50.0f;
    bool        g_UntargetedHostileYellEnabled = true;

    bool        g_RequirePrefix = false;
    std::string g_Prefix;

    // Debug helper for bark auto-scan. When enabled, the player receives
    // system messages explaining why a nearby/selected bark did or did not fire.
    bool        g_BarkDebug = false;
    int         g_BarkDebugCooldownSec = 15;
    std::map<uint64, time_t> g_BarkDebugCooldownUntil;

    // Comma-separated account IDs allowed to create/edit global sub-prompt files
    // without requiring GM security. Example: NpcChat.SubPromptCreatorAccounts = 1,7,42
    std::vector<uint32> g_SubPromptCreatorAccounts;

    // When true, disk sub-prompt .prompt files are imported into the npcchat_subprompt SQL table on
    // first world update (new rows only; never clobbers SQL edits). Lets users author on disk and
    // have it injected to SQL automatically. Force a full re-sync with: .npcc sub import overwrite
    bool g_SubPromptImportOnStartup = true;

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

    // History-aware proximity whispers: an NPC the player has actually spoken with before may
    // privately recognize them when they pass nearby. Unlike cached relationship barks, this uses
    // the normal chat model and real personal chat history, so keep the chance/cooldowns conservative.
    bool        g_HistoryWhispersEnabled = false;
    float       g_HistoryWhispersTriggerDistance = 18.0f;
    int         g_HistoryWhispersChancePct = 12;
    int         g_HistoryWhispersPlayerCooldownSec = 300;
    int         g_HistoryWhispersPairCooldownSec = 900;
    int         g_HistoryWhispersScanIntervalMs = 5000;
    int         g_HistoryWhispersHistoryMaxLines = 8;

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

    // Cached quest intro barks. Ashbringer private-server default:
    // nearby questgivers can speak/generate automatically.
    bool        g_QuestBarksEnabled = true;
    bool        g_QuestBarksRealPlayersOnly = true;
    bool        g_QuestBarksGenerateMissing = false;
    bool        g_QuestBarksSelectedOnly = false;
    float       g_QuestBarksTriggerDistance = 14.0f;
    int         g_QuestBarksChancePct = 35;
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
        g_VerifyCert = sConfigMgr->GetOption<bool>("NpcChat.Api.VerifyCert", true);
        g_MaxConcurrent = sConfigMgr->GetOption<int32>("NpcChat.Api.MaxConcurrentRequests", 4);

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

        g_UntargetedChatEnabled = sConfigMgr->GetOption<bool>("NpcChat.UntargetedChat.Enable", false);
        g_UntargetedYellRange = sConfigMgr->GetOption<float>("NpcChat.UntargetedChat.YellRange", 50.0f);
        g_UntargetedHostileYellEnabled = sConfigMgr->GetOption<bool>("NpcChat.UntargetedChat.HostileYell", true);

        g_RequirePrefix = sConfigMgr->GetOption<bool>("NpcChat.RequirePrefix", false);
        g_Prefix = sConfigMgr->GetOption<std::string>("NpcChat.Prefix", "");

        g_BarkDebug = sConfigMgr->GetOption<bool>("NpcChat.BarkDebug", false);
        g_BarkDebugCooldownSec = sConfigMgr->GetOption<int32>("NpcChat.BarkDebugCooldownSec", 15);

        g_SubPromptCreatorAccounts = ParseAccountIdList(
            sConfigMgr->GetOption<std::string>("NpcChat.SubPromptCreatorAccounts", ""));
        g_SubPromptImportOnStartup = sConfigMgr->GetOption<bool>("NpcChat.SubPrompt.ImportOnStartup", true);

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

        g_HistoryWhispersEnabled = sConfigMgr->GetOption<bool>("NpcChat.HistoryWhispers.Enabled", false);
        g_HistoryWhispersTriggerDistance = sConfigMgr->GetOption<float>("NpcChat.HistoryWhispers.TriggerDistance", 18.0f);
        g_HistoryWhispersChancePct = sConfigMgr->GetOption<int32>("NpcChat.HistoryWhispers.ChancePct", 12);
        g_HistoryWhispersPlayerCooldownSec = sConfigMgr->GetOption<int32>("NpcChat.HistoryWhispers.PlayerCooldownSec", 300);
        g_HistoryWhispersPairCooldownSec = sConfigMgr->GetOption<int32>("NpcChat.HistoryWhispers.PairCooldownSec", 900);
        g_HistoryWhispersScanIntervalMs = sConfigMgr->GetOption<int32>("NpcChat.HistoryWhispers.ScanIntervalMs", 5000);
        g_HistoryWhispersHistoryMaxLines = sConfigMgr->GetOption<int32>("NpcChat.HistoryWhispers.HistoryMaxLines", 8);
        g_HistoryWhispersChancePct = std::max(0, std::min(100, g_HistoryWhispersChancePct));
        g_HistoryWhispersScanIntervalMs = std::max(1000, g_HistoryWhispersScanIntervalMs);
        g_HistoryWhispersHistoryMaxLines = std::max(2, std::min(20, g_HistoryWhispersHistoryMaxLines));

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

        g_QuestBarksEnabled = sConfigMgr->GetOption<bool>("NpcChat.QuestBarks.Enabled", true);
        g_QuestBarksRealPlayersOnly = sConfigMgr->GetOption<bool>("NpcChat.QuestBarks.RealPlayersOnly", true);
        g_QuestBarksGenerateMissing = sConfigMgr->GetOption<bool>("NpcChat.QuestBarks.GenerateMissing", false);
        g_QuestBarksSelectedOnly = sConfigMgr->GetOption<bool>("NpcChat.QuestBarks.SelectedOnly", false);
        g_QuestBarksTriggerDistance = sConfigMgr->GetOption<float>("NpcChat.QuestBarks.TriggerDistance", 14.0f);
        g_QuestBarksChancePct = sConfigMgr->GetOption<int32>("NpcChat.QuestBarks.ChancePct", 35);
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
        cfg.verifyCert = g_VerifyCert;
        cfg.maxConcurrent = g_MaxConcurrent;
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
        cfg.verifyCert = g_VerifyCert;
        cfg.maxConcurrent = g_MaxConcurrent;
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

        std::string autoTags;         // auto-linker: resolved creature archetype tags (CSV)
        std::string trainerInfo;      // if a trainer: leveled list of teachable spells (for conversation)
        std::string message;
    };

    struct ChatReply
    {
        uint64_t    playerGuidRaw = 0;
        uint64_t    npcGuidRaw = 0;
        std::string text;
        bool        forcePrivateReply = false;
        bool        whisper = false; // true = real creature whisper packet, not targeted /say
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
        std::string autoTags;         // auto-linker: resolved creature archetype tags (CSV)
        std::string trainerInfo;      // if a trainer: leveled curriculum baked into the generated identity
        std::string extraInstruction;
        std::string outputPath;
    };

    struct GenBarkRequest
    {
        uint64_t    playerGuidRaw = 0;
        uint64_t    npcGuidRaw = 0;
        uint32_t    npcEntry = 0;

        std::string playerName;
        uint8       playerRace = 0;
        uint8       playerClass = 0;
        uint8       faction = 0;
        uint16      phase = 0;
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
        std::string autoTags;         // auto-linker: resolved creature archetype tags (CSV)
        std::string cacheContext;      // hostile_first_talk, trainer_approach, etc.
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
        uint8       minLevel = 0;
        uint8       maxLevel = 0;
        uint8       generatedLevel = 0;
        std::string questKey;
        std::vector<QuestBarkQuestInfo> quests;
        std::string autoTags;         // auto-linker: resolved creature archetype tags (CSV)
        std::string barkType = "quest_available"; // quest_available | quest_ender
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

    // Main-thread-only cooldowns for history-aware NPC whispers.
    std::map<uint64_t, time_t> g_HistoryWhisperPlayerCooldownUntil;
    std::map<std::string, time_t> g_HistoryWhisperPairCooldownUntil;

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

    // Cross-thread guard so proximity auto-generation cannot queue the same
    // NPC/context/player-specific cache row every scan tick while the first
    // LLM request is still running.
    std::mutex g_NpcBarkGenerationMutex;
    std::map<std::string, bool> g_NpcBarkGenerationPending;
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

    std::string NormalizeGeneratedKeyValueText(std::string text)
    {
        // Some smaller/cheaper models obey "key=value lines" but return every
        // key=value pair on one physical line. Normalize that into one key per
        // line before saving and before loading cached bark files.
        static std::vector<std::string> const keys =
        {
            // relationship bark keys
            "public_friendly", "private_friendly", "public_neutral", "private_neutral",
            "public_hostile", "private_hostile", "public_close", "private_close",

            // hostile bark keys
            "aggro_intro", "warning", "threat", "combat_taunt", "low_health",
            "victory", "general",

            // trainer bark keys
            "available", "trainer_available", "class_available", "profession_available"
        };

        for (std::string const& key : keys)
        {
            std::string token = key + "=";
            size_t pos = 0;
            while ((pos = text.find(token, pos)) != std::string::npos)
            {
                if (pos > 0 && text[pos - 1] != '\n' && text[pos - 1] != '\r')
                {
                    // Prefer splitting only when the key looks like a new pair
                    // after whitespace, semicolon, pipe, or another common separator.
                    char prev = text[pos - 1];
                    if (std::isspace(static_cast<unsigned char>(prev)) || prev == ';' || prev == '|')
                    {
                        text.insert(pos, "\n");
                        pos += 1;
                    }
                }
                pos += token.size();
            }
        }

        // Trim each normalized line and drop empty/comment lines.
        std::ostringstream out;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line))
        {
            line = TrimCopy(line);
            if (line.empty() || line[0] == '#')
                continue;
            out << line << "\n";
        }

        return TrimCopy(out.str());
    }

    std::map<std::string, std::string> ParseKeyValueText(std::string text)
    {
        std::map<std::string, std::string> out;
        text = NormalizeGeneratedKeyValueText(text);

        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line))
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

    std::map<std::string, std::string> LoadKeyValueFile(std::string const& path)
    {
        std::string text = ReadWholeTextFile(path);
        if (text.empty())
            return {};
        return ParseKeyValueText(text);
    }

    bool IsBadGeneratedBarkValue(std::string const& value)
    {
        std::string v = TrimCopy(value);
        if (v.size() < 8)
            return true;

        std::string low = ToLowerCopy(v);
        if (low == "none" || low == "null" || low == "n/a" || low == "na" || low == "undefined")
            return true;
        if (low.rfind("none ", 0) == 0 || low.rfind("null ", 0) == 0 || low.rfind("n/a ", 0) == 0)
            return true;
        if (low.find("key=value") != std::string::npos || low.find("markdown") != std::string::npos)
            return true;

        return false;
    }

    bool HasUsableBarkKeys(std::string const& text, std::vector<std::string> const& preferredKeys, size_t minimumGoodValues)
    {
        std::map<std::string, std::string> kv = ParseKeyValueText(text);
        size_t good = 0;

        for (std::string const& key : preferredKeys)
        {
            auto itr = kv.find(ToLowerCopy(key));
            if (itr == kv.end())
                continue;

            if (!IsBadGeneratedBarkValue(itr->second))
                ++good;
        }

        return good >= minimumGoodValues;
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

    // Forward declaration: SqlEscape is defined far below, but the sub-prompt SQL helpers here need it.
    std::string SqlEscape(std::string s);

    // ---- Sub-prompt SQL layer (SQL is source of truth; disk is the authoring/fallback surface) ----

    void EnsureSubPromptTable()
    {
        WorldDatabase.Execute(
            "CREATE TABLE IF NOT EXISTS `npcchat_subprompt` ("
            "`id` VARCHAR(96) NOT NULL,"
            "`category` VARCHAR(32) NOT NULL DEFAULT 'trait',"
            "`aliases` VARCHAR(255) NOT NULL DEFAULT '',"
            "`priority` SMALLINT NOT NULL DEFAULT 50,"
            "`relationship_aware` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "`enabled` TINYINT UNSIGNED NOT NULL DEFAULT 1,"
            "`source` VARCHAR(16) NOT NULL DEFAULT 'disk',"
            "`text` TEXT NOT NULL,"
            "`created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            "`updated_at` TIMESTAMP NULL DEFAULT NULL ON UPDATE CURRENT_TIMESTAMP,"
            "PRIMARY KEY (`id`),"
            "KEY `idx_npcchat_subprompt_cat` (`category`, `priority`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    }

    bool IsKnownSubPromptCategory(std::string const& c)
    {
        static const char* kinds[] = { "race", "gender", "place", "archetype",
            "relationship", "tone", "npc", "class", "faction", "trait" };
        for (char const* k : kinds)
            if (c == k)
                return true;
        return false;
    }

    int DefaultSubPromptPriority(std::string const& cat)
    {
        if (cat == "npc") return 150;
        if (cat == "relationship") return 110;
        if (cat == "race") return 100;
        if (cat == "archetype") return 90;
        if (cat == "place") return 80;
        if (cat == "faction") return 70;
        if (cat == "class") return 60;
        if (cat == "trait" || cat == "tone") return 40;
        if (cat == "gender") return 30;
        return 50;
    }

    struct SubPromptRecord
    {
        std::string id;
        std::string category = "trait";
        std::string aliases;
        int priority = 50;
        int relationshipAware = 0;
        std::string text;
    };

    // Parses both the structured header format ("# id / # aliases / # category / # priority /
    // # relationship") and plain-prose .prompt files. For plain files, category is inferred from a
    // dotted filename suffix (e.g. foo.race -> race) and priority from the category default.
    SubPromptRecord ParseSubPrompt(std::string const& stem, std::string const& raw)
    {
        SubPromptRecord rec;
        rec.id = NormalizeSubPromptName(stem);

        std::string inferredCat;
        {
            size_t dot = stem.find_last_of('.');
            if (dot != std::string::npos)
            {
                std::string suf = ToLowerCopy(stem.substr(dot + 1));
                if (IsKnownSubPromptCategory(suf))
                    inferredCat = suf;
            }
        }

        std::istringstream in(raw);
        std::string line;
        std::ostringstream body;
        bool inHeader = true;
        bool haveCategory = false;
        bool havePriority = false;

        while (std::getline(in, line))
        {
            std::string trimmed = TrimCopy(line);
            if (inHeader && !trimmed.empty() && trimmed[0] == '#')
            {
                std::string h = TrimCopy(trimmed.substr(1));
                size_t colon = h.find(':');
                if (colon != std::string::npos)
                {
                    std::string key = ToLowerCopy(TrimCopy(h.substr(0, colon)));
                    std::string val = TrimCopy(h.substr(colon + 1));
                    if (key == "category") { rec.category = ToLowerCopy(val); haveCategory = true; }
                    else if (key == "aliases") rec.aliases = val;
                    else if (key == "priority") { try { rec.priority = std::stoi(val); havePriority = true; } catch (...) {} }
                    else if (key == "relationship")
                    {
                        std::string lv = ToLowerCopy(val);
                        rec.relationshipAware = (lv == "yes" || lv == "1" || lv == "true") ? 1 : 0;
                    }
                    // an explicit "# id:" is intentionally ignored for the DB key; we key off the
                    // normalized filename so SQL lookups match the rest of the module.
                }
                continue;
            }
            if (inHeader && trimmed.empty())
                continue; // blank line(s) before the body
            inHeader = false;
            body << line << "\n";
        }

        rec.text = TrimCopy(body.str());
        if (!haveCategory && !inferredCat.empty())
            rec.category = inferredCat;
        if (rec.category.empty())
            rec.category = "trait";
        if (rec.category == "relationship")
            rec.relationshipAware = 1;
        if (!havePriority)
            rec.priority = DefaultSubPromptPriority(rec.category);
        return rec;
    }

    // Scans the on-disk subprompts folder and upserts each fragment into SQL. overwrite=false uses
    // INSERT IGNORE (adds new prompts only, never clobbers SQL edits); overwrite=true forces a
    // full disk->SQL re-sync. Returns the number of files processed.
    int ImportSubPromptsFromDisk(bool overwrite)
    {
        EnsureSubPromptTable();
        int count = 0;
        try
        {
            std::filesystem::path root(SubPromptRootPath());
            if (!std::filesystem::exists(root))
                return 0;

            for (auto const& it : std::filesystem::directory_iterator(root))
            {
                if (!it.is_regular_file())
                    continue;
                if (it.path().extension().string() != ".prompt")
                    continue;

                std::string raw = ReadWholeTextFile(it.path().string());
                if (raw.empty())
                    continue;

                SubPromptRecord rec = ParseSubPrompt(it.path().stem().string(), raw);
                if (rec.id.empty() || rec.text.empty())
                    continue;

                std::ostringstream sql;
                sql << (overwrite ? "REPLACE INTO" : "INSERT IGNORE INTO")
                    << " `npcchat_subprompt` (`id`,`category`,`aliases`,`priority`,`relationship_aware`,`source`,`text`) VALUES ('"
                    << SqlEscape(rec.id) << "','"
                    << SqlEscape(rec.category) << "','"
                    << SqlEscape(rec.aliases) << "',"
                    << rec.priority << ","
                    << rec.relationshipAware << ","
                    << "'disk','"
                    << SqlEscape(rec.text) << "')";
                WorldDatabase.Execute(sql.str().c_str());
                ++count;
            }
        }
        catch (std::exception const&)
        {
            // best effort
        }
        return count;
    }

    // SQL first, disk fallback. key is already normalized by the caller.
    std::string LoadSubPromptTextSqlFirst(std::string const& key)
    {
        if (key.empty())
            return "";

        EnsureSubPromptTable();
        if (QueryResult r = WorldDatabase.Query(
            "SELECT `text` FROM `npcchat_subprompt` WHERE `id`='{}' AND `enabled`=1 LIMIT 1",
            SqlEscape(key)))
            return TrimCopy(r->Fetch()[0].Get<std::string>());

        return ReadWholeTextFile(SubPromptFilePath(key)); // disk fallback
    }

    std::string LoadSubPromptBlocks(std::vector<std::string> const& names)
    {
        std::ostringstream ss;
        for (std::string const& raw : names)
        {
            std::string key = NormalizeSubPromptName(raw);
            if (key.empty())
                continue;

            std::string text = LoadSubPromptTextSqlFirst(key);
            if (text.empty())
                continue;

            ss << "[" << key << "]\n" << text << "\n\n";
        }
        return TrimCopy(ss.str());
    }

    // ---- Per-NPC speak profile (SQL): eligibility, speak chance, kind, and explicit archetype tags ----
    // This is the "NPCs in general" table: it carries can_speak (the hostile eligibility record),
    // an optional per-NPC speak_chance override, a kind, and explicit tags that the auto-linker can't
    // derive from the creature itself (most importantly humanoid race, e.g. "dwarf,bronzebeard").

    void EnsureNpcProfileTable()
    {
        WorldDatabase.Execute(
            "CREATE TABLE IF NOT EXISTS `npcchat_npc_profile` ("
            "`npc_entry` INT UNSIGNED NOT NULL,"
            "`can_speak` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "`speak_chance` TINYINT UNSIGNED NOT NULL DEFAULT 0,"   // 0 = use the global default
            "`npc_kind` VARCHAR(16) NOT NULL DEFAULT 'auto',"        // quest | regular | hostile | auto
            "`tags` VARCHAR(255) NOT NULL DEFAULT '',"               // explicit archetype tags (CSV)
            "`created_by_account` INT UNSIGNED NOT NULL DEFAULT 0,"
            "`created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            "`updated_at` TIMESTAMP NULL DEFAULT NULL ON UPDATE CURRENT_TIMESTAMP,"
            "PRIMARY KEY (`npc_entry`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    }

    struct NpcProfile
    {
        bool        hasRow = false;
        int         canSpeak = 0;
        int         speakChance = 0;
        std::string kind = "auto";
        std::string tags;
    };

    NpcProfile LoadNpcProfile(uint32 npcEntry)
    {
        NpcProfile p;
        EnsureNpcProfileTable();
        if (QueryResult r = WorldDatabase.Query(
            "SELECT `can_speak`,`speak_chance`,`npc_kind`,`tags` FROM `npcchat_npc_profile` WHERE `npc_entry`={} LIMIT 1",
            npcEntry))
        {
            Field* f = r->Fetch();
            p.hasRow = true;
            p.canSpeak = f[0].Get<uint8>();
            p.speakChance = f[1].Get<uint8>();
            p.kind = f[2].Get<std::string>();
            p.tags = f[3].Get<std::string>();
        }
        return p;
    }

    std::string LoadNpcProfileTags(uint32 npcEntry)
    {
        return LoadNpcProfile(npcEntry).tags;
    }

    // Opt-in eligibility used by the aggro-driven hostile speak: an NPC only auto-taunts once a
    // profile row marks it can_speak (which generating a hostile bark does automatically).
    bool NpcProfileCanSpeak(uint32 npcEntry)
    {
        return LoadNpcProfile(npcEntry).canSpeak != 0;
    }

    // Upsert that preserves other columns. createdByAccount is recorded only when the row is new.
    void UpsertNpcProfileField(uint32 npcEntry, std::string const& column, std::string const& valueLiteral, uint32 createdByAccount)
    {
        EnsureNpcProfileTable();
        std::ostringstream sql;
        sql << "INSERT INTO `npcchat_npc_profile` (`npc_entry`,`" << column << "`,`created_by_account`) VALUES ("
            << npcEntry << "," << valueLiteral << "," << createdByAccount << ") "
            << "ON DUPLICATE KEY UPDATE `" << column << "`=" << valueLiteral;
        WorldDatabase.Execute(sql.str().c_str());
    }

    void SetNpcProfileCanSpeak(uint32 npcEntry, bool canSpeak, uint32 createdByAccount = 0)
    {
        UpsertNpcProfileField(npcEntry, "can_speak", canSpeak ? "1" : "0", createdByAccount);
    }

    // Effective speak chance: per-NPC override when set (>0), else the supplied global default.
    int EffectiveSpeakChance(uint32 npcEntry, int globalDefault)
    {
        NpcProfile p = LoadNpcProfile(npcEntry);
        return (p.hasRow && p.speakChance > 0) ? p.speakChance : globalDefault;
    }

    // Attribute auto-linker (resolver half, MAIN THREAD: reads creature fields only, no DB).
    // Derives a CSV of archetype tags from a creature so the matcher can pull the right sub-prompts
    // without anyone manually attaching them. Humanoid player-race (human/dwarf/orc) is NOT derivable
    // from the creature here (the core has no such field) and is left to per-NPC tags / profiles.
    std::string ResolveCreatureAutoTags(Creature* npc)
    {
        if (!npc)
            return "";

        std::vector<std::string> tags;
        tags.push_back(npc->getGender() == GENDER_FEMALE ? "female" : "male");

        if (CreatureTemplate const* t = npc->GetCreatureTemplate())
        {
            switch (t->type)
            {
            case CREATURE_TYPE_BEAST:      tags.push_back("beast"); break;
            case CREATURE_TYPE_DRAGONKIN:  tags.push_back("dragonkin"); break;
            case CREATURE_TYPE_DEMON:      tags.push_back("demon"); break;
            case CREATURE_TYPE_ELEMENTAL:  tags.push_back("elemental"); break;
            case CREATURE_TYPE_GIANT:      tags.push_back("giant"); break;
            case CREATURE_TYPE_UNDEAD:     tags.push_back("undead"); break;
            case CREATURE_TYPE_HUMANOID:   tags.push_back("humanoid"); break;
            case CREATURE_TYPE_MECHANICAL: tags.push_back("mechanical"); break;
            case CREATURE_TYPE_CRITTER:    tags.push_back("critter"); break;
            default: break;
            }

            uint32 nf = t->npcflag;
            if (nf & UNIT_NPC_FLAG_QUESTGIVER)   tags.push_back("quest_giver");
            if (nf & UNIT_NPC_FLAG_VENDOR)       tags.push_back("vendor");
            if (nf & UNIT_NPC_FLAG_REPAIR)       tags.push_back("repair_vendor");
            if (nf & UNIT_NPC_FLAG_FLIGHTMASTER) tags.push_back("flight_master");
            if (nf & UNIT_NPC_FLAG_INNKEEPER)    tags.push_back("innkeeper");
            if (nf & UNIT_NPC_FLAG_BANKER)       tags.push_back("banker");
            if (nf & UNIT_NPC_FLAG_STABLEMASTER) tags.push_back("stable_master");
            if (nf & UNIT_NPC_FLAG_TRAINER)      tags.push_back("trainer");
        }

        if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(npc->GetZoneId()))
        {
            std::string zn = zone->area_name[0] ? ToLowerCopy(zone->area_name[0]) : "";
            size_t sp = zn.find(' ');
            std::string first = NormalizeSubPromptName(sp == std::string::npos ? zn : zn.substr(0, sp));
            if (!first.empty())
                tags.push_back(first);
        }

        // Explicit profile tags supply what the creature can't tell us by itself - most importantly
        // humanoid race (dwarf/human/orc) and any custom archetype the author assigned.
        std::string profileTags = LoadNpcProfileTags(npc->GetEntry());
        if (!profileTags.empty())
        {
            std::istringstream ts(profileTags);
            std::string t;
            while (std::getline(ts, t, ','))
            {
                std::string n = NormalizeSubPromptName(t);
                if (!n.empty())
                    tags.push_back(n);
            }
        }

        std::ostringstream csv;
        for (size_t i = 0; i < tags.size(); ++i)
            csv << (i ? "," : "") << tags[i];
        return csv.str();
    }

    // Attribute auto-linker (matcher half, runs in the generation worker via the DB pool).
    // Maps the resolver's tags to concrete sub-prompt ids by matching each tag against a fragment's
    // base name (id minus its "_<category>" suffix) or any of its aliases, then orders by priority so
    // a specific NPC/archetype fragment outranks a broad gender one. Returns sub-prompt ids to attach.
    std::vector<std::string> MatchSubPromptNamesForTags(std::string const& csv)
    {
        std::vector<std::string> out;
        if (csv.empty())
            return out;

        EnsureSubPromptTable();

        std::vector<std::string> tags;
        {
            std::istringstream ss(csv);
            std::string t;
            while (std::getline(ss, t, ','))
            {
                std::string n = NormalizeSubPromptName(t);
                if (!n.empty())
                    tags.push_back(n);
            }
        }
        if (tags.empty())
            return out;

        QueryResult r = WorldDatabase.Query(
            "SELECT `id`,`category`,`aliases`,`priority` FROM `npcchat_subprompt` WHERE `enabled`=1");
        if (!r)
            return out;

        struct Hit { std::string id; int priority; };
        std::vector<Hit> hits;
        do
        {
            Field* f = r->Fetch();
            std::string id = f[0].Get<std::string>();
            std::string cat = f[1].Get<std::string>();
            std::string aliases = f[2].Get<std::string>();
            int prio = f[3].Get<int32>();

            std::vector<std::string> keys;
            std::string base = id;
            std::string suffix = "_" + NormalizeSubPromptName(cat);
            if (suffix.size() > 1 && base.size() > suffix.size() &&
                base.compare(base.size() - suffix.size(), suffix.size(), suffix) == 0)
                base = base.substr(0, base.size() - suffix.size());
            keys.push_back(base);

            std::istringstream as(aliases);
            std::string a;
            while (std::getline(as, a, ','))
            {
                std::string n = NormalizeSubPromptName(a);
                if (!n.empty())
                    keys.push_back(n);
            }

            bool matched = false;
            for (std::string const& tag : tags)
            {
                for (std::string const& k : keys)
                    if (k == tag) { matched = true; break; }
                if (matched)
                    break;
            }
            if (matched)
                hits.push_back({ id, prio });
        } while (r->NextRow());

        std::sort(hits.begin(), hits.end(),
            [](Hit const& a, Hit const& b) { return a.priority > b.priority; });

        for (Hit const& h : hits)
        {
            bool dup = false;
            for (std::string const& existing : out)
                if (existing == h.id) { dup = true; break; }
            if (!dup)
                out.push_back(h.id);
        }
        return out;
    }

    // Convenience: merge manually-attached names with auto-linked names (deduped, manual first).
    std::vector<std::string> MergeSubPromptNames(std::vector<std::string> manual, std::string const& autoTagsCsv)
    {
        for (std::string const& n : MatchSubPromptNamesForTags(autoTagsCsv))
        {
            bool dup = false;
            for (std::string const& existing : manual)
                if (existing == n) { dup = true; break; }
            if (!dup)
                manual.push_back(n);
        }
        return manual;
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

    void BarkDebug(Player* player, std::string const& text)
    {
        if (!g_BarkDebug || !player)
            return;

        WorldSession* session = player->GetSession();
        if (!session || session->IsBot())
            return;

        uint64 key = player->GetGUID().GetRawValue();
        time_t now = std::time(nullptr);
        if (g_BarkDebugCooldownSec > 0 && g_BarkDebugCooldownUntil[key] > now)
            return;

        if (g_BarkDebugCooldownSec > 0)
            g_BarkDebugCooldownUntil[key] = now + g_BarkDebugCooldownSec;

        ChatHandler(session).PSendSysMessage("|cff7fd6ffNPC BarkDebug:|r {}", text.c_str());
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

    // ---- Personal relationship SQL layer (SQL source of truth; disk = fallback/backup) ----
    // Relationships are per (player, npc_entry). Putting them in SQL makes "which NPCs near me do I
    // know" a single query, which is what powers the pass-by greeting. Reads are SQL-first with a
    // disk fallback that lazily migrates a disk-only relationship into SQL on read, so existing files
    // move over as they're touched. Writes go to SQL and keep the disk file as a backup.

    void EnsureRelationshipTable()
    {
        WorldDatabase.Execute(
            "CREATE TABLE IF NOT EXISTS `npcchat_relationship` ("
            "`player_guid` BIGINT UNSIGNED NOT NULL,"
            "`npc_entry` INT UNSIGNED NOT NULL,"
            "`player_name` VARCHAR(64) NOT NULL DEFAULT '',"
            "`npc_name` VARCHAR(64) NOT NULL DEFAULT '',"
            "`score` INT NOT NULL DEFAULT 0,"
            "`stance` VARCHAR(32) NOT NULL DEFAULT '',"
            "`data` TEXT NOT NULL,"
            "`updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
            "PRIMARY KEY (`player_guid`, `npc_entry`),"
            "KEY `idx_npcchat_rel_player` (`player_guid`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    }

    std::string SerializeKeyValueMap(std::map<std::string, std::string> const& kv)
    {
        std::ostringstream ss;
        for (auto const& p : kv)
            if (!TrimCopy(p.second).empty())
                ss << p.first << "=" << TrimCopy(p.second) << "\n";
        return ss.str();
    }

    void UpsertRelationshipRow(uint64_t playerGuidRaw, uint32 npcEntry,
        std::string const& playerName, std::string const& npcName,
        std::map<std::string, std::string> const& kv)
    {
        EnsureRelationshipTable();
        std::string blob = SerializeKeyValueMap(kv);
        int score = ToIntOrDefault(MapGet(kv, "score"), 0);
        std::string stance = MapGet(kv, "stance");
        std::ostringstream sql;
        sql << "REPLACE INTO `npcchat_relationship` "
            << "(`player_guid`,`npc_entry`,`player_name`,`npc_name`,`score`,`stance`,`data`) VALUES ("
            << playerGuidRaw << "," << npcEntry << ",'"
            << SqlEscape(playerName) << "','" << SqlEscape(npcName) << "',"
            << score << ",'" << SqlEscape(stance) << "','" << SqlEscape(blob) << "')";
        WorldDatabase.Execute(sql.str().c_str());
    }

    std::map<std::string, std::string> LoadRelationshipKV(uint64_t playerGuidRaw, uint32 npcEntry,
        std::string const& playerName, std::string const& npcName)
    {
        EnsureRelationshipTable();
        if (QueryResult r = WorldDatabase.Query(
            "SELECT `data` FROM `npcchat_relationship` WHERE `player_guid`={} AND `npc_entry`={} LIMIT 1",
            playerGuidRaw, npcEntry))
            return ParseKeyValueText(r->Fetch()[0].Get<std::string>());

        // disk fallback + lazy migrate
        std::map<std::string, std::string> kv =
            LoadKeyValueFile(PersonalRelationshipFilePath(playerName, playerGuidRaw, npcName, npcEntry));
        if (!kv.empty())
            UpsertRelationshipRow(playerGuidRaw, npcEntry, playerName, npcName, kv);
        return kv;
    }

    std::string LoadRelationshipText(uint64_t playerGuidRaw, uint32 npcEntry,
        std::string const& playerName, std::string const& npcName)
    {
        return TrimCopy(SerializeKeyValueMap(
            LoadRelationshipKV(playerGuidRaw, npcEntry, playerName, npcName)));
    }

    bool SaveRelationshipKV(uint64_t playerGuidRaw, uint32 npcEntry,
        std::string const& playerName, std::string const& npcName,
        std::map<std::string, std::string> const& kv)
    {
        UpsertRelationshipRow(playerGuidRaw, npcEntry, playerName, npcName, kv);
        return WriteKeyValueFile(
            PersonalRelationshipFilePath(playerName, playerGuidRaw, npcName, npcEntry), kv);
    }

    void DeleteRelationship(uint64_t playerGuidRaw, uint32 npcEntry)
    {
        EnsureRelationshipTable();
        std::ostringstream sql;
        sql << "DELETE FROM `npcchat_relationship` WHERE `player_guid`=" << playerGuidRaw
            << " AND `npc_entry`=" << npcEntry;
        WorldDatabase.Execute(sql.str().c_str());
    }

    // Up to `cap` npc_entries this player has a relationship with, newest first (pass-by greeting).
    std::vector<uint32> GetPlayerRelationshipEntries(uint64_t playerGuidRaw, uint32 cap)
    {
        std::vector<uint32> out;
        EnsureRelationshipTable();
        std::ostringstream q;
        q << "SELECT `npc_entry` FROM `npcchat_relationship` WHERE `player_guid`=" << playerGuidRaw
            << " ORDER BY `updated_at` DESC LIMIT " << cap;
        if (QueryResult r = WorldDatabase.Query(q.str().c_str()))
            do { out.push_back(r->Fetch()[0].Get<uint32>()); } while (r->NextRow());
        return out;
    }

    // Lightweight index of NPCs the player has actually chatted with. The history remains file-backed;
    // this table exists only so proximity scans can cheaply ask which creature entries are worth checking.
    void EnsureNpcContactTable()
    {
        WorldDatabase.Execute(
            "CREATE TABLE IF NOT EXISTS `npcchat_contact` ("
            "`player_guid` BIGINT UNSIGNED NOT NULL,"
            "`npc_entry` INT UNSIGNED NOT NULL,"
            "`player_name` VARCHAR(64) NOT NULL DEFAULT '',"
            "`npc_name` VARCHAR(64) NOT NULL DEFAULT '',"
            "`last_talked_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
            "PRIMARY KEY (`player_guid`, `npc_entry`),"
            "KEY `idx_npcchat_contact_player` (`player_guid`, `last_talked_at`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    }

    void TouchNpcContact(uint64_t playerGuidRaw, uint32 npcEntry,
        std::string const& playerName, std::string const& npcName)
    {
        if (!playerGuidRaw || !npcEntry)
            return;
        EnsureNpcContactTable();
        std::ostringstream sql;
        sql << "INSERT INTO `npcchat_contact` (`player_guid`,`npc_entry`,`player_name`,`npc_name`) VALUES ("
            << playerGuidRaw << "," << npcEntry << ",'" << SqlEscape(playerName) << "','" << SqlEscape(npcName) << "') "
            << "ON DUPLICATE KEY UPDATE `player_name`=VALUES(`player_name`),`npc_name`=VALUES(`npc_name`),"
            << "`last_talked_at`=CURRENT_TIMESTAMP";
        WorldDatabase.Execute(sql.str().c_str());
    }

    std::vector<uint32> GetPlayerContactEntries(uint64_t playerGuidRaw, uint32 cap)
    {
        std::vector<uint32> out;
        EnsureNpcContactTable();
        std::ostringstream q;
        q << "SELECT `npc_entry` FROM `npcchat_contact` WHERE `player_guid`=" << playerGuidRaw
            << " ORDER BY `last_talked_at` DESC LIMIT " << cap;
        if (QueryResult r = WorldDatabase.Query(q.str().c_str()))
            do { out.push_back(r->Fetch()[0].Get<uint32>()); } while (r->NextRow());
        return out;
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

    uint8 NpcChatFactionId(Player const* player)
    {
        std::string faction = PlayerFactionName(player);
        if (faction == "Alliance")
            return 1;
        if (faction == "Horde")
            return 2;
        return 0;
    }

    uint16 NpcChatProgressionPhase()
    {
        // Keep this neutral for now. Later this can read your progression
        // manager/config so the same NPC can have phase-specific barks.
        return 0;
    }

    std::string NpcBarkSqlEscape(std::string s)
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

    void EnsureNpcBarkCacheTable()
    {
        WorldDatabase.Execute(
            "CREATE TABLE IF NOT EXISTS `npcchat_npc_bark_cache` ("
            "`id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
            "`npc_entry` INT UNSIGNED NOT NULL,"
            "`bark_context` VARCHAR(64) NOT NULL,"
            "`faction` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "`race_id` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "`class_id` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "`phase` SMALLINT UNSIGNED NOT NULL DEFAULT 0,"
            "`text` TEXT NOT NULL,"
            "`created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            "`updated_at` TIMESTAMP NULL DEFAULT NULL ON UPDATE CURRENT_TIMESTAMP,"
            "PRIMARY KEY (`id`),"
            "UNIQUE KEY `uq_npcchat_npc_bark` (`npc_entry`, `bark_context`, `faction`, `race_id`, `class_id`, `phase`),"
            "KEY `idx_npcchat_npc_bark_lookup` (`npc_entry`, `bark_context`, `faction`, `race_id`, `class_id`, `phase`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    }

    void EnsureNpcBarkDisableTable()
    {
        WorldDatabase.Execute(
            "CREATE TABLE IF NOT EXISTS `npcchat_npc_bark_disable` ("
            "`npc_entry` INT UNSIGNED NOT NULL,"
            "`bark_context` VARCHAR(64) NOT NULL,"
            "`disabled` TINYINT UNSIGNED NOT NULL DEFAULT 1,"
            "`reason` VARCHAR(255) NOT NULL DEFAULT '',"
            "`created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            "`updated_at` TIMESTAMP NULL DEFAULT NULL ON UPDATE CURRENT_TIMESTAMP,"
            "PRIMARY KEY (`npc_entry`, `bark_context`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    }

    bool IsNpcBarkDisabled(uint32 npcEntry, std::string const& context, std::string* reason = nullptr)
    {
        if (!npcEntry || context.empty())
            return false;

        EnsureNpcBarkDisableTable();

        QueryResult result = WorldDatabase.Query(
            "SELECT `reason` FROM `npcchat_npc_bark_disable` WHERE `npc_entry`={} AND `bark_context`='{}' AND `disabled`=1 LIMIT 1",
            npcEntry, NpcBarkSqlEscape(context));

        if (!result)
            return false;

        if (reason)
        {
            Field* fields = result->Fetch();
            *reason = TrimCopy(fields[0].Get<std::string>());
        }

        return true;
    }

    void SetNpcBarkDisabled(uint32 npcEntry, std::string const& context, bool disabled, std::string const& reason)
    {
        EnsureNpcBarkDisableTable();

        if (disabled)
        {
            WorldDatabase.Execute(
                "REPLACE INTO `npcchat_npc_bark_disable` (`npc_entry`,`bark_context`,`disabled`,`reason`) VALUES ({},'{}',1,'{}')",
                npcEntry, NpcBarkSqlEscape(context), NpcBarkSqlEscape(reason));
        }
        else
        {
            WorldDatabase.Execute(
                "REPLACE INTO `npcchat_npc_bark_disable` (`npc_entry`,`bark_context`,`disabled`,`reason`) VALUES ({},'{}',0,'{}')",
                npcEntry, NpcBarkSqlEscape(context), NpcBarkSqlEscape(reason));
        }
    }

    std::string NpcBarkGenerationKey(uint32 npcEntry, std::string const& context, uint8 faction, uint8 race, uint8 cls, uint16 phase)
    {
        return std::to_string(npcEntry) + ":" + context + ":" + std::to_string(faction) + ":" +
            std::to_string(race) + ":" + std::to_string(cls) + ":" + std::to_string(phase);
    }

    bool TryMarkNpcBarkGenerationPending(std::string const& key)
    {
        std::lock_guard<std::mutex> lock(g_NpcBarkGenerationMutex);
        if (g_NpcBarkGenerationPending[key])
            return false;
        g_NpcBarkGenerationPending[key] = true;
        return true;
    }

    void ClearNpcBarkGenerationPending(std::string const& key)
    {
        std::lock_guard<std::mutex> lock(g_NpcBarkGenerationMutex);
        g_NpcBarkGenerationPending.erase(key);
    }

    std::string LookupNpcBarkCache(uint32 npcEntry, std::string const& context, uint8 faction, uint8 race, uint8 cls, uint16 phase)
    {
        EnsureNpcBarkCacheTable();

        std::ostringstream sql;
        sql << "SELECT `text` FROM `npcchat_npc_bark_cache` "
            << "WHERE `npc_entry`=" << npcEntry
            << " AND `bark_context`='" << NpcBarkSqlEscape(context) << "'"
            << " AND `phase` IN (" << uint32(phase) << ",0)"
            << " AND `faction` IN (" << uint32(faction) << ",0)"
            << " AND `race_id` IN (" << uint32(race) << ",0)"
            << " AND `class_id` IN (" << uint32(cls) << ",0)"
            << " ORDER BY ((`faction`=" << uint32(faction) << ") + (`race_id`=" << uint32(race)
            << ") + (`class_id`=" << uint32(cls) << ") + (`phase`=" << uint32(phase)
            << ")) DESC, `updated_at` DESC, `id` DESC LIMIT 1";

        QueryResult result = WorldDatabase.Query(sql.str().c_str());
        if (!result)
            return "";

        Field* fields = result->Fetch();
        return TrimCopy(fields[0].Get<std::string>());
    }

    bool SaveNpcBarkCache(GenBarkRequest const& req, std::string const& context, std::string const& text)
    {
        std::string line = TrimCopy(text);
        if (req.npcEntry == 0 || context.empty() || line.empty())
            return false;

        EnsureNpcBarkCacheTable();

        std::ostringstream sql;
        sql << "REPLACE INTO `npcchat_npc_bark_cache` "
            << "(`npc_entry`,`bark_context`,`faction`,`race_id`,`class_id`,`phase`,`text`) VALUES ("
            << req.npcEntry << ","
            << "'" << NpcBarkSqlEscape(context) << "',"
            // Universal cache (see SaveQuestBarkCache): 0/0/0 so one bark is reused for everyone and
            // personalized via placeholders at speak time, instead of regenerating per race/class.
            << uint32(0) << ","
            << uint32(0) << ","
            << uint32(0) << ","
            << uint32(req.phase) << ","
            << "'" << NpcBarkSqlEscape(line) << "')";
        WorldDatabase.Execute(sql.str().c_str());
        return true;
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
            relationship = LoadRelationshipKV(playerKey, npc->GetEntry(), player->GetName(), npc->GetName());
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

void GenerateHostileBarkCacheWorker(GenBarkRequest req);

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

    GenBarkRequest BuildAutoHostileBarkRequest(Player* player, Creature* npc)
    {
        GenBarkRequest req;
        req.playerGuidRaw = player->GetGUID().GetRawValue();
        req.npcGuidRaw = npc->GetGUID().GetRawValue();
        req.npcEntry = npc->GetEntry();
        req.playerName = player->GetName();
        req.playerRace = player->getRace();
        req.playerClass = player->getClass();
        req.faction = NpcChatFactionId(player);
        req.phase = NpcChatProgressionPhase();
        req.npcName = npc->GetName();
        req.npcLevel = npc->GetLevel();
        req.creatureType = CreatureTypeStr(npc->GetCreatureType());
        req.isHostile = npc->IsHostileTo(player);
        req.stance = "an enemy";
        req.barkKind = "hostile";
        req.cacheContext = "hostile_first_talk";

        if (CreatureTemplate const* ct = npc->GetCreatureTemplate())
        {
            req.npcSubName = ct->SubName;
            if (req.creatureType.empty())
                req.creatureType = CreatureTypeStr(ct->type);
        }

        if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(npc->GetZoneId()))
            req.zoneName = zone->area_name[0];

        return req;
    }

    bool QueueAutoHostileBarkGeneration(Player* player, Creature* npc, std::string* reason = nullptr)
    {
        if (!player || !npc)
            return false;

        GenBarkRequest req = BuildAutoHostileBarkRequest(player, npc);
        std::string pendingKey = NpcBarkGenerationKey(req.npcEntry, req.cacheContext, req.faction, req.playerRace, req.playerClass, req.phase);
        if (!TryMarkNpcBarkGenerationPending(pendingKey))
        {
            if (reason) *reason = "No cached hostile SQL bark exists; generation is already pending.";
            return false;
        }

        std::thread([req = std::move(req)]() mutable { ::GenerateHostileBarkCacheWorker(std::move(req)); }).detach();
        if (reason) *reason = "No cached hostile SQL bark exists; queued automatic generation.";
        return true;
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

        std::string disabledReason;
        if (IsNpcBarkDisabled(npc->GetEntry(), "hostile_first_talk", &disabledReason))
        {
            if (reason)
                *reason = disabledReason.empty() ?
                "Hostile first-talk is disabled for this NPC." :
                "Hostile first-talk is disabled for this NPC: " + disabledReason;
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

        uint8 faction = NpcChatFactionId(player);
        uint8 race = player->getRace();
        uint8 cls = player->getClass();
        uint16 phase = NpcChatProgressionPhase();

        std::string bark = LookupNpcBarkCache(npc->GetEntry(), "hostile_first_talk", faction, race, cls, phase);

        if (bark.empty() && g_HostileFirstTalkGenerateMissing && !bypassChanceAndCooldown)
        {
            QueueAutoHostileBarkGeneration(player, npc, reason);
            g_HostileBarkPlayerCooldownUntil[playerKey] = now + std::max(10, g_HostileFirstTalkPlayerCooldownSec);
            g_HostileBarkNpcCooldownUntil[npcKey] = now + std::max(10, g_HostileFirstTalkNpcCooldownSec);
            if (!pairKey.empty())
                g_HostileBarkPairCooldownUntil[pairKey] = now + std::max(10, g_HostileFirstTalkPairCooldownSec);
            return false;
        }

        // Backward compatibility: old manually generated .hostile_barks files
        // can still be used if no SQL cache row exists.
        if (bark.empty())
        {
            std::map<std::string, std::string> barks;
            std::string barksPath = SharedHostileBarksFilePath(npc->GetName(), npc->GetEntry());
            {
                std::lock_guard<std::mutex> lock(g_FileMutex);
                barks = LoadKeyValueFile(barksPath);
            }

            if (barks.empty())
            {
                if (reason) *reason = g_HostileFirstTalkGenerateMissing ?
                    "No cached hostile SQL bark or legacy hostile bark file exists; automatic generation did not queue." :
                    "No cached hostile SQL bark exists. Use .npcc gen bark hostile first, or enable NpcChat.HostileFirstTalk.GenerateMissing.";
                return false;
            }

            bark = SelectHostileBark(barks, npc);
        }
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

    struct LearnableSpell
    {
        uint32 spellId = 0;
        uint32 reqLevel = 0;
        std::string name;
    };

    std::string TrainerSpellName(uint32 spellId)
    {
        if (SpellInfo const* si = sSpellMgr->GetSpellInfo(spellId))
            if (si->SpellName[0] && *si->SpellName[0])
                return si->SpellName[0];
        return "";
    }

    // Spells the targeted trainer can teach this player RIGHT NOW. Uses the core's own eligibility
    // (class/profession/skill/level/already-known) via Trainer::CanTeachSpell, so it matches exactly
    // what the real trainer window would offer.
    std::vector<LearnableSpell> GetLearnableTrainerSpells(Player* player, Creature* npc)
    {
        std::vector<LearnableSpell> out;
        if (!player || !npc)
            return out;

        Trainer::Trainer const* trainer = sObjectMgr->GetTrainer(npc->GetEntry());
        if (!trainer || !trainer->IsTrainerValidForPlayer(player))
            return out;

        for (Trainer::Spell const& s : trainer->GetSpells())
        {
            if (!trainer->CanTeachSpell(player, &s))
                continue;

            LearnableSpell ls;
            ls.spellId = s.SpellId;
            ls.reqLevel = s.ReqLevel;
            ls.name = TrainerSpellName(s.SpellId);
            if (ls.name.empty())
                ls.name = "a new skill";
            out.push_back(ls);
        }
        return out;
    }

    // The single most interesting spell to mention: the highest required level the player has just
    // qualified for (the newest unlock), ties broken by lowest spell id for stability.
    LearnableSpell PickTrainerSpellToMention(std::vector<LearnableSpell> const& spells)
    {
        LearnableSpell best;
        bool have = false;
        for (LearnableSpell const& s : spells)
        {
            if (!have || s.reqLevel > best.reqLevel ||
                (s.reqLevel == best.reqLevel && s.spellId < best.spellId))
            {
                best = s;
                have = true;
            }
        }
        return best;
    }

    std::string TrainerTypeName(Creature* npc)
    {
        if (npc)
            if (Trainer::Trainer const* trainer = sObjectMgr->GetTrainer(npc->GetEntry()))
            {
                switch (trainer->GetTrainerType())
                {
                case Trainer::Type::Class:      return "class trainer";
                case Trainer::Type::Mount:      return "riding trainer";
                case Trainer::Type::Tradeskill: return "profession trainer";
                case Trainer::Type::Pet:        return "pet trainer";
                default: break;
                }
            }
        return "trainer";
    }

    // A compact, leveled description of everything this trainer teaches, for feeding to the AI so it
    // can hold a real training conversation. Trainers teach a LOT, so when focusPlayerLevel is set we
    // list a window around the player's level (what they can train now or soon) and summarize the rest
    // as a count; with it off we list the whole curriculum (capped) for static prompt generation.
    std::string BuildTrainerCurriculumText(Creature* npc, Player* player, bool focusPlayerLevel)
    {
        Trainer::Trainer const* trainer = npc ? sObjectMgr->GetTrainer(npc->GetEntry()) : nullptr;
        if (!trainer)
            return "";

        struct Entry { uint32 level; std::string name; };
        std::vector<Entry> all;
        for (Trainer::Spell const& s : trainer->GetSpells())
        {
            std::string n = TrainerSpellName(s.SpellId);
            if (n.empty())
                continue;
            all.push_back({ uint32(s.ReqLevel), n });
        }
        if (all.empty())
            return "";

        std::sort(all.begin(), all.end(), [](Entry const& a, Entry const& b)
            {
                if (a.level != b.level) return a.level < b.level;
                return a.name < b.name;
            });

        uint32 plevel = player ? player->GetLevel() : 0;
        int low = focusPlayerLevel ? std::max(0, int(plevel) - 6) : 0;
        int high = focusPlayerLevel ? int(plevel) + 8 : 100000;
        uint32 minLvl = all.front().level;
        uint32 maxLvl = all.back().level;

        std::map<uint32, std::vector<std::string>> byLevel;
        size_t listed = 0;
        size_t omitted = 0;
        for (Entry const& e : all)
        {
            if (focusPlayerLevel && (int(e.level) < low || int(e.level) > high)) { ++omitted; continue; }
            if (listed >= 30) { ++omitted; continue; }
            byLevel[e.level].push_back(e.name);
            ++listed;
        }

        std::ostringstream ss;
        for (auto const& kv : byLevel)
        {
            ss << "- " << (kv.first == 0 ? std::string("Basic") : ("Level " + std::to_string(kv.first))) << ": ";
            for (size_t i = 0; i < kv.second.size(); ++i)
            {
                if (i) ss << ", ";
                ss << kv.second[i];
            }
            ss << "\n";
        }
        if (omitted)
            ss << "- (and " << omitted << " more abilities ranging from level " << minLvl << " to " << maxLvl << ")\n";

        return TrimCopy(ss.str());
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

    // Spell-aware trainer bark: greets the player about an actual spell/skill they can learn here now.
    // The cached line is universal and uses a {spell} placeholder filled at speak time, so one
    // generated bark serves every eligible player and every spell. Cache-only (pre-generate first).
    bool SpeakCachedTrainerSpellBark(Player* player, Creature* npc, bool bypassChanceAndCooldown, std::string* reason = nullptr)
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

        std::vector<LearnableSpell> learnable = GetLearnableTrainerSpells(player, npc);
        if (learnable.empty())
        {
            if (reason) *reason = "Player has nothing new to learn from this trainer right now.";
            return false;
        }

        time_t now = std::time(nullptr);
        uint64_t playerKey = player->GetGUID().GetRawValue();
        uint64_t npcKey = npc->GetGUID().GetRawValue();
        std::string pairKey = TrainerPairKey(player, npc);
        if (!bypassChanceAndCooldown)
        {
            if (player->IsInCombat()) { if (reason) *reason = "Player is in combat."; return false; }
            if (g_TrainerBarkPlayerCooldownUntil[playerKey] > now) { if (reason) *reason = "Player trainer bark cooldown is still active."; return false; }
            if (g_TrainerBarkNpcCooldownUntil[npcKey] > now) { if (reason) *reason = "Trainer NPC cooldown is still active."; return false; }
            if (!pairKey.empty() && g_TrainerBarkPairCooldownUntil[pairKey] > now) { if (reason) *reason = "Player/trainer pair cooldown is still active."; return false; }
            if (g_TrainerBarksChancePct <= 0) { if (reason) *reason = "Chance is set to 0."; return false; }
            if (g_TrainerBarksChancePct < 100 && (std::rand() % 100) >= g_TrainerBarksChancePct) { if (reason) *reason = "Chance roll did not fire."; return false; }
        }

        uint8 faction = NpcChatFactionId(player);
        uint16 phase = NpcChatProgressionPhase();
        std::string bark = LookupNpcBarkCache(npc->GetEntry(), "trainer_spell", faction, player->getRace(), player->getClass(), phase);
        if (bark.empty())
        {
            if (reason) *reason = "No cached trainer-spell bark exists. Use .npcc gen bark trainerspell first.";
            return false;
        }

        LearnableSpell chosen = PickTrainerSpellToMention(learnable);
        ReplaceAllInPlace(bark, "{spell}", chosen.name);
        ReplaceAllInPlace(bark, "{spell_level}", std::to_string(chosen.reqLevel));
        bark = ApplyPlayerPlaceholders(bark, player, npc);
        npc->Say(bark, LANG_UNIVERSAL);

        if (!bypassChanceAndCooldown)
        {
            g_TrainerBarkPlayerCooldownUntil[playerKey] = now + std::max(1, g_TrainerBarksPlayerCooldownSec);
            g_TrainerBarkNpcCooldownUntil[npcKey] = now + std::max(1, g_TrainerBarksNpcCooldownSec);
            if (!pairKey.empty())
                g_TrainerBarkPairCooldownUntil[pairKey] = now + std::max(1, g_TrainerBarksPairCooldownSec);
        }
        if (reason) *reason = "Trainer-spell bark spoken (" + chosen.name + ").";
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

        if (!req.trainerInfo.empty())
        {
            ss << "\n\nYou are a trainer. These are abilities you can personally teach, and the level a student must reach for each:\n";
            ss << req.trainerInfo;
            ss << "\nThis list is centered on the current student's level. If they ask what they can learn or train, answer from it naturally - name specific abilities and the levels they unlock, point out what they're ready for now, and tease what's coming soon.";
            ss << "\nSpeak as a mentor, not a menu: do not dump the whole list at once, and do not invent abilities that are not listed.";
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

    if (!req.trainerInfo.empty())
    {
        ss << "\nThis NPC is a trainer. Abilities they can teach, by level:\n";
        ss << req.trainerInfo << "\n";
        ss << "Weave this expertise into their identity - what they take pride in teaching, how they speak about the craft, what they expect of students - but do NOT just list the abilities verbatim.\n";
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

        sharedSubPromptNames = MergeSubPromptNames(LoadSubPromptNameList(SharedSubPromptListFilePath(req.npcName, req.npcEntry)), req.autoTags);
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
        "Output ONLY key=value lines. Each key=value pair MUST be on its own separate newline. Do not use markdown. Do not use quotes around the values. "
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
        relationshipText = LoadRelationshipText(req.playerGuidRaw, req.npcEntry, req.playerName, req.npcName);
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
    generated = NormalizeGeneratedKeyValueText(generated);

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
        "Output EXACTLY these seven key=value lines, one key per physical newline, in this order: "
        "aggro_intro, warning, threat, combat_taunt, low_health, victory, general. "
        "Do not use markdown, numbering, bullets, quotes around values, JSON, or paragraphs. "
        "Each value must be one short in-character spoken line suitable for the enemy to say when a real player approaches. "
        "Never output None, null, N/A, undefined, or empty values. "
        "Never put another key name inside a value. "
        "Do not mention AI, files, prompts, players, servers, tokens, aggro tables, or game mechanics. "
        "Use reusable placeholders only when useful: {player}, {race}, {class}, {level}, {faction}, {npc}, {zone}, {sir_miss}. "
        "Prefer placeholders over hardcoding the player name, race, or class. "
        "Example format: aggro_intro=You have trespassed far enough, {sir_miss}.";
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

std::string BuildFallbackHostileBarks(GenBarkRequest const& req)
{
    std::string enemyName = req.npcName.empty() ? std::string("This enemy") : req.npcName;

    std::ostringstream ss;
    ss << "aggro_intro=" << enemyName << " fixes a hard stare on {player}. You have come too far, {sir_miss}.\n";
    ss << "warning=Turn back, {race}, before this place becomes your grave.\n";
    ss << "threat=You will not leave here alive, {class}.\n";
    ss << "combat_taunt=Fight, then. Let your courage break against me.\n";
    ss << "low_health=No... this is not how my story ends.\n";
    ss << "victory=Another fool falls where they should never have stood.\n";
    ss << "general=This ground belongs to those strong enough to hold it.";
    return ss.str();
}

std::string BuildFallbackHostileSingleBark(GenBarkRequest const& req)
{
    std::string enemyName = req.npcName.empty() ? std::string("This enemy") : req.npcName;
    return enemyName + " fixes a hard stare on {player}. You have come too far, {sir_miss}.";
}

std::string BuildGenerateHostileSingleBarkSystemPrompt()
{
    return
        "You are creating ONE cached hostile NPC first-talk bark for a World of Warcraft enemy. "
        "Return ONLY the spoken line. No markdown, no quotes, no key names, no JSON, no bullets. "
        "Write one short in-character sentence, or two very short sentences at most. "
        "Never output None, null, N/A, undefined, or empty text. "
        "Do not mention AI, files, prompts, servers, SQL, tokens, NPC IDs, aggro tables, or game mechanics. "
        "Use reusable placeholders only when useful: {player}, {race}, {class}, {level}, {faction}, {npc}, {zone}, {sir_miss}. "
        "Prefer placeholders over hardcoding the player name, race, or class.";
}

std::string BuildGenerateHostileSingleBarkUserPrompt(GenBarkRequest const& req, std::string const& sharedPrompt, std::string const& sharedSubPrompts, std::string const& existingBark)
{
    std::ostringstream ss;
    ss << "Hostile NPC facts:\n";
    ss << "- Name: " << req.npcName << "\n";
    ss << "- Entry ID: " << req.npcEntry << "\n";
    if (!req.npcSubName.empty()) ss << "- Subname/title: " << req.npcSubName << "\n";
    if (req.npcLevel) ss << "- Level: " << req.npcLevel << "\n";
    if (!req.creatureType.empty()) ss << "- Creature type: " << req.creatureType << "\n";
    if (!req.rankStr.empty()) ss << "- Rank: " << req.rankStr << "\n";
    if (!req.roleStr.empty()) ss << "- Role/NPC flags: " << req.roleStr << "\n";
    if (!req.zoneName.empty()) ss << "- Zone: " << req.zoneName << "\n";

    ss << "\nApproaching player placeholders:\n";
    ss << "- Player: {player}\n";
    ss << "- Race: " << PlayerRaceName(req.playerRace) << " ({race})\n";
    ss << "- Class: " << PlayerClassName(req.playerClass) << " ({class})\n";
    ss << "- Faction: " << (req.faction == 1 ? "Alliance" : req.faction == 2 ? "Horde" : "Neutral") << " ({faction})\n";

    if (!sharedSubPrompts.empty())
        ss << "\nAttached shared subprompts:\n" << sharedSubPrompts << "\n";

    if (!sharedPrompt.empty())
        ss << "\nShared NPC profile:\n" << sharedPrompt << "\n";

    if (!existingBark.empty())
        ss << "\nExisting cached bark, if improving/replacing it:\n" << existingBark << "\n";

    if (!req.extraInstruction.empty())
        ss << "\nExtra direction:\n" << req.extraInstruction << "\n";

    ss << "\nGenerate ONE reusable hostile first-talk line now. It should sound like the NPC, not a generic monster.";
    return ss.str();
}

void GenerateHostileBarkCacheWorker(GenBarkRequest req)
{
    std::string pendingKey = NpcBarkGenerationKey(req.npcEntry, req.cacheContext, req.faction, req.playerRace, req.playerClass, req.phase);

    std::string sharedPrompt;
    std::string sharedSubPrompts;
    std::string existingBark;

    {
        std::lock_guard<std::mutex> lock(g_FileMutex);
        EnsureNpcChatDirectoriesAndDefaultPrompt();
        sharedPrompt = ReadWholeTextFile(SharedPromptFilePath(req.npcName, req.npcEntry));
        sharedSubPrompts = LoadSubPromptBlocks(MergeSubPromptNames(LoadSubPromptNameList(SharedSubPromptListFilePath(req.npcName, req.npcEntry)), req.autoTags));
    }

    existingBark = LookupNpcBarkCache(req.npcEntry, req.cacheContext, req.faction, req.playerRace, req.playerClass, req.phase);

    NpcChat_ApiConfig cfg = BuildGenerationApiConfig(260);
    NpcChat_LLMResult res = NpcChat_CallLLM(
        cfg,
        BuildGenerateHostileSingleBarkSystemPrompt(),
        BuildGenerateHostileSingleBarkUserPrompt(req, sharedPrompt, sharedSubPrompts, existingBark));

    if (!res.success || TrimCopy(res.text).empty())
    {
        std::string fallback = BuildFallbackHostileSingleBark(req);
        SaveNpcBarkCache(req, req.cacheContext, fallback);
        QueueSystemMessage(req.playerGuidRaw, "NPC Chat hostile SQL bark generation failed or returned empty text; saved fallback DB cache row.");
        ClearNpcBarkGenerationPending(pendingKey);
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

    // Single-line cache: take the first non-empty line and reject key=value / weak junk.
    std::istringstream in(generated);
    std::string firstLine;
    std::string line;
    while (std::getline(in, line))
    {
        line = TrimCopy(line);
        if (!line.empty())
        {
            firstLine = line;
            break;
        }
    }
    generated = StripWrappingQuotes(firstLine.empty() ? generated : firstLine);

    if (generated.find('=') != std::string::npos || IsBadGeneratedBarkValue(generated))
    {
        QueueSystemMessage(req.playerGuidRaw, "NPC Chat hostile SQL bark generation returned malformed/weak text; saved fallback DB cache row.");
        generated = BuildFallbackHostileSingleBark(req);
    }

    if (!SaveNpcBarkCache(req, req.cacheContext, generated))
        QueueSystemMessage(req.playerGuidRaw, "NPC Chat hostile SQL bark generation finished but failed to save DB cache row.");
    else
    {
        // Generating a hostile bark opts this NPC into aggro-driven speak.
        SetNpcProfileCanSpeak(req.npcEntry, true, 0);
        QueueSystemMessage(req.playerGuidRaw, "NPC Chat hostile SQL bark saved for " + req.npcName + " entry " + std::to_string(req.npcEntry) + ".");
    }

    ClearNpcBarkGenerationPending(pendingKey);
}

std::string BuildGenerateTrainerSpellBarkSystemPrompt()
{
    return
        "You are writing ONE short, reusable in-character line for a World of Warcraft trainer who has just noticed a nearby student is ready to learn a new ability. "
        "Use the literal placeholder {spell} exactly where the ability's name should appear - do NOT invent or name a specific spell yourself. You may also use {spell_level} for its level. "
        "Return ONLY the spoken line. No quotes, no markdown, no key names, no JSON. One or two short sentences, under about 25 words. "
        "Sound like a proud, eager, or gruff mentor as fits the trainer. Do not mention AI, files, prompts, servers, gold cost, trainer windows, menus, SQL, or game mechanics. "
        "Other placeholders you may use: {player}, {race}, {class}, {level}, {faction}, {npc}, {zone}, {sir_miss}. Prefer placeholders over hardcoding. "
        "Example shape: Ah, {player}! You're ready for {spell} at last. Step closer and I'll show you.";
}

std::string BuildGenerateTrainerSpellBarkUserPrompt(GenBarkRequest const& req)
{
    std::ostringstream ss;
    ss << "Trainer facts:\n";
    ss << "- Name: " << req.npcName << "\n";
    if (!req.npcSubName.empty()) ss << "- Title: " << req.npcSubName << "\n";
    if (!req.roleStr.empty())    ss << "- Kind: " << req.roleStr << "\n";
    if (!req.zoneName.empty())   ss << "- Zone: " << req.zoneName << "\n";
    if (!req.extraInstruction.empty())
        ss << "\n" << req.extraInstruction << "\n";
    ss << "\nWrite the single reusable line now. Put {spell} where the ability name goes; do not name a real spell.";
    return ss.str();
}

void GenerateTrainerSpellBarkCacheWorker(GenBarkRequest req)
{
    NpcChat_ApiConfig cfg = BuildGenerationApiConfig(220);
    NpcChat_LLMResult res = NpcChat_CallLLM(
        cfg,
        BuildGenerateTrainerSpellBarkSystemPrompt(),
        BuildGenerateTrainerSpellBarkUserPrompt(req));

    std::string const fallback = "Ah, {player}! You look ready to learn {spell}. Step closer and I will teach you.";
    std::string generated;

    if (res.success && !TrimCopy(res.text).empty())
    {
        generated = TrimCopy(res.text);
        if (generated.rfind("```", 0) == 0)
        {
            size_t firstNl = generated.find('\n');
            size_t lastFence = generated.rfind("```");
            if (firstNl != std::string::npos && lastFence != std::string::npos && lastFence > firstNl)
                generated = TrimCopy(generated.substr(firstNl + 1, lastFence - firstNl - 1));
        }

        std::istringstream in(generated);
        std::string line, first;
        while (std::getline(in, line))
        {
            line = TrimCopy(line);
            if (!line.empty()) { first = line; break; }
        }
        generated = StripWrappingQuotes(first.empty() ? generated : first);
    }

    // The {spell} placeholder is mandatory: without it the speak-time substitution has nowhere to put
    // the ability name. Fall back to a safe template if the model omitted it or returned junk.
    if (generated.empty() || IsBadGeneratedBarkValue(generated) || generated.find("{spell}") == std::string::npos)
        generated = fallback;

    if (!SaveNpcBarkCache(req, "trainer_spell", generated))
        QueueSystemMessage(req.playerGuidRaw, "NPC Chat trainer-spell bark generation finished but failed to save the DB cache row.");
    else
        QueueSystemMessage(req.playerGuidRaw, "NPC Chat trainer-spell bark saved for " + req.npcName + " entry " + std::to_string(req.npcEntry) + ".");
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
        sharedSubPrompts = LoadSubPromptBlocks(MergeSubPromptNames(LoadSubPromptNameList(SharedSubPromptListFilePath(req.npcName, req.npcEntry)), req.autoTags));
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
    generated = NormalizeGeneratedKeyValueText(generated);
    if (!HasUsableBarkKeys(generated, { "aggro_intro", "warning", "threat", "combat_taunt", "low_health", "victory", "general" }, 3))
    {
        QueueSystemMessage(req.playerGuidRaw, "NPC Chat hostile bark generation returned malformed/weak key-value text; saving safe fallback barks instead.");
        generated = BuildFallbackHostileBarks(req);
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
            "`min_level` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "`max_level` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "`generated_level` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
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

    std::vector<uint32> GetNpcEnderQuestIds(uint32 npcEntry)
    {
        std::vector<uint32> ids;
        QueryResult result = WorldDatabase.Query("SELECT `quest` FROM `creature_questender` WHERE `id` = {} ORDER BY `quest`", npcEntry);
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

    std::vector<uint32> GetNearbyQuestgiverEntriesFromDb(Player const* player, float range, uint32 maxRows, char const* joinTable = "creature_queststarter")
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
            << ((joinTable && *joinTable) ? (std::string("INNER JOIN `") + joinTable + "` qs ON qs.`id` = c.`id` ") : std::string())
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

    // Quests this NPC is the ENDER of, that the player is currently on but has NOT yet completed
    // (objectives unfinished). This is the "did you do it yet?" nudge set. A quest sitting at
    // QUEST_STATUS_COMPLETE (ready to hand in) is intentionally excluded - that's a different,
    // happier "you're done, turn it in" line we can add later.
    std::vector<QuestBarkQuestInfo> GetActiveEnderQuestInfos(Player* player, Creature* npc, uint32 onlyQuestId = 0)
    {
        std::vector<QuestBarkQuestInfo> out;
        if (!player || !npc)
            return out;

        std::vector<uint32> enderIds = GetNpcEnderQuestIds(npc->GetEntry());
        for (uint32 questId : enderIds)
        {
            if (onlyQuestId && questId != onlyQuestId)
                continue;
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;
            if (player->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
                continue;
            out.push_back(BuildQuestInfo(quest));
            if (static_cast<int>(out.size()) >= g_QuestBarksMaxQuestsCheckedPerNpc)
                break;
        }
        return out;
    }

    // Generation-time quest sets: pulled straight from the NPC's quest tables, ignoring the player's
    // quest log. The gen commands use these so you can pre-build (or rebuild) barks for quests you've
    // already taken or already finished. The player only supplies requesting context; the saved cache
    // row is universal (faction/race/class stored as 0 and personalized at speak time).
    std::vector<QuestBarkQuestInfo> GetAllNpcStarterQuestInfos(Creature* npc, uint32 onlyQuestId = 0)
    {
        std::vector<QuestBarkQuestInfo> out;
        if (!npc)
            return out;
        for (uint32 questId : GetNpcStarterQuestIds(npc->GetEntry()))
        {
            if (onlyQuestId && questId != onlyQuestId)
                continue;
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;
            out.push_back(BuildQuestInfo(quest));
            if (static_cast<int>(out.size()) >= g_QuestBarksMaxQuestsCheckedPerNpc)
                break;
        }
        return out;
    }

    std::vector<QuestBarkQuestInfo> GetAllNpcEnderQuestInfos(Creature* npc, uint32 onlyQuestId = 0)
    {
        std::vector<QuestBarkQuestInfo> out;
        if (!npc)
            return out;
        for (uint32 questId : GetNpcEnderQuestIds(npc->GetEntry()))
        {
            if (onlyQuestId && questId != onlyQuestId)
                continue;
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
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

    uint8 QuestBarkMinLevel(std::vector<QuestBarkQuestInfo> const& quests)
    {
        uint32 minLevel = 0;
        for (QuestBarkQuestInfo const& q : quests)
        {
            uint32 candidate = q.minLevel ? q.minLevel : (q.questLevel > 0 ? static_cast<uint32>(q.questLevel) : 0);
            if (!candidate)
                continue;
            if (!minLevel || candidate < minLevel)
                minLevel = candidate;
        }
        return static_cast<uint8>(std::min<uint32>(minLevel, 255));
    }

    uint8 QuestBarkMaxLevel(std::vector<QuestBarkQuestInfo> const& quests)
    {
        // 0 means no upper limit. The cache key already identifies the current
        // acceptable quest set; max_level is available for later stricter phase
        // or trainer-chain policies.
        return 0;
    }

    std::string LookupQuestBarkCache(uint32 npcEntry, std::string const& questKey, uint8 faction, uint8 race, uint8 cls, uint16 phase, uint8 playerLevel, std::string const& barkType = "quest_available")
    {
        EnsureQuestBarkCacheTable();
        std::ostringstream sql;
        sql << "SELECT `text` FROM `npcchat_quest_bark_cache` "
            << "WHERE `npc_entry`=" << npcEntry
            << " AND `quest_key`='" << SqlEscape(questKey) << "'"
            << " AND `phase` IN (" << phase << ",0)"
            << " AND `bark_type`='" << SqlEscape(barkType) << "'"
            << " AND (`min_level`=0 OR `min_level` <= " << uint32(playerLevel) << ")"
            << " AND (`max_level`=0 OR `max_level` >= " << uint32(playerLevel) << ")"
            << " AND `faction` IN (" << uint32(faction) << ",0)"
            << " AND `race_id` IN (" << uint32(race) << ",0)"
            << " AND `class_id` IN (" << uint32(cls) << ",0)"
            << " ORDER BY ((`faction`=" << uint32(faction) << ") + (`race_id`=" << uint32(race)
            << ") + (`class_id`=" << uint32(cls) << ") + (`phase`=" << phase << ") + (`min_level` > 0)) DESC, `min_level` DESC, `updated_at` DESC, `id` DESC LIMIT 1";
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
            << "(`npc_entry`,`quest_key`,`quest_ids`,`faction`,`race_id`,`class_id`,`phase`,`min_level`,`max_level`,`generated_level`,`bark_type`,`text`) VALUES ("
            << req.npcEntry << ","
            << "'" << SqlEscape(req.questKey) << "',"
            << "'" << SqlEscape(QuestIdsString(req.quests)) << "',"
            // Universal cache: store 0/0/0 for faction/race/class so ONE bark is reused for every
            // player and never regenerated per-race. The text uses {race}/{class}/{faction}/{player}
            // placeholders that ApplyPlayerPlaceholders fills in per viewer at speak time. Quest
            // acceptability (GetAcceptableQuestInfos) still gates who actually hears it.
            << uint32(0) << ","
            << uint32(0) << ","
            << uint32(0) << ","
            << uint32(req.phase) << ","
            << uint32(req.minLevel) << ","
            << uint32(req.maxLevel) << ","
            << uint32(req.generatedLevel) << ","
            << "'" << SqlEscape(req.barkType.empty() ? std::string("quest_available") : req.barkType) << "',"
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
            "This request is for ONE quest only. Do not blend multiple quests into one bark. "
            "Use reusable placeholders when useful: {player}, {race}, {class}, {level}, {faction}, {npc}, {zone}, {sir_miss}. "
            "Prefer placeholders over hardcoding the player name, race, or class.";
    }

    std::string BuildGenerateQuestEnderBarkSystemPrompt()
    {
        return
            "You are creating one reusable cached quest-progress bark for a World of Warcraft questgiver. "
            "The player is CURRENTLY ON this NPC's quest but has NOT finished it yet, and is walking past. "
            "The NPC is checking in - essentially asking 'have you done it yet?' - impatient, hopeful, gruff, or encouraging as fits the NPC. "
            "Return ONLY the spoken line. No markdown, no quotes, no key names. "
            "Write one or two short in-character sentences. "
            "Do not give away the solution or summarize the quest; just prod the player about their unfinished task. "
            "Do not mention AI, files, prompts, servers, tokens, quest IDs, gossip windows, SQL, or game mechanics. "
            "This request is for ONE quest only. Do not blend multiple quests into one bark. "
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
        ss << "- Quest bark min level stored for this cache row: " << uint32(req.minLevel) << "\n";
        ss << "- Faction: " << (req.faction == 1 ? "Alliance" : req.faction == 2 ? "Horde" : "Neutral") << " ({faction})\n";
        if (!sharedPrompt.empty())
            ss << "\nShared NPC character prompt:\n" << TruncateText(sharedPrompt, 900) << "\n";
        ss << "\nQuest this bark is for:\n";
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
        existingBark = LookupQuestBarkCache(req.npcEntry, req.questKey, req.faction, req.playerRace, req.playerClass, req.phase, req.playerLevel, req.barkType);
        NpcChat_ApiConfig cfg = BuildGenerationApiConfig(350);
        bool isEnder = (req.barkType == "quest_ender");
        NpcChat_LLMResult res = NpcChat_CallLLM(cfg,
            isEnder ? BuildGenerateQuestEnderBarkSystemPrompt() : BuildGenerateQuestBarkSystemPrompt(),
            BuildGenerateQuestBarkUserPrompt(req, sharedPrompt, existingBark));
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
        generated = NormalizeGeneratedKeyValueText(generated);
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
            QueueSystemMessage(req.playerGuidRaw, "NPC Chat " + std::string(req.barkType == "quest_ender" ? "quest-ender" : "quest intro") +
                " bark saved to DB cache for quest key " + req.questKey + ".");
    }

    GenQuestBarkRequest BuildQuestBarkRequest(Player* player, Creature* npc, std::vector<QuestBarkQuestInfo> quests, std::string const& extraInstruction, bool notify, std::string const& barkType = "quest_available")
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
        req.minLevel = QuestBarkMinLevel(req.quests);
        req.maxLevel = QuestBarkMaxLevel(req.quests);
        req.generatedLevel = req.playerLevel;
        req.questKey = BuildQuestKey(req.quests);
        req.extraInstruction = extraInstruction;
        req.notifyPlayer = notify;
        req.barkType = barkType;
        req.autoTags = ResolveCreatureAutoTags(npc);
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

        time_t now = std::time(nullptr);
        for (QuestBarkQuestInfo const& q : quests)
        {
            std::vector<QuestBarkQuestInfo> singleQuest;
            singleQuest.push_back(q);

            GenQuestBarkRequest req = BuildQuestBarkRequest(player, npc, singleQuest, "", false);
            if (req.questKey.empty())
                continue;

            std::string genKey = std::to_string(req.npcEntry) + ":" + req.questKey + ":" + std::to_string(req.faction) + ":" + std::to_string(req.playerRace) + ":" + std::to_string(req.playerClass) + ":" + std::to_string(req.minLevel);
            if (g_QuestBarkGenerationCooldownUntil[genKey] > now)
                continue;

            g_QuestBarkGenerationCooldownUntil[genKey] = now + 3600;
            std::thread(GenerateQuestBarkWorker, std::move(req)).detach();
        }
    }

    uint32 StartQuestBarkGenerationForEachQuest(ChatHandler* handler, Player* player, Creature* npc, std::vector<QuestBarkQuestInfo> const& quests, std::string const& extraInstruction, std::string const& barkType = "quest_available")
    {
        if (!player || !npc || quests.empty())
            return 0;

        uint32 started = 0;
        std::ostringstream keys;

        for (QuestBarkQuestInfo const& q : quests)
        {
            std::vector<QuestBarkQuestInfo> singleQuest;
            singleQuest.push_back(q);

            GenQuestBarkRequest qReq = BuildQuestBarkRequest(player, npc, singleQuest, StripWrappingQuotes(extraInstruction), true, barkType);
            if (qReq.questKey.empty())
                continue;

            if (started)
                keys << ", ";
            keys << qReq.questKey;

            std::thread(GenerateQuestBarkWorker, std::move(qReq)).detach();
            ++started;
        }

        if (handler)
        {
            if (started == 1)
                handler->PSendSysMessage("NPC Chat quest bark generation started for {} quest key {}.", npc->GetName(), keys.str().c_str());
            else
                handler->PSendSysMessage("NPC Chat quest bark generation started for {} separate quest barks on {}. Quest keys: {}.", started, npc->GetName(), keys.str().c_str());

            if (started)
                handler->PSendSysMessage("Each quest is saved as its own DB bark row. You will get system messages as they save.");
        }

        return started;
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

        std::string selectedQuestKey;
        std::string bark;

        // Quest barks are stored and played per quest, never as a combined multi-quest message.
        for (QuestBarkQuestInfo const& q : quests)
        {
            std::vector<QuestBarkQuestInfo> singleQuest;
            singleQuest.push_back(q);

            std::string questKey = BuildQuestKey(singleQuest);
            if (questKey.empty())
                continue;

            bark = LookupQuestBarkCache(npc->GetEntry(), questKey, PlayerFactionId(player), player->getRace(), player->getClass(), 0, player->GetLevel());
            if (!bark.empty())
            {
                selectedQuestKey = questKey;
                break;
            }
        }

        if (bark.empty())
        {
            MaybeGenerateMissingQuestBark(player, npc, quests);
            if (reason) *reason = g_QuestBarksGenerateMissing ?
                "No cached per-quest bark existed. Generation was queued for each acceptable quest; try again after it saves." :
                "No cached per-quest bark exists yet. Target this questgiver and use .npcc gen quest or .npcc gen bark quest first.";
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

        if (reason) *reason = selectedQuestKey.empty() ? "Quest bark spoken." : "Quest bark spoken for quest key " + selectedQuestKey + ".";
        return true;
    }

    // ---- Quest ENDER barks: "did you do it yet?" when you pass the turn-in NPC mid-quest ----

    void MaybeGenerateMissingQuestEnderBark(Player* player, Creature* npc, std::vector<QuestBarkQuestInfo> quests)
    {
        if (!g_QuestBarksGenerateMissing || quests.empty())
            return;

        time_t now = std::time(nullptr);
        for (QuestBarkQuestInfo const& q : quests)
        {
            std::vector<QuestBarkQuestInfo> singleQuest;
            singleQuest.push_back(q);

            GenQuestBarkRequest req = BuildQuestBarkRequest(player, npc, singleQuest, "", false, "quest_ender");
            if (req.questKey.empty())
                continue;

            std::string genKey = "ender:" + std::to_string(req.npcEntry) + ":" + req.questKey;
            if (g_QuestBarkGenerationCooldownUntil[genKey] > now)
                continue;

            g_QuestBarkGenerationCooldownUntil[genKey] = now + 3600;
            std::thread(GenerateQuestBarkWorker, std::move(req)).detach();
        }
    }

    Creature* FindNearbyQuestEnderForBark(Player* player)
    {
        if (!player || !player->IsInWorld() || player->IsInCombat())
            return nullptr;

        if (Unit* selected = player->GetSelectedUnit())
        {
            if (Creature* selectedNpc = selected->ToCreature())
            {
                if (selectedNpc->IsInWorld() && selectedNpc->IsAlive() &&
                    player->IsWithinDist(selectedNpc, g_QuestBarksTriggerDistance, true) &&
                    !GetActiveEnderQuestInfos(player, selectedNpc).empty())
                    return selectedNpc;
            }
        }

        if (g_QuestBarksSelectedOnly)
            return nullptr;

        std::vector<uint32> entries = GetNearbyQuestgiverEntriesFromDb(player, g_QuestBarksTriggerDistance, 32, "creature_questender");
        Creature* best = nullptr;
        float bestDist = g_QuestBarksTriggerDistance + 1.0f;

        for (uint32 entry : entries)
        {
            Creature* candidate = player->FindNearestCreature(entry, g_QuestBarksTriggerDistance, true);
            if (!candidate || !candidate->IsInWorld() || !candidate->IsAlive())
                continue;
            if (!player->IsWithinDist(candidate, g_QuestBarksTriggerDistance, true))
                continue;
            if (GetActiveEnderQuestInfos(player, candidate).empty())
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

    bool SpeakCachedQuestEnderBark(Player* player, Creature* npc, bool bypassChanceAndCooldown, uint32 onlyQuestId = 0, std::string* reason = nullptr)
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
        if (!player->IsWithinDist(npc, g_QuestBarksTriggerDistance, true))
        {
            if (reason) *reason = "Quest ender is outside quest bark range.";
            return false;
        }

        uint64 playerKey = player->GetGUID().GetRawValue();
        uint64 npcKey = npc->GetGUID().GetRawValue();
        std::string pairKey = std::to_string(playerKey) + ":questender:" + std::to_string(npc->GetEntry());
        time_t now = std::time(nullptr);

        if (!bypassChanceAndCooldown)
        {
            if (player->IsInCombat()) { if (reason) *reason = "Player is in combat."; return false; }
            if (g_QuestBarkPlayerCooldownUntil[playerKey] > now) { if (reason) *reason = "Player quest bark cooldown is still active."; return false; }
            if (g_QuestBarkNpcCooldownUntil[npcKey] > now) { if (reason) *reason = "Quest ender cooldown is still active."; return false; }
            if (!pairKey.empty() && g_QuestBarkPairCooldownUntil[pairKey] > now) { if (reason) *reason = "Player/quest-ender pair cooldown is still active."; return false; }
            if (g_QuestBarksChancePct <= 0) { if (reason) *reason = "Quest bark chance is 0."; return false; }
            if (g_QuestBarksChancePct < 100 && (std::rand() % 100) >= g_QuestBarksChancePct) { if (reason) *reason = "Quest bark chance roll did not pass."; return false; }
        }

        std::vector<QuestBarkQuestInfo> quests = GetActiveEnderQuestInfos(player, npc, onlyQuestId);
        if (quests.empty())
        {
            if (reason) *reason = onlyQuestId ? "That quest is not currently in progress and ended by this NPC." : "No in-progress quests ended by this NPC for this player.";
            return false;
        }

        std::string selectedQuestKey;
        std::string bark;
        for (QuestBarkQuestInfo const& q : quests)
        {
            std::vector<QuestBarkQuestInfo> singleQuest;
            singleQuest.push_back(q);

            std::string questKey = BuildQuestKey(singleQuest);
            if (questKey.empty())
                continue;

            bark = LookupQuestBarkCache(npc->GetEntry(), questKey, PlayerFactionId(player), player->getRace(), player->getClass(), 0, player->GetLevel(), "quest_ender");
            if (!bark.empty())
            {
                selectedQuestKey = questKey;
                break;
            }
        }

        if (bark.empty())
        {
            MaybeGenerateMissingQuestEnderBark(player, npc, quests);
            if (reason) *reason = g_QuestBarksGenerateMissing ?
                "No cached quest-ender bark existed. Generation was queued; try again after it saves." :
                "No cached quest-ender bark exists yet. Target this turn-in NPC and use .npcc gen questender first.";
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

        if (reason) *reason = selectedQuestKey.empty() ? "Quest ender bark spoken." : "Quest ender bark spoken for quest key " + selectedQuestKey + ".";
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
            "Output EXACTLY these five key=value lines, one key per physical newline, in this order: "
            "available, trainer_available, class_available, profession_available, general. "
            "Do not use markdown, numbering, bullets, quotes around values, JSON, or paragraphs. "
            "Each value must be one short in-character spoken line suitable for a trainer to say when a real player approaches. "
            "Never output None, null, N/A, undefined, or empty values. "
            "Never put another key name inside a value. "
            "Do not mention AI, files, prompts, players, servers, tokens, trainer windows, menus, or game mechanics. "
            "Use reusable placeholders only when useful: {player}, {race}, {class}, {level}, {faction}, {npc}, {zone}, {sir_miss}. "
            "Prefer placeholders over hardcoding the player name, race, or class. "
            "Example format: available=Come here, {class}; a little training may keep you alive.";
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

    std::string BuildFallbackTrainerBarks(GenBarkRequest const& req)
    {
        std::ostringstream ss;
        ss << "available=Come here, {class}; a little training may keep you alive.\n";
        ss << "trainer_available=Stand straight, {sir_miss}. We have work to do.\n";
        ss << "class_available=Your path still has lessons for you, {class}.\n";
        ss << "profession_available=Steady hands and patience turn practice into skill.\n";
        ss << "general=Discipline first, confidence second.";
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
        generated = NormalizeGeneratedKeyValueText(generated);
        if (!HasUsableBarkKeys(generated, { "available", "trainer_available", "class_available", "profession_available", "general" }, 2))
        {
            QueueSystemMessage(req.playerGuidRaw, "NPC Chat trainer bark generation returned malformed/weak key-value text; saving safe fallback barks instead.");
            generated = BuildFallbackTrainerBarks(req);
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
            relationshipText = LoadRelationshipText(
                req.playerGuidRaw, req.npcEntry, req.playerName, req.npcName);

            sharedHistory = LoadHistoryTail(sharedPath, g_SharedHistoryTail);
            personalHistory = LoadHistoryTail(personalPath, g_PersonalHistoryTail);

            AppendHistoryLine(sharedPath, "[" + req.playerName + "] " + req.playerName + ": " + req.message);
            AppendHistoryLine(personalPath, req.playerName + ": " + req.message);
        }

        // Record that these two have genuinely spoken. This powers cheap future nearby-history scans.
        TouchNpcContact(req.playerGuidRaw, req.npcEntry, req.playerName, req.npcName);

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

    std::string HistoryWhisperPairKey(Player const* player, Creature const* npc)
    {
        if (!player || !npc)
            return "";
        return std::to_string(player->GetGUID().GetRawValue()) + ":history-whisper:" + std::to_string(npc->GetEntry());
    }

    void NpcHistoryWhisperWorker(ChatRequest req)
    {
        std::string const sharedPath = SharedHistoryFilePath(req.npcName, req.npcEntry);
        std::string const personalPath = PersonalHistoryFilePath(req.playerName, req.playerGuidRaw, req.npcName, req.npcEntry);

        std::deque<std::string> sharedHistory;
        std::deque<std::string> personalHistory;
        std::string defaultPrompt;
        std::string sharedSubPrompts;
        std::string sharedPrompt;
        std::string personalSubPrompts;
        std::string personalPrompt;
        std::string relationshipText;

        {
            std::lock_guard<std::mutex> lock(g_FileMutex);
            EnsureNpcChatDirectoriesAndDefaultPrompt();
            personalHistory = LoadHistoryTail(personalPath, g_HistoryWhispersHistoryMaxLines);
            if (personalHistory.empty())
                return; // contact index is only a hint; history is the actual eligibility check

            sharedHistory = LoadHistoryTail(sharedPath, std::min(g_SharedHistoryTail, g_HistoryWhispersHistoryMaxLines));
            defaultPrompt = ReadWholeTextFile(DefaultPromptFilePath());
            sharedSubPrompts = LoadSubPromptBlocks(LoadSubPromptNameList(SharedSubPromptListFilePath(req.npcName, req.npcEntry)));
            sharedPrompt = ReadWholeTextFile(SharedPromptFilePath(req.npcName, req.npcEntry));
            personalSubPrompts = LoadSubPromptBlocks(LoadSubPromptNameList(
                PersonalSubPromptListFilePath(req.playerName, req.playerGuidRaw, req.npcName, req.npcEntry)));
            personalPrompt = ReadWholeTextFile(PersonalPromptFilePath(req.playerName, req.playerGuidRaw, req.npcName, req.npcEntry));
            relationshipText = LoadRelationshipText(req.playerGuidRaw, req.npcEntry, req.playerName, req.npcName);
        }

        std::ostringstream user;
        user << "You notice " << req.playerName << " nearby again. You have spoken with this person before. "
            << "Privately whisper one short, natural, in-character line that feels like recognition or a continuation of your prior conversations. "
            << "Base it on the history below; do not invent major shared events that are not present. Do not narrate actions.\n\n"
            << "Recent one-on-one history:\n";
        for (std::string const& line : personalHistory)
            user << line << "\n";
        user << "\nWhisper as " << req.npcName << ":";

        NpcChat_LLMResult res = NpcChat_CallLLM(
            BuildChatApiConfig(),
            BuildSystemPrompt(req, defaultPrompt, sharedSubPrompts, sharedPrompt, personalSubPrompts, personalPrompt, relationshipText),
            user.str());
        if (!res.success || TrimCopy(res.text).empty())
            return;

        {
            std::lock_guard<std::mutex> lock(g_FileMutex);
            AppendHistoryLine(sharedPath, req.npcName + " privately to [" + req.playerName + "]: " + res.text);
            AppendHistoryLine(personalPath, req.npcName + " whispers: " + res.text);
        }

        ChatReply reply;
        reply.playerGuidRaw = req.playerGuidRaw;
        reply.npcGuidRaw = req.npcGuidRaw;
        reply.text = res.text;
        reply.forcePrivateReply = true;
        reply.whisper = true;
        std::lock_guard<std::mutex> lock(g_ReplyMutex);
        g_ReplyQueue.push(std::move(reply));
    }

    ChatRequest CaptureHistoryWhisperRequest(Player* player, Creature* npc)
    {
        ChatRequest req;
        if (!player || !npc)
            return req;
        req.playerGuidRaw = player->GetGUID().GetRawValue();
        req.npcGuidRaw = npc->GetGUID().GetRawValue();
        req.npcEntry = npc->GetEntry();
        req.playerName = player->GetName();
        req.npcName = npc->GetName();
        req.npcLevel = npc->GetLevel();
        req.gender = GenderStr(npc->getGender());
        req.creatureType = CreatureTypeStr(npc->GetCreatureType());
        req.autoTags = ResolveCreatureAutoTags(npc);
        req.distance = player->GetDistance(npc);
        req.playerHealthPct = UnitHealthPct(player);
        req.npcHealthPct = UnitHealthPct(npc);
        req.playerInCombat = player->IsInCombat();
        req.npcInCombat = npc->IsInCombat();
        req.npcTargetingPlayer = npc->GetVictim() == player;
        req.isHostile = npc->IsHostileTo(player);
        req.stance = req.isHostile ? "an enemy" : (npc->IsFriendlyTo(player) ? "a friend" : "a familiar stranger");
        req.forcePrivateReply = true;
        if (CreatureTemplate const* tmpl = npc->GetCreatureTemplate())
        {
            req.npcSubName = tmpl->SubName;
            req.rankStr = RankStr(tmpl->rank);
            req.roleStr = RolesFromNpcFlags(tmpl->npcflag);
        }
        if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(npc->GetZoneId()))
            req.zoneName = zone->area_name[0];
        return req;
    }

    bool TryDispatchNpcHistoryWhisper(Player* player, Creature* npc, std::string* reason = nullptr)
    {
        if (!g_HistoryWhispersEnabled || !player || !npc || !npc->IsAlive())
            return false;
        if (!player->GetSession() || player->GetSession()->IsBot())
            return false;
        if (player->IsInCombat() || npc->IsInCombat())
        {
            if (reason) *reason = "player or NPC is in combat";
            return false;
        }
        if (npc->IsHostileTo(player))
        {
            if (reason) *reason = "NPC is hostile";
            return false;
        }
        if (!player->IsWithinDist(npc, g_HistoryWhispersTriggerDistance, true))
        {
            if (reason) *reason = "NPC is outside history-whisper range";
            return false;
        }

        uint64_t playerKey = player->GetGUID().GetRawValue();
        std::string pairKey = HistoryWhisperPairKey(player, npc);
        time_t now = std::time(nullptr);
        if (g_HistoryWhisperPlayerCooldownUntil[playerKey] > now)
        {
            if (reason) *reason = "player history-whisper cooldown is active";
            return false;
        }
        if (!pairKey.empty() && g_HistoryWhisperPairCooldownUntil[pairKey] > now)
        {
            if (reason) *reason = "player/NPC history-whisper cooldown is active";
            return false;
        }
        if (g_HistoryWhispersChancePct <= 0 ||
            (g_HistoryWhispersChancePct < 100 && (std::rand() % 100) >= g_HistoryWhispersChancePct))
        {
            if (reason) *reason = "history-whisper chance roll did not fire";
            return false;
        }

        // Claim cooldown before the worker starts, so a slow API call cannot be queued repeatedly.
        g_HistoryWhisperPlayerCooldownUntil[playerKey] = now + std::max(1, g_HistoryWhispersPlayerCooldownSec);
        if (!pairKey.empty())
            g_HistoryWhisperPairCooldownUntil[pairKey] = now + std::max(1, g_HistoryWhispersPairCooldownSec);

        ChatRequest req = CaptureHistoryWhisperRequest(player, npc);
        std::thread(NpcHistoryWhisperWorker, std::move(req)).detach();
        if (reason) *reason = "history-aware whisper queued";
        return true;
    }

    // =======================================================================
    // BOT CHAT  (whisper / party-raid / target a playerbot)
    // Only genuine bots reply; gate is WorldSession::IsBot(), fails closed.
    // =======================================================================
// --- gate ------------------------------------------------------------------
    inline bool IsGenuineBot(Player* p)
    {
        return p && p->GetSession() && p->GetSession()->IsBot();
    }
    inline bool IsRealPlayerSession(Player* p)
    {
        return p && p->GetSession() && !p->GetSession()->IsBot();
    }

    // --- config (NpcChat.Bot.*) ------------------------------------------------
    // Read these values when a conversation is dispatched so `.npcc reload` applies
    // immediately. Character cards themselves are always read fresh from disk.
    struct BotCfg
    {
        bool        enable = false;
        bool        replyWhisper = true;
        bool        replyPartyRaid = true;
        bool        replyTarget = true;
        float       triggerRange = 25.0f;
        int         historyTail = 20;
        std::string characterCardsPath = "./characters";
    };

    inline BotCfg GetBotCfg()
    {
        BotCfg cfg;
        cfg.enable = sConfigMgr->GetOption<bool>("NpcChat.Bot.Enable", false);
        cfg.replyWhisper = sConfigMgr->GetOption<bool>("NpcChat.Bot.ReplyWhisper", true);
        cfg.replyPartyRaid = sConfigMgr->GetOption<bool>("NpcChat.Bot.ReplyPartyRaid", true);
        cfg.replyTarget = sConfigMgr->GetOption<bool>("NpcChat.Bot.ReplyTarget", true);
        cfg.triggerRange = sConfigMgr->GetOption<float>("NpcChat.Bot.TriggerRange", 25.0f);
        cfg.historyTail = sConfigMgr->GetOption<int32>("NpcChat.Bot.HistoryMaxLines", 20);
        cfg.characterCardsPath = sConfigMgr->GetOption<std::string>(
            "NpcChat.Bot.CharacterCardsPath", "./characters");
        return cfg;
    }

    // --- race / class / gender text --------------------------------------------
    inline const char* BotGenderName(uint8 g)
    {
    switch (g) { case 0: return "male"; case 1: return "female"; default: return ""; }
    }
    inline const char* BotRaceName(uint8 r)
    {
        switch (r)
        {
        case 1:  return "Human";      case 2:  return "Orc";        case 3:  return "Dwarf";
        case 4:  return "Night Elf";  case 5:  return "Undead";     case 6:  return "Tauren";
        case 7:  return "Gnome";      case 8:  return "Troll";      case 10: return "Blood Elf";
        case 11: return "Draenei";    default: return "";
        }
    }
    inline const char* BotClassName(uint8 c)
    {
        switch (c)
        {
        case 1:  return "Warrior";  case 2:  return "Paladin";      case 3:  return "Hunter";
        case 4:  return "Rogue";    case 5:  return "Priest";       case 6:  return "Death Knight";
        case 7:  return "Shaman";   case 8:  return "Mage";         case 9:  return "Warlock";
        case 11: return "Druid";    default: return "";
        }
    }

    // --- bot character cards + simple per-player history --------------------------
    // Character identity is name-based on purpose: this matches mod-playerbots-characters
    // (`characters/ExactCharacterName.card.txt`) and survives bot GUID changes.
    // GUIDs remain useful for private conversation history only.
    inline std::string BotBase(std::string const& botName, uint64_t botGuidRaw)
    {
        return SanitizeName(botName) + "_" + std::to_string(botGuidRaw);
    }

    inline std::string BotCharacterCardFilePath(std::string const& botName)
    {
        return (std::filesystem::path(GetBotCfg().characterCardsPath) /
            (botName + ".card.txt")).string();
    }

    inline std::string BotPersonalHistoryFilePath(std::string const& playerName, uint64_t playerGuidRaw,
        std::string const& botName, uint64_t botGuidRaw)
    {
        return g_HistoryPath + "/bots/personal/" + PlayerHistoryBase(playerName, playerGuidRaw) +
            "/" + BotBase(botName, botGuidRaw) + ".history";
    }

    inline void EnsureBotChatDirectories()
    {
        try
        {
            std::filesystem::create_directories(GetBotCfg().characterCardsPath);
            std::filesystem::create_directories(g_HistoryPath + "/bots/personal");
        }
        catch (std::exception const&) {}
    }

    // --- cross-thread structs / queue ------------------------------------------
    enum class BotChannel { Whisper, Say, Party, Raid };

    struct BotChatRequest
    {
        uint64_t    playerGuidRaw = 0;
        std::string playerName;
        uint64_t    botGuidRaw = 0;
        std::string botName;
        uint32_t    botLevel = 0;
        std::string botGender;
        std::string botRace;
        std::string botClass;
        BotChannel  channel = BotChannel::Whisper;
        std::string message;
    };

    struct BotChatReply
    {
        uint64_t    playerGuidRaw = 0;
        uint64_t    botGuidRaw = 0;
        BotChannel  channel = BotChannel::Whisper;
        std::string text;
    };

    inline std::queue<BotChatReply>& BotReplyQueue() { static std::queue<BotChatReply> q; return q; }
    inline std::mutex& BotReplyMutex() { static std::mutex m; return m; }

    // --- persona + prompts ------------------------------------------------------
    inline std::string BuildBotPersona(BotChatRequest const& req)
    {
        std::ostringstream ss;
        ss << "a level " << req.botLevel;
        if (!req.botGender.empty()) ss << " " << req.botGender;
        if (!req.botRace.empty())   ss << " " << req.botRace;
        if (!req.botClass.empty())  ss << " " << req.botClass;
        return ss.str();
    }

    // PBC-compatible starter file. This is deliberately NOT generated by an LLM.
    // It exists only so every bot gets an immediately editable character card on first contact.
    inline std::string BuiltInBotCharacterCardTemplate()
    {
        return
            "You are {char_name}, a {char_gender} {char_race} {char_class}.\n\n"
            "You are an adventurer living in Azeroth. Your personality, background, mannerisms, "
            "beliefs, likes, dislikes, and relationships can be described here.\n\n"
            "Stay in character when speaking.";
    }

    // Caller holds g_FileMutex. Read the card every conversation so a manual edit is
    // visible on the very next line the bot speaks. Never overwrite an existing file.
    inline std::string LoadBotCharacterCard(std::string const& botName)
    {
        std::string const path = BotCharacterCardFilePath(botName);
        std::string card = ReadWholeTextFile(path);

        if (card.empty())
        {
            WriteWholeTextFile(path, BuiltInBotCharacterCardTemplate(), false);
            card = ReadWholeTextFile(path);
        }

        // If the path cannot be written (or an intentionally empty file exists),
        // still give the current conversation the harmless built-in template.
        return card.empty() ? BuiltInBotCharacterCardTemplate() : card;
    }

    inline std::string ExpandBotCharacterCard(BotChatRequest const& req, std::string card)
    {
        ReplaceAllInPlace(card, "{char_name}", req.botName);
        ReplaceAllInPlace(card, "{char_gender}", req.botGender);
        ReplaceAllInPlace(card, "{char_race}", req.botRace);
        ReplaceAllInPlace(card, "{char_class}", req.botClass);
        ReplaceAllInPlace(card, "{char_level}", std::to_string(req.botLevel));
        return card;
    }

    inline std::string BuildBotSystemPrompt(BotChatRequest const& req,
        std::string const& characterCard)
    {
        std::ostringstream ss;
        ss << "You are " << req.botName << ", " << BuildBotPersona(req)
            << ", adventuring in Azeroth (World of Warcraft) as a companion to fellow players.\n";
        if (!characterCard.empty())
            ss << "\nCharacter card:\n" << characterCard << "\n";
        ss << "Stay fully in character. Use only your own spoken words: no narration, no "
            "asterisks, no out-of-character text, no game mechanics. Keep replies to one or "
            "two short sentences suitable for a single line of in-game chat.";
        return ss.str();
    }

    inline std::string BuildBotUserPrompt(BotChatRequest const& req, std::deque<std::string> const& history)
    {
        std::ostringstream ss;
        if (!history.empty())
        {
            ss << "Recent conversation:\n";
            for (auto const& l : history) ss << l << "\n";
            ss << "\n";
        }
        ss << req.playerName << " says to you: \"" << req.message << "\"\n\n"
            << "Reply as " << req.botName << ":";
        return ss.str();
    }

    // --- worker ----------------------------------------------------------------
    inline void BotWorkerRun(BotChatRequest req)
    {
        std::string const historyPath = BotPersonalHistoryFilePath(
            req.playerName, req.playerGuidRaw, req.botName, req.botGuidRaw);
        BotCfg const cfg = GetBotCfg();

        std::deque<std::string> history;
        std::string characterCard;

        {
            std::lock_guard<std::mutex> lock(g_FileMutex);
            EnsureBotChatDirectories();
            characterCard = LoadBotCharacterCard(req.botName);
            history = LoadHistoryTail(historyPath, cfg.historyTail);
            AppendHistoryLine(historyPath, req.playerName + ": " + req.message);
        }

        characterCard = ExpandBotCharacterCard(req, std::move(characterCard));

        NpcChat_ApiConfig apiCfg = BuildChatApiConfig();
        NpcChat_LLMResult res = NpcChat_CallLLM(apiCfg,
            BuildBotSystemPrompt(req, characterCard),
            BuildBotUserPrompt(req, history));

        if (!res.success || res.text.empty())
            return;

        {
            std::lock_guard<std::mutex> lock(g_FileMutex);
            AppendHistoryLine(historyPath, req.botName + ": " + res.text);
        }

        BotChatReply reply;
        reply.playerGuidRaw = req.playerGuidRaw;
        reply.botGuidRaw = req.botGuidRaw;
        reply.channel = req.channel;
        reply.text = res.text;

        std::lock_guard<std::mutex> lock(BotReplyMutex());
        BotReplyQueue().push(std::move(reply));
    }

    // Capture bot/player data on the MAIN THREAD, then hand the worker only copies.
    inline void DispatchBot(Player* realPlayer, Player* bot, BotChannel channel, std::string const& message)
    {
        BotChatRequest req;
        req.playerGuidRaw = realPlayer->GetGUID().GetRawValue();
        req.playerName = realPlayer->GetName();
        req.botGuidRaw = bot->GetGUID().GetRawValue();
        req.botName = bot->GetName();
        req.botLevel = bot->GetLevel();
        req.botGender = BotGenderName(bot->getGender());
        req.botRace = BotRaceName(bot->getRace());
        req.botClass = BotClassName(bot->getClass());
        req.channel = channel;
        req.message = message;
        std::thread(BotWorkerRun, std::move(req)).detach();
    }

    // --- controlled social conversations ----------------------------------------
    struct BotSocialCfg
    {
        bool enable = false;
        uint32 partyChancePct = 70;
        uint32 raidChancePct = 35;
        uint32 guildChancePct = 55;
        int partyMaxSpeakers = 3;
        int raidMaxSpeakers = 2;
        int guildMaxSpeakers = 3;
        uint32 randomBotChancePct = 25;
        int cooldownSec = 20;
    };

    inline BotSocialCfg GetBotSocialCfg()
    {
        BotSocialCfg c;
        c.enable = sConfigMgr->GetOption<bool>("NpcChat.Bot.Social.Enable", false);
        c.partyChancePct = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("NpcChat.Bot.Social.PartyChancePct", 70));
        c.raidChancePct = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("NpcChat.Bot.Social.RaidChancePct", 35));
        c.guildChancePct = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("NpcChat.Bot.Social.GuildChancePct", 55));
        c.partyMaxSpeakers = std::max(1, std::min(4, sConfigMgr->GetOption<int32>("NpcChat.Bot.Social.PartyMaxSpeakers", 3)));
        c.raidMaxSpeakers = std::max(1, std::min(4, sConfigMgr->GetOption<int32>("NpcChat.Bot.Social.RaidMaxSpeakers", 2)));
        c.guildMaxSpeakers = std::max(1, std::min(4, sConfigMgr->GetOption<int32>("NpcChat.Bot.Social.GuildMaxSpeakers", 3)));
        c.randomBotChancePct = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("NpcChat.Bot.Social.RandomBotChancePct", 25));
        c.cooldownSec = std::max(0, sConfigMgr->GetOption<int32>("NpcChat.Bot.Social.CooldownSec", 20));
        return c;
    }

    inline std::map<std::string, time_t>& BotSocialCooldownUntil()
    {
        static std::map<std::string, time_t> m;
        return m;
    }

    inline bool BotSocialAmbientAllowed(std::string const& key, int cooldownSec)
    {
        time_t now = std::time(nullptr);
        if (BotSocialCooldownUntil()[key] > now)
            return false;
        BotSocialCooldownUntil()[key] = now + std::max(1, cooldownSec);
        return true;
    }

    inline bool MessageMentionsBot(std::string const& text, std::string const& botName)
    {
        std::string hay = ToLowerCopy(text);
        std::string needle = ToLowerCopy(botName);
        if (needle.empty())
            return false;
        size_t pos = 0;
        while ((pos = hay.find(needle, pos)) != std::string::npos)
        {
            bool leftOk = pos == 0 || !std::isalnum(static_cast<unsigned char>(hay[pos - 1]));
            size_t end = pos + needle.size();
            bool rightOk = end >= hay.size() || !std::isalnum(static_cast<unsigned char>(hay[end]));
            if (leftOk && rightOk)
                return true;
            pos = end;
        }
        return false;
    }

    inline Player* TakeRandomBot(std::vector<Player*>& bots)
    {
        if (bots.empty())
            return nullptr;
        size_t idx = static_cast<size_t>(std::rand()) % bots.size();
        Player* p = bots[idx];
        bots.erase(bots.begin() + idx);
        return p;
    }

    inline std::vector<Player*> ChooseSocialBots(std::vector<Player*> candidates, std::string const& text,
        int maxSpeakers, uint32 randomBotChancePct)
    {
        std::vector<Player*> named;
        std::vector<Player*> alts;
        std::vector<Player*> randoms;
        for (Player* bot : candidates)
        {
            if (!bot || !IsGenuineBot(bot))
                continue;
            if (MessageMentionsBot(text, bot->GetName()))
                named.push_back(bot);
            else if (RandomPlayerbotMgr::instance().IsRandomBot(bot))
                randoms.push_back(bot);
            else
                alts.push_back(bot);
        }

        std::vector<Player*> out;
        auto addUnique = [&](Player* bot)
        {
            if (!bot || static_cast<int>(out.size()) >= maxSpeakers)
                return;
            if (std::find(out.begin(), out.end(), bot) == out.end())
                out.push_back(bot);
        };

        for (Player* bot : named)
            addUnique(bot);

        // Prefer at least one non-random alt when available; these are the server owner's
        // character bots and are the best candidates for authored .card.txt personalities.
        if (out.empty() && !alts.empty())
            addUnique(TakeRandomBot(alts));

        // Let a random playerbot occasionally join the banter so a 40-man raid feels populated.
        if (static_cast<int>(out.size()) < maxSpeakers && !randoms.empty() &&
            (alts.empty() || (std::rand() % 100) < randomBotChancePct))
            addUnique(TakeRandomBot(randoms));

        while (static_cast<int>(out.size()) < maxSpeakers && !alts.empty())
            addUnique(TakeRandomBot(alts));
        while (static_cast<int>(out.size()) < maxSpeakers && out.empty() && !randoms.empty())
            addUnique(TakeRandomBot(randoms));

        return out;
    }

    enum class BotSocialChannel { Party, Raid, Guild };
    struct BotSocialConversationRequest
    {
        uint64_t playerGuidRaw = 0;
        std::string playerName;
        BotSocialChannel channel = BotSocialChannel::Party;
        std::string message;
        std::string contextHint;
        std::vector<BotChatRequest> turns;
    };
    struct BotSocialReply
    {
        uint64_t playerGuidRaw = 0;
        uint64_t botGuidRaw = 0;
        BotSocialChannel channel = BotSocialChannel::Party;
        std::string text;
    };
    inline std::queue<BotSocialReply>& BotSocialReplyQueue() { static std::queue<BotSocialReply> q; return q; }
    inline std::mutex& BotSocialReplyMutex() { static std::mutex m; return m; }

    inline void BotSocialConversationWorker(BotSocialConversationRequest req)
    {
        std::string conversation = req.playerName + ": " + req.message;
        BotCfg const cfg = GetBotCfg();

        for (BotChatRequest turn : req.turns)
        {
            std::string historyPath = BotPersonalHistoryFilePath(
                turn.playerName, turn.playerGuidRaw, turn.botName, turn.botGuidRaw);
            std::deque<std::string> history;
            std::string card;
            {
                std::lock_guard<std::mutex> lock(g_FileMutex);
                EnsureBotChatDirectories();
                card = LoadBotCharacterCard(turn.botName);
                history = LoadHistoryTail(historyPath, cfg.historyTail);
                AppendHistoryLine(historyPath, turn.playerName + ": " + turn.message);
            }

            card = ExpandBotCharacterCard(turn, std::move(card));
            std::string system = BuildBotSystemPrompt(turn, card);
            system += "\n\nYou are participating in a live group conversation. Other characters may already have replied. "
                "React to what was actually said, stay concise, and do not speak for anyone else.";
            if (!req.contextHint.empty())
                system += "\nCurrent situation: " + req.contextHint;

            std::ostringstream user;
            if (!history.empty())
            {
                user << "Your recent history with " << turn.playerName << ":\n";
                for (std::string const& line : history) user << line << "\n";
                user << "\n";
            }
            user << "Conversation so far:\n" << conversation
                << "\n\nAdd one short natural line as " << turn.botName
                << ". If you truly have nothing to add, output exactly [SKIP].";

            NpcChat_LLMResult res = NpcChat_CallLLM(BuildChatApiConfig(), system, user.str());
            std::string line = TrimCopy(res.text);
            if (!res.success || line.empty() || ToLowerCopy(line) == "[skip]")
                continue;

            {
                std::lock_guard<std::mutex> lock(g_FileMutex);
                AppendHistoryLine(historyPath, turn.botName + ": " + line);
            }
            conversation += "\n" + turn.botName + ": " + line;

            BotSocialReply reply;
            reply.playerGuidRaw = req.playerGuidRaw;
            reply.botGuidRaw = turn.botGuidRaw;
            reply.channel = req.channel;
            reply.text = std::move(line);
            std::lock_guard<std::mutex> lock(BotSocialReplyMutex());
            BotSocialReplyQueue().push(std::move(reply));
        }
    }

    inline void DispatchBotSocialConversation(Player* player, std::vector<Player*> const& bots,
        BotSocialChannel channel, std::string const& message, std::string const& contextHint)
    {
        if (!player || bots.empty())
            return;
        BotSocialConversationRequest req;
        req.playerGuidRaw = player->GetGUID().GetRawValue();
        req.playerName = player->GetName();
        req.channel = channel;
        req.message = message;
        req.contextHint = contextHint;
        for (Player* bot : bots)
        {
            if (!bot || !IsGenuineBot(bot))
                continue;
            BotChatRequest turn;
            turn.playerGuidRaw = req.playerGuidRaw;
            turn.playerName = req.playerName;
            turn.botGuidRaw = bot->GetGUID().GetRawValue();
            turn.botName = bot->GetName();
            turn.botLevel = bot->GetLevel();
            turn.botGender = BotGenderName(bot->getGender());
            turn.botRace = BotRaceName(bot->getRace());
            turn.botClass = BotClassName(bot->getClass());
            turn.channel = channel == BotSocialChannel::Raid ? BotChannel::Raid : BotChannel::Party;
            turn.message = message;
            req.turns.push_back(std::move(turn));
        }
        if (!req.turns.empty())
            std::thread(BotSocialConversationWorker, std::move(req)).detach();
    }

    inline void EmitBotSocialReplies()
    {
        std::queue<BotSocialReply> local;
        {
            std::lock_guard<std::mutex> lock(BotSocialReplyMutex());
            if (BotSocialReplyQueue().empty()) return;
            std::swap(local, BotSocialReplyQueue());
        }
        while (!local.empty())
        {
            BotSocialReply& r = local.front();
            Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(r.botGuidRaw));
            if (bot && bot->IsInWorld())
            {
                if (r.channel == BotSocialChannel::Guild)
                {
                    if (Guild* guild = bot->GetGuild())
                        guild->BroadcastToGuild(bot->GetSession(), false, r.text, LANG_UNIVERSAL);
                }
                else if (Group* group = bot->GetGroup())
                {
                    ChatMsg type = r.channel == BotSocialChannel::Raid ? CHAT_MSG_RAID : CHAT_MSG_PARTY;
                    WorldPacket data;
                    ChatHandler::BuildChatPacket(data, type, LANG_UNIVERSAL, bot, bot, r.text);
                    group->BroadcastPacket(&data, false);
                }
            }
            local.pop();
        }
    }

    // --- surface entrypoints (called from the PlayerScript hooks) ---------------

    // Whisper a bot -> it whispers back. `text` is the already-trimmed message.
    inline bool HandleBotWhisper(Player* player, uint32 type, Player* receiver, std::string const& text)
    {
        BotCfg const cfg = GetBotCfg();
        if (!cfg.enable || !cfg.replyWhisper) return false;
        if (type != CHAT_MSG_WHISPER) return false;
        if (!IsRealPlayerSession(player)) return false;   // only real senders
        if (!IsGenuineBot(receiver)) return false;         // only bot receivers
        if (text.empty() || text[0] == '.') return false;
        DispatchBot(player, receiver, BotChannel::Whisper, text);
        return true;
    }

    // Party/raid chat can now feel populated without allowing recursive bot hooks. A real player
    // message starts one bounded conversation worker; selected bots reply sequentially and later
    // speakers see the lines produced earlier in the same conversation.
    inline void HandleBotGroup(Player* player, uint32 type, Group* group, std::string const& text)
    {
        BotCfg const cfg = GetBotCfg();
        if (!cfg.enable || !cfg.replyPartyRaid || !group) return;
        if (!IsRealPlayerSession(player)) return;
        if (text.empty() || text[0] == '.') return;

        BotSocialChannel socialChannel;
        BotChannel legacyChannel;
        switch (type)
        {
        case CHAT_MSG_PARTY: case CHAT_MSG_PARTY_LEADER:
            socialChannel = BotSocialChannel::Party; legacyChannel = BotChannel::Party; break;
        case CHAT_MSG_RAID: case CHAT_MSG_RAID_LEADER: case CHAT_MSG_RAID_WARNING:
            socialChannel = BotSocialChannel::Raid; legacyChannel = BotChannel::Raid; break;
        default: return;
        }

        std::vector<Player*> candidates;
        bool named = false;
        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member || member == player || !IsGenuineBot(member)) continue;
            candidates.push_back(member);
            named = named || MessageMentionsBot(text, member->GetName());
        }
        if (candidates.empty()) return;

        BotSocialCfg const social = GetBotSocialCfg();
        if (!social.enable)
        {
            for (Player* bot : candidates)
                if (MessageMentionsBot(text, bot->GetName()))
                    DispatchBot(player, bot, legacyChannel, text);
            return;
        }

        uint32 chance = socialChannel == BotSocialChannel::Raid ? social.raidChancePct : social.partyChancePct;
        int maxSpeakers = socialChannel == BotSocialChannel::Raid ? social.raidMaxSpeakers : social.partyMaxSpeakers;
        if (!named)
        {
            if (chance == 0 || (chance < 100 && (std::rand() % 100) >= chance))
                return;
            std::string cooldownKey = "group:" + std::to_string(group->GetGUID().GetRawValue());
            if (!BotSocialAmbientAllowed(cooldownKey, social.cooldownSec))
                return;
        }

        std::vector<Player*> chosen = ChooseSocialBots(candidates, text, maxSpeakers, social.randomBotChancePct);
        DispatchBotSocialConversation(player, chosen, socialChannel, text,
            socialChannel == BotSocialChannel::Raid ? "This is raid chat in a large adventuring group." : "This is party chat among companions.");
    }

    // Target a bot and /say -> it answers. `text` is the already-trimmed message.
    inline bool HandleBotSay(Player* player, Player* bot, std::string const& text)
    {
        BotCfg const cfg = GetBotCfg();
        if (!cfg.enable || !cfg.replyTarget) return false;
        if (!IsRealPlayerSession(player) || !IsGenuineBot(bot)) return false;
        if (!bot->IsAlive() || !player->IsWithinDist(bot, cfg.triggerRange, true)) return false;
        if (text.empty() || text[0] == '.') return false;
        DispatchBot(player, bot, BotChannel::Say, text);
        return true;
    }

    // --- emit (called from WorldScript::OnUpdate on the MAIN THREAD) ------------
    inline void EmitBotReplies()
    {
        std::queue<BotChatReply> local;
        {
            std::lock_guard<std::mutex> lock(BotReplyMutex());
            if (BotReplyQueue().empty()) return;
            std::swap(local, BotReplyQueue());
        }

        while (!local.empty())
        {
            BotChatReply& r = local.front();

            Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(r.botGuidRaw));
            Player* player = ObjectAccessor::FindPlayer(ObjectGuid(r.playerGuidRaw));

            if (bot && bot->IsInWorld())
            {
                switch (r.channel)
                {
                case BotChannel::Whisper:
                    if (player) bot->Whisper(r.text, LANG_UNIVERSAL, player); // [AC-API]
                    break;
                case BotChannel::Say:
                    bot->Say(r.text, LANG_UNIVERSAL);                          // [AC-API]
                    break;
                case BotChannel::Party:
                case BotChannel::Raid:
                    if (Group* g = bot->GetGroup())
                    {
                        ChatMsg cm = (r.channel == BotChannel::Raid) ? CHAT_MSG_RAID : CHAT_MSG_PARTY;
                        WorldPacket data;
                        // [AC-API] BuildChatPacket / BroadcastPacket signatures vary by core.
                        // Fallback: loop g members and SendDirectMessage per receiver.
                        ChatHandler::BuildChatPacket(data, cm, LANG_UNIVERSAL, bot, bot, r.text);
                        g->BroadcastPacket(&data, false);
                    }
                    break;
                }
            }
            local.pop();
        }
    }

    // =======================================================================
    // BOT ROSTER + LFG MATCHER + GUILD/CHANNEL SURFACES
    // =======================================================================
    enum class BotRole : uint8 { None, Tank, Heal, Dps };

    struct BotRosterEntry
    {
        uint64_t    guidRaw = 0;
        std::string name;
        uint8       level = 0;
        uint8       cls = 0;      // Classes enum (CLASS_*)
        uint8       race = 0;
        BotRole     role = BotRole::None;
        std::string specName;       // "Holy Priest"
        bool        isRandom = false; // true = PLAYER BOT (rndbot); false = ALT PLAYER BOT
        bool        knownToPlayer = false; // has prior personal history with the requester
    };

    // --- WoW shorthand ---------------------------------------------------------
    inline const char* ClassShort(uint8 c)
    {
        switch (c)
        {
        case CLASS_WARRIOR: return "WAR";   case CLASS_PALADIN: return "PAL";
        case CLASS_HUNTER:  return "HUNT";  case CLASS_ROGUE:   return "ROG";
        case CLASS_PRIEST:  return "PRIEST"; case CLASS_DEATH_KNIGHT: return "DK";
        case CLASS_SHAMAN:  return "SHM";   case CLASS_MAGE:    return "MAGE";
        case CLASS_WARLOCK: return "LOCK";  case CLASS_DRUID:   return "DRUID";
        default: return "";
        }
    }
    inline const char* RoleShort(BotRole r)
    {
        switch (r) {
        case BotRole::Tank: return "TANK"; case BotRole::Heal: return "HEALS";
        case BotRole::Dps: return "DPS"; default: return "";
        }
    }

    // spec tab + class -> role. Mirrors LfgJoinAction::GetRoles() from mod-playerbots.
    inline BotRole ResolveBotRole(Player* bot)
    {
        uint8 spec = AiFactory::GetPlayerSpecTab(bot);   // dominant talent tree 0/1/2
        switch (bot->getClass())
        {
        case CLASS_WARRIOR: return spec == 2 ? BotRole::Tank : BotRole::Dps;
        case CLASS_PALADIN: return spec == 1 ? BotRole::Tank : (spec == 0 ? BotRole::Heal : BotRole::Dps);
        case CLASS_PRIEST:  return spec != 2 ? BotRole::Heal : BotRole::Dps;
        case CLASS_SHAMAN:  return spec == 2 ? BotRole::Heal : BotRole::Dps;
        case CLASS_DRUID:
            if (spec == 2) return BotRole::Heal;
            if (spec == 1 && bot->HasAura(16931)) return BotRole::Tank; // thick hide = bear tank
            return BotRole::Dps;
        case CLASS_DEATH_KNIGHT: return spec == 0 ? BotRole::Tank : BotRole::Dps; // blood approx
        default: return BotRole::Dps; // hunter / rogue / mage / warlock
        }
    }

    // --- Zone roster (MAIN THREAD only -- touches live Player objects) ----------
    inline std::vector<BotRosterEntry> BuildZoneBotRoster(Player* me)
    {
        std::vector<BotRosterEntry> roster;
        if (!me || !me->IsInWorld() || !me->GetMap())
            return roster;

        uint32 const zone = me->GetZoneId();
        std::string const meName = me->GetName();
        uint64_t const meGuid = me->GetGUID().GetRawValue();

        Map::PlayerList const& players = me->GetMap()->GetPlayers();
        for (Map::PlayerList::const_iterator it = players.begin(); it != players.end(); ++it)
        {
            Player* p = it->GetSource();  // [AC-API]
            if (!p || !p->IsInWorld() || p == me) continue;
            if (!IsGenuineBot(p)) continue;                 // reuse existing gate
            if (p->GetZoneId() != zone) continue;

            BotRosterEntry e;
            e.guidRaw = p->GetGUID().GetRawValue();
            e.name = p->GetName();
            e.level = p->GetLevel();
            e.cls = p->getClass();
            e.race = p->getRace();
            e.role = ResolveBotRole(p);
            e.specName = ChatHelper::FormatClass(p, AiFactory::GetPlayerSpecTab(p)); // [PB-API]
            e.isRandom = RandomPlayerbotMgr::instance().IsRandomBot(p);              // [PB-API]

            // "known" bias: does the requester already have a personal history with this bot?
            std::string hist = BotPersonalHistoryFilePath(meName, meGuid, e.name, e.guidRaw);
            try { e.knownToPlayer = std::filesystem::exists(hist); }
            catch (...) { e.knownToPlayer = false; }

            roster.push_back(std::move(e));
        }
        return roster;
    }

    // --- LFG intent (parse the player's request into what they're after) --------
    struct LfgIntent
    {
        bool    isLfg = false;
        BotRole wantedRole = BotRole::None;
        uint8   wantedClass = 0;   // 0 = any
    };

    // whole-word contains (so "war" doesn't fire on "toward")
    inline bool ContainsWord(std::string const& hayLower, std::string const& wLower)
    {
        size_t pos = 0;
        while ((pos = hayLower.find(wLower, pos)) != std::string::npos)
        {
            bool leftOk = (pos == 0) || !std::isalnum((unsigned char)hayLower[pos - 1]);
            size_t end = pos + wLower.size();
            bool rightOk = (end >= hayLower.size()) || !std::isalnum((unsigned char)hayLower[end]);
            if (leftOk && rightOk) return true;
            pos = end;
        }
        return false;
    }

    inline LfgIntent ParseLfgIntent(std::string const& msg)
    {
        std::string m = ToLowerCopy(msg);
        LfgIntent in;
        auto sub = [&](const char* w) { return m.find(w) != std::string::npos; };
        auto word = [&](const char* w) { return ContainsWord(m, w); };

        if (sub("lfg") || sub("lfm") || sub("looking for") || word("lf") || sub("need a") ||
            sub("need healer") || sub("need tank") || sub("need dps") || sub("wtb group"))
            in.isLfg = true;

        // roles
        if (sub("heal"))                      in.wantedRole = BotRole::Heal; // heals/healer/healz
        else if (word("tank") || word("mt") || word("ot")) in.wantedRole = BotRole::Tank;
        else if (word("dps") || word("deeps")) in.wantedRole = BotRole::Dps;

        // classes (shorthand + full)
        if (word("priest"))                  in.wantedClass = CLASS_PRIEST;
        else if (word("lock") || word("warlock")) in.wantedClass = CLASS_WARLOCK;
        else if (word("shm") || word("sham") || word("shaman")) in.wantedClass = CLASS_SHAMAN;
        else if (word("war") || word("warr") || word("warrior")) in.wantedClass = CLASS_WARRIOR;
        else if (word("pal") || word("pally") || word("paladin")) in.wantedClass = CLASS_PALADIN;
        else if (word("dk") || word("deathknight")) in.wantedClass = CLASS_DEATH_KNIGHT;
        else if (word("hunt") || word("hunter"))  in.wantedClass = CLASS_HUNTER;
        else if (word("rog") || word("rogue"))   in.wantedClass = CLASS_ROGUE;
        else if (word("mage"))                    in.wantedClass = CLASS_MAGE;
        else if (word("druid") || word("boomy") || word("resto")) in.wantedClass = CLASS_DRUID;

        // a role/class word plus any group-ish word counts as an LFG line even without lfg/lfm
        if ((in.wantedRole != BotRole::None || in.wantedClass) &&
            (sub("group") || sub("dungeon") || sub("run") || sub("inv") || in.isLfg))
            in.isLfg = true;

        return in;
    }

    // --- Matcher: the single most appropriate bot, or nothing -------------------
    inline BotRosterEntry const* MatchBestBot(std::vector<BotRosterEntry> const& roster,
        LfgIntent const& in, uint8 requesterLevel,
        int levelBracket)
    {
        std::vector<BotRosterEntry const*> cand;
        for (auto const& e : roster)
        {
            if (in.wantedClass && e.cls != in.wantedClass) continue;
            if (in.wantedRole != BotRole::None && e.role != in.wantedRole) continue;
            if (std::abs(int(e.level) - int(requesterLevel)) > levelBracket) continue;
            cand.push_back(&e);
        }
        if (cand.empty()) return nullptr;

        std::sort(cand.begin(), cand.end(), [&](BotRosterEntry const* a, BotRosterEntry const* b)
            {
                if (a->knownToPlayer != b->knownToPlayer) return a->knownToPlayer;      // bots you know first
                return std::abs(int(a->level) - int(requesterLevel)) <
                    std::abs(int(b->level) - int(requesterLevel));                    // then closest level
            });
        return cand.front();
    }

    // --- Guild conversation turn-cap (token-abuse guard) -----------------------
    // Guild chat NEVER initiates. It only replies when the player provokes a bot,
    // and only for a few turns per window before it goes quiet.
    struct GuildConvoState { int turns = 0; time_t windowStart = 0; };

    inline std::map<std::pair<uint64_t, uint64_t>, GuildConvoState>& GuildConvos()
    {
        static std::map<std::pair<uint64_t, uint64_t>, GuildConvoState> m;
        return m;
    }

    // Call ONLY on player provocation. Returns true if the bot may reply now.
    // maxTurns: replies allowed per window. windowResetSec: idle gap that starts a fresh window.
    inline bool GuildTurnAllowed(uint64_t playerGuid, uint64_t botGuid, int maxTurns, int windowResetSec)
    {
        auto key = std::make_pair(playerGuid, botGuid);
        GuildConvoState& st = GuildConvos()[key];
        time_t now = std::time(nullptr);

        if (st.windowStart == 0 || (now - st.windowStart) > windowResetSec)
        {
            st.windowStart = now;   // fresh conversation window
            st.turns = 0;
        }
        if (st.turns >= maxTurns)
            return false;           // cap reached -> stay silent until the window ages out
        st.turns++;
        return true;
    }

    // --- bot surface config ------------------------------------------------------
    // Like BotCfg, read on use so `.npcc reload` is effective for guild/LFG settings.
    struct BotSurfaceCfg
    {
        bool   lfgEnable = true;
        int    levelBracket = 8;
        bool   lfgGeneral = true;
        bool   lfgTrade = false;
        uint32 ambientChance = 0;    // General ambient reply chance (0 = off)
        bool   guildEnable = true;
        int    guildMaxTurns = 4;
        int    guildWindowSec = 300;
    };

    inline BotSurfaceCfg GetSurfaceCfg()
    {
        BotSurfaceCfg c;
        c.lfgEnable = sConfigMgr->GetOption<bool>("NpcChat.Bot.Lfg.Enable", true);
        c.levelBracket = sConfigMgr->GetOption<int32>("NpcChat.Bot.Lfg.LevelBracket", 8);
        c.lfgGeneral = sConfigMgr->GetOption<bool>("NpcChat.Bot.Lfg.General", true);
        c.lfgTrade = sConfigMgr->GetOption<bool>("NpcChat.Bot.Lfg.Trade", false);
        c.ambientChance = sConfigMgr->GetOption<uint32>("NpcChat.Bot.General.Chance", 0);
        c.guildEnable = sConfigMgr->GetOption<bool>("NpcChat.Bot.Guild.Enable", true);
        c.guildMaxTurns = sConfigMgr->GetOption<int32>("NpcChat.Bot.Guild.MaxTurns", 4);
        c.guildWindowSec = sConfigMgr->GetOption<int32>("NpcChat.Bot.Guild.WindowSec", 300);
        return c;
    }

    // --- cross-thread structs / queue ------------------------------------------
    enum class SurfaceKind { GuildMsg, ChannelMsg };

    struct BotSurfaceRequest
    {
        uint64_t    playerGuidRaw = 0;
        std::string playerName;
        uint64_t    botGuidRaw = 0;
        std::string botName;
        uint32_t    botLevel = 0;
        std::string botGender, botRace, botClass;
        SurfaceKind kind = SurfaceKind::GuildMsg;
        std::string channelName;   // for ChannelMsg emit
        std::string contextHint;   // e.g. LFG note steering the reply
        std::string message;
    };
    struct BotSurfaceReply
    {
        uint64_t    playerGuidRaw = 0;
        uint64_t    botGuidRaw = 0;
        SurfaceKind kind = SurfaceKind::GuildMsg;
        std::string channelName;
        std::string text;
    };
    inline std::queue<BotSurfaceReply>& SurfaceReplyQueue() { static std::queue<BotSurfaceReply> q; return q; }
    inline std::mutex& SurfaceReplyMutex() { static std::mutex m; return m; }

    // --- worker (isolated; reuses card/history/LLM leaf helpers) ---------------
    inline void BotSurfaceWorkerRun(BotSurfaceRequest req)
    {
        std::string const historyPath = BotPersonalHistoryFilePath(
            req.playerName, req.playerGuidRaw, req.botName, req.botGuidRaw);
        BotCfg const cfg = GetBotCfg();

        std::deque<std::string> history;
        std::string characterCard;
        {
            std::lock_guard<std::mutex> lock(g_FileMutex);
            EnsureBotChatDirectories();
            characterCard = LoadBotCharacterCard(req.botName);
            history = LoadHistoryTail(historyPath, cfg.historyTail);
            AppendHistoryLine(historyPath, req.playerName + ": " + req.message);
        }

        // Reuse the exact same character-card and conversation prompt path as whisper,
        // target say, party and raid. Guild/LFG only add a small situation hint.
        BotChatRequest promptReq;
        promptReq.playerGuidRaw = req.playerGuidRaw;
        promptReq.playerName = req.playerName;
        promptReq.botGuidRaw = req.botGuidRaw;
        promptReq.botName = req.botName;
        promptReq.botLevel = req.botLevel;
        promptReq.botGender = req.botGender;
        promptReq.botRace = req.botRace;
        promptReq.botClass = req.botClass;
        promptReq.message = req.message;

        characterCard = ExpandBotCharacterCard(promptReq, std::move(characterCard));
        std::string systemPrompt = BuildBotSystemPrompt(promptReq, characterCard);
        if (!req.contextHint.empty())
            systemPrompt += "\n\nCurrent situation:\n" + req.contextHint;

        NpcChat_LLMResult res = NpcChat_CallLLM(
            BuildChatApiConfig(), systemPrompt, BuildBotUserPrompt(promptReq, history));
        if (!res.success || res.text.empty())
            return;

        {
            std::lock_guard<std::mutex> lock(g_FileMutex);
            AppendHistoryLine(historyPath, req.botName + ": " + res.text);
        }

        BotSurfaceReply rep;
        rep.playerGuidRaw = req.playerGuidRaw;
        rep.botGuidRaw = req.botGuidRaw;
        rep.kind = req.kind;
        rep.channelName = req.channelName;
        rep.text = res.text;

        std::lock_guard<std::mutex> lock(SurfaceReplyMutex());
        SurfaceReplyQueue().push(std::move(rep));
    }

    // Capture on MAIN THREAD, hand the worker copies only.
    inline void DispatchBotSurface(Player* player, Player* bot, SurfaceKind kind,
        std::string const& channelName, std::string const& contextHint,
        std::string const& message)
    {
        BotSurfaceRequest req;
        req.playerGuidRaw = player->GetGUID().GetRawValue();
        req.playerName = player->GetName();
        req.botGuidRaw = bot->GetGUID().GetRawValue();
        req.botName = bot->GetName();
        req.botLevel = bot->GetLevel();
        req.botGender = BotGenderName(bot->getGender());
        req.botRace = BotRaceName(bot->getRace());
        req.botClass = BotClassName(bot->getClass());
        req.kind = kind;
        req.channelName = channelName;
        req.contextHint = contextHint;
        req.message = message;
        std::thread(BotSurfaceWorkerRun, std::move(req)).detach();
    }

    // --- emit (MAIN THREAD, from OnUpdate) -------------------------------------
    inline void EmitBotSurfaceReplies()
    {
        std::queue<BotSurfaceReply> local;
        {
            std::lock_guard<std::mutex> lk(SurfaceReplyMutex());
            if (SurfaceReplyQueue().empty()) return;
            std::swap(local, SurfaceReplyQueue());
        }

        while (!local.empty())
        {
            BotSurfaceReply& r = local.front();
            Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(r.botGuidRaw));
            Player* player = ObjectAccessor::FindPlayer(ObjectGuid(r.playerGuidRaw));

            if (bot && bot->IsInWorld())
            {
                if (r.kind == SurfaceKind::GuildMsg)
                {
                    if (Guild* g = bot->GetGuild())
                        g->BroadcastToGuild(bot->GetSession(), false, r.text, LANG_UNIVERSAL); // [AC-API]
                }
                else // ChannelMsg
                {
                    // Channel::Say needs the bot to be a channel member and Channel::IsOn is
                    // private in this core, so we can't gate it. Whisper the requester instead:
                    // reliable, and the natural LFG UX (the recruiter is told directly).
                    if (player)
                        bot->Whisper(r.text, LANG_UNIVERSAL, player);
                }
            }
            local.pop();
        }
    }

    // --- Guild surface: controlled multi-bot social conversation ----------------
    inline void HandleBotGuild(Player* player, uint32 /*type*/, Guild* guild, std::string const& text)
    {
        if (!GetBotCfg().enable || !GetSurfaceCfg().guildEnable || !guild) return;
        if (!IsRealPlayerSession(player)) return;
        if (text.empty() || text[0] == '.') return;

        BotSurfaceCfg const cfg = GetSurfaceCfg();
        BotSocialCfg const social = GetBotSocialCfg();
        uint64_t const playerGuid = player->GetGUID().GetRawValue();

        std::vector<Player*> candidates;
        auto collectOnlineBot = [&](Player* member)
        {
            if (member && member != player && IsGenuineBot(member) && member->GetGuildId() == player->GetGuildId())
                candidates.push_back(member);
        };
        guild->BroadcastWorker(collectOnlineBot, player);
        if (candidates.empty()) return;

        bool named = false;
        for (Player* bot : candidates)
            named = named || MessageMentionsBot(text, bot->GetName());

        if (!social.enable)
        {
            for (Player* bot : candidates)
                if (MessageMentionsBot(text, bot->GetName()) &&
                    GuildTurnAllowed(playerGuid, bot->GetGUID().GetRawValue(), cfg.guildMaxTurns, cfg.guildWindowSec))
                    DispatchBotSurface(player, bot, SurfaceKind::GuildMsg, "", "", text);
            return;
        }

        if (!named)
        {
            if (social.guildChancePct == 0 ||
                (social.guildChancePct < 100 && (std::rand() % 100) >= social.guildChancePct))
                return;
            std::string cooldownKey = "guild:" + std::to_string(player->GetGuildId());
            if (!BotSocialAmbientAllowed(cooldownKey, social.cooldownSec))
                return;
        }

        std::vector<Player*> chosen = ChooseSocialBots(candidates, text, social.guildMaxSpeakers, social.randomBotChancePct);
        chosen.erase(std::remove_if(chosen.begin(), chosen.end(), [&](Player* bot)
            {
                return !GuildTurnAllowed(playerGuid, bot->GetGUID().GetRawValue(), cfg.guildMaxTurns, cfg.guildWindowSec);
            }), chosen.end());
        DispatchBotSocialConversation(player, chosen, BotSocialChannel::Guild, text,
            "This is guild chat. You are speaking as a guildmate, not as an NPC or assistant.");
    }

    // --- Channel surface: General/Trade, LFG matchmaking + optional ambient ----
    inline void HandleBotChannel(Player* player, uint32 /*type*/, Channel* channel, std::string const& text)
    {
        if (!GetBotCfg().enable || !channel) return;
        if (!IsRealPlayerSession(player)) return;
        if (text.empty() || text[0] == '.') return;

        BotSurfaceCfg const cfg = GetSurfaceCfg();
        std::string const chName = channel->GetName();   // e.g. "General - Elwynn Forest"
        std::string const chLow = ToLowerCopy(chName);
        bool const isGeneral = chLow.find("general") != std::string::npos;
        bool const isTrade = chLow.find("trade") != std::string::npos;
        if (!isGeneral && !isTrade) return;

        // LFG matchmaking
        if (cfg.lfgEnable && ((isGeneral && cfg.lfgGeneral) || (isTrade && cfg.lfgTrade)))
        {
            LfgIntent intent = ParseLfgIntent(text);
            if (intent.isLfg)
            {
                std::vector<BotRosterEntry> roster = BuildZoneBotRoster(player);
                if (BotRosterEntry const* best = MatchBestBot(roster, intent, player->GetLevel(), cfg.levelBracket))
                {
                    if (Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(best->guidRaw)))
                    {
                        std::string what = best->specName.empty() ? std::string(ClassShort(best->cls)) : best->specName;
                        std::string hint = "Someone nearby is forming a group and needs a "
                            + std::string(RoleShort(best->role)) + " (" + what + "). You are that "
                            + what + " and you are close by. Offer briefly and in character to join.";
                        DispatchBotSurface(player, bot, SurfaceKind::ChannelMsg, chName, hint, text);
                    }
                }
                return; // treated as an LFG line whether or not a match was found
            }
        }

        // Ambient chatter (default off): occasionally one local bot chimes in.
        if (isGeneral && cfg.ambientChance > 0 && (uint32)(std::rand() % 100) < cfg.ambientChance)
        {
            std::vector<BotRosterEntry> roster = BuildZoneBotRoster(player);
            if (!roster.empty())
            {
                // prefer a bot the player already knows, else pick one at random
                BotRosterEntry const* pick = nullptr;
                for (auto const& e : roster) if (e.knownToPlayer) { pick = &e; break; }
                if (!pick) pick = &roster[std::rand() % roster.size()];
                if (Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(pick->guidRaw)))
                    DispatchBotSurface(player, bot, SurfaceKind::ChannelMsg, chName, "", text);
            }
        }
    }
}

// ===========================================================================
// Untargeted chat target selection (no NPC selected: who answers a say/yell?)
// ===========================================================================
namespace
{
    bool MessageSoundsHostile(std::string const& text)
    {
        std::string t = ToLowerCopy(text);
        static const char* words[] = {
            "die", "kill", "death", "fight", "attack", "coward", "blood", "slay",
            "destroy", "enemy", "face me", "come here", "you'll pay", "i'll end",
            "murder", "crush", "burn", "bleed", "weak", "pathetic", "challenge",
            "duel", "strike", "fear me", "begone", "filth", "scum"
        };
        for (char const* w : words)
            if (t.find(w) != std::string::npos)
                return true;
        return false;
    }

    // No NPC selected: pick one to answer an untargeted say/yell.
    //  - A yell (or a hostile-sounding line), near hostile creatures, lets the nearest enemy answer.
    //  - Otherwise the nearest friendly/neutral NPC answers, within normal say range.
    // Uses the same cheap DB spatial scan as the quest barks (no grid iteration); the "" join table
    // means "any creature", not just questgivers.
    Creature* FindUntargetedChatNpc(Player* player, bool isYell, std::string const& text, bool& outHostile)
    {
        outHostile = false;
        if (!player || !player->IsInWorld())
            return nullptr;

        float sayRange = g_TriggerRange;
        float yellRange = std::max(sayRange, g_UntargetedYellRange);
        float scanRange = isYell ? yellRange : sayRange;

        std::vector<uint32> entries = GetNearbyQuestgiverEntriesFromDb(player, scanRange, 48, nullptr);
        if (entries.empty())
            return nullptr;

        bool wantHostile = g_UntargetedHostileYellEnabled && g_AllowHostileChat && (isYell || MessageSoundsHostile(text));

        Creature* bestHostile = nullptr;  float bestHostileDist = scanRange + 1.0f;
        Creature* bestFriendly = nullptr; float bestFriendlyDist = sayRange + 1.0f;

        for (uint32 entry : entries)
        {
            Creature* c = player->FindNearestCreature(entry, scanRange, true);
            if (!c || !c->IsInWorld() || !c->IsAlive())
                continue;

            float dist = player->GetDistance(c);

            if (c->IsHostileTo(player))
            {
                if (!wantHostile || dist > yellRange)
                    continue;

                std::string type = CreatureTypeStr(c->GetCreatureType());
                if (type.empty())
                    if (CreatureTemplate const* ct = c->GetCreatureTemplate())
                        type = CreatureTypeStr(ct->type);
                if (!CanSpeakCreatureType(type))
                    continue;

                if (!bestHostile || dist < bestHostileDist)
                {
                    bestHostile = c;
                    bestHostileDist = dist;
                }
            }
            else
            {
                if (dist > sayRange)
                    continue;
                if (!bestFriendly || dist < bestFriendlyDist)
                {
                    bestFriendly = c;
                    bestFriendlyDist = dist;
                }
            }
        }

        if (wantHostile && bestHostile)
        {
            outHostile = true;
            return bestHostile;
        }
        return bestFriendly;
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
            PLAYERHOOK_CAN_PLAYER_USE_GUILD_CHAT,
            PLAYERHOOK_CAN_PLAYER_USE_CHANNEL_CHAT,
            PLAYERHOOK_ON_AFTER_UPDATE,
            PLAYERHOOK_ON_LOGOUT
        }) {}

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg) override
    {
        return HandleNpcChat(player, type, msg);
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Player* receiver) override
    {
        HandleBotWhisper(player, type, receiver, TrimCopy(msg));
        return HandleNpcChat(player, type, msg);
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Group* group) override
    {
        HandleBotGroup(player, type, group, TrimCopy(msg));
        return HandleNpcChat(player, type, msg);
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Guild* guild) override
    {
        HandleBotGuild(player, type, guild, TrimCopy(msg));
        return true;
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Channel* channel) override
    {
        HandleBotChannel(player, type, channel, TrimCopy(msg));
        return true;
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
        uint32 hostileAggroMs = 0;
        uint32 relationshipGreetMs = 0;
        uint32 historyWhisperMs = 0;
        uint32 trainerMs = 0;
        uint32 questMs = 0;
        uint32 questEnderMs = 0;
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
        if (!player || !player->IsAlive())
            return;

        Unit* selected = player->GetSelectedUnit();
        Creature* selectedNpc = selected ? selected->ToCreature() : nullptr;

        PlayerBarkTimers& timers = m_PlayerBarkTimers[player->GetGUID().GetCounter()];

        // Relationship/trainer/hostile first-talk remain selected-NPC based in this focused patch.
        // Quest barks can scan nearby questgivers when NpcChat.QuestBarks.SelectedOnly = 0.
        if (selectedNpc && selectedNpc->IsInWorld())
        {
            if (g_RelationshipBarksEnabled && ShouldRunTimer(timers.relationshipMs, diff, g_RelationshipBarksScanIntervalMs))
            {
                std::string reason;
                bool ok = SpeakCachedRelationshipBark(player, selectedNpc, false, &reason);
                if (!ok)
                    BarkDebug(player, "relationship bark skipped for " + std::string(selectedNpc->GetName()) + ": " + reason);
            }

            if (g_TrainerBarksEnabled && ShouldRunTimer(timers.trainerMs, diff, g_TrainerBarksScanIntervalMs))
            {
                std::string reason;
                // Prefer the spell-aware "you're ready to learn X" bark; fall back to the generic one.
                bool ok = SpeakCachedTrainerSpellBark(player, selectedNpc, false, &reason);
                if (!ok)
                    ok = SpeakCachedTrainerBark(player, selectedNpc, false, &reason);
                if (!ok)
                    BarkDebug(player, "trainer bark skipped for " + std::string(selectedNpc->GetName()) + ": " + reason);
            }

            if (g_HostileFirstTalkEnabled && ShouldRunTimer(timers.hostileMs, diff, g_HostileFirstTalkScanIntervalMs))
            {
                std::string reason;
                bool ok = SpeakCachedHostileBark(player, selectedNpc, false, &reason);
                if (!ok)
                    BarkDebug(player, "hostile first-talk skipped for " + std::string(selectedNpc->GetName()) + ": " + reason);
            }
        }
        else if ((g_RelationshipBarksEnabled || g_TrainerBarksEnabled || g_HostileFirstTalkEnabled) && g_BarkDebug)
            BarkDebug(player, "selected-NPC barks skipped: no selected NPC.");

        if (g_QuestBarksEnabled && ShouldRunTimer(timers.questMs, diff, g_QuestBarksScanIntervalMs))
        {
            Creature* questNpc = FindNearbyQuestgiverForBark(player);
            if (!questNpc)
            {
                BarkDebug(player, std::string("quest scan found no eligible nearby questgiver. selectedOnly=") +
                    (g_QuestBarksSelectedOnly ? "1" : "0") +
                    " range=" + std::to_string(g_QuestBarksTriggerDistance));
                return;
            }

            std::string reason;
            bool ok = SpeakCachedQuestBark(player, questNpc, false, 0, &reason);
            if (!ok)
                BarkDebug(player, "quest bark skipped for " + std::string(questNpc->GetName()) + ": " + reason);
        }

        // Quest-ender nudge: walk near the turn-in NPC for a quest you're still working on and it
        // asks "did you do it yet?". Shares the quest bark toggles/cooldowns and the universal cache.
        if (g_QuestBarksEnabled && ShouldRunTimer(timers.questEnderMs, diff, g_QuestBarksScanIntervalMs))
        {
            if (Creature* enderNpc = FindNearbyQuestEnderForBark(player))
            {
                std::string reason;
                bool ok = SpeakCachedQuestEnderBark(player, enderNpc, false, 0, &reason);
                if (!ok)
                    BarkDebug(player, "quest ender bark skipped for " + std::string(enderNpc->GetName()) + ": " + reason);
            }
        }

        // Aggro-driven hostile speak: mobs actually attacking the player taunt when they engage.
        // No grid scan - we read the player's attacker set. Opt-in per NPC via the speak profile
        // (can_speak), which generating a hostile bark sets automatically.
        if (g_HostileFirstTalkEnabled && player->IsInCombat() &&
            ShouldRunTimer(timers.hostileAggroMs, diff, g_HostileFirstTalkScanIntervalMs))
        {
            for (Unit* attacker : player->getAttackers())
            {
                Creature* foe = attacker ? attacker->ToCreature() : nullptr;
                if (!foe || !foe->IsInWorld() || !foe->IsHostileTo(player))
                    continue;
                if (!NpcProfileCanSpeak(foe->GetEntry()))
                    continue;

                std::string reason;
                bool ok = SpeakCachedHostileBark(player, foe, false, &reason);
                if (!ok)
                    BarkDebug(player, "hostile aggro bark skipped for " + std::string(foe->GetName()) + ": " + reason);
                break; // at most one taunt per tick
            }
        }

        // Pass-by greeting: an NPC you already have a relationship with greets you when you walk
        // near it - so the world isn't dead. We pull this player's known NPC entries from SQL and
        // ask the grid for any spawn of those entries within range in a single call (no grid scan).
        if (g_RelationshipBarksEnabled &&
            ShouldRunTimer(timers.relationshipGreetMs, diff, g_RelationshipBarksScanIntervalMs))
        {
            std::vector<uint32> knownEntries =
                GetPlayerRelationshipEntries(player->GetGUID().GetRawValue(), 64);
            if (!knownEntries.empty())
            {
                std::list<Creature*> nearbyKnown;
                player->GetCreatureListWithEntryInGrid(nearbyKnown, knownEntries, g_RelationshipBarksTriggerDistance);
                for (Creature* friendNpc : nearbyKnown)
                {
                    if (!friendNpc || !friendNpc->IsInWorld() || !friendNpc->IsAlive())
                        continue;
                    if (friendNpc->IsHostileTo(player))
                        continue;

                    std::string reason;
                    bool ok = SpeakCachedRelationshipBark(player, friendNpc, false, &reason);
                    if (!ok)
                        BarkDebug(player, "relationship greeting skipped for " + std::string(friendNpc->GetName()) + ": " + reason);
                    break; // one greeting per tick
                }
            }
        }

        // A simpler history-based social pass: if the player has genuinely talked to an NPC before,
        // that nearby non-hostile NPC may privately recognize them without requiring a .barks file.
        if (g_HistoryWhispersEnabled &&
            ShouldRunTimer(timers.historyWhisperMs, diff, g_HistoryWhispersScanIntervalMs))
        {
            std::vector<uint32> contactEntries = GetPlayerContactEntries(player->GetGUID().GetRawValue(), 64);
            if (!contactEntries.empty())
            {
                std::list<Creature*> nearbyContacts;
                player->GetCreatureListWithEntryInGrid(nearbyContacts, contactEntries, g_HistoryWhispersTriggerDistance);
                for (Creature* knownNpc : nearbyContacts)
                {
                    if (!knownNpc || !knownNpc->IsInWorld() || !knownNpc->IsAlive() || knownNpc->IsHostileTo(player))
                        continue;
                    std::string reason;
                    if (TryDispatchNpcHistoryWhisper(player, knownNpc, &reason))
                        break; // at most one API-backed recognition whisper per scan
                    BarkDebug(player, "history whisper skipped for " + std::string(knownNpc->GetName()) + ": " + reason);
                }
            }
        }
    }

    bool HandleNpcChat(Player* player, uint32 type, std::string& msg)
    {
        // Returning true lets normal chat continue. This module listens; it does not block.
        if (!g_Enable || !player)
            return true;

        if (type != CHAT_MSG_SAY && type != CHAT_MSG_YELL)
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
        // Private NPC chat now uses "@p message" directly. This is checked
        // before generic prefix stripping so it works even when NpcChat.Prefix = "!".
        if ((text.rfind("@p", 0) == 0 || text.rfind("@P", 0) == 0) &&
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

        // Bot chat: if the selected target is a genuine bot player, let it answer and stop.
        if (Player* botTarget = sel ? sel->ToPlayer() : nullptr)
            if (HandleBotSay(player, botTarget, text))
                return true;

        // No NPC selected: let a nearby NPC field the say/yell, if enabled. A hostile-sounding yell
        // near enemies is answered by the nearest enemy; otherwise the nearest friendly NPC answers.
        if ((!npc || !npc->IsAlive()) && g_UntargetedChatEnabled)
        {
            bool untargetedHostile = false;
            npc = FindUntargetedChatNpc(player, type == CHAT_MSG_YELL, text, untargetedHostile);
        }

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
        req.autoTags = ResolveCreatureAutoTags(npc);
        if (IsTrainerNpc(npc))
            req.trainerInfo = BuildTrainerCurriculumText(npc, player, true);

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
        EnsureNpcContactTable();
    }

    void OnUpdate(uint32 diff) override
    {
        if (!g_Enable)
            return;

        // One-time disk->SQL sub-prompt import (new rows only). Runs on the main thread after the
        // DB is up. Force a full re-sync any time with: .npcc sub import overwrite
        static bool s_subPromptsImported = false;
        if (!s_subPromptsImported)
        {
            s_subPromptsImported = true;
            if (g_SubPromptImportOnStartup)
            {
                std::lock_guard<std::mutex> lock(g_FileMutex);
                int imported = ImportSubPromptsFromDisk(false);
                LOG_INFO("module", "[NpcChat] Sub-prompt disk->SQL import: {} file(s) processed (new rows only).", imported);
            }
        }

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
                    if (r.whisper)
                    {
                        npc->Whisper(r.text, LANG_UNIVERSAL, anchor);
                    }
                    else if (r.forcePrivateReply)
                    {
                        // Targeted creature say: useful for @p private chat and hostile parley at long range.
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

        EmitBotReplies();
        EmitBotSurfaceReplies();
        EmitBotSocialReplies();

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
        req.autoTags = ResolveCreatureAutoTags(npc);

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
            handler->PSendSysMessage(".npcc quest list");
            handler->PSendSysMessage(".npcc gen quest [questId] [quoted extra direction] - generates separate DB barks per quest");
            handler->PSendSysMessage(".npcc gen questender [questId] [quoted extra direction] - 'did you do it yet?' barks for in-progress quests");
            handler->PSendSysMessage(".npcc gen bark quest [questId] [quoted extra direction] - same as gen quest");
            handler->PSendSysMessage(".npcc gen bark relationship [quoted extra direction]");
            handler->PSendSysMessage(".npcc gen bark hostile [quoted extra direction]");
            handler->PSendSysMessage(".npcc trainer - list spells the selected trainer can teach you now");
            handler->PSendSysMessage("GM/Allowed: .npcc gen bark trainerspell - 'ready to learn X' bark (uses {{spell}})");
            handler->PSendSysMessage(".npcc bark trainerspell - test the trainer-spell bark");
            handler->PSendSysMessage(".npcc bark quest [questId]");
            handler->PSendSysMessage(".npcc bark questender [questId]");
            handler->PSendSysMessage(".npcc bark relationship");
            handler->PSendSysMessage(".npcc bark hostile");
            handler->PSendSysMessage(".npcc hostile status");
            handler->PSendSysMessage("GM: .npcc hostile disable [reason]");
            handler->PSendSysMessage("GM: .npcc hostile enable");
            handler->PSendSysMessage(".npcc profile show | GM: speak <0|1> | chance <0-100> | kind <k> | tag <csv>");
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

        if (StartsWithWord(arg, "hostile", rest) || StartsWithWord(arg, "enemy", rest))
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

            std::string subRest;
            std::string disabledReason;
            bool disabled = IsNpcBarkDisabled(npc->GetEntry(), "hostile_first_talk", &disabledReason);

            if (rest.empty() || StartsWithWord(rest, "status", subRest))
            {
                handler->PSendSysMessage("Hostile first-talk status for {} entry {}: {}",
                    npc->GetName(), npc->GetEntry(), disabled ? "disabled" : "enabled");
                if (disabled && !disabledReason.empty())
                    handler->PSendSysMessage("Reason: {}", disabledReason.c_str());
                handler->PSendSysMessage("SQL cache context: hostile_first_talk");
                return true;
            }

            if (StartsWithWord(rest, "disable", subRest) || StartsWithWord(rest, "off", subRest))
            {
                if (!CanManageSharedSubPrompts(handler))
                {
                    handler->PSendSysMessage("Only GMs or configured NPC Chat creator accounts may disable hostile first-talk for NPCs.");
                    return true;
                }
                std::string why = StripWrappingQuotes(TrimCopy(subRest));
                if (why.empty())
                    why = "Manually disabled; NPC already has scripted dialogue or should remain silent.";
                SetNpcBarkDisabled(npc->GetEntry(), "hostile_first_talk", true, why);
                handler->PSendSysMessage("Disabled hostile first-talk for {} entry {}.", npc->GetName(), npc->GetEntry());
                return true;
            }

            if (StartsWithWord(rest, "enable", subRest) || StartsWithWord(rest, "on", subRest))
            {
                if (!CanManageSharedSubPrompts(handler))
                {
                    handler->PSendSysMessage("Only GMs or configured NPC Chat creator accounts may enable hostile first-talk for NPCs.");
                    return true;
                }
                SetNpcBarkDisabled(npc->GetEntry(), "hostile_first_talk", false, "");
                handler->PSendSysMessage("Enabled hostile first-talk for {} entry {}.", npc->GetName(), npc->GetEntry());
                return true;
            }

            handler->PSendSysMessage("Usage: .npcc hostile status | disable [reason] | enable");
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
            uint64_t relGuid = player->GetGUID().GetRawValue();
            uint32 relEntry = npc->GetEntry();
            std::string relPName = player->GetName();
            std::string relNName = npc->GetName();
            std::string relRest;

            std::lock_guard<std::mutex> lock(g_FileMutex);
            EnsureNpcChatDirectoriesAndDefaultPrompt();

            if (rest.empty() || StartsWithWord(rest, "show", relRest))
            {
                std::string current = LoadRelationshipText(relGuid, relEntry, relPName, relNName);
                handler->PSendSysMessage("NPC Chat relationship file: {}", path.c_str());
                if (current.empty())
                    handler->PSendSysMessage("No relationship memory exists yet. Use .npcc rel set summary \"...\" or .npcc rel tag <name>.");
                else
                    handler->PSendSysMessage("{}", current.c_str());
                return true;
            }

            if (StartsWithWord(rest, "clear", relRest))
            {
                DeleteRelationship(relGuid, relEntry);
                bool removed = RemoveFileIfExists(path);
                handler->PSendSysMessage(removed ? "NPC Chat relationship memory cleared." : "Relationship memory cleared (SQL).");
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

                std::map<std::string, std::string> kv = LoadRelationshipKV(relGuid, relEntry, relPName, relNName);
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

                if (!SaveRelationshipKV(relGuid, relEntry, relPName, relNName, kv))
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

                std::map<std::string, std::string> kv = LoadRelationshipKV(relGuid, relEntry, relPName, relNName);
                kv["summary"] = text;
                if (kv.find("score") == kv.end())
                    kv["score"] = "0";
                if (kv.find("intimacy") == kv.end())
                    kv["intimacy"] = "0";
                if (kv.find("stance") == kv.end())
                    kv["stance"] = npc->IsHostileTo(player) ? "hostile" : "neutral";

                if (!SaveRelationshipKV(relGuid, relEntry, relPName, relNName, kv))
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

                std::map<std::string, std::string> kv = LoadRelationshipKV(relGuid, relEntry, relPName, relNName);
                kv[key] = value;
                if (!SaveRelationshipKV(relGuid, relEntry, relPName, relNName, kv))
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
            else if (StartsWithWord(rest, "trainerspell", barkRest) || StartsWithWord(rest, "spell", barkRest))
                barkMode = "trainerspell";
            else if (StartsWithWord(rest, "trainer", barkRest) || StartsWithWord(rest, "train", barkRest))
                barkMode = "trainer";
            else if (StartsWithWord(rest, "questender", barkRest) || StartsWithWord(rest, "ender", barkRest))
                barkMode = "questender";
            else if (StartsWithWord(rest, "quest", barkRest) || StartsWithWord(rest, "quests", barkRest))
                barkMode = "quest";
            else if (rest.empty() || StartsWithWord(rest, "relationship", barkRest) || StartsWithWord(rest, "rel", barkRest))
                barkMode = "relationship";
            else
            {
                handler->PSendSysMessage("Usage: .npcc bark <relationship|hostile|trainer|trainerspell|quest|questender> [questId]");
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
            else if (barkMode == "trainerspell")
            {
                if (SpeakCachedTrainerSpellBark(player, npc, true, &reason))
                    handler->PSendSysMessage("NPC Chat cached trainer-spell bark fired.");
                else
                    handler->PSendSysMessage("NPC Chat trainer-spell bark did not fire: {}", reason.c_str());
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
            else if (barkMode == "questender")
            {
                uint32 questId = 0;
                std::string qText = TrimCopy(barkRest);
                if (!qText.empty())
                {
                    try { questId = static_cast<uint32>(std::stoul(qText)); }
                    catch (std::exception const&) { questId = 0; }
                }
                if (SpeakCachedQuestEnderBark(player, npc, true, questId, &reason))
                    handler->PSendSysMessage("NPC Chat cached quest-ender bark fired.");
                else
                    handler->PSendSysMessage("NPC Chat quest-ender bark did not fire: {}", reason.c_str());
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
            handler->PSendSysMessage("NPC Chat bark debug: {} cooldownSec={}", g_BarkDebug ? "yes" : "no", g_BarkDebugCooldownSec);
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
            handler->PSendSysMessage("NPC Chat quest barks enabled: {} generateMissing={} selectedOnly={} chance={} range={} scanMs={}",
                g_QuestBarksEnabled ? "yes" : "no",
                g_QuestBarksGenerateMissing ? "yes" : "no",
                g_QuestBarksSelectedOnly ? "yes" : "no",
                g_QuestBarksChancePct,
                g_QuestBarksTriggerDistance,
                g_QuestBarksScanIntervalMs);
            handler->PSendSysMessage("NPC Chat untargeted chat: {} yellRange={} hostileYell={}",
                g_UntargetedChatEnabled ? "yes" : "no",
                g_UntargetedYellRange,
                g_UntargetedHostileYellEnabled ? "yes" : "no");
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

        if (StartsWithWord(arg, "trainer", rest))
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
                handler->PSendSysMessage("Target a trainer first.");
                return true;
            }
            if (!sObjectMgr->GetTrainer(npc->GetEntry()))
            {
                handler->PSendSysMessage("{} (entry {}) has no trainer spell list.", npc->GetName(), npc->GetEntry());
                return true;
            }

            std::vector<LearnableSpell> learnable = GetLearnableTrainerSpells(player, npc);
            handler->PSendSysMessage("Trainer {} ({}): {} ability/abilities learnable by you right now.",
                npc->GetName(), TrainerTypeName(npc).c_str(), static_cast<uint32>(learnable.size()));
            int shown = 0;
            for (LearnableSpell const& s : learnable)
            {
                handler->PSendSysMessage("  {} - {} (reqLevel {})", s.spellId, s.name.c_str(), s.reqLevel);
                if (++shown >= 15)
                {
                    handler->PSendSysMessage("  ...and more.");
                    break;
                }
            }
            if (learnable.empty())
                handler->PSendSysMessage("Nothing new for you here right now (level/skill not met, or already known).");
            else
                handler->PSendSysMessage("Pre-generate the bark with: .npcc gen bark trainerspell");
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
            if (StartsWithWord(rest, "questender", questGenRest) || StartsWithWord(rest, "ender", questGenRest))
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
                std::vector<QuestBarkQuestInfo> quests = GetAllNpcEnderQuestInfos(npc, onlyQuestId);
                if (quests.empty())
                {
                    handler->PSendSysMessage(onlyQuestId ?
                        "NPC Chat quest-ender generation failed: this NPC does not end that quest." :
                        "NPC Chat quest-ender generation failed: this NPC is not the turn-in NPC for any quest.");
                    return true;
                }
                uint32 started = StartQuestBarkGenerationForEachQuest(nullptr, player, npc, quests, extraQuest, "quest_ender");
                if (!started)
                    handler->PSendSysMessage("NPC Chat quest-ender bark generation did not start (no valid quest keys).");
                else
                    handler->PSendSysMessage("Quest-ender bark generation queued: {} quest(s). These generate asynchronously and save in a few seconds - you will get a 'saved to DB cache' confirmation per quest.", started);
                return true;
            }

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
                std::vector<QuestBarkQuestInfo> starters = GetAllNpcStarterQuestInfos(npc, onlyQuestId);
                std::vector<QuestBarkQuestInfo> enders = GetAllNpcEnderQuestInfos(npc, onlyQuestId);
                if (starters.empty() && enders.empty())
                {
                    handler->PSendSysMessage(onlyQuestId ?
                        "NPC Chat quest bark generation failed: this NPC does not start or end that quest." :
                        "NPC Chat quest bark generation failed: this NPC has no quests to start or end.");
                    return true;
                }
                uint32 introStarted = starters.empty() ? 0u :
                    StartQuestBarkGenerationForEachQuest(nullptr, player, npc, starters, extraQuest, "quest_available");
                uint32 enderStarted = enders.empty() ? 0u :
                    StartQuestBarkGenerationForEachQuest(nullptr, player, npc, enders, extraQuest, "quest_ender");
                uint32 started = introStarted + enderStarted;
                if (!started)
                    handler->PSendSysMessage("NPC Chat quest bark generation did not start (no valid quest keys).");
                else
                    handler->PSendSysMessage("Quest bark generation queued: {} intro + {} ender. These generate asynchronously and save in a few seconds - you will get a 'saved to DB cache' confirmation per quest.",
                        introStarted, enderStarted);
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
                else if (StartsWithWord(genKindRest, "trainerspell", barkRest) || StartsWithWord(genKindRest, "spell", barkRest))
                {
                    barkMode = "trainerspell";
                    extraBark = barkRest;
                }
                else if (StartsWithWord(genKindRest, "trainer", barkRest) || StartsWithWord(genKindRest, "train", barkRest))
                {
                    barkMode = "trainer";
                    extraBark = barkRest;
                }
                else if (StartsWithWord(genKindRest, "quest", barkRest) || StartsWithWord(genKindRest, "quests", barkRest))
                {
                    uint32 onlyQuestId = 0;
                    std::string extraQuest = barkRest;
                    std::string trimmedQuest = TrimCopy(barkRest);
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

                    std::vector<QuestBarkQuestInfo> starters = GetAllNpcStarterQuestInfos(npc, onlyQuestId);
                    std::vector<QuestBarkQuestInfo> enders = GetAllNpcEnderQuestInfos(npc, onlyQuestId);
                    if (starters.empty() && enders.empty())
                    {
                        handler->PSendSysMessage(onlyQuestId ?
                            "NPC Chat quest bark generation failed: this NPC does not start or end that quest." :
                            "NPC Chat quest bark generation failed: this NPC has no quests to start or end.");
                        return true;
                    }

                    uint32 introStarted = starters.empty() ? 0u :
                        StartQuestBarkGenerationForEachQuest(nullptr, player, npc, starters, extraQuest, "quest_available");
                    uint32 enderStarted = enders.empty() ? 0u :
                        StartQuestBarkGenerationForEachQuest(nullptr, player, npc, enders, extraQuest, "quest_ender");
                    uint32 started = introStarted + enderStarted;
                    if (!started)
                        handler->PSendSysMessage("NPC Chat quest bark generation did not start (no valid quest keys).");
                    else
                        handler->PSendSysMessage("Quest bark generation queued: {} intro + {} ender. These generate asynchronously and save in a few seconds - you will get a 'saved to DB cache' confirmation per quest.",
                            introStarted, enderStarted);
                    return true;
                }
                else if (!genKindRest.empty())
                {
                    handler->PSendSysMessage("Usage: .npcc gen bark <relationship|hostile|trainer|quest> [quoted extra direction]");
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
                    std::string disabledReason;
                    if (IsNpcBarkDisabled(npc->GetEntry(), "hostile_first_talk", &disabledReason))
                    {
                        handler->PSendSysMessage("NPC Chat hostile first-talk is disabled for {} entry {}{}{}.",
                            npc->GetName(), npc->GetEntry(),
                            disabledReason.empty() ? "" : ": ",
                            disabledReason.empty() ? "" : disabledReason.c_str());
                        return true;
                    }
                    barkReq.cacheContext = "hostile_first_talk";
                    std::string pendingKey = NpcBarkGenerationKey(barkReq.npcEntry, barkReq.cacheContext, barkReq.faction, barkReq.playerRace, barkReq.playerClass, barkReq.phase);
                    if (!TryMarkNpcBarkGenerationPending(pendingKey))
                    {
                        handler->PSendSysMessage("NPC Chat hostile bark generation is already pending for this NPC/context/player type.");
                        return true;
                    }
                    std::thread([barkReq = std::move(barkReq)]() mutable { ::GenerateHostileBarkCacheWorker(std::move(barkReq)); }).detach();
                    handler->PSendSysMessage("NPC Chat hostile SQL bark generation started for {}.", npc->GetName());
                    handler->PSendSysMessage("You will get a system message when the DB cache row is saved.");
                }
                else if (barkMode == "trainerspell")
                {
                    if (!CanManageSharedSubPrompts(handler))
                    {
                        handler->PSendSysMessage("Only GMs or configured NPC Chat creator accounts may generate shared trainer-spell barks.");
                        return true;
                    }
                    if (!IsTrainerNpc(npc) || !sObjectMgr->GetTrainer(npc->GetEntry()))
                    {
                        handler->PSendSysMessage("Target NPC is not a trainer with a spell list.");
                        return true;
                    }
                    barkReq.roleStr = TrainerTypeName(npc);
                    std::string curriculum = BuildTrainerCurriculumText(npc, nullptr, false);
                    if (!curriculum.empty())
                    {
                        std::string s = "Abilities this trainer teaches, by level (flavor/grounding only - do NOT name a specific one in the line; always use {spell}):\n" + curriculum;
                        barkReq.extraInstruction = barkReq.extraInstruction.empty() ? s : (barkReq.extraInstruction + "\n" + s);
                    }
                    barkReq.cacheContext = "trainer_spell";
                    std::thread([barkReq = std::move(barkReq)]() mutable { ::GenerateTrainerSpellBarkCacheWorker(std::move(barkReq)); }).detach();
                    handler->PSendSysMessage("NPC Chat trainer-spell bark generation started for {}.", npc->GetName());
                    handler->PSendSysMessage("Saves one universal DB row using a {{spell}} placeholder, filled per learnable spell at speak time.");
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
            if (IsTrainerNpc(npc))
                req.trainerInfo = BuildTrainerCurriculumText(npc, nullptr, false);
            if (mode == "personal")
                req.outputPath = PersonalPromptFilePath(player->GetName(), player->GetGUID().GetRawValue(), npc->GetName(), npc->GetEntry());
            else
                req.outputPath = SharedPromptFilePath(npc->GetName(), npc->GetEntry());

            std::thread(GeneratePromptWorker, std::move(req)).detach();

            handler->PSendSysMessage("NPC Chat character prompt generation started for {} ({}).", npc->GetName(), mode.c_str());
            handler->PSendSysMessage("You will get a system message when the file is saved.");
            return true;
        }

        if (StartsWithWord(arg, "profile", rest))
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
                handler->PSendSysMessage("Select an NPC first.");
                return true;
            }

            uint32 entry = npc->GetEntry();
            std::string pRest;

            if (rest.empty() || StartsWithWord(rest, "show", pRest) || StartsWithWord(rest, "help", pRest))
            {
                NpcProfile p = LoadNpcProfile(entry);
                handler->PSendSysMessage("Profile for {} (entry {}):", npc->GetName().c_str(), entry);
                handler->PSendSysMessage("  can_speak={} speak_chance={} kind={}",
                    p.canSpeak, p.speakChance, p.kind.empty() ? "auto" : p.kind.c_str());
                handler->PSendSysMessage("  tags: {}", p.tags.empty() ? "(none)" : p.tags.c_str());
                handler->PSendSysMessage("Edit (GM/allowed): .npcc profile speak <0|1> | chance <0-100> | kind <quest|regular|hostile|auto> | tag <csv>");
                return true;
            }

            // All edits below change shared, all-spawn data -> gate like global sub-prompt creation.
            if (!CanCreateSubPrompts(handler))
            {
                handler->PSendSysMessage("Only GMs or configured NPC Chat creator accounts may edit NPC profiles.");
                return true;
            }

            uint32 acct = player->GetSession() ? player->GetSession()->GetAccountId() : 0;

            if (StartsWithWord(rest, "speak", pRest))
            {
                std::string v = TrimCopy(pRest);
                bool on = (v == "1" || ToLowerCopy(v) == "on" || ToLowerCopy(v) == "yes" || ToLowerCopy(v) == "true");
                SetNpcProfileCanSpeak(entry, on, acct);
                handler->PSendSysMessage("Profile can_speak set to {} for entry {}.", on ? 1 : 0, entry);
                return true;
            }

            if (StartsWithWord(rest, "chance", pRest))
            {
                int n = 0;
                try { n = std::stoi(TrimCopy(pRest)); }
                catch (...) { n = 0; }
                if (n < 0) n = 0;
                if (n > 100) n = 100;
                UpsertNpcProfileField(entry, "speak_chance", std::to_string(n), acct);
                handler->PSendSysMessage("Profile speak_chance set to {} for entry {} (0 = use global default).", n, entry);
                return true;
            }

            if (StartsWithWord(rest, "kind", pRest))
            {
                std::string k = ToLowerCopy(TrimCopy(pRest));
                if (k != "quest" && k != "regular" && k != "hostile" && k != "auto")
                {
                    handler->PSendSysMessage("kind must be one of: quest, regular, hostile, auto.");
                    return true;
                }
                UpsertNpcProfileField(entry, "npc_kind", "'" + SqlEscape(k) + "'", acct);
                handler->PSendSysMessage("Profile kind set to {} for entry {}.", k.c_str(), entry);
                return true;
            }

            if (StartsWithWord(rest, "tag", pRest))
            {
                std::string csv = TrimCopy(pRest);
                UpsertNpcProfileField(entry, "tags", "'" + SqlEscape(csv) + "'", acct);
                handler->PSendSysMessage("Profile tags set to '{}' for entry {}. These feed the auto-linker (e.g. dwarf,bronzebeard).", csv.c_str(), entry);
                return true;
            }

            handler->PSendSysMessage("Unknown .npcc profile subcommand. Try: show | speak | chance | kind | tag");
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
                handler->PSendSysMessage(".npcc sub auto   (preview auto-linked sub-prompts for the selected NPC)");
                handler->PSendSysMessage(".npcc sub import   (disk -> SQL, new rows only)");
                handler->PSendSysMessage("GM: .npcc sub import overwrite   (force full disk -> SQL re-sync)");
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

            if (StartsWithWord(rest, "auto", subRest))
            {
                Creature* npc = GetSelectedCreature(handler);
                if (!npc)
                {
                    handler->PSendSysMessage("Select an NPC first to preview its auto-linked sub-prompts.");
                    return true;
                }

                std::string tags = ResolveCreatureAutoTags(npc);
                std::vector<std::string> matched = MatchSubPromptNamesForTags(tags);
                handler->PSendSysMessage("Auto-linker for {} (entry {}):", npc->GetName().c_str(), npc->GetEntry());
                handler->PSendSysMessage("  tags: {}", tags.empty() ? "(none)" : tags.c_str());
                handler->PSendSysMessage("  matched sub-prompts: {}", matched.empty() ? "(none)" : JoinNames(matched).c_str());
                return true;
            }

            if (StartsWithWord(rest, "import", subRest))
            {
                bool overwrite = false;
                std::string ov;
                if (StartsWithWord(subRest, "overwrite", ov))
                {
                    if (!IsGm(handler))
                    {
                        handler->PSendSysMessage("Only GMs may force-overwrite SQL sub-prompts from disk. Use .npcc sub import (new-only) instead.");
                        return true;
                    }
                    overwrite = true;
                }

                std::lock_guard<std::mutex> lock(g_FileMutex);
                EnsureNpcChatDirectoriesAndDefaultPrompt();
                int imported = ImportSubPromptsFromDisk(overwrite);
                handler->PSendSysMessage("NPC Chat imported {} disk sub-prompt(s) into SQL{}.",
                    imported, overwrite ? " (overwrite/full re-sync)" : " (new rows only)");
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
