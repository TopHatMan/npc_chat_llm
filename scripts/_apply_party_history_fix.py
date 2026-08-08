from pathlib import Path

CPP = Path('src/mod_npcchat.cpp')
CONF = Path('conf/mod_npcchat.conf.dist')
DOC = Path('PLAYERBOT_CHAT.md')


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f'{label}: expected 1 anchor, found {count}')
    return text.replace(old, new, 1)


cpp = CPP.read_text()

# 1) Repair old bot RP history before it is ever sent back to the model.
anchor = '''        return false;\n    }\n\n\n    // --- guild bot presence -----------------------------------------------------\n'''
insert = '''        return false;\n    }\n\n    struct BotHistoryScrubStats\n    {\n        size_t filesScanned = 0;\n        size_t linesRemoved = 0;\n    };\n\n    inline std::string BotHistoryPayload(std::string const& line)\n    {\n        size_t const colon = line.find(':');\n        if (colon == std::string::npos)\n            return TrimCopy(line);\n        return TrimCopy(line.substr(colon + 1));\n    }\n\n    inline bool LooksLikeAddonProtocolHistory(std::string const& text)\n    {\n        if (text.empty())\n            return false;\n\n        std::string const low = ToLowerCopy(text);\n        static char const* markers[] =\n        {\n            "elvui_versionchk", "dpsmate_", "libgrouptalents", "ora3", "skada",\n            "healbot", "hbcomms", "questie", "pfquest", "dbmv4-ver", "bwvq3",\n            "lhc40", "recount", "gtfo_v", "altoholic", "datastore", "ds_", "crb"\n        };\n        for (char const* marker : markers)\n            if (low.find(marker) != std::string::npos)\n                return true;\n\n        // Old model replies sometimes literally discussed the leaked packet. Those lines are just\n        // as poisonous to future RP as the original payload, so remove obvious protocol commentary.\n        bool const mentionsAddon = low.find("addon") != std::string::npos;\n        bool const mentionsProtocol = low.find("protocol") != std::string::npos ||\n            low.find("version check") != std::string::npos ||\n            low.find("sync message") != std::string::npos ||\n            low.find("synchronization") != std::string::npos;\n        if (mentionsAddon && mentionsProtocol)\n            return true;\n\n        for (unsigned char c : text)\n            if (c < 0x20 && c != '\\t')\n                return true;\n        return false;\n    }\n\n    inline bool IsBotHistoryNoiseLine(std::string const& line, std::string const& blacklist)\n    {\n        std::string const payload = BotHistoryPayload(line);\n        return IsBotRpBlacklisted(payload, blacklist) || LooksLikeAddonProtocolHistory(payload);\n    }\n\n    // Caller holds g_FileMutex. The entire file is rewritten only when junk is found; the returned\n    // deque is still limited to the configured prompt tail. This permanently repairs contaminated\n    // histories lazily as each bot is spoken to.\n    inline std::deque<std::string> ScrubAndLoadBotHistoryTail(std::string const& path, int maxLines,\n        std::string const& blacklist, size_t* removedOut = nullptr)\n    {\n        std::ifstream f(path);\n        if (!f.is_open())\n        {\n            if (removedOut) *removedOut = 0;\n            return {};\n        }\n\n        std::vector<std::string> kept;\n        size_t removed = 0;\n        std::string line;\n        while (std::getline(f, line))\n        {\n            line = TrimCopy(line);\n            if (line.empty())\n                continue;\n            if (IsBotHistoryNoiseLine(line, blacklist))\n            {\n                ++removed;\n                continue;\n            }\n            kept.push_back(line);\n        }\n        f.close();\n\n        if (removed)\n        {\n            std::ostringstream clean;\n            for (std::string const& keptLine : kept)\n                clean << keptLine << "\\n";\n            WriteWholeTextFile(path, TrimCopy(clean.str()), true);\n            LOG_INFO("module", "[NpcChat] Scrubbed {} addon/Playerbots command line(s) from bot RP history {}.",\n                removed, path);\n        }\n\n        if (removedOut)\n            *removedOut = removed;\n\n        std::deque<std::string> tail;\n        if (maxLines <= 0)\n            return tail;\n        size_t const start = kept.size() > static_cast<size_t>(maxLines) ?\n            kept.size() - static_cast<size_t>(maxLines) : 0;\n        for (size_t i = start; i < kept.size(); ++i)\n            tail.push_back(kept[i]);\n        return tail;\n    }\n\n    // Caller holds g_FileMutex. Used by `.npcc bot history scrub` for an immediate one-shot cleanup.\n    inline BotHistoryScrubStats ScrubAllBotPersonalHistory(std::string const& blacklist)\n    {\n        BotHistoryScrubStats stats;\n        std::filesystem::path const root = std::filesystem::path(g_HistoryPath) / "bots" / "personal";\n        try\n        {\n            if (!std::filesystem::exists(root))\n                return stats;\n            for (auto const& entry : std::filesystem::recursive_directory_iterator(root))\n            {\n                if (!entry.is_regular_file() || entry.path().extension() != ".history")\n                    continue;\n                ++stats.filesScanned;\n                size_t removed = 0;\n                (void)ScrubAndLoadBotHistoryTail(entry.path().string(), 0, blacklist, &removed);\n                stats.linesRemoved += removed;\n            }\n        }\n        catch (std::exception const& e)\n        {\n            LOG_ERROR("module", "[NpcChat] Bot history scrub stopped early: {}", e.what());\n        }\n        return stats;\n    }\n\n\n    // --- guild bot presence -----------------------------------------------------\n'''
cpp = replace_once(cpp, anchor, insert, 'history scrub helper insertion')

# All three personal-bot history readers should now scrub legacy junk before prompt construction.
old_load = '            history = LoadHistoryTail(historyPath, cfg.historyTail);\n'
count = cpp.count(old_load)
if count != 3:
    raise SystemExit(f'bot history load sites: expected 3, found {count}')
cpp = cpp.replace(old_load, '            history = ScrubAndLoadBotHistoryTail(historyPath, cfg.historyTail, cfg.blacklist);\n')

# 2) Party chat should feel responsive: 100% trigger, short party-specific cooldown.
cpp = replace_once(cpp,
'''        uint32 partyChancePct = 70;\n        uint32 raidChancePct = 35;\n''',
'''        uint32 partyChancePct = 100;\n        uint32 raidChancePct = 35;\n''', 'party chance struct default')
cpp = replace_once(cpp,
'''        uint32 randomBotChancePct = 25;\n        int cooldownSec = 20;\n''',
'''        uint32 randomBotChancePct = 25;\n        int partyCooldownSec = 4;\n        int cooldownSec = 20;\n''', 'party cooldown struct')
cpp = replace_once(cpp,
'''        c.partyChancePct = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("NpcChat.Bot.Social.PartyChancePct", 70, false));\n''',
'''        c.partyChancePct = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("NpcChat.Bot.Social.PartyChancePct", 100, false));\n''', 'party chance config default')
cpp = replace_once(cpp,
'''        c.randomBotChancePct = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("NpcChat.Bot.Social.RandomBotChancePct", 25, false));\n        c.cooldownSec = std::max(0, sConfigMgr->GetOption<int32>("NpcChat.Bot.Social.CooldownSec", 20, false));\n''',
'''        c.randomBotChancePct = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("NpcChat.Bot.Social.RandomBotChancePct", 25, false));\n        c.partyCooldownSec = std::max(0, sConfigMgr->GetOption<int32>("NpcChat.Bot.Social.PartyCooldownSec", 4, false));\n        c.cooldownSec = std::max(0, sConfigMgr->GetOption<int32>("NpcChat.Bot.Social.CooldownSec", 20, false));\n''', 'party cooldown config load')

# First selected party companion must answer. Later party/raid speakers may still opt out.
cpp = replace_once(cpp,
'''        for (BotChatRequest turn : req.turns)\n        {\n''',
'''        for (size_t turnIndex = 0; turnIndex < req.turns.size(); ++turnIndex)\n        {\n            BotChatRequest turn = req.turns[turnIndex];\n''', 'social worker indexed loop')
cpp = replace_once(cpp,
'''            if (req.channel == BotSocialChannel::Guild)\n                user << ". You were selected to participate in guild chat, so give a natural in-character reply; do not output [SKIP].";\n            else\n                user << ". If you truly have nothing to add, output exactly [SKIP].";\n''',
'''            if (req.channel == BotSocialChannel::Guild)\n                user << ". You were selected to participate in guild chat, so give a natural in-character reply; do not output [SKIP].";\n            else if (req.channel == BotSocialChannel::Party && turnIndex == 0)\n                user << ". You are the first selected party companion. Give one natural in-character reply so the player's party message is acknowledged; do not output [SKIP].";\n            else\n                user << ". If you truly have nothing to add, output exactly [SKIP].";\n''', 'party first reply requirement')

# Party gets a short dedicated cooldown instead of the 20-second raid/guild ambient cooldown.
cpp = replace_once(cpp,
'''            std::string cooldownKey = "group:" + std::to_string(group->GetGUID().GetRawValue());\n            if (!BotSocialAmbientAllowed(cooldownKey, social.cooldownSec))\n                return;\n''',
'''            std::string cooldownKey = "group:" + std::to_string(group->GetGUID().GetRawValue());\n            int const cooldownSec = socialChannel == BotSocialChannel::Party ?\n                social.partyCooldownSec : social.cooldownSec;\n            if (!BotSocialAmbientAllowed(cooldownKey, cooldownSec))\n                return;\n''', 'party cooldown use')

# 3) Explicit command for immediate cleanup of every bot personal-history file.
cpp = replace_once(cpp,
'''            handler->PSendSysMessage(".npcc health [test|reset]");\n            handler->PSendSysMessage(".npcc reload");\n''',
'''            handler->PSendSysMessage(".npcc health [test|reset]");\n            handler->PSendSysMessage(".npcc bot history scrub");\n            handler->PSendSysMessage(".npcc reload");\n''', 'help command line')
cpp = replace_once(cpp,
'''        if (StartsWithWord(arg, "key", rest) || StartsWithWord(arg, "paths", rest))\n        {\n''',
'''        if (StartsWithWord(arg, "bot", rest))\n        {\n            std::string historyRest;\n            if (StartsWithWord(rest, "history", historyRest))\n            {\n                std::string actionRest;\n                if (StartsWithWord(historyRest, "scrub", actionRest))\n                {\n                    BotCfg const cfg = GetBotCfg();\n                    BotHistoryScrubStats stats;\n                    {\n                        std::lock_guard<std::mutex> lock(g_FileMutex);\n                        stats = ScrubAllBotPersonalHistory(cfg.blacklist);\n                    }\n                    handler->PSendSysMessage("Bot RP history scrub complete: {} file(s) scanned, {} addon/Playerbots command line(s) removed.",\n                        stats.filesScanned, stats.linesRemoved);\n                    return true;\n                }\n            }\n            handler->PSendSysMessage("Usage: .npcc bot history scrub");\n            return true;\n        }\n\n        if (StartsWithWord(arg, "key", rest) || StartsWithWord(arg, "paths", rest))\n        {\n''', 'bot history scrub command')

CPP.write_text(cpp)

# Canonical config: party is responsive but still bounded by one short cooldown window.
conf = CONF.read_text()
conf = replace_once(conf,
'''NpcChat.Bot.Social.PartyChancePct = 70\n''',
'''NpcChat.Bot.Social.PartyChancePct = 100\n''', 'config party chance')
conf = replace_once(conf,
'''NpcChat.Bot.Social.RandomBotChancePct = 25\nNpcChat.Bot.Social.CooldownSec = 20\n''',
'''NpcChat.Bot.Social.RandomBotChancePct = 25\n# Party is direct companion conversation, so it uses a short cooldown. CooldownSec remains raid/guild ambient throttle.\nNpcChat.Bot.Social.PartyCooldownSec = 4\nNpcChat.Bot.Social.CooldownSec = 20\n''', 'config party cooldown')
CONF.write_text(conf)

# Documentation updates.
doc = DOC.read_text()
doc = doc.replace('NpcChat.Bot.Social.PartyChancePct = 70', 'NpcChat.Bot.Social.PartyChancePct = 100')
doc = doc.replace('NpcChat.Bot.Social.RandomBotChancePct = 25\nNpcChat.Bot.Social.CooldownSec = 20',
                  'NpcChat.Bot.Social.RandomBotChancePct = 25\nNpcChat.Bot.Social.PartyCooldownSec = 4\nNpcChat.Bot.Social.CooldownSec = 20')
append = '''\n\n## Repairing old addon/command history\n\nOlder builds could accidentally save addon packets and Playerbots command text into personal bot RP history.\nCurrent builds hard-filter new traffic and also scrub each personal bot history file before loading it into an LLM prompt.\nIf junk is found, the cleaned history is rewritten to disk automatically.\n\nTo repair every existing personal bot history immediately:\n\n```text\n.npcc bot history scrub\n```\n\nThe command recursively scans `NpcChat.HistoryPath/bots/personal/` and removes lines whose payload matches the configured `NpcChat.Bot.Blacklist`, known addon protocol identifiers, control-byte payloads, or obvious old AI commentary about leaked addon protocol traffic.\n\n## Party responsiveness\n\nParty chat is intentionally more responsive than raid/guild ambient chatter. By default a real player's eligible party message triggers the bot conversation path at 100%, uses a short 4-second party cooldown, and the first selected companion must produce a real reply instead of `[SKIP]`. Additional selected companions may still stay quiet. Bot-control/addon messages remain filtered before this logic.\n'''
if '## Repairing old addon/command history' not in doc:
    doc = doc.rstrip() + append + '\n'
DOC.write_text(doc)

print('party/history repair patch applied')
