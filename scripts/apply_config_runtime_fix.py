from pathlib import Path
import re

cpp_path = Path('src/mod_npcchat.cpp')
text = cpp_path.read_text()

# Legacy/alias keys are optional compatibility paths. Missing aliases must not warn.
text = text.replace(
    'sConfigMgr->GetOption<int32>("NpcChat.GeneratePromptMaxTokens", 700)',
    'sConfigMgr->GetOption<int32>("NpcChat.GeneratePromptMaxTokens", 700, false)')
text = text.replace(
    'sConfigMgr->GetOption<float>("NpcChat.GeneratePromptTemperature", 0.75f)',
    'sConfigMgr->GetOption<float>("NpcChat.GeneratePromptTemperature", 0.75f, false)')
text = text.replace(
    'sConfigMgr->GetOption<std::string>("NpcChat.SubPromptCreatorAccountIds", "")',
    'sConfigMgr->GetOption<std::string>("NpcChat.SubPromptCreatorAccountIds", "", false)')
text = text.replace(
    'sConfigMgr->GetOption<std::string>("NpcChat.SubPromptCreatorAccountIDs", "")',
    'sConfigMgr->GetOption<std::string>("NpcChat.SubPromptCreatorAccountIDs", "", false)')

# Hot-path bot config is intentionally defaultable. Do not emit a missing-key warning
# on every chat/update tick when an older deployed .conf lacks a newly added option.
def silence_getoptions_between(source, start_marker, end_marker):
    start = source.index(start_marker)
    end = source.index(end_marker, start)
    block = source[start:end]
    pattern = re.compile(r'sConfigMgr->GetOption<([^>]+)>\(("NpcChat\.Bot\.[^"]+"),\s*([^\)\n]+)\)')

    def repl(match):
        args = match.group(3).rstrip()
        if args.endswith(', false'):
            return match.group(0)
        return f'sConfigMgr->GetOption<{match.group(1)}>({match.group(2)}, {args}, false)'

    block = pattern.sub(repl, block)
    return source[:start] + block + source[end:]

text = silence_getoptions_between(text, 'inline BotCfg GetBotCfg()', '// --- guild bot presence')
text = silence_getoptions_between(text, 'inline GuildPresenceCfg GetGuildPresenceCfg()', 'inline std::map<uint32, time_t>& GuildPresenceNextScanByGuild()')
text = silence_getoptions_between(text, 'inline BotSocialCfg GetBotSocialCfg()', 'inline std::map<std::string, time_t>& BotSocialCooldownUntil()')
text = silence_getoptions_between(text, 'inline BotSurfaceCfg GetSurfaceCfg()', '// --- cross-thread structs / queue')

anchor = '''        if (g_QuestBarksMaxQuestsCheckedPerNpc < 1)\n            g_QuestBarksMaxQuestsCheckedPerNpc = 1;\n    }\n\n    NpcChat_ApiConfig BuildChatApiConfig()\n'''
insert = '''        if (g_QuestBarksMaxQuestsCheckedPerNpc < 1)\n            g_QuestBarksMaxQuestsCheckedPerNpc = 1;\n    }\n\n    std::vector<std::string> LoadedNpcChatConfigKeys()\n    {\n        try\n        {\n            return sConfigMgr->GetKeysByString("NpcChat.");\n        }\n        catch (...)\n        {\n            return {};\n        }\n    }\n\n    void LogNpcChatConfigState(char const* context)\n    {\n        std::vector<std::string> const keys = LoadedNpcChatConfigKeys();\n        if (keys.empty())\n        {\n            LOG_ERROR("module", "[NpcChat] {}: ConfigMgr has ZERO loaded NpcChat.* keys. The runtime module config is not being loaded. Check the deployed modules config directory, not only the source .conf.dist file.", context);\n            return;\n        }\n\n        LOG_INFO("module", "[NpcChat] {}: loaded {} NpcChat.* key(s); enabled={}; baseUrl='{}'; model='{}'; apiKey={}",\n            context, keys.size(), g_Enable ? "yes" : "no", g_BaseUrl,\n            g_Model.empty() ? "(missing)" : g_Model, g_ApiKey.empty() ? "missing" : "set");\n\n        if (!g_Enable)\n            LOG_ERROR("module", "[NpcChat] {}: NpcChat.Enable resolved to 0. All NPC/playerbot LLM chat is disabled.", context);\n        if (g_BaseUrl.empty())\n            LOG_ERROR("module", "[NpcChat] {}: NpcChat.BaseUrl resolved empty. LLM requests cannot be sent.", context);\n        if (g_Model.empty())\n            LOG_ERROR("module", "[NpcChat] {}: NpcChat.Model resolved empty. LLM requests will not produce usable replies.", context);\n    }\n\n    NpcChat_ApiConfig BuildChatApiConfig()\n'''
if anchor not in text:
    raise SystemExit('LoadConfig end anchor not found')
text = text.replace(anchor, insert, 1)

old = '''    void OnStartup() override\n    {\n        LoadConfig();\n        EnsureNpcChatDirectoriesAndDefaultPrompt();\n'''
new = '''    void OnStartup() override\n    {\n        LoadConfig();\n        LogNpcChatConfigState("startup");\n        EnsureNpcChatDirectoriesAndDefaultPrompt();\n'''
if old not in text:
    raise SystemExit('OnStartup anchor not found')
text = text.replace(old, new, 1)

old = '            handler->PSendSysMessage(".npcc key");\n            handler->PSendSysMessage(".npcc reload");\n'
new = '            handler->PSendSysMessage(".npcc key");\n            handler->PSendSysMessage(".npcc status");\n            handler->PSendSysMessage(".npcc reload");\n'
if old not in text:
    raise SystemExit('help anchor not found')
text = text.replace(old, new, 1)

old = '''        if (StartsWithWord(arg, "reload", rest))\n        {\n            LoadConfig();\n            std::lock_guard<std::mutex> lock(g_FileMutex);\n            EnsureNpcChatDirectoriesAndDefaultPrompt();\n            handler->PSendSysMessage("NPC Chat config and prompt paths reloaded.");\n            return true;\n        }\n'''
new = '''        if (StartsWithWord(arg, "status", rest))\n        {\n            std::vector<std::string> const keys = LoadedNpcChatConfigKeys();\n            handler->PSendSysMessage("NPC Chat effective runtime status:");\n            handler->PSendSysMessage("  loaded NpcChat.* keys: {}", keys.size());\n            handler->PSendSysMessage("  NpcChat.Enable: {}", g_Enable ? "1" : "0");\n            handler->PSendSysMessage("  BaseUrl: {}", g_BaseUrl.empty() ? "(missing)" : g_BaseUrl.c_str());\n            handler->PSendSysMessage("  Model: {}", g_Model.empty() ? "(missing)" : g_Model.c_str());\n            handler->PSendSysMessage("  ApiKey: {}", g_ApiKey.empty() ? "missing" : "set");\n            handler->PSendSysMessage("  Config path: {}", sConfigMgr->GetConfigPath().c_str());\n            if (keys.empty())\n                handler->PSendSysMessage("  ERROR: no NpcChat.* options are loaded. Check the deployed module config under the runtime config path/modules directory.");\n            else if (!g_Enable || g_Model.empty() || g_BaseUrl.empty())\n                handler->PSendSysMessage("  ERROR: live NPC chat cannot work with the effective values above.");\n            return true;\n        }\n\n        if (StartsWithWord(arg, "reload", rest))\n        {\n            bool const moduleConfigsReloaded = sConfigMgr->LoadModulesConfigs(true, true);\n            LoadConfig();\n            LogNpcChatConfigState(".npcc reload");\n            {\n                std::lock_guard<std::mutex> lock(g_FileMutex);\n                EnsureNpcChatDirectoriesAndDefaultPrompt();\n            }\n            handler->PSendSysMessage("NPC Chat module config files reread from disk: {}", moduleConfigsReloaded ? "yes" : "NO");\n            handler->PSendSysMessage("Effective: Enable={} Model={} ApiKey={} NpcChatKeys={}",\n                g_Enable ? "1" : "0", g_Model.empty() ? "(missing)" : g_Model.c_str(),\n                g_ApiKey.empty() ? "missing" : "set", LoadedNpcChatConfigKeys().size());\n            if (!moduleConfigsReloaded)\n                handler->PSendSysMessage("Config reload failed. Check the runtime modules config directory and worldserver log.");\n            return true;\n        }\n'''
if old not in text:
    raise SystemExit('reload block anchor not found')
text = text.replace(old, new, 1)
cpp_path.write_text(text)

conf = '''###################################################################################################
# NPC Chat LLM - canonical runtime configuration
#
# IMPORTANT:
# - This .conf.dist is the source template. AzerothCore normally runs from a deployed module config
#   under the server runtime config path/modules directory.
# - Do not keep duplicate NpcChat.* keys in the runtime file. One key = one effective value.
# - `.npcc status` shows what ConfigMgr actually loaded.
# - `.npcc reload` now rereads module config files from disk before reloading NPC Chat values.
###################################################################################################

###################################################################################################
# Main live NPC chat model
###################################################################################################
NpcChat.Enable = 1
NpcChat.BaseUrl =
NpcChat.ApiKey =
NpcChat.Model =
NpcChat.MaxResponseTokens = 180
NpcChat.Temperature = 0.85
NpcChat.RequestTimeoutSec = 30
NpcChat.ModelExtraParameters =
NpcChat.Api.VerifyCert = 1
NpcChat.Api.MaxConcurrentRequests = 4

###################################################################################################
# Runtime memory / prompt paths
###################################################################################################
NpcChat.HistoryPath = ./AI_RP/npc_history
NpcChat.HistoryMaxLines = 20
NpcChat.SharedHistoryMaxLines = 12
NpcChat.PersonalHistoryMaxLines = 20
NpcChat.NameByEntry = 1

###################################################################################################
# Direct targeted NPC chat
###################################################################################################
NpcChat.TriggerRange = 25.0
NpcChat.RequirePrefix = 0
NpcChat.Prefix = !
NpcChat.AllowHostileChat = 1
NpcChat.HostileAllowCloseChat = 1
NpcChat.HostileAllowCombatChat = 1
NpcChat.HostileMinDistance = 30.0
NpcChat.HostileMaxDistance = 100.0
NpcChat.HostileForcePrivateReply = 0

###################################################################################################
# Untargeted /say and /yell
###################################################################################################
NpcChat.UntargetedChat.Enable = 1
NpcChat.UntargetedChat.YellRange = 50
NpcChat.UntargetedChat.HostileYell = 1

###################################################################################################
# Optional separate generation model. Blank endpoint/key/model values fall back to live-chat values.
# Old GeneratePrompt* aliases remain accepted by code but are intentionally not duplicated here.
###################################################################################################
NpcChat.Generation.BaseUrl =
NpcChat.Generation.ApiKey =
NpcChat.Generation.Model =
NpcChat.Generation.MaxTokens = 700
NpcChat.Generation.Temperature = 0.75
NpcChat.Generation.RequestTimeoutSec = 60
NpcChat.Generation.ModelExtraParameters =

###################################################################################################
# Trusted non-GM worldbuilder account IDs, comma-separated
###################################################################################################
NpcChat.SubPromptCreatorAccounts =
NpcChat.SubPrompt.ImportOnStartup = 1

###################################################################################################
# Diagnostics
###################################################################################################
NpcChat.BarkDebug = 0
NpcChat.BarkDebugCooldownSec = 15

###################################################################################################
# Cached relationship barks
###################################################################################################
NpcChat.RelationshipBarks.Enabled = 0
NpcChat.RelationshipBarks.RealPlayersOnly = 1
NpcChat.RelationshipBarks.TriggerDistance = 12.0
NpcChat.RelationshipBarks.ChancePct = 8
NpcChat.RelationshipBarks.PlayerCooldownSec = 600
NpcChat.RelationshipBarks.NpcCooldownSec = 300
NpcChat.RelationshipBarks.PairCooldownSec = 900
NpcChat.RelationshipBarks.ScanIntervalMs = 2000
NpcChat.RelationshipBarks.GenerateMissing = 0

###################################################################################################
# History-aware familiar NPC whispers
###################################################################################################
NpcChat.HistoryWhispers.Enabled = 1
NpcChat.HistoryWhispers.TriggerDistance = 18.0
NpcChat.HistoryWhispers.ChancePct = 12
NpcChat.HistoryWhispers.PlayerCooldownSec = 300
NpcChat.HistoryWhispers.PairCooldownSec = 900
NpcChat.HistoryWhispers.ScanIntervalMs = 5000
NpcChat.HistoryWhispers.HistoryMaxLines = 8

###################################################################################################
# Hostile first-talk
# Preserves the most recent testing behavior from the old duplicated config.
###################################################################################################
NpcChat.HostileFirstTalk.Enabled = 1
NpcChat.HostileFirstTalk.RealPlayersOnly = 1
NpcChat.HostileFirstTalk.TriggerDistance = 35.0
NpcChat.HostileFirstTalk.ChancePct = 100
NpcChat.HostileFirstTalk.PlayerCooldownSec = 60
NpcChat.HostileFirstTalk.NpcCooldownSec = 60
NpcChat.HostileFirstTalk.PairCooldownSec = 120
NpcChat.HostileFirstTalk.ScanIntervalMs = 3000
NpcChat.HostileFirstTalk.GenerateMissing = 1
NpcChat.HostileFirstTalk.ElitesOnly = 1
NpcChat.HostileFirstTalk.AllowInCombat = 0

###################################################################################################
# Trainer barks
###################################################################################################
NpcChat.TrainerBarks.Enabled = 0
NpcChat.TrainerBarks.RealPlayersOnly = 1
NpcChat.TrainerBarks.TriggerDistance = 12.0
NpcChat.TrainerBarks.ChancePct = 10
NpcChat.TrainerBarks.PlayerCooldownSec = 600
NpcChat.TrainerBarks.NpcCooldownSec = 300
NpcChat.TrainerBarks.PairCooldownSec = 900
NpcChat.TrainerBarks.ScanIntervalMs = 3000
NpcChat.TrainerBarks.GenerateMissing = 0

###################################################################################################
# Quest / quest-ender barks
###################################################################################################
NpcChat.QuestBarks.Enabled = 0
NpcChat.QuestBarks.SelectedOnly = 1
NpcChat.QuestBarks.RealPlayersOnly = 1
NpcChat.QuestBarks.GenerateMissing = 0
NpcChat.QuestBarks.TriggerDistance = 12.0
NpcChat.QuestBarks.ChancePct = 10
NpcChat.QuestBarks.PlayerCooldownSec = 900
NpcChat.QuestBarks.NpcCooldownSec = 300
NpcChat.QuestBarks.PairCooldownSec = 1800
NpcChat.QuestBarks.ScanIntervalMs = 3000
NpcChat.QuestBarks.MaxQuestsCheckedPerNpc = 8

###################################################################################################
# Playerbot direct chat / character cards
###################################################################################################
NpcChat.Bot.Enable = 1
NpcChat.Bot.CharacterCardsPath = ./characters
NpcChat.Bot.ReplyWhisper = 1
NpcChat.Bot.ReplyPartyRaid = 1
NpcChat.Bot.ReplyTarget = 1
NpcChat.Bot.TriggerRange = 25.0
NpcChat.Bot.HistoryMaxLines = 20

###################################################################################################
# Playerbot social party / raid / guild conversations
###################################################################################################
NpcChat.Bot.Social.Enable = 1
NpcChat.Bot.Social.PartyChancePct = 70
NpcChat.Bot.Social.RaidChancePct = 35
NpcChat.Bot.Social.GuildChancePct = 55
NpcChat.Bot.Social.PartyMaxSpeakers = 3
NpcChat.Bot.Social.RaidMaxSpeakers = 2
NpcChat.Bot.Social.GuildMaxSpeakers = 3
NpcChat.Bot.Social.RandomBotChancePct = 25
NpcChat.Bot.Social.CooldownSec = 20

###################################################################################################
# Keep actual Playerbots guild characters online while a real guild member is online
###################################################################################################
NpcChat.Bot.GuildPresence.Enable = 1
NpcChat.Bot.GuildPresence.ScanIntervalSec = 10
NpcChat.Bot.GuildPresence.MaxLoginsPerScan = 20
NpcChat.Bot.GuildPresence.IncludeAddClass = 1

###################################################################################################
# Playerbot LFG / General
###################################################################################################
NpcChat.Bot.Lfg.Enable = 1
NpcChat.Bot.Lfg.LevelBracket = 8
NpcChat.Bot.Lfg.General = 1
NpcChat.Bot.Lfg.Trade = 0
NpcChat.Bot.General.Chance = 0

###################################################################################################
# Playerbot guild reply window
###################################################################################################
NpcChat.Bot.Guild.Enable = 1
NpcChat.Bot.Guild.MaxTurns = 4
NpcChat.Bot.Guild.WindowSec = 300
'''
Path('conf/mod_npcchat.conf.dist').write_text(conf)

readme_path = Path('README.md')
readme = readme_path.read_text()
marker = '\n## Runtime file layout\n'
section = '''
## Runtime config troubleshooting

If **all** NPC and playerbot LLM chat stops at once, run:

```text
.npcc status
```

This reports the effective `NpcChat.Enable`, API/model presence, how many `NpcChat.*` options ConfigMgr actually loaded, and the runtime config path. If it reports zero loaded keys, the problem is the deployed module config, not NPC selection/history.

After editing the deployed module config, use:

```text
.npcc reload
```

The command now rereads AzerothCore module config files from disk before refreshing NPC Chat values. Older builds only reread values already cached by ConfigMgr, which made a changed file appear to be ignored until restart.

Keep only one copy of each `NpcChat.*` option in the runtime file. The source `conf/mod_npcchat.conf.dist` is now canonical and no longer contains conflicting duplicate QuestBarks/HostileFirstTalk blocks.
'''
if '## Runtime config troubleshooting' not in readme:
    if marker not in readme:
        raise SystemExit('README runtime layout marker not found')
    readme = readme.replace(marker, '\n' + section + marker, 1)
    readme_path.write_text(readme)

# Validate canonical config has no duplicate keys.
lines = Path('conf/mod_npcchat.conf.dist').read_text().splitlines()
keys = []
for line in lines:
    stripped = line.strip()
    if stripped and not stripped.startswith('#') and '=' in stripped:
        keys.append(stripped.split('=', 1)[0].strip())
duplicates = sorted({key for key in keys if keys.count(key) > 1})
if duplicates:
    raise SystemExit('duplicate config keys: ' + ', '.join(duplicates))

cpp = cpp_path.read_text()
for required in [
    'LoadedNpcChatConfigKeys()',
    'LogNpcChatConfigState("startup")',
    'LoadModulesConfigs(true, true)',
    'StartsWithWord(arg, "status", rest)',
]:
    if required not in cpp:
        raise SystemExit('missing expected patch token: ' + required)
