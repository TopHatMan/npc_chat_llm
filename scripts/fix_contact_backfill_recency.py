from pathlib import Path

p = Path('src/mod_npcchat.cpp')
s = p.read_text(encoding='utf-8')
old = '''                TouchNpcContact(playerGuid, npcEntry, folder.substr(0, playerSep), stem.substr(0, npcSep));\n                ++imported;\n'''
new = '''                // Backfill only missing rows. Do not refresh last_talked_at on every restart;\n                // live conversations are the only thing that should move a contact to the front.\n                std::ostringstream sql;\n                sql << "INSERT IGNORE INTO `npcchat_contact` (`player_guid`,`npc_entry`,`player_name`,`npc_name`) VALUES ("\n                    << playerGuid << "," << npcEntry << ",'"\n                    << SqlEscape(folder.substr(0, playerSep)) << "','"\n                    << SqlEscape(stem.substr(0, npcSep)) << "')";\n                WorldDatabase.Execute(sql.str().c_str());\n                ++imported;\n'''
if s.count(old) != 1:
    raise SystemExit(f'backfill replacement marker count={s.count(old)}')
p.write_text(s.replace(old, new, 1), encoding='utf-8')
print('Fixed contact backfill recency behavior.')
