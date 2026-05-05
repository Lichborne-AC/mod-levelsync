-- mod-levelsync v2 migration: add founder_guid to levelsync_groups
--
-- Fixes a race condition in CreateGroup() where the previous code relied on
-- LAST_INSERT_ID() across MySQL connections. With AzerothCore's connection
-- pool, the INSERT and the SELECT LAST_INSERT_ID() can land on different
-- connections, returning either 0 or an unrelated auto_increment value.
--
-- The fix: write a deterministic lookup key (the founder character's GUID)
-- into the new row, then SELECT WHERE founder_guid = ? ORDER BY group_id
-- DESC LIMIT 1 to find the most recently created group for that founder.
-- This works regardless of which connection serves the SELECT.
--
-- Idempotent: skips the ALTER if the column already exists (safe for fresh
-- installs where the base schema already includes founder_guid).

SET @dbname = DATABASE();
SET @preparedStatement = (
    SELECT IF(
        (SELECT COUNT(*) FROM INFORMATION_SCHEMA.COLUMNS
         WHERE TABLE_SCHEMA = @dbname
           AND TABLE_NAME   = 'levelsync_groups'
           AND COLUMN_NAME  = 'founder_guid') > 0,
        'SELECT 1',
        'ALTER TABLE `levelsync_groups`
             ADD COLUMN `founder_guid` INT UNSIGNED NOT NULL DEFAULT 0 AFTER `group_id`,
             ADD KEY `idx_founder` (`founder_guid`)'
    )
);
PREPARE alterIfNotExists FROM @preparedStatement;
EXECUTE alterIfNotExists;
DEALLOCATE PREPARE alterIfNotExists;
