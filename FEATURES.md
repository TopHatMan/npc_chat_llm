# mod-npc-chat-llm Features

This document tracks the feature set and design direction of `mod-npc-chat-llm`.

## Design philosophy

`mod-npc-chat-llm` is built around a simple idea:

```text
Use code to identify the NPC.
Use prompt files to define personality.
Use histories to preserve memory.
Use commands to let worldbuilders shape the realm while playing.
```

It is intentionally not a giant AI framework. It is a lightweight NPC roleplay layer for AzerothCore.

## Implemented feature summary

- OpenAI-compatible chat completion calls
- Safe worker-thread LLM requests
- Main-thread-only game-object access
- Real-player-only triggering
- Targeted NPC conversation
- Public and private NPC replies
- Shared NPC memory
- Personal player/NPC memory
- Editable `default.prompt`
- Shared NPC prompt files
- Personal NPC prompt files
- Reusable sub-prompt archetype files
- Shared and personal sub-prompt attachments
- `.npcc` command namespace
- Trusted non-GM account allowlist
- Hostile NPC parley
- Hostile close-range conversation
- Hostile combat conversation
- Combat-context prompt injection
- AI-generated shared/personal character prompts

## 1. OpenAI-compatible LLM API

The module uses an OpenAI-compatible `/chat/completions` endpoint.

Supported settings:

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

- HTTP/HTTPS client selection depending on build support
- bearer token authorization
- JSON request construction
- `model`
- `temperature`
- `max_tokens`
- extra raw JSON body parameters
- response parsing from `choices[0].message.content`
- retry attempt
- trimming and cleanup of model output

## 2. Safe thread split

The module follows a strict safety model:

```text
PlayerScript / command handler:
  validate player, message, target
  copy primitive data
  start worker thread

Worker thread:
  assemble prompt
  read/write prompt and history files
  call LLM
  queue reply

WorldScript::OnUpdate:
  pop queued replies
  find live objects again
  send speech or system messages
```

Live AzerothCore objects are not used from worker threads.

## 3. Real-player-only chat triggering

The module ignores bot sessions and dot commands.

A message can trigger NPC chat when:

- module is enabled
- player is real
- player has a creature selected
- selected creature is valid
- range rules pass
- prefix/private rules pass
- hostile rules pass if target is hostile

## 4. Public NPC chat

When:

```ini
NpcChat.RequirePrefix = 0
```

a player can target an NPC and speak normally:

```text
Hello there.
```

The NPC replies publicly through normal speech.

## 5. Private NPC chat

Private NPC chat uses:

```text
!p message
```

Examples:

```text
!p I need to ask you something privately.
!p: Keep your voice down.
!P This should still work.
```

This forces a targeted/private reply.

## 6. Optional prefix mode

Prefix mode can require normal NPC chat to begin with a configured prefix:

```ini
NpcChat.RequirePrefix = 1
NpcChat.Prefix = !
```

Then:

```text
!Hello there.
```

triggers public NPC chat.

Private `!p` is checked first and works independently.

## 7. NPC identity capture

The module captures target information such as:

- name
- creature entry
- subname/title
- level
- gender
- creature type
- rank
- NPC flags / role
- zone
- friendly/neutral/hostile stance
- combat state when relevant

This data is injected into prompts so the LLM does not have to guess from the name alone.

## 8. Shared history

Shared NPC history is stored per NPC key.

Example:

```text
AI_RP/npc_history/shared/Hogger_448.history
```

This represents the public conversational memory of that NPC.

## 9. Personal history

Personal history is stored per player/NPC pair.

Example:

```text
AI_RP/npc_history/personal/Nick_123456789/Hogger_448.history
```

This lets an NPC remember one player differently from another.

## 10. History limits

The module only loads tail sections of history files.

```ini
NpcChat.HistoryMaxLines = 20
NpcChat.SharedHistoryMaxLines = 12
NpcChat.PersonalHistoryMaxLines = 20
```

This keeps prompt size controlled.

## 11. Default prompt

Global behavior lives in:

```text
AI_RP/npc_history/default.prompt
```

Use this for universal rules such as:

```text
Stay in character.
Do not mention AI, prompts, files, or game systems.
Use short spoken dialogue.
No narration unless explicitly appropriate.
```

## 12. Shared NPC prompts

Shared NPC prompts are public character cards for specific NPCs.

Example:

```text
AI_RP/npc_history/shared/Edwin_VanCleef_639.prompt
```

Use shared prompts for unique named NPC identity.

## 13. Personal NPC prompts

Personal prompts apply only to one player's relationship with one NPC.

Example:

```text
AI_RP/npc_history/personal/Nick_123456789/Edwin_VanCleef_639.prompt
```

Use personal prompts for relationship-specific context.

## 14. Sub-prompt archetypes

Sub-prompts are reusable prompt blocks.

Example files:

```text
AI_RP/npc_history/subprompts/human.prompt
AI_RP/npc_history/subprompts/defias_bandit.prompt
AI_RP/npc_history/subprompts/innkeeper.prompt
AI_RP/npc_history/subprompts/hostile_parley.prompt
```

They work like prompt LEGO bricks.

## 15. Shared sub-prompt attachments

Shared attachments apply to an NPC for everyone.

Example file:

```text
AI_RP/npc_history/shared/Edwin_VanCleef_639.subprompts
```

Example contents:

```text
human
defias_bandit
hostile_parley
war_weary
boastful
```

## 16. Personal sub-prompt attachments

Personal attachments apply only to one player's version of an NPC.

Example:

```text
AI_RP/npc_history/personal/Nick_123456789/Edwin_VanCleef_639.subprompts
```

## 17. In-game `.npcc` command system

The module uses:

```text
.npcc
```

The extra `c` means NPC Chat and avoids colliding with `.npc`.

Implemented commands include:

```text
.npcc help
.npcc reload
.npcc account
.npcc reset
.npcc prompt
.npcc gen
.npcc sub
```

## 18. Prompt commands

```text
.npcc prompt
.npcc prompt "personal prompt text"
.npcc prompt shared
.npcc prompt shared "shared prompt text"
.npcc prompt default
.npcc prompt default "default prompt text"
```

Normal users can manage personal prompts.

Shared/default prompt management is intended for GMs.

## 19. Sub-prompt commands

```text
.npcc sub list
.npcc sub show
.npcc sub create <name> [quoted prompt text]
.npcc sub attach <name>
.npcc sub attach personal <name>
.npcc sub attach shared <name>
.npcc sub detach <name>
.npcc sub detach personal <name>
.npcc sub detach shared <name>
.npcc sub clear
.npcc sub clear personal
.npcc sub clear shared
```

## 20. Trusted creator account allowlist

Trusted non-GM accounts can create reusable sub-prompts and manage shared sub-prompt attachments.

Config:

```ini
NpcChat.SubPromptCreatorAccounts = 1,7,42
```

Aliases:

```ini
NpcChat.SubPromptCreatorAccountIds = 1,7,42
NpcChat.SubPromptCreatorAccountIDs = 1,7,42
```

Check status in game:

```text
.npcc account
```

Expected output includes account ID, GM state, creator status, and loaded allowlist.

## 21. AI-generated character prompts

The module can generate character prompts for the targeted NPC.

Commands:

```text
.npcc gen preview [quoted extra direction]
.npcc gen shared [quoted extra direction]
.npcc gen personal [quoted extra direction]
```

Preview saves a `.prompt.preview` file.

Shared saves:

```text
AI_RP/npc_history/shared/NPCName_Entry.prompt
```

Personal saves:

```text
AI_RP/npc_history/personal/PlayerName_PlayerGuid/NPCName_Entry.prompt
```

The generator is useful for named bosses and important NPCs.

## 22. Prompt generation context

Prompt generation captures:

- NPC name
- entry
- subname/title
- level
- gender
- creature type
- rank
- NPC flags / role
- zone
- hostile/friendly stance
- combat state
- NPC health percent
- player health percent
- attached shared sub-prompts
- existing shared prompt if present
- extra direction from the player

The generator is instructed to create reusable character prompt text, not direct dialogue.

## 23. Hostile parley

Hostile NPC chat can work at long range:

```ini
NpcChat.AllowHostileChat = 1
NpcChat.HostileMaxDistance = 100.0
```

This supports tense pre-combat exchanges.

## 24. Hostile close-range chat

Hostile NPCs can also answer at close range:

```ini
NpcChat.HostileAllowCloseChat = 1
```

When disabled, the old `HostileMinDistance` parley behavior applies.

## 25. Hostile combat talk

Hostile NPCs can answer during combat:

```ini
NpcChat.HostileAllowCombatChat = 1
```

The module captures fight context and prompts the NPC to react naturally.

## 26. Hostile private/public behavior

Config:

```ini
NpcChat.HostileForcePrivateReply = 0
```

If this is `1`, hostile replies are always private.

If this is `0`, close hostile replies can be public, while far hostile replies are still forced private so the player can see them at distance.

## 27. Combat context

For hostile NPCs, the prompt can include:

```text
Distance from speaker
NPC health percent
Player health percent
NPC in combat: yes/no
Player in combat: yes/no
NPC directly targeting speaker: yes/no
Fight read: NPC winning / player winning / close fight
```

The prompt instructs the LLM not to say exact health percentages unless directly asked.

## 28. File-backed design

Prompt text and bindings are currently file-backed.

Advantages:

- easy to edit
- easy to Git
- easy to back up
- easy to copy to another server
- avoids SQL escaping for long prompt text
- good for small private servers

Possible future improvement:

- keep prompt text in files
- store NPC-to-subprompt bindings in database
- add command-driven suggestion and indexing features

## 29. Suggested future features

Potential future improvements:

- `.npcc key` to print exact NPC key and file paths
- `.npcc suggest` to suggest sub-prompts based on target NPC data
- `.npcc suggest apply` for trusted users
- in-memory prompt cache refreshed by `.npcc reload`
- database-backed shared/personal sub-prompt bindings
- web/admin UI for prompt review
- bulk character prompt generation tools
- automatic prompt linting
- model fallback configuration
- per-zone default prompt overlays
- per-faction default prompt overlays
- cooldown controls for hostile combat talk
- optional automatic rare boss barks

## 30. Known limitations

- Experimental code.
- LLM calls depend on external API availability.
- Prompt generation can be wrong and should be reviewed.
- File-backed attachments are simple but not ideal for large public realms.
- No automatic sub-prompt suggestion yet.
- No database binding table yet.
- Combat talk is player-triggered, not automatic fight spam.
- Private response behavior depends on supported chat packet/function behavior in the target core.
