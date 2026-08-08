from pathlib import Path
import re

ROOT = Path('.')
cpp_path = ROOT / 'src/mod_npcchat.cpp'
conf_path = ROOT / 'conf/mod_npcchat.conf.dist'
doc_path = ROOT / 'PLAYERBOT_CHAT.md'

cpp = cpp_path.read_text(encoding='utf-8')
conf = conf_path.read_text(encoding='utf-8')
doc = doc_path.read_text(encoding='utf-8')


def replace_once(text, old, new, label):
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f'{label}: expected exactly one match, found {count}')
    return text.replace(old, new, 1)


def regex_once(text, pattern, repl, label, flags=0):
    out, count = re.subn(pattern, repl, text, count=1, flags=flags)
    if count != 1:
        raise RuntimeError(f'{label}: expected exactly one regex match, found {count}')
    return out


# PlayerbotAIConfig exposes the configured random-bot account list.  That is a
# better offline classifier than IsRandomBot(lowGuid), which only describes the
# current active random-bot set.
cpp = replace_once(
    cpp,
    '#include "Playerbots.h"     // PlayerbotAI, RandomPlayerbotMgr, GET_PLAYERBOT_AI\n',
    '#include "Playerbots.h"     // PlayerbotAI, RandomPlayerbotMgr, GET_PLAYERBOT_AI\n'
    '#include "PlayerbotAIConfig.h" // configured random-bot account classification\n',
    'playerbot config include')

presence_code = r'''

    // --- guild bot presence -----------------------------------------------------
    // Keep actual Playerbots guild characters online while at least one real
    // member of that guild is online.  Eligibility is intentionally based on
    // Playerbots-owned accounts/addclass classification, not merely guild
    // membership, so ordinary offline player alts are never pulled online.
    struct GuildPresenceCfg
    {
        bool   enable = true;
        uint32 scanIntervalSec = 10;
        uint32 maxLoginsPerScan = 20;
        bool   includeAddClass = true;
    };

    inline GuildPresenceCfg GetGuildPresenceCfg()
    {
        GuildPresenceCfg cfg;
        cfg.enable = sConfigMgr->GetOption<bool>("NpcChat.Bot.GuildPresence.Enable", true);
        cfg.scanIntervalSec = std::max<uint32>(1,
            sConfigMgr->GetOption<uint32>("NpcChat.Bot.GuildPresence.ScanIntervalSec", 10));
        cfg.maxLoginsPerScan = sConfigMgr->GetOption<uint32>(
            "NpcChat.Bot.GuildPresence.MaxLoginsPerScan", 20);
        cfg.includeAddClass = sConfigMgr->GetOption<bool>(
            "NpcChat.Bot.GuildPresence.IncludeAddClass", true);
        return cfg;
    }

    inline std::map<uint32, time_t>& GuildPresenceNextScan()
    {
        static std::map<uint32, time_t> nextScanByGuild;
        return nextScanByGuild;
    }

    inline void EnsureGuildBotsOnline(Player* realPlayer)
    {
        if (!IsRealPlayerSession(realPlayer))
            return;

        GuildPresenceCfg const cfg = GetGuildPresenceCfg();
        if (!cfg.enable || !PlayerbotAIConfig::instance().enabled)
            return;

        uint32 const guildId = realPlayer->GetGuildId();
        if (!guildId)
            return;

        time_t const now = std::time(nullptr);
        time_t& nextScan = GuildPresenceNextScan()[guildId];
        if (nextScan > now)
            return;
        nextScan = now + cfg.scanIntervalSec;

        // guild_member alone does not expose account ownership.  Joining the
        // character row lets us distinguish configured Playerbots accounts
        // from normal player accounts while the character is offline.
        QueryResult result = CharacterDatabase.Query(
            "SELECT gm.`guid`, c.`account` "
            "FROM `guild_member` gm "
            "INNER JOIN `characters` c ON c.`guid` = gm.`guid` "
            "WHERE gm.`guildid` = {} ORDER BY gm.`guid`",
            guildId);
        if (!result)
            return;

        RandomPlayerbotMgr& randomMgr = RandomPlayerbotMgr::instance();
        uint32 requested = 0;

        do
        {
            Field* fields = result->Fetch();
            ObjectGuid::LowType const lowGuid = fields[0].Get<uint32>();
            uint32 const accountId = fields[1].Get<uint32>();
            if (!lowGuid || lowGuid == realPlayer->GetGUID().GetCounter())
                continue;

            ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(lowGuid);

            // Connected includes characters still loading into the world.  Do
            // not duplicate a login request just because IsInWorld is not set yet.
            if (ObjectAccessor::FindConnectedPlayer(guid))
                continue;

            bool eligible = PlayerbotAIConfig::instance().IsInRandomAccountList(accountId);
            if (!eligible && cfg.includeAddClass)
                eligible = randomMgr.IsAddclassBot(lowGuid);
            if (!eligible)
                continue;

            // masterAccountId=0 routes the login through RandomPlayerbotMgr.
            // These remain autonomous guild characters rather than becoming
            // personal followers of whichever real guild member triggered us.
            randomMgr.AddPlayerBot(guid, 0);
            ++requested;

            if (cfg.maxLoginsPerScan && requested >= cfg.maxLoginsPerScan)
                break;
        } while (result->NextRow());

        if (requested)
        {
            LOG_INFO("module",
                "[NpcChat] GuildPresence guild {} requested {} offline playerbot login(s) because real member {} is online.",
                guildId, requested, realPlayer->GetName());
        }
    }
'''

# Insert immediately after GetBotCfg(), before the race/class helper section.
pattern = (
    r'(    inline BotCfg GetBotCfg\(\)\n'
    r'    \{.*?'
    r'        return cfg;\n'
    r'    \}\n)'
    r'(\n    // --- race / class / gender text)'
)
cpp = regex_once(cpp, pattern, r'\1' + presence_code + r'\2', 'guild presence helper', re.S)

old_update = '''    void OnPlayerAfterUpdate(Player* player, uint32 diff) override
    {
        if (!g_Enable || !player || !player->IsInWorld() || !player->IsAlive())
            return;

        WorldSession* session = player->GetSession();
        if (!session || session->IsBot())
            return;

        ProcessCachedBarksForPlayer(player, diff);
    }
'''
new_update = '''    void OnPlayerAfterUpdate(Player* player, uint32 diff) override
    {
        if (!g_Enable || !player || !player->IsInWorld())
            return;

        WorldSession* session = player->GetSession();
        if (!session || session->IsBot())
            return;

        // Guild presence is independent of whether the real player is currently
        // alive; dying/ghosting should not make their guild roster disappear.
        EnsureGuildBotsOnline(player);

        if (!player->IsAlive())
            return;

        ProcessCachedBarksForPlayer(player, diff);
    }
'''
cpp = replace_once(cpp, old_update, new_update, 'real-player update hook')

presence_conf = '''
###################################################################################################
# PLAYERBOT GUILD PRESENCE - keep guild bots online while a real guild member is online
#
# This is deliberately stricter than "every offline guild character": only characters belonging
# to configured Playerbots random-bot accounts (plus optional addclass bots) are eligible. Normal
# human alts in the guild are ignored.
###################################################################################################

# Master switch. Default: 1
NpcChat.Bot.GuildPresence.Enable = 1

# Per-guild rescan interval while at least one real guild member is online. Default: 10 seconds.
NpcChat.Bot.GuildPresence.ScanIntervalSec = 10

# Maximum offline guild bots requested per scan. Repeated scans continue until all eligible bots
# are online. 0 = unlimited; a finite value avoids a login storm in very large guilds. Default: 20.
NpcChat.Bot.GuildPresence.MaxLoginsPerScan = 20

# Also allow Playerbots addclass characters to be treated as guild bots. Default: 1
NpcChat.Bot.GuildPresence.IncludeAddClass = 1

'''
conf_marker = '''
###################################################################################################
# PLAYERBOT LFG
###################################################################################################
'''
conf = replace_once(conf, conf_marker, '\n' + presence_conf + conf_marker, 'guild presence config block')

presence_doc = '''
## Keeping guild bots online

`NpcChat.Bot.GuildPresence.Enable = 1` makes guild presence follow the **real players** in that guild. While at least one real guild member is online, `npc_chat_llm` periodically scans the guild roster and asks Playerbots to log in eligible offline guild bots.

This does **not** mean "log in every offline character in the guild." The module checks the character's account against Playerbots' configured random-bot account list, and can optionally include Playerbots addclass characters. Ordinary human alts are ignored.

```ini
NpcChat.Bot.GuildPresence.Enable = 1
NpcChat.Bot.GuildPresence.ScanIntervalSec = 10
NpcChat.Bot.GuildPresence.MaxLoginsPerScan = 20
NpcChat.Bot.GuildPresence.IncludeAddClass = 1
```

The login is routed through `RandomPlayerbotMgr` with no real-player master account, so these characters remain autonomous guild members rather than becoming followers of the player who happened to trigger the scan. A per-guild cooldown prevents the player update hook from querying the roster every frame, and the login cap spreads very large guilds across multiple scans.

The module does **not** force guild bots to log out when the last real guild member leaves. Playerbots keeps ownership of the normal bot lifecycle. If Playerbots later logs one of these bots out while a real guild member is still online, a later guild-presence scan can request it again.

'''
doc_marker = '## Quick test checklist\n'
doc = replace_once(doc, doc_marker, presence_doc + doc_marker, 'guild presence documentation')

# Extend the checklist without renumbering every existing item manually.
doc = replace_once(
    doc,
    '8. Send a guild message with several online guild bots and confirm a small multi-bot conversation can occur.\n',
    '8. Log in a real guild member with several eligible guild bots offline; within the configured scans, confirm those bot characters appear online in the guild roster.\n'
    '9. Confirm an ordinary non-bot alt in the same guild remains offline.\n'
    '10. Send a guild message after the bots have logged in and confirm a small multi-bot conversation can occur.\n',
    'guild presence quick tests')
# Existing 9/10 are now duplicate numbers; make their wording unambiguous and renumber them.
doc = replace_once(doc, '9. Name one specific bot and confirm it is favored as a speaker.\n',
                   '11. Name one specific bot and confirm it is favored as a speaker.\n', 'renumber named-bot test')
doc = replace_once(doc, '10. Confirm recent bot history remains under `NpcChat.HistoryPath/bots/personal/` and no GUID-based `.card` files are created.\n',
                   '12. Confirm recent bot history remains under `NpcChat.HistoryPath/bots/personal/` and no GUID-based `.card` files are created.\n', 'renumber history test')

cpp_path.write_text(cpp, encoding='utf-8')
conf_path.write_text(conf, encoding='utf-8')
doc_path.write_text(doc, encoding='utf-8')
print('guild bot presence patch applied')
