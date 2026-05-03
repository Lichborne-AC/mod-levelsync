-- mod-levelsync base schema (group architecture)

CREATE TABLE IF NOT EXISTS `levelsync_groups` (
    `group_id`           INT UNSIGNED    NOT NULL AUTO_INCREMENT,
    `founder_guid`       INT UNSIGNED    NOT NULL DEFAULT 0 COMMENT 'char_guid used to look up the row immediately after INSERT; not authoritative after group creation',
    `level_sync_enabled` TINYINT(1)      NOT NULL DEFAULT 0,
    `sync_progression`   TINYINT(1)      NOT NULL DEFAULT 0,
    PRIMARY KEY (`group_id`),
    KEY `idx_founder` (`founder_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `levelsync_members` (
    `group_id`   INT UNSIGNED    NOT NULL,
    `char_guid`  INT UNSIGNED    NOT NULL,
    `account_id` INT UNSIGNED    NOT NULL,
    PRIMARY KEY (`char_guid`),
    KEY `idx_group_id` (`group_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `levelsync_account_keys` (
    `account_id`   INT UNSIGNED NOT NULL,
    `security_key` VARCHAR(64)  NOT NULL,
    PRIMARY KEY (`account_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
