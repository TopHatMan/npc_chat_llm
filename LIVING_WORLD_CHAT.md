# Living world chat

This module adds deliberately bounded ways for Azeroth to feel less static without importing the full complexity of a companion/memory framework.

## History-aware NPC whispers

A world NPC that a real player has genuinely spoken with before may privately recognize that player when they pass nearby later.

Eligibility is based on **actual personal NPC chat history**. A lightweight SQL table named `npcchat_contact` is only an index that lets the proximity scan cheaply determine which creature entries are worth looking for. The personal `.history` file remains the source of conversational context, and the worker refuses to generate a recognition whisper if that history is empty.

Normal NPC conversations update the contact index automatically. On startup, when `NpcChat.NameByEntry = 1`, existing personal history files are also imported into the contact index so older conversations can participate immediately.

The feature does not fire when:

- the player is a playerbot
- the player or NPC is in combat
- the NPC is hostile to the player
- the NPC is outside the configured distance
- the player or player/NPC pair is on cooldown
- the chance roll does not fire
- personal chat history is missing or empty

When it does fire, the normal NPC identity/prompt layers and recent personal history are sent to the chat model with a narrowly scoped instruction to produce one short recognition or continuation line. The response is emitted with AzerothCore's actual creature `Whisper` chat path.

Default settings are intentionally conservative:

```ini
NpcChat.HistoryWhispers.Enabled = 1
NpcChat.HistoryWhispers.TriggerDistance = 18.0
NpcChat.HistoryWhispers.ChancePct = 5
NpcChat.HistoryWhispers.PlayerCooldownSec = 600
NpcChat.HistoryWhispers.PairCooldownSec = 1800
NpcChat.HistoryWhispers.ScanIntervalMs = 8000
NpcChat.HistoryWhispers.HistoryMaxLines = 8
```

The earlier 12% / five-second scan made familiar characters feel active but could become noisy in a populated area. The quieter defaults still allow recognition moments while enforcing a ten-minute player cooldown and a thirty-minute per-NPC pair cooldown after a successful whisper.

## Playerbot social conversations

Party, raid, and reactive guild chat use a bounded coordinator. A real player message may select a small number of eligible playerbots, favoring explicitly named bots and non-random/alt bots. Selected bots generate sequentially so later speakers can react to earlier bot replies.

This deliberately avoids recursive chat-hook behavior. Generated bot messages are emitted directly into the appropriate group/guild channel and do not trigger another autonomous generation chain.

Large raids therefore get the impression of a populated social group without one player message causing dozens of simultaneous LLM requests.

## Autonomous guild chatter

Keeping guild Playerbots online makes the roster look alive, but presence alone does not create conversation. `NpcChat.Bot.GuildAmbient.*` adds a separate low-frequency bot-to-bot guild-chat loop while at least one real guild member is online.

Default settings:

```ini
NpcChat.Bot.GuildAmbient.Enable = 1
NpcChat.Bot.GuildAmbient.MinIntervalSec = 90
NpcChat.Bot.GuildAmbient.MaxIntervalSec = 240
NpcChat.Bot.GuildAmbient.MaxSpeakers = 2
NpcChat.Bot.GuildAmbient.RandomBotChancePct = 15
NpcChat.Bot.GuildAmbient.HistoryMaxLines = 12
```

Behavior:

- the first ambient conversation is delayed; bots do not immediately flood guild chat at login
- each guild gets one randomized next-conversation time, so multiple real players do not multiply the rate
- one or two bots are selected per conversation
- authored/non-random alt bots are preferred when available
- random Playerbots may occasionally join
- the first bot starts a normal short guild-chat topic rather than pretending a player addressed it
- a second bot can react to the first line
- recent ambient guild lines are kept in a small guild history file to reduce repetitive openers
- generated lines are broadcast directly to guild chat and do not recurse through the PlayerScript chat hooks
- ambient calls use the lower-priority background transport lane, leaving reserved capacity for direct NPC/playerbot conversations

The goal is a guild that occasionally sounds inhabited, not a permanent AI group chat running every few seconds.

## Whisper note for Playerbots

`npc_chat_llm` does not currently contain an unsolicited playerbot-to-player whisper loop. Playerbot whispers produced by this module are direct replies to a player whisper, or the General/LFG fallback reply path. If actual Playerbots begin sending unsolicited tells independently of those interactions, that behavior should be tuned in Playerbots itself rather than by increasing NPC Chat cooldowns.

## Suggested smoke test

1. Talk to a normal friendly NPC and get at least one successful reply.
2. Move away, return within the configured whisper range, and confirm recognition whispers are now noticeably less frequent.
3. Confirm an NPC whisper references real prior conversation rather than inventing a major event.
4. Put several non-random alt bots and random playerbots into a party and send normal party chat.
5. Verify only the configured number of bots respond and the later response can react to the earlier one.
6. Repeat in raid and reactive guild chat.
7. Stay online in a guild with several Playerbots and wait through the configured ambient interval; confirm one or two bots eventually start a small guild conversation without a player message.
8. Confirm ambient guild messages do not recursively create an endless conversation.
