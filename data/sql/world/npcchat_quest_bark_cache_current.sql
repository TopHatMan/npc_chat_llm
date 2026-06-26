-- ---------------------------------------------------------------------------
-- NPC Chat LLM - Hostile disable table + quest bark level columns
-- Run in the WORLD database.
-- ---------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS `npcchat_npc_bark_disable` (
    `npc_entry` INT UNSIGNED NOT NULL,
    `bark_context` VARCHAR(64) NOT NULL,
    `disabled` TINYINT UNSIGNED NOT NULL DEFAULT 1,
    `reason` VARCHAR(255) NOT NULL DEFAULT '',
    `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `updated_at` TIMESTAMP NULL DEFAULT NULL ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`npc_entry`, `bark_context`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Disable hostile first-talk for specific bosses/NPCs that already have scripted intro dialogue.
-- Example:
-- REPLACE INTO `npcchat_npc_bark_disable`
-- (`npc_entry`, `bark_context`, `disabled`, `reason`)
-- VALUES
-- (639, 'hostile_first_talk', 1, 'VanCleef already has encounter/scripted dialogue');

-- Add level metadata to quest bark cache without dropping existing cache rows.
SET @db := DATABASE();

SET @sql := (
    SELECT IF(
        COUNT(*) = 0,
        'ALTER TABLE `npcchat_quest_bark_cache` ADD COLUMN `min_level` TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER `phase`',
        'SELECT 1'
    )
    FROM INFORMATION_SCHEMA.COLUMNS
    WHERE TABLE_SCHEMA = @db
      AND TABLE_NAME = 'npcchat_quest_bark_cache'
      AND COLUMN_NAME = 'min_level'
);
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @sql := (
    SELECT IF(
        COUNT(*) = 0,
        'ALTER TABLE `npcchat_quest_bark_cache` ADD COLUMN `max_level` TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER `min_level`',
        'SELECT 1'
    )
    FROM INFORMATION_SCHEMA.COLUMNS
    WHERE TABLE_SCHEMA = @db
      AND TABLE_NAME = 'npcchat_quest_bark_cache'
      AND COLUMN_NAME = 'max_level'
);
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @sql := (
    SELECT IF(
        COUNT(*) = 0,
        'ALTER TABLE `npcchat_quest_bark_cache` ADD COLUMN `generated_level` TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER `max_level`',
        'SELECT 1'
    )
    FROM INFORMATION_SCHEMA.COLUMNS
    WHERE TABLE_SCHEMA = @db
      AND TABLE_NAME = 'npcchat_quest_bark_cache'
      AND COLUMN_NAME = 'generated_level'
);
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- Backfill existing cache rows from quest_template using the first quest ID in quest_ids.
-- For combined quest keys, this uses the first quest as a safe lower bound; future generated rows
-- will write exact metadata from the code.
UPDATE `npcchat_quest_bark_cache` qbc
LEFT JOIN `quest_template` qt
  ON qt.`ID` = CAST(SUBSTRING_INDEX(qbc.`quest_ids`, ',', 1) AS UNSIGNED)
SET
    qbc.`min_level` = COALESCE(NULLIF(qt.`MinLevel`, 0), qbc.`min_level`),
    qbc.`generated_level` = IF(qbc.`generated_level` = 0, COALESCE(NULLIF(qt.`MinLevel`, 0), 0), qbc.`generated_level`)
WHERE qbc.`min_level` = 0;
