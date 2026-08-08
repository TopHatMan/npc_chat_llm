from pathlib import Path

p = Path('src/mod_npcchat.cpp')
s = p.read_text()

old_include = '#include "Channel.h"        // Channel::Say / IsOn\n'
new_include = '#include "Channel.h"        // Channel::Say\n#include "ChannelMgr.h"     // find General/Trade channel on world thread\n'
if old_include not in s:
    raise SystemExit('Channel include anchor not found')
s = s.replace(old_include, new_include, 1)

old = '''                else // ChannelMsg\n                {\n                    // Channel::Say needs the bot to be a channel member and Channel::IsOn is\n                    // private in this core, so we can't gate it. Whisper the requester instead:\n                    // reliable, and the natural LFG UX (the recruiter is told directly).\n                    if (player)\n                        bot->Whisper(r.text, LANG_UNIVERSAL, player);\n                }\n'''
new = '''                else // ChannelMsg\n                {\n                    // A playerbot only whispers when the real player whispered it first. Channel/LFG\n                    // replies therefore stay in the originating channel instead of falling back to a DM.\n                    // GetChannel(..., pkt=false) quietly returns null when the bot is not a member.\n                    if (ChannelMgr* channelMgr = ChannelMgr::forTeam(bot->GetTeamId()))\n                        if (Channel* channel = channelMgr->GetChannel(r.channelName, bot, false))\n                            channel->Say(bot->GetGUID(), r.text, LANG_UNIVERSAL);\n                }\n'''
if s.count(old) != 1:
    raise SystemExit(f'channel fallback anchor count={s.count(old)}')
s = s.replace(old, new, 1)

p.write_text(s)
print('channel replies routed without unsolicited whisper')
