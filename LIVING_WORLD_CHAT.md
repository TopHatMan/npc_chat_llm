# Living world chat

This pass adds two deliberately bounded ways for Azeroth to feel less static without importing the full complexity of a companion/memory framework.

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

Default settings:

```ini
NpcChat.HistoryWhispers.Enabled = 1
NpcChat.HistoryWhispers.TriggerDistance = 18.0
NpcChat.HistoryWhispers.ChancePct = 12
NpcChat.HistoryWhispers.PlayerCooldownSec = 300
NpcChat.HistoryWhispers.PairCooldownSec = 900
NpcChat.HistoryWhispers.ScanIntervalMs = 5000
NpcChat.HistoryWhispers.HistoryMaxLines = 8
```

These defaults mean proximity checks are cheap and frequent, but an actual API-backed whisper is uncommon and subject to a five-minute player cooldown plus a fifteen-minute per-NPC relationship cooldown.

## Playerbot social conversations

Party, raid, and guild chat use a bounded coordinator. A real player message may select a small number of eligible playerbots, favoring explicitly named bots and non-random/alt bots. Selected bots generate sequentially so later speakers can react to earlier bot replies.

This deliberately avoids recursive chat-hook behavior. Generated bot messages are emitted directly into the appropriate group/guild channel and do not trigger another autonomous generation chain.

Large raids therefore get the impression of a populated social group without one player message causing dozens of simultaneous LLM requests.

## Suggested smoke test

1. Talk to a normal friendly NPC and get at least one successful reply.
2. Move away, return within the configured whisper range, and wait for the chance/cooldown conditions to allow a recognition whisper.
3. Confirm the NPC whisper references real prior conversation rather than inventing a major event.
4. Put several non-random alt bots and random playerbots into a party and send normal party chat.
5. Verify only the configured number of bots respond and the later response can react to the earlier one.
6. Repeat in raid and guild chat.
7. Confirm no bot-generated response recursively creates an endless conversation.
