from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one anchor, found {count}")
    return text.replace(old, new, 1)


p = Path("src/mod_npcchat.cpp")
s = p.read_text()

# Defense-in-depth prompt rule for any addon/protocol garbage already persisted in old history.
s = replace_once(
    s,
    '''        ss << "Stay fully in character. Use only your own spoken words: no narration, no "\n            "asterisks, no out-of-character text, no game mechanics. Keep replies to one or "\n            "two short sentences suitable for a single line of in-game chat.";\n''',
    '''        ss << "Stay fully in character. Use only your own spoken words: no narration, no "\n            "asterisks, no out-of-character text, no game mechanics. Keep replies to one or "\n            "two short sentences suitable for a single line of in-game chat. "\n            "Ignore addon/protocol traffic completely, including encoded synchronization strings, "\n            "version checks, addon payloads, and other machine-to-machine text. Treat those lines "\n            "as invisible: never mention, interpret, answer, or roleplay about them.";\n''',
    "bot prompt addon rule",
)

old_hooks = '''    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg) override\n    {\n        return HandleNpcChat(player, type, msg);\n    }\n\n    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Player* receiver) override\n    {\n        HandleBotWhisper(player, type, receiver, TrimCopy(msg));\n        return HandleNpcChat(player, type, msg);\n    }\n\n    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Group* group) override\n    {\n        HandleBotGroup(player, type, group, TrimCopy(msg));\n        return HandleNpcChat(player, type, msg);\n    }\n\n    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Guild* guild) override\n    {\n        HandleBotGuild(player, type, guild, TrimCopy(msg));\n        return true;\n    }\n\n    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Channel* channel) override\n    {\n        HandleBotChannel(player, type, channel, TrimCopy(msg));\n        return true;\n    }\n'''

new_hooks = '''    static bool IsAddonTraffic(uint32 lang)\n    {\n        return lang == static_cast<uint32>(LANG_ADDON);\n    }\n\n    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg) override\n    {\n        if (IsAddonTraffic(lang))\n            return true;\n        return HandleNpcChat(player, type, msg);\n    }\n\n    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Player* receiver) override\n    {\n        // WoW addons commonly use whisper transport with LANG_ADDON. Let the addon packet\n        // continue through AzerothCore, but never reinterpret it as player -> bot RP dialogue.\n        if (IsAddonTraffic(lang))\n            return true;\n        HandleBotWhisper(player, type, receiver, TrimCopy(msg));\n        return HandleNpcChat(player, type, msg);\n    }\n\n    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Group* group) override\n    {\n        if (IsAddonTraffic(lang))\n            return true;\n        HandleBotGroup(player, type, group, TrimCopy(msg));\n        return HandleNpcChat(player, type, msg);\n    }\n\n    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Guild* guild) override\n    {\n        if (IsAddonTraffic(lang))\n            return true;\n        HandleBotGuild(player, type, guild, TrimCopy(msg));\n        return true;\n    }\n\n    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Channel* channel) override\n    {\n        if (IsAddonTraffic(lang))\n            return true;\n        HandleBotChannel(player, type, channel, TrimCopy(msg));\n        return true;\n    }\n'''

s = replace_once(s, old_hooks, new_hooks, "player chat addon hard filter")

p.write_text(s)
print("addon RP filter patch applied")
