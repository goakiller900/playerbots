-- Persistent random-bot retirement state and audit data.
-- DEVELOPMENT SCHEMA: do not apply to production yet. Retirement execution
-- remains unavailable pending isolated Linux/MariaDB acceptance gates.
-- This script is intentionally non-destructive so it can be applied to an
-- existing characters database without resetting an in-progress lifecycle.

CREATE TABLE IF NOT EXISTS `ai_playerbot_lifecycle` (
  `guid` int(10) unsigned NOT NULL,
  `account` int(10) unsigned NOT NULL,
  `status` tinyint(3) unsigned NOT NULL DEFAULT '0',
  `protected` tinyint(3) unsigned NOT NULL DEFAULT '0',
  `max_level_since` bigint(20) unsigned NOT NULL DEFAULT '0',
  `retirement_started_at` bigint(20) unsigned NOT NULL DEFAULT '0',
  `retired_at` bigint(20) unsigned NOT NULL DEFAULT '0',
  `disposition` tinyint(3) unsigned NOT NULL DEFAULT '0',
  `disposition_completed` tinyint(3) unsigned NOT NULL DEFAULT '0',
  `replacement_required` tinyint(3) unsigned NOT NULL DEFAULT '0',
  `replacement_status` tinyint(3) unsigned NOT NULL DEFAULT '0',
  `replacement_guid` int(10) unsigned NOT NULL DEFAULT '0',
  `replacement_account` int(10) unsigned NOT NULL DEFAULT '0',
  `replacement_name` varchar(12) NOT NULL DEFAULT '',
  `replacement_race` tinyint(3) unsigned NOT NULL DEFAULT '0',
  `replacement_class` tinyint(3) unsigned NOT NULL DEFAULT '0',
  `replacement_gender` tinyint(3) unsigned NOT NULL DEFAULT '0',
  `updated_at` bigint(20) unsigned NOT NULL DEFAULT '0',
  PRIMARY KEY (`guid`),
  KEY `status_updated` (`status`,`updated_at`),
  KEY `account_status` (`account`,`status`),
  KEY `replacement_guid` (`replacement_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;
