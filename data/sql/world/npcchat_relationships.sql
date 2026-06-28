-- ---------------------------------------------------------------------------
-- NPC Chat LLM - Personal relationship table (run in the WORLD database)
--
-- One row per (player, npc_entry). SQL is the source of truth; the on-disk
-- .relationship files remain as a fallback/backup. Reads are SQL-first and a
-- relationship found only on disk is migrated into SQL the first time it's read,
-- so existing files move over as they're touched. The module also creates this
-- table itself (CREATE TABLE IF NOT EXISTS) on first use.
--
-- Why SQL: it makes "which NPCs near me do I already know" a single indexed query,
-- which is what powers the pass-by greeting (an NPC you have a relationship with
-- greets you when you walk near it).
--
--   data    the full key=value relationship blob (score, intimacy, stance,
--           summary, tags, last_contact, plus any custom keys) - round-trips
--           exactly what the .relationship files stored.
--   score / stance are also denormalized into columns for quick filtering.
--
-- Manage in-game:  .npcc rel show | set <...> | tag <name> | summary "..." | clear
-- ---------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS `npcchat_relationship` (
    `player_guid` BIGINT UNSIGNED NOT NULL,
    `npc_entry` INT UNSIGNED NOT NULL,
    `player_name` VARCHAR(64) NOT NULL DEFAULT '',
    `npc_name` VARCHAR(64) NOT NULL DEFAULT '',
    `score` INT NOT NULL DEFAULT 0,
    `stance` VARCHAR(32) NOT NULL DEFAULT '',
    `data` TEXT NOT NULL,
    `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`player_guid`, `npc_entry`),
    KEY `idx_npcchat_rel_player` (`player_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;