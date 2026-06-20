-- mod-npc-chat-llm command registration for AzerothCore world.command
--
-- Current C++ design:
--   The module registers one root command: .npcc
--   Subcommands are parsed inside NpcChat_CommandScript in mod_npcchat.cpp.
--
-- Security values used by AzerothCore command table:
--   0 = regular player
--   1 = moderator
--   2 = gamemaster
--   3 = administrator
--
-- Why only one SQL row?
--   The current C++ command table exposes only the root command "npcc".
--   Player-vs-GM rules for subcommands are enforced inside the C++ handler.
--
-- Regular player access:
--   .npcc help
--   .npcc reload
--   .npcc reset
--   .npcc prompt
--   .npcc prompt "personal prompt text"
--
-- GM-only access, enforced in C++:
--   .npcc prompt shared
--   .npcc prompt shared "shared prompt text"
--   .npcc prompt default
--   .npcc prompt default "default prompt text"
--   .npcc reset also performs a wider GM reset for the targeted NPC

DELETE FROM `command`
WHERE `name` = 'npcc';

INSERT INTO `command` (`name`, `security`, `help`) VALUES
(
    'npcc',
    0,
    'Syntax: .npcc help | .npcc reload | .npcc reset | .npcc prompt ["text"] | GM: .npcc prompt shared ["text"] | GM: .npcc prompt default ["text"]. NPC Chat LLM command root. Regular players may reset their personal target-NPC history and create/edit personal prompts. GM-only shared/default prompt editing is enforced in C++.'
);
