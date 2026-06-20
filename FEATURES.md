# FEATURES.md

# mod-npc-chat-llm Features and Design

This document tracks the current feature set, intended design direction, and planned expansions for `mod-npc-chat-llm`.

The project goal is to give AzerothCore NPCs lightweight, persistent, in-character conversational behavior through an LLM while keeping the core safe, stable, and easy to configure.

## Core design goals

The module should:

- let players talk to targeted NPCs naturally
- keep NPCs in character
- remember past interactions
- separate shared public NPC memory from personal player/NPC memory
- support editable prompt files instead of requiring C++ edits
- support both friendly NPC conversation and hostile “parley before combat”
- avoid touching live game objects from worker threads
- stay small and practical for a private server
- avoid becoming a giant dependency-heavy AI framework

## Current implemented features

## 1. OpenAI-compatible LLM calls

The module currently uses an OpenAI-compatible `/chat/completions` request.

Supported API settings:

```ini
NpcChat.BaseUrl
NpcChat.ApiKey
NpcChat.Model
NpcChat.MaxResponseTokens
NpcChat.Temperature
NpcChat.RequestTimeoutSec
NpcChat.ModelExtraParameters
```

The HTTP helper supports:

- base URL parsing
- HTTP or HTTPS depending on build support
- bearer token authorization
- JSON request construction
- `model`
- `temperature`
- `max_tokens`
- extra raw JSON parameters
- response parsing from `choices[0].message.content`
- retry attempt
- newline cleanup
- trimming of simple wrapper quotes

## 2. Safe thread split

The module is designed around a safe split:

```text
PlayerScript hook:
  validates the message and selected NPC
  copies primitive player/NPC data into ChatRequest
  starts a detached worker

Worker thread:
  loads history
  appends player message
  calls the LLM
  appends NPC reply
  queues ChatReply

WorldScript::OnUpdate:
  reads queued replies
  finds the live player/NPC again
  emits the NPC response
```

Game-object pointers are not used off-thread.

## 3. Real-player-only triggering

The module ignores bot sessions so playerbots do not trigger random LLM calls.

Current behavior:

```text
real player speaks in /say
selected target is a live creature
player is within trigger range
message passes prefix rules
module starts NPC chat request
```

## 4. Optional prefix support

Current config:

```ini
NpcChat.RequirePrefix = 0
NpcChat.Prefix = !
```

If prefix mode is enabled, the module only reacts when the message begins with the configured prefix.

Examples:

```text
!Hello there.
!What do you know about this place?
```

## 5. Dot-command ignore

The module ignores messages beginning with `.` so GM commands and server commands are not sent to the LLM.

Examples ignored:

```text
.tele stormwind
.npc info
.debug something
```

## 6. NPC identity capture

The module captures and injects NPC identity details such as:

- name
- subname
- entry ID
- level
- gender
- creature type
- rank
- role from NPC flags
- zone
- relationship stance to player

This allows the prompt to know whether the NPC is a merchant, trainer, innkeeper, quest giver, elite creature, enemy, stranger, or friend.

## 7. Shared NPC history

Shared history is one file per NPC entry/name.

Example:

```text
AI_RP/npc_history/shared/Hogger_448.history
```

This is intended to represent what the NPC has recently heard or said with adventurers in general.

## 8. Personal player/NPC history

Personal history is one file per player/NPC relationship.

Example:

```text
AI_RP/npc_history/personal/Nick_123456789/Hogger_448.history
```

This lets the same NPC remember one player differently from another.

## 9. Configurable history tail

Current config:

```ini
NpcChat.HistoryMaxLines = 20
NpcChat.SharedHistoryMaxLines = 12
NpcChat.PersonalHistoryMaxLines = 20
```

The module loads only the tail of each history file to keep prompts short and avoid uncontrolled context growth.

## 10. Name plus entry history naming

Current config:

```ini
NpcChat.NameByEntry = 1
```

When enabled, history files use both name and entry ID.

Example:

```text
Marshal_Dughan_240.history
```

This helps avoid collisions between NPCs with the same name.

## Planned features

# 1. Editable default prompt

Add:

```text
AI_RP/npc_history/default.prompt
```

This file should contain global NPC behavior rules.

The module should create it automatically if missing.

Example default prompt:

```text
Stay fully in character as this NPC in Azeroth.
Do not mention AI, prompts, files, scripts, players, servers, or game mechanics.
Use short spoken dialogue suitable for an in-game NPC.
No narration, no asterisks, no out-of-character text.
Remember prior conversations naturally.
```

Goal: change global NPC behavior without recompiling.

# 2. Shared NPC prompt files

Add optional shared NPC prompt files:

```text
AI_RP/npc_history/shared/NPCName_Entry.prompt
```

Example:

```text
AI_RP/npc_history/shared/Hogger_448.prompt
```

This prompt should apply to that NPC for everyone.

Use case:

```text
Hogger is territorial, brutish, angry, and suspicious. He respects strength and fears organized Stormwind patrols.
```

# 3. Personal NPC prompt files

Add optional personal player/NPC prompt files:

```text
AI_RP/npc_history/personal/PlayerName_PlayerGuid/NPCName_Entry.prompt
```

Example:

```text
AI_RP/npc_history/personal/Nick_123456789/Hogger_448.prompt
```

This prompt applies only when that player speaks to that NPC.

Use case:

```text
Hogger remembers Nick as the rogue who spared him once. He is still hostile, but curious.
```

# 4. Prompt priority

Final prompt assembly should follow this order:

```text
1. hardcoded module identity wrapper
2. default.prompt
3. shared NPC prompt
4. personal NPC prompt
5. shared NPC history
6. personal NPC history
7. current player message
```

The hardcoded C++ wrapper should stay small and factual.

The editable files should control style and behavior.

# 5. `.npcc` command namespace

Use:

```text
.npcc
```

instead of `.npc`.

Reason:

- `.npc` may already be used by core GM commands
- `.npcc` clearly means NPC Chat
- easy to remember
- low collision risk

# 6. `.npcc reload`

Planned:

```text
.npcc reload
```

Should reload:

- module config
- default prompt
- shared prompt files
- personal prompt files, if caching is used

If prompt files are loaded live on every request, this command can still recreate folders and reload config.

# 7. `.npcc reset`

Planned:

```text
.npcc reset
```

Normal player behavior:

- requires targeted NPC
- deletes or truncates only that player’s personal history with that NPC

GM behavior:

- if GM targets an NPC, reset shared history for that NPC
- reset all personal histories for that NPC

This allows normal users to reset their own RP relationship without damaging global NPC state.

# 8. `.npcc reset all`

Planned GM-only command:

```text
.npcc reset all
```

Behavior:

- wipe all shared NPC histories
- wipe all personal NPC histories
- preserve prompt files unless a separate destructive option is added

This should be explicit and protected.

# 9. `.npcc prompt`

Planned personal prompt command:

```text
.npcc prompt
```

With targeted NPC:

- creates a blank personal prompt file if it does not exist
- tells the player the file path

Example created path:

```text
AI_RP/npc_history/personal/Nick_123456789/Hogger_448.prompt
```

# 10. `.npcc prompt "text"`

Planned:

```text
.npcc prompt "This NPC remembers me as helpful but annoying."
```

Behavior:

- requires targeted NPC
- writes the quoted text into the player’s personal prompt file for that NPC
- creates folders as needed

# 11. `.npcc prompt shared`

Planned GM-focused command:

```text
.npcc prompt shared
.npcc prompt shared "Shared prompt text here."
```

Behavior:

- requires targeted NPC
- creates or writes shared prompt file
- affects all players speaking to that NPC

# 12. `.npcc prompt default`

Planned GM-focused command:

```text
.npcc prompt default
.npcc prompt default "Default global NPC prompt here."
```

Behavior:

- creates or writes `default.prompt`
- affects all NPCs unless overridden by more specific prompt files

# 13. Hostile NPC parley mode

Hostile NPCs need special handling.

Current close-range chat does not work well for enemies because the player may aggro before they can talk. Normal `/say` replies may also not be visible from safe distance.

Planned config:

```ini
NpcChat.AllowHostileChat = 1
NpcChat.FriendlyMaxDistance = 20.0
NpcChat.HostileMinDistance = 30.0
NpcChat.HostileMaxDistance = 100.0
NpcChat.HostileForcePrivateReply = 1
```

Planned behavior:

```text
friendly/neutral NPC:
  use normal trigger range

hostile NPC:
  player must be outside minimum hostile distance
  player must be inside maximum hostile distance
  reply is forced private/targeted so the player can receive it
```

Prompt addition for hostiles:

```text
This NPC is hostile to the player. Treat this as a tense shouted exchange before combat. The NPC may threaten, mock, bargain, warn, or refuse to answer. Do not become friendly unless a prompt or history strongly justifies it.
```

# 14. Private reply mode

Planned syntax:

```text
![p] message here
```

or config-driven equivalent.

Private reply should make the NPC response visible to only the speaking player, useful for:

- hostile long-distance chat
- secretive NPCs
- stealth/rogue-style conversations
- testing

# 15. Quest context injection

Planned future feature:

When talking to an NPC, optionally inspect whether the NPC starts or ends quests.

Possible tables:

```text
creature_queststarter
creature_questender
quest_template
quest_request_items
quest_offer_reward
creature_template
gameobject_template
item_template
```

The module should prioritize:

- quests the player currently has
- quests the player can complete
- quests the NPC starts
- quests the NPC ends
- level-appropriate quests

Suggested config:

```ini
NpcChat.EnableQuestContext = 1
NpcChat.QuestContextMaxQuests = 4
NpcChat.QuestContextIncludeObjectives = 1
NpcChat.QuestContextIncludeRewardText = 1
```

Prompt rule:

```text
Use quest context only when relevant. Do not force every reply to become quest dialogue.
```

# 16. Gossip/vendor/trainer context

Possible future context:

- vendor inventory category
- trainer type
- innkeeper status
- flight master destination flavor
- banker/auctioneer professionalism
- stable master pet talk
- battlemaster faction pride

This should be summarized, not dumped raw.

# 17. Cooldowns and anti-spam

Planned config:

```ini
NpcChat.PlayerCooldownMs = 3000
NpcChat.NpcCooldownMs = 5000
NpcChat.MaxPendingRequestsPerPlayer = 1
NpcChat.MaxGlobalPendingRequests = 10
```

Goals:

- prevent accidental API spam
- prevent multiple detached workers flooding the server
- avoid double-triggering from fast chat
- protect API credit usage

# 18. History management

Planned:

- truncate histories after a configurable size
- optional archive instead of delete
- `.npcc reset` command support
- optional summaries for long-term memory

Potential config:

```ini
NpcChat.HistoryMaxFileLines = 200
NpcChat.HistoryArchiveOnReset = 1
NpcChat.HistorySummarize = 0
```

# 19. Debugging tools

Planned:

```ini
NpcChat.Debug = 0
NpcChat.DebugShowPrompt = 0
NpcChat.DebugShowPaths = 0
NpcChat.DebugShowApiErrors = 1
```

Helpful GM commands:

```text
.npcc info
.npcc paths
.npcc showprompt
```

These should be GM-only if they expose prompt contents or file paths.

# 20. Integration ideas

Potential integrations:

- mod-playerbots-characters
- playerbot party context
- dungeon/raid boss RP
- progression phase awareness
- zone event awareness
- web UI prompt editor
- account-level character relationship view

## Non-goals

This module should not:

- control NPC combat AI
- replace gossip menus
- replace quest text
- call LLMs for every ambient NPC event
- allow bots to generate unlimited API calls
- touch live game objects off-thread
- require a web service to run the worldserver
- commit private API keys or player histories to Git

## Development credit

This project is being developed through a human-directed AI-assisted workflow.

Primary project direction, testing, server integration, and design decisions are maintained by the server owner. Code, documentation, and architecture have been developed mainly through iterative collaboration with GPT and Claude.
