from pathlib import Path
p = Path('src/mod_npcchat.cpp')
s = p.read_text(encoding='utf-8')
repls = {
    'bool        g_HistoryWhispersEnabled = false;': 'bool        g_HistoryWhispersEnabled = true;',
    'sConfigMgr->GetOption<bool>("NpcChat.HistoryWhispers.Enabled", false)': 'sConfigMgr->GetOption<bool>("NpcChat.HistoryWhispers.Enabled", true)',
    '        bool enable = false;\n        uint32 partyChancePct = 70;': '        bool enable = true;\n        uint32 partyChancePct = 70;',
    'sConfigMgr->GetOption<bool>("NpcChat.Bot.Social.Enable", false)': 'sConfigMgr->GetOption<bool>("NpcChat.Bot.Social.Enable", true)',
}
for old, new in repls.items():
    if s.count(old) != 1:
        raise SystemExit(f'expected one match for {old!r}, got {s.count(old)}')
    s = s.replace(old, new, 1)
p.write_text(s, encoding='utf-8')
print('Aligned new social feature code defaults with enabled dist settings.')
