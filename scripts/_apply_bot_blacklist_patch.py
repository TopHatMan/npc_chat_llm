from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one anchor, found {count}")
    return text.replace(old, new, 1)


cpp_path = Path("src/mod_npcchat.cpp")
cpp = cpp_path.read_text()

blacklist = (
    "t ,c ,r ,items,autogear,talents,reset botAI,summon,los,release,revive,leave,attack,follow,flee,stay,"
    "runaway,grind,disperse,give leader,spells,cast ,quests,accept,drop,talk,reset,ss ,trainer,rti,rtsc,do ,"
    "ll ,e ,ue ,nc ,open,destroy,s ,b ,bank,gb ,u ,co ,ELVUI_VERSIONCHK,Asked,DPSMate_,LibGroupTalents,BLT,"
    "oRA3,Skada,HealBot,hbComms,questie,pfQuest,DBMv4-Ver,BWVQ3,add,remove,reset ai,report,state,help,log,stats,"
    "tank,offtank,healer,cc ,damage,boost,passive,defensive,aggressive,stay,guard,free,follow,assist,pet,stance,"
    "formation,rpg,emote,cheer,applaud,drink,eat,dance,attackers,reset instances,home,zone,who ,who,pos ,tele,"
    "grind,loot,quest,trainer,travel,teleport,homebind,unfollow,invite,uninvite,join,leave,leader,ready,release,"
    "save,update,reset talents,gear,trade,mail,ah ,ahscan,ahbid,ahbuy,ahsell,ahcancel,bag,repair,vendor,train,"
    "spells,reset spells,learn,unlearn,cast,uncast,use ,move,go ,look,stop,turn,face,wait,party,followleader,"
    "stayleader,moveleader,info,distance,debug,reset path,reset state,reset all,reset dungeon,reset raid,zone info,"
    "LHC40,RECOUNT,GTFO_v,Altoholic,DS_,Crb,Crb ,maintenance ,DataStore"
)

cpp = replace_once(
    cpp,
    '''    struct BotCfg\n    {\n        bool        enable = false;\n        bool        replyWhisper = true;\n        bool        replyPartyRaid = true;\n        bool        replyTarget = true;\n        float       triggerRange = 25.0f;\n        int         historyTail = 20;\n        std::string characterCardsPath = "./characters";\n    };\n''',
    f'''    inline std::string const& DefaultBotRpBlacklist()\n    {{\n        static std::string const value =\n            "{blacklist}";\n        return value;\n    }}\n\n    struct BotCfg\n    {{\n        bool        enable = false;\n        bool        replyWhisper = true;\n        bool        replyPartyRaid = true;\n        bool        replyTarget = true;\n        float       triggerRange = 25.0f;\n        int         historyTail = 20;\n        std::string characterCardsPath = "./characters";\n        std::string blacklist = DefaultBotRpBlacklist();\n    }};\n''',
    "BotCfg blacklist field",
)

cpp = replace_once(
    cpp,
    '''        cfg.characterCardsPath = sConfigMgr->GetOption<std::string>(\n            "NpcChat.Bot.CharacterCardsPath", "./characters", false);\n        return cfg;\n    }\n''',
    '''        cfg.characterCardsPath = sConfigMgr->GetOption<std::string>(\n            "NpcChat.Bot.CharacterCardsPath", "./characters", false);\n        cfg.blacklist = sConfigMgr->GetOption<std::string>(\n            "NpcChat.Bot.Blacklist", DefaultBotRpBlacklist(), false);\n        return cfg;\n    }\n\n    // PBC-compatible prefix blacklist. Leading formatting whitespace after a comma is ignored,\n    // but trailing literal spaces are preserved because `t ` and `t` intentionally mean\n    // different things in the Playerbots command vocabulary. Matching is case-insensitive.\n    inline bool IsBotRpBlacklisted(std::string const& text, std::string const& rawBlacklist)\n    {\n        if (text.empty() || rawBlacklist.empty())\n            return false;\n\n        std::string const hay = ToLowerCopy(text);\n        size_t start = 0;\n        while (start <= rawBlacklist.size())\n        {\n            size_t end = rawBlacklist.find(',', start);\n            std::string prefix = rawBlacklist.substr(\n                start, end == std::string::npos ? std::string::npos : end - start);\n\n            while (!prefix.empty() && (prefix.front() == ' ' || prefix.front() == '\\t'))\n                prefix.erase(prefix.begin());\n            while (!prefix.empty() && (prefix.back() == '\\r' || prefix.back() == '\\n' || prefix.back() == '\\t'))\n                prefix.pop_back();\n\n            if (!prefix.empty())\n            {\n                std::string const needle = ToLowerCopy(prefix);\n                if (hay.rfind(needle, 0) == 0)\n                    return true;\n            }\n\n            if (end == std::string::npos)\n                break;\n            start = end + 1;\n        }\n        return false;\n    }\n''',
    "GetBotCfg blacklist load",
)

cpp = replace_once(
    cpp,
    '''        if (!IsGenuineBot(receiver)) return false;         // only bot receivers\n        if (text.empty() || text[0] == '.') return false;\n        DispatchBot(player, receiver, BotChannel::Whisper, text);\n''',
    '''        if (!IsGenuineBot(receiver)) return false;         // only bot receivers\n        if (text.empty() || text[0] == '.') return false;\n        if (IsBotRpBlacklisted(text, cfg.blacklist)) return true;\n        DispatchBot(player, receiver, BotChannel::Whisper, text);\n''',
    "whisper blacklist",
)

cpp = replace_once(
    cpp,
    '''        if (!IsRealPlayerSession(player)) return;\n        if (text.empty() || text[0] == '.') return;\n\n        BotSocialChannel socialChannel;\n''',
    '''        if (!IsRealPlayerSession(player)) return;\n        if (text.empty() || text[0] == '.') return;\n        if (IsBotRpBlacklisted(text, cfg.blacklist)) return;\n\n        BotSocialChannel socialChannel;\n''',
    "group blacklist",
)

cpp = replace_once(
    cpp,
    '''        if (!bot->IsAlive() || !player->IsWithinDist(bot, cfg.triggerRange, true)) return false;\n        if (text.empty() || text[0] == '.') return false;\n        DispatchBot(player, bot, BotChannel::Say, text);\n''',
    '''        if (!bot->IsAlive() || !player->IsWithinDist(bot, cfg.triggerRange, true)) return false;\n        if (text.empty() || text[0] == '.') return false;\n        if (IsBotRpBlacklisted(text, cfg.blacklist)) return true;\n        DispatchBot(player, bot, BotChannel::Say, text);\n''',
    "target say blacklist",
)

cpp = replace_once(
    cpp,
    '''        if (!GetBotCfg().enable || !GetSurfaceCfg().guildEnable || !guild) return;\n        if (!IsRealPlayerSession(player)) return;\n        if (text.empty() || text[0] == '.') return;\n\n        BotSurfaceCfg const cfg = GetSurfaceCfg();\n''',
    '''        BotCfg const botCfg = GetBotCfg();\n        if (!botCfg.enable || !GetSurfaceCfg().guildEnable || !guild) return;\n        if (!IsRealPlayerSession(player)) return;\n        if (text.empty() || text[0] == '.') return;\n        if (IsBotRpBlacklisted(text, botCfg.blacklist)) return;\n\n        BotSurfaceCfg const cfg = GetSurfaceCfg();\n''',
    "guild blacklist",
)

cpp = replace_once(
    cpp,
    '''        if (!GetBotCfg().enable || !channel) return;\n        if (!IsRealPlayerSession(player)) return;\n        if (text.empty() || text[0] == '.') return;\n\n        BotSurfaceCfg const cfg = GetSurfaceCfg();\n''',
    '''        BotCfg const botCfg = GetBotCfg();\n        if (!botCfg.enable || !channel) return;\n        if (!IsRealPlayerSession(player)) return;\n        if (text.empty() || text[0] == '.') return;\n        if (IsBotRpBlacklisted(text, botCfg.blacklist)) return;\n\n        BotSurfaceCfg const cfg = GetSurfaceCfg();\n''',
    "channel blacklist",
)

cpp_path.write_text(cpp)

conf_path = Path("conf/mod_npcchat.conf.dist")
conf = conf_path.read_text()
anchor = '''NpcChat.Bot.HistoryMaxLines = 20\n'''
replacement = f'''NpcChat.Bot.HistoryMaxLines = 20\n# PBC-style comma-separated prefixes ignored by playerbot RP. Prefix matching is case-insensitive.\n# Trailing spaces are meaningful: `t ` is narrower than `t`. These messages never reach RP history/LLM.\nNpcChat.Bot.Blacklist = {blacklist}\n'''
if conf.count(anchor) != 1:
    raise SystemExit(f"config blacklist anchor count={conf.count(anchor)}")
conf = conf.replace(anchor, replacement, 1)
conf_path.write_text(conf)

print("playerbot RP blacklist patch applied")
