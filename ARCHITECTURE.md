# NPC Chat LLM architecture

This document describes the intended runtime boundaries after the stabilization pass. The module grew quickly during experimentation; these boundaries are the rule for future work so new living-world features do not destabilize basic NPC conversation.

## Core invariant

**Direct targeted player -> NPC conversation is the protected baseline.**

A player targeting an NPC and speaking to it must keep working even when optional systems are busy, broken, disabled, or rate-limited. History whispers, bark generation, playerbot social chatter, guild/LFG chatter, and other ambient features are secondary.

## Current source boundaries

### `src/npcchat_llm.h` / `src/npcchat_llm.cpp`

Owns the OpenAI-compatible transport layer.

Responsibilities:

- endpoint normalization
- HTTP/HTTPS transport
- TLS availability/certificate verification behavior
- provider/HTTP/JSON error classification
- transient retry policy
- concurrency admission
- reserved capacity for interactive requests
- transport health counters
- rate-limited safe error logging

It must **never** touch AzerothCore `Player*`, `Creature*`, `WorldSession*`, or other live game objects. Inputs and outputs are copied strings/config/result data.

The public entry point is `NpcChatTransport`, with compatibility helpers such as `NpcChat_CallLLM`.

### `src/mod_npcchat.cpp`

Still contains most game-facing behavior. It owns:

- AzerothCore script hooks
- primitive context capture on the world thread
- NPC prompt/history assembly
- NPC/playerbot reply queues
- world-thread chat emission
- bark/cache systems
- playerbot direct/social/guild/LFG behavior
- guild bot presence
- `.npcc` commands

This file is still larger than desirable. The stabilization pass intentionally avoids a giant mechanical file split while runtime behavior is being repaired.

## Request classes

Transport calls are assigned a class:

### Interactive

Player-driven conversation. It may use the full configured concurrency budget.

Examples:

- direct targeted NPC chat
- direct playerbot whisper/target/party interaction

### Background

Ambient or optional live-world behavior. It must yield capacity to interactive chat.

Examples:

- familiar-NPC history whispers
- multi-bot social chatter
- guild/LFG/general bot surface replies

### Generation

One-time or maintenance generation jobs. These also yield capacity to interactive chat.

Examples:

- character prompt generation
- relationship/hostile/trainer/quest bark generation

### HealthCheck

Explicit `.npcc health test` probes. Treated like interactive traffic so diagnostics remain useful when background traffic is saturated.

## Admission rule

With the default:

```ini
NpcChat.Api.MaxConcurrentRequests = 4
NpcChat.Api.ReserveInteractiveSlots = 1
```

interactive/health traffic may use all 4 slots, while background/generation traffic may use at most 3. This prevents optional chatter from starving basic conversation.

If `MaxConcurrentRequests = 1` and `ReserveInteractiveSlots = 1`, background/generation calls are intentionally denied. Set the reserve to `0` if that is not desired.

## Threading contract

1. **World thread:** capture only the primitive values required by a request.
2. **Worker thread:** file I/O, prompt assembly where safe, and the LLM transport call.
3. **World thread:** resolve live GUIDs again and emit queued chat.

Do not carry live `Player*`/`Creature*` pointers into detached LLM workers.

## Failure behavior

Transport failures must be diagnosable, not silent.

The transport classifies failures such as:

- invalid config
- capacity rejection
- invalid URL
- HTTPS unavailable in the build
- connection/transport failure
- non-success HTTP status
- provider error object
- malformed OpenAI-compatible response
- empty response

Repeated identical log errors are rate-limited. Direct NPC interaction can additionally report a short failure to the player and point to `.npcc health`.

No diagnostic should print the API key or full user/system prompts.

## Runtime diagnostics

```text
.npcc status
.npcc health
.npcc health test
.npcc health reset
```

`status` answers “what config did AzerothCore actually load?”

`health` answers “what is the transport doing/failing on?”

`health test` sends one tiny asynchronous model request and reports success or a classified failure in game.

## Config contract

`scripts/validate_config_contract.py` and `.github/workflows/config-contract.yml` enforce:

- no duplicate active `NpcChat.*` keys in the canonical `.conf.dist`
- every non-legacy source `GetOption("NpcChat.*")` has a canonical config entry
- canonical config entries are actually read by source

Backward-compatible aliases remain explicitly allowlisted rather than silently reappearing in the canonical file.

## Safe future split of `mod_npcchat.cpp`

Once this stabilized build has passed Windows compile/runtime testing, split the remaining large translation unit by behavior rather than by arbitrary line count. A low-risk order is:

1. `npcchat_config.*` — typed configuration snapshot and validation
2. `npcchat_history.*` — file paths, history read/write, relationship memory
3. `npcchat_barks.*` — bark cache/generation/proximity systems
4. `npcchat_playerbots.*` — cards, direct bot chat, social/LFG/guild behavior
5. `npcchat_commands.*` — `.npcc` parsing and presentation
6. `mod_npcchat.cpp` — thin AzerothCore script registration/hooks and world-thread dispatch

Do those moves incrementally with a compiling server between each step. Avoid combining another major feature with a large mechanical extraction.
