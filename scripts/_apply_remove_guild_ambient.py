from pathlib import Path

CPP = Path('src/mod_npcchat.cpp')
CONF = Path('conf/mod_npcchat.conf.dist')
DOC = Path('PLAYERBOT_CHAT.md')
VALIDATOR = Path('scripts/validate_config_contract.py')


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f'{label}: expected exactly 1 match, found {count}')
    return text.replace(old, new, 1)


cpp = CPP.read_text(encoding='utf-8')
cpp = replace_once(
    cpp,
    '        EnsureGuildBotsOnline(player);\n        MaybeStartGuildAmbientChat(player);\n',
    '        EnsureGuildBotsOnline(player);\n        // Guild LLM traffic is player-driven only. Do not schedule autonomous bot-to-bot\n        // guild conversations from the player update loop; reactive guild chat is dispatched\n        // only when a real player actually sends a guild message.\n',
    'remove autonomous guild update trigger',
)
cpp = replace_once(
    cpp,
    '    struct GuildAmbientCfg\n    {\n        bool enable = true;\n',
    '    struct GuildAmbientCfg\n    {\n        // Legacy implementation retained for now, but intentionally unscheduled.\n        // Keep disabled by default in case it is ever called explicitly during debugging.\n        bool enable = false;\n',
    'disable legacy ambient default',
)
cpp = replace_once(
    cpp,
    '        c.enable = sConfigMgr->GetOption<bool>("NpcChat.Bot.GuildAmbient.Enable", true, false);\n',
    '        c.enable = sConfigMgr->GetOption<bool>("NpcChat.Bot.GuildAmbient.Enable", false, false);\n',
    'disable legacy ambient config fallback',
)
CPP.write_text(cpp, encoding='utf-8')

conf = CONF.read_text(encoding='utf-8')
conf = replace_once(
    conf,
    '###################################################################################################\n# Autonomous low-frequency guild chatter while a real guild member is online\n# This is direct guild broadcast, so generated lines do not recurse through chat hooks.\n###################################################################################################\nNpcChat.Bot.GuildAmbient.Enable = 1\n',
    '###################################################################################################\n# Legacy autonomous guild chatter - DISABLED\n# Guild LLM traffic is intentionally player-driven only. Presence may keep bots online, but bots\n# do not spend API credits talking to each other while the real player is idle. Reactive guild\n# replies are handled by NpcChat.Bot.Social when a real player actually sends guild chat.\n###################################################################################################\nNpcChat.Bot.GuildAmbient.Enable = 0\n',
    'disable canonical guild ambient config',
)
CONF.write_text(conf, encoding='utf-8')

doc = DOC.read_text(encoding='utf-8')
doc = replace_once(
    doc,
    'Named bot messages bypass the ambient chance roll. Ambient conversations use the chance and cooldown settings.\n',
    'Named bot messages bypass the chance roll. Party/raid/guild social generation is player-driven: no autonomous guild bot-to-bot LLM loop is scheduled.\n',
    'document player-driven social behavior',
)
insert_after = 'The module does **not** force guild bots to log out when the last real guild member leaves. Playerbots keeps ownership of the normal bot lifecycle. If Playerbots later logs one of these bots out while a real guild member is still online, a later guild-presence scan can request it again.\n'
addition = '\n### Guild token-safety rule\n\nKeeping guild bots online is separate from generating guild dialogue. `npc_chat_llm` does **not** start autonomous guild bot-to-bot LLM conversations from the player update loop. Guild LLM replies happen only after a real player sends guild chat through the reactive social path. This means an idle logged-in player does not cause periodic guild API spend, and once no real player is online there is no guild LLM trigger at all.\n'
if addition.strip() not in doc:
    doc = replace_once(doc, insert_after, insert_after + addition, 'add token-safety documentation')
DOC.write_text(doc, encoding='utf-8')

validator = VALIDATOR.read_text(encoding='utf-8')
anchor = '    if stale_config:\n        failed = True\n        print("ERROR: canonical config contains options no longer read by source:")\n        for key in stale_config:\n            print(f"  - {key}")\n\n'
addition = '''    # Token-safety invariant: the legacy ambient implementation may remain for reference, but\n    # it must never be scheduled from the player update loop. Guild LLM work is reactive to\n    # actual real-player guild chat only.\n    ambient_calls = source.count("MaybeStartGuildAmbientChat(")\n    if ambient_calls != 1:\n        failed = True\n        print(\n            "ERROR: autonomous guild ambience was re-enabled or duplicated; "\n            f"expected only the dormant function definition, found {ambient_calls} references."\n        )\n\n    if "NpcChat.Bot.GuildAmbient.Enable = 0" not in conf:\n        failed = True\n        print("ERROR: canonical GuildAmbient setting must remain disabled (0).")\n\n'''
validator = replace_once(validator, anchor, anchor + addition, 'add token-safety validator')
VALIDATOR.write_text(validator, encoding='utf-8')
