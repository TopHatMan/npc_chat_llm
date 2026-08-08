# mod-npc-chat-llm

`mod-npc-chat-llm` is an experimental AzerothCore WotLK module that lets real players speak to targeted NPCs and receive short, in-character LLM-generated replies.

The goal is not to turn every NPC into a generic chatbot. The goal is to make Azeroth feel more alive: guards, vendors, quest givers, trainers, dungeon bosses, enemies, and odd one-off creatures can answer like people who belong in the world.

This module was built for a private progressive AzerothCore / Playerbots realm and is designed as a practical living-world roleplay tool.

## AI-assisted development note

This module was designed and built through human-directed AI-assisted development. GPT and Claude were used to help generate, revise, debug, and document code, prompts, and architecture. Project direction, testing decisions, and final integration are driven by the server owner/maintainer.

Treat this as an actively tested experimental module, not a polished upstream AzerothCore module.

## Current status

- **Status:** experimental / alpha
- **Target core:** AzerothCore WotLK 3.3.5a
- **API style:** OpenAI-compatible `/chat/completions`
- **Primary use case:** a real player targets an NPC, speaks in `/say`, and the NPC responds in-character
- **Thread model:** game-object access stays on the main thread; file I/O and LLM requests happen on worker threads; replies are queued back to the world thread

## Major features

- Real-player-only NPC chat
- Targeted NPC identity capture
- Shared NPC memory
- Personal player-to-NPC memory
- Editable global prompt file
- Shared NPC character prompt files
- Personal NPC prompt files
- Reusable sub-prompt archetype system
- In-game `.npcc` command system
- Trusted account allowlist for non-GM worldbuilders
- Public NPC chat by normal targeted `/say`
- Private NPC chat with `!p`
- Hostile NPC parley at distance
- Hostile close-range and combat conversation support
- NPC/player combat context injected into hostile prompts
- AI-generated shared or personal character prompts with `.npcc gen`

## How talking works

### Public NPC chat

Target an NPC and speak normally in `/say`:

```text
Hello there.
```

If `NpcChat.RequirePrefix = 0`, this is public NPC chat. The NPC replies through normal speech.

### Private NPC chat

Target an NPC and start the message with `!p`:

```text
!p Keep this between us.
```

This forces a targeted/private NPC reply to the player.

`!p` is checked before normal prefix stripping, so it works even if `NpcChat.Prefix = !`.

### Optional normal prefix mode

If you want normal NPC chat to require a prefix:

```ini
NpcChat.RequirePrefix = 1
NpcChat.Prefix = !
```

Then public chat becomes:

```text
!Hello there.
```

Private chat remains:

```text
!p Keep this between us.
```


## Runtime config troubleshooting

If **all** NPC and playerbot LLM chat stops at once, run:

```text
.npcc status
```

This reports the effective `NpcChat.Enable`, API/model presence, how many `NpcChat.*` options ConfigMgr actually loaded, and the runtime config path. If it reports zero loaded keys, the problem is the deployed module config, not NPC selection/history.

After editing the deployed module config, use:

```text
.npcc reload
```

The command now rereads AzerothCore module config files from disk before refreshing NPC Chat values. Older builds only reread values already cached by ConfigMgr, which made a changed file appear to be ignored until restart.

Keep only one copy of each `NpcChat.*` option in the runtime file. The source `conf/mod_npcchat.conf.dist` is now canonical and no longer contains conflicting duplicate QuestBarks/HostileFirstTalk blocks.

## Runtime file layout

The module stores prompts and memory under `NpcChat.HistoryPath`.

Default:

```text
AI_RP/npc_history/
```

Typical layout:

```text
AI_RP/
└─ npc_history/
   ├─ default.prompt
   ├─ subprompts/
   │  ├─ alliance.prompt
   │  ├─ human.prompt
   │  ├─ innkeeper.prompt
   │  └─ defias_bandit.prompt
   ├─ shared/
   │  ├─ Edwin_VanCleef_639.prompt
   │  ├─ Edwin_VanCleef_639.subprompts
   │  └─ Edwin_VanCleef_639.history
   └─ personal/
      └─ PlayerName_PlayerGuid/
         ├─ Edwin_VanCleef_639.prompt
         ├─ Edwin_VanCleef_639.subprompts
         └─ Edwin_VanCleef_639.history
```

## Prompt layers

When a player talks to an NPC, the module builds a prompt from layered context:

1. Hardcoded game identity wrapper
2. NPC data captured from the live target
3. `default.prompt`
4. Shared attached sub-prompts
5. Shared NPC character prompt
6. Personal attached sub-prompts
7. Personal NPC prompt
8. Shared NPC history
9. Personal player/NPC history
10. Current player message

The code knows **who** the NPC is. Prompt files control **how** that NPC behaves.

## Sub-prompts

Sub-prompts are reusable archetype blocks.

Examples:

```text
alliance
human
defias_bandit
innkeeper
warrior_trainer
hostile_parley
stern_authority
duskwood_local
```

Sub-prompt files live here:

```text
AI_RP/npc_history/subprompts/
```

Example file:

```text
AI_RP/npc_history/subprompts/defias_bandit.prompt
```

Attach shared sub-prompts to a targeted NPC:

```text
.npcc sub attach shared human
.npcc sub attach shared defias_bandit
.npcc sub attach shared hostile_parley
```

Attach personal sub-prompts:

```text
.npcc sub attach personal suspicious
```

For normal players, `.npcc sub attach <name>` defaults to personal. For GMs and trusted creator accounts, it defaults to shared.

## Unique NPC character prompts

For famous named NPCs and bosses, use a shared character prompt instead of only generic sub-prompts.

Example:

```text
AI_RP/npc_history/shared/Edwin_VanCleef_639.prompt
```

The number `639` is the NPC's `creature_template.entry`, not an item ID.

The module builds the NPC key from:

```text
NPC name + creature entry
```

Example:

```text
Edwin VanCleef + 639 = Edwin_VanCleef_639
```

## AI character prompt generation

The `.npcc gen` command can generate a shared or personal character prompt for the currently targeted NPC.

Preview only:

```text
.npcc gen preview
.npcc gen preview "focus on his bitterness toward Stormwind"
```

Generate and save a shared character prompt:

```text
.npcc gen shared
.npcc gen shared "focus on his betrayal by Stormwind and pride as a master stonemason"
```

Generate and save a personal prompt:

```text
.npcc gen personal
.npcc gen personal "he remembers that I mocked him before he killed me"
```

The generator captures useful target information before the worker thread runs, including:

- NPC name
- creature entry
- subname/title
- level
- gender
- creature type
- rank
- NPC flags / role
- zone
- friendly/hostile stance
- combat state
- NPC health percent
- player health percent
- attached shared sub-prompts
- existing shared prompt if present
- extra direction from the command

Generated files are saved under `shared/` or `personal/` depending on the command.

## Hostile NPC communication

Hostile NPCs can talk at range, up close, and during combat if enabled.

The module captures combat context such as:

- NPC health percent
- player health percent
- distance
- whether the NPC is in combat
- whether the player is in combat
- whether the NPC is directly targeting the player
- an approximate fight read such as winning, losing, or close fight

The prompt tells the model to turn this into natural hostile dialogue rather than saying exact health percentages.

Example behaviors:

```text
The NPC is badly wounded and desperate.
The NPC is still strong and threatening.
The player is losing badly.
The fight is close.
```

## Commands

Root command:

```text
.npcc
```

Help:

```text
.npcc help
```

Reload module config/prompt state:

```text
.npcc reload
```

Show account and allowlist status:

```text
.npcc account
```

Reset history for the targeted NPC:

```text
.npcc reset
```

Prompt management:

```text
.npcc prompt
.npcc prompt "personal prompt text"
.npcc prompt shared
.npcc prompt shared "shared NPC prompt text"
.npcc prompt default
.npcc prompt default "global default prompt text"
```

Generator:

```text
.npcc gen preview [quoted extra direction]
.npcc gen shared [quoted extra direction]
.npcc gen personal [quoted extra direction]
```

Sub-prompt commands:

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

## Permissions

Normal players can:

- talk to NPCs
- use private `!p` chat
- create personal NPC prompts
- attach/detach/clear personal sub-prompts
- reset their own personal history with the target NPC

Trusted sub-prompt creator accounts can also:

- create reusable sub-prompts
- attach/detach/clear shared sub-prompts
- build shared NPC archetype layers without logging into a GM character

GMs can:

- edit shared NPC prompts
- edit the global default prompt
- create and manage shared sub-prompts
- use admin-level reset behavior

Trusted accounts are configured by account ID:

```ini
NpcChat.SubPromptCreatorAccounts = 1,7,42
```

The module also accepts these aliases:

```ini
NpcChat.SubPromptCreatorAccountIds = 1,7,42
NpcChat.SubPromptCreatorAccountIDs = 1,7,42
```

Use this command in-game to find the account ID:

```text
.npcc account
```

## Configuration

Example config:

```ini
###################################################################################################
# NPC Chat LLM
###################################################################################################

NpcChat.Enable = 1
NpcChat.BaseUrl = https://openrouter.ai/api/v1
NpcChat.ApiKey =
NpcChat.Model =
NpcChat.MaxResponseTokens = 180
NpcChat.Temperature = 0.85
NpcChat.RequestTimeoutSec = 30
NpcChat.ModelExtraParameters = 'frequency_penalty':0.3,'presence_penalty':0.2

NpcChat.GeneratePromptMaxTokens = 700
NpcChat.GeneratePromptTemperature = 0.75

NpcChat.HistoryPath = ./AI_RP/npc_history
NpcChat.HistoryMaxLines = 20
NpcChat.SharedHistoryMaxLines = 12
NpcChat.PersonalHistoryMaxLines = 20
NpcChat.NameByEntry = 1

NpcChat.TriggerRange = 25.0
NpcChat.RequirePrefix = 0
NpcChat.Prefix = !

NpcChat.AllowHostileChat = 1
NpcChat.HostileAllowCloseChat = 1
NpcChat.HostileAllowCombatChat = 1
NpcChat.HostileMinDistance = 30.0
NpcChat.HostileMaxDistance = 100.0
NpcChat.HostileForcePrivateReply = 0

NpcChat.SubPromptCreatorAccounts =
```

## SQL command registration

The root `.npcc` command should be registered with player-level access. The C++ command handler enforces the higher-level subcommand permissions internally.

Recommended design:

```text
.npcc = available to players
GM/trusted-only actions = enforced in code
```

## Build

Standard AzerothCore module build flow:

```bat
cd /d Z:\ac\build
ninja -j4
```

For a clean rebuild:

```bat
cd /d Z:\ac\build
ninja clean
ninja -j4
```

## Safety notes

The module intentionally avoids using live `Player*` or `Creature*` pointers off-thread.

The main thread captures primitive data into request structs. The worker thread performs prompt assembly, file I/O, and the LLM request. Replies are queued back to `WorldScript::OnUpdate`, where the live player/NPC are looked up again before sending speech.

## Recommended workflow

For a new important NPC:

```text
Target NPC
.npcc gen preview "optional direction"
.npcc gen shared "optional direction"
.npcc sub attach shared human
.npcc sub attach shared defias_bandit
.npcc sub attach shared hostile_parley
Talk to NPC
```

For personal relationship flavor:

```text
Target NPC
.npcc gen personal "he remembers our last hostile conversation"
!p You remember me, don't you?
```

## Known limitations

- This is experimental and private-server oriented.
- LLM response quality depends heavily on the model and prompt files.
- The module does not yet store prompt bindings in the database.
- Sub-prompt attachment is currently file-backed.
- Automatic sub-prompt suggestion is not implemented yet.
- Prompt generation is useful but should still be reviewed by a human.
- Hostile combat talk is reactive; it is not intended to spam automatic fight barks.

## Stabilized transport and health checks

Direct targeted NPC conversation is the protected baseline. Automatic history whispers, social
playerbot chatter, guild/LFG surfaces, and generation jobs use lower-priority transport lanes so they
cannot consume every configured LLM slot. `NpcChat.Api.ReserveInteractiveSlots` controls the reserved
capacity (default `1` out of `4`).

The transport no longer turns connection/provider failures into unexplained silence. It tracks
request/success/failure/capacity counters, records the latest safe error, rate-limits repeated log
messages, and retries only failures that are plausibly transient (transport errors, HTTP 408/425/429,
and 5xx responses).

Runtime commands:

```text
.npcc status
.npcc health
.npcc health test
.npcc health reset
```

`.npcc health test` performs one tiny asynchronous model request and reports the result back in game;
it never blocks the world thread and never prints the API key or prompt contents to the server log.
