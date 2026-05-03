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
-- founder_guid is only read during the lookup step inside CreateGroup().
-- After group creation it is not authoritative — players may leave the
-- group, and the founder_guid column may end up referencing a deleted
-- character. That's intentional and harmless.

ALTER TABLE `levelsync_groups`
    ADD COLUMN `founder_guid` INT UNSIGNED NOT NULL DEFAULT 0
        COMMENT 'char_guid used to look up the row immediately after INSERT; not authoritative after group creation'
        AFTER `group_id`,
    ADD KEY `idx_founder` (`founder_guid`);
