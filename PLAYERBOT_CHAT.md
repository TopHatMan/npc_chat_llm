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
characters/Monica.card.txt
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

## Chat surfaces

The same card and recent-history prompt path is used for:

- direct whisper to a playerbot
- targeted `/say`
- party and raid messages that name a bot
- player-provoked guild replies
- General/Trade LFG matching
- optional General ambient replies

Guild and LFG add only a small current-situation hint; they do not maintain a second character-card implementation.

## Quick test checklist

1. Move or rename any existing card for a test bot so no `characters/<BotName>.card.txt` exists.
2. Whisper the bot and confirm the starter `.card.txt` file appears.
3. Confirm the bot replies without a second card-generation LLM request.
4. Edit the new card with a very obvious personality or phrase and save it.
5. Talk to the bot again and confirm the next reply reflects the edit without restarting or reloading.
6. Repeat through targeted `/say`, party/raid mention, guild mention, and LFG to confirm every surface uses the same card.
7. Confirm recent history remains under `NpcChat.HistoryPath/bots/personal/` and no new GUID-based `.card` files are created.
