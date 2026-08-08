from pathlib import Path

CPP = Path('src/mod_npcchat.cpp')
DOC = Path('PLAYERBOT_CHAT.md')


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f'{label}: expected 1 anchor, found {count}')
    return text.replace(old, new, 1)


cpp = CPP.read_text()

# A directly typed party message deserves the reserved interactive capacity for its first reply.
cpp = replace_once(cpp,
'''            NpcChat_LLMResult res = NpcChat_CallBackgroundLLM(BuildChatApiConfig(), system, user.str(), "bot-social");\n            std::string line = TrimCopy(res.text);\n''',
'''            NpcChat_LLMResult res;\n            if (req.channel == BotSocialChannel::Party && turnIndex == 0)\n            {\n                res = NpcChat_CallLLM(BuildChatApiConfig(), system, user.str(),\n                    NpcChat_RequestClass::Interactive, "party-direct");\n            }\n            else\n            {\n                res = NpcChat_CallBackgroundLLM(BuildChatApiConfig(), system, user.str(), "bot-social");\n            }\n            std::string line = TrimCopy(res.text);\n''', 'first party turn interactive')

# Align group packet emission with AzerothCore's native ChatHandler shape. Party responses in a raid
# should stay in the bot's subgroup; raid responses still broadcast to the full raid.
old_social_emit = '''                else if (Group* group = bot->GetGroup())\n                {\n                    ChatMsg type = r.channel == BotSocialChannel::Raid ? CHAT_MSG_RAID : CHAT_MSG_PARTY;\n                    WorldPacket data;\n                    ChatHandler::BuildChatPacket(data, type, LANG_UNIVERSAL, bot, bot, r.text);\n                    group->BroadcastPacket(&data, false);\n                }\n'''
new_social_emit = '''                else if (Group* group = bot->GetGroup())\n                {\n                    ChatMsg type = r.channel == BotSocialChannel::Raid ? CHAT_MSG_RAID : CHAT_MSG_PARTY;\n                    WorldPacket data;\n                    ChatHandler::BuildChatPacket(data, type, LANG_UNIVERSAL, bot, nullptr, r.text);\n                    if (r.channel == BotSocialChannel::Party)\n                        group->BroadcastPacket(&data, false, group->GetMemberGroup(bot->GetGUID()));\n                    else\n                        group->BroadcastPacket(&data, false);\n                }\n'''
cpp = replace_once(cpp, old_social_emit, new_social_emit, 'social group packet routing')

old_legacy_emit = '''                        ChatHandler::BuildChatPacket(data, cm, LANG_UNIVERSAL, bot, bot, r.text);\n                        g->BroadcastPacket(&data, false);\n'''
new_legacy_emit = '''                        ChatHandler::BuildChatPacket(data, cm, LANG_UNIVERSAL, bot, nullptr, r.text);\n                        if (r.channel == BotChannel::Party)\n                            g->BroadcastPacket(&data, false, g->GetMemberGroup(bot->GetGUID()));\n                        else\n                            g->BroadcastPacket(&data, false);\n'''
cpp = replace_once(cpp, old_legacy_emit, new_legacy_emit, 'legacy group packet routing')

CPP.write_text(cpp)

# Keep diff hygiene clean.
DOC.write_text(DOC.read_text().rstrip() + '\n')

print('final party transport/routing patch applied')
