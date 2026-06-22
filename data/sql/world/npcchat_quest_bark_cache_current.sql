DROP TABLE IF EXISTS `npcchat_quest_bark_cache`;

CREATE TABLE `npcchat_quest_bark_cache` (
    `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `npc_entry` INT UNSIGNED NOT NULL,
    `quest_key` VARCHAR(128) NOT NULL,
    `quest_ids` VARCHAR(128) NOT NULL,
    `faction` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `race_id` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `class_id` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `phase` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
    `bark_type` VARCHAR(32) NOT NULL DEFAULT 'quest_available',
    `text` TEXT NOT NULL,
    `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `updated_at` TIMESTAMP NULL DEFAULT NULL ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    UNIQUE KEY `uq_npcchat_quest_bark` (`npc_entry`, `quest_key`, `faction`, `race_id`, `class_id`, `phase`, `bark_type`),
    KEY `idx_npcchat_quest_bark_lookup` (`npc_entry`, `quest_key`, `bark_type`),
    KEY `idx_npcchat_quest_bark_specificity` (`npc_entry`, `quest_key`, `faction`, `race_id`, `class_id`, `phase`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;