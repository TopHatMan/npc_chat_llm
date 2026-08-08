from pathlib import Path

p = Path("src/mod_npcchat.cpp")
s = p.read_text()

old = '''            user << "Conversation so far:\\n" << conversation
                << "\\n\\nAdd one short natural line as " << turn.botName
                << ". If you truly have nothing to add, output exactly [SKIP].";
'''
new = '''            user << "Conversation so far:\\n" << conversation
                << "\\n\\nAdd one short natural line as " << turn.botName;
            if (req.channel == BotSocialChannel::Guild)
                user << ". You were selected to participate in guild chat, so give a natural in-character reply; do not output [SKIP].";
            else
                user << ". If you truly have nothing to add, output exactly [SKIP].";
'''

if old in s:
    s = s.replace(old, new, 1)
elif "You were selected to participate in guild chat" not in s:
    raise SystemExit("guild selected-reply anchor not found")

p.write_text(s)
print("guild selected-reply behavior tightened")
