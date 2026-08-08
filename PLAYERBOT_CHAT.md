# Playerbot chat

`npc_chat_llm` keeps playerbot roleplay intentionally simple. It borrows the editable character-card convention from `mod-playerbots-characters` without copying that module's larger memory, relationship, web UI, or event systems.

## Character cards

By default, playerbot cards live in:

```text
./characters/
```

The filename is the exact in-game character name:

```text
characters/Mang.card.txt
characters/Odelix.card.txt
```

This is configurable with:

```ini
NpcChat.Bot.CharacterCardsPath = ./characters
```

The default path and filename convention are compatible with `mod-playerbots-characters`, so the same card files can be reused when switching between the two modules.

### First conversation

When a real player first causes a playerbot to reply and no card exists, `npc_chat_llm` creates a local starter file. This does **not** call the LLM and does not spend an extra API request.

The starter card is:

```text
You are {char_name}, a {char_gender} {char_race} {char_class}.

You are an adventurer living in Azeroth. Your personality, background, mannerisms, beliefs, likes, dislikes, and relationships can be described here.

Stay in character when speaking.
```

The generated file is only a template for the server owner to edit later.

### Live editing

An existing card is never overwritten automatically. The module reads the `.card.txt` file from disk again for every conversation, so saving an edit changes the bot's next response without restarting the worldserver or running a reload command.

Placeholders currently expanded by `npc_chat_llm` are:

```text
{char_name}
{char_gender}
{char_race}
{char_class}
{char_level}
```

The placeholders are replaced only in the in-memory prompt. The `.card.txt` file itself remains unchanged and reusable.

## Conversation history

Character identity is name-based, but personal conversation history remains tied to the real player and bot GUIDs:

```text
<NpcChat.HistoryPath>/bots/personal/<player-name>_<player-guid>/<bot-name>_<bot-guid>.history
```

Only the recent history tail configured by this setting is sent back to the LLM:

```ini
NpcChat.Bot.HistoryMaxLines = 20
```

There is no narrator memory condensation, relationship scoring, vector database, or separate personal character card in this playerbot system.

## Addon traffic is not roleplay

WoW addons communicate through chat packets marked with AzerothCore's `LANG_ADDON`. Those packets can use whisper, party, raid, guild, or channel transport even though they are not human dialogue.

`npc_chat_llm` ignores `LANG_ADDON` traffic **before** any NPC/playerbot RP handler runs. Addon packets therefore:

- do not trigger a playerbot whisper reply
- do not start party/raid/guild social generation
- do not trigger NPC chat
- do not consume an LLM request
- do not get appended to new RP history

The bot system prompt also explicitly treats addon/protocol payloads as non-dialogue. This second layer is intentional: older history files may already contain addon garbage from builds before the hard filter existed, and the model should ignore rather than roleplay about those lines.

For direct whispers, the intended behavior is simple: **the real player whispers first, then the bot may whisper back.** `npc_chat_llm` does not use addon traffic as permission to initiate an RP whisper.

## Chat surfaces

The same card and recent-history prompt path is used for:

- direct whisper to a playerbot
- targeted `/say`
- party chat
- raid chat
- guild chat
- General/Trade LFG matching
- optional General ambient replies

## Social party, raid, and guild chat

When `NpcChat.Bot.Social.Enable = 1`, a real player's party, raid, or guild message can start one bounded multi-bot conversation.

This is intentionally **not** implemented by feeding bot messages back through the normal chat hooks. Instead, one conversation worker selects a small number of bots and generates their turns sequentially. Later speakers see the earlier generated lines before they answer. This prevents recursive bot-to-bot loops while still allowing natural banter.

Selection rules are deliberately biased toward authored characters:

1. A bot explicitly named in the message is preferred.
2. Non-random/alt playerbots are preferred for ambient chatter because they are most likely to have intentionally edited character cards.
3. Random playerbots may occasionally join so large raids and guilds feel populated.
4. Speaker caps keep a 40-person raid from generating 40 LLM calls for one chat line.

Default settings:

```ini
NpcChat.Bot.Social.Enable = 1
NpcChat.Bot.Social.PartyChancePct = 70
NpcChat.Bot.Social.RaidChancePct = 35
NpcChat.Bot.Social.GuildChancePct = 55
NpcChat.Bot.Social.PartyMaxSpeakers = 3
NpcChat.Bot.Social.RaidMaxSpeakers = 2
NpcChat.Bot.Social.GuildMaxSpeakers = 3
NpcChat.Bot.Social.RandomBotChancePct = 25
NpcChat.Bot.Social.CooldownSec = 20
```

Named bot messages bypass the ambient chance roll. Ambient conversations use the chance and cooldown settings.

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

## Quick test checklist

1. Move or rename any existing card for a test bot so no `characters/<BotName>.card.txt` exists.
2. Whisper the bot and confirm the starter `.card.txt` file appears.
3. Confirm the bot replies without a second card-generation LLM request.
4. Edit the new card with a very obvious personality or phrase and save it.
5. Talk to the bot again and confirm the next reply reflects the edit without restarting or reloading.
6. With an addon that communicates through addon whispers enabled, confirm those packets no longer produce RP replies or new bot-history lines.
7. Manually whisper the bot afterward and confirm it still answers normally.
8. Put several playerbots in a party and send a normal party message; confirm only a bounded set replies.
9. Repeat in raid chat and confirm the raid speaker cap is respected.
10. Log in a real guild member with several eligible guild bots offline; within the configured scans, confirm those bot characters appear online in the guild roster.
11. Confirm an ordinary non-bot alt in the same guild remains offline.
12. Send a guild message after the bots have logged in and confirm a small multi-bot conversation can occur.
13. Name one specific bot and confirm it is favored as a speaker.
14. Confirm recent bot history remains under `NpcChat.HistoryPath/bots/personal/` and no GUID-based `.card` files are created.
