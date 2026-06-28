-- ---------------------------------------------------------------------------
-- NPC Chat LLM - Per-NPC speak profile (run in the WORLD database)
--
-- The "NPCs in general" table. One row per creature entry (so it applies to every
-- spawn of that creature and is shippable to other servers). The module also creates
-- this table itself (CREATE TABLE IF NOT EXISTS) on first use.
--
--   can_speak     1 = eligible for aggro-driven hostile speak. Generating a hostile
--                 bark (.npcc gen bark hostile) sets this automatically. NPCs with no
--                 row / can_speak=0 never auto-taunt.
--   speak_chance  per-NPC chance override (1-100). 0 = use the global default.
--   npc_kind      quest | regular | hostile | auto  (informational / future routing)
--   tags          explicit archetype tags fed to the auto-linker for things the creature
--                 can't tell us by itself - most importantly humanoid race, e.g.
--                 'dwarf,bronzebeard' or 'human,stormwind'. Comma-separated.
--
-- Manage in-game (GM / configured creator accounts):
--   .npcc profile show
--   .npcc profile speak  <0|1>
--   .npcc profile chance <0-100>
--   .npcc profile kind   <quest|regular|hostile|auto>
--   .npcc profile tag    <csv>
-- ---------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS `npcchat_npc_profile` (
    `npc_entry` INT UNSIGNED NOT NULL,
    `can_speak` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `speak_chance` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `npc_kind` VARCHAR(16) NOT NULL DEFAULT 'auto',
    `tags` VARCHAR(255) NOT NULL DEFAULT '',
    `created_by_account` INT UNSIGNED NOT NULL DEFAULT 0,
    `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `updated_at` TIMESTAMP NULL DEFAULT NULL ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`npc_entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;