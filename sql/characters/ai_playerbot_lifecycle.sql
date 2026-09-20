-- Persistent random-bot retirement state and audit data.
-- DEVELOPMENT SCHEMA: do not apply to production yet. Retirement execution
-- remains unavailable pending isolated Linux/MariaDB acceptance gates.
-- This script is intentionally non-destructive so it can be applied to an
-- existing characters database without resetting an in-progress lifecycle.

CREATE TABLE IF NOT EXISTS `ai_playerbot_lifecycle_auction_return` (
  `auction_id` int(10) unsigned NOT NULL,
  `owner` int(10) unsigned NOT NULL,
  `item_guid` int(10) unsigned NOT NULL,
  `item_entry` int(10) unsigned NOT NULL,
  `item_count` int(10) unsigned NOT NULL,
  `expires_at` bigint(20) unsigned NOT NULL,
  `mail_id` int(10) unsigned NOT NULL,
  `settled_at` bigint(20) unsigned NOT NULL,
  PRIMARY KEY (`auction_id`),
  KEY `owner` (`owner`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- A pending seller payout is credited directly to escrow with the normal core
-- bid + refunded deposit - cut amount. Receipt and auction removal are atomic.
-- Reused auction IDs conflict and quarantine; they never replay an old payout.
CREATE TABLE IF NOT EXISTS `ai_playerbot_lifecycle_auction` (
  `auction_id` int(10) unsigned NOT NULL,
  `owner` int(10) unsigned NOT NULL,
  `bidder` int(10) unsigned NOT NULL,
  `item_entry` int(10) unsigned NOT NULL,
  `bid` int(10) unsigned NOT NULL,
  `deposit` int(10) unsigned NOT NULL,
  `cut` int(10) unsigned NOT NULL,
  `proceeds` bigint(20) unsigned NOT NULL,
  `expires_at` bigint(20) unsigned NOT NULL,
  `payout_at` bigint(20) unsigned NOT NULL,
  `settled_at` bigint(20) unsigned NOT NULL,
  PRIMARY KEY (`auction_id`),
  KEY `owner` (`owner`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- Durable receipt for a single serialized core asset save. A receipt is inserted
-- in the very transaction that saves removal/collection and increments escrow.
-- No item/mail operation is considered complete without this receipt.
CREATE TABLE IF NOT EXISTS `ai_playerbot_lifecycle_operation` (
  `guid` int(10) unsigned NOT NULL,
  `kind` tinyint(3) unsigned NOT NULL,
  `asset_guid` int(10) unsigned NOT NULL,
  `item_entry` int(10) unsigned NOT NULL DEFAULT '0',
  `item_count` int(10) unsigned NOT NULL DEFAULT '0',
  `proceeds` bigint(20) unsigned NOT NULL DEFAULT '0',
  `source_type` tinyint(3) unsigned NOT NULL DEFAULT '0',
  `completed_at` bigint(20) unsigned NOT NULL,
  PRIMARY KEY (`guid`,`kind`,`asset_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS `ai_playerbot_lifecycle` (
  `guid` int(10) unsigned NOT NULL,
  `account` int(10) unsigned NOT NULL,
  `status` tinyint(3) unsigned NOT NULL DEFAULT '0',
  `generation` int(10) unsigned NOT NULL DEFAULT '0',
  `protected` tinyint(3) unsigned NOT NULL DEFAULT '0',
  `max_level_since` bigint(20) unsigned NOT NULL DEFAULT '0',
  `retirement_started_at` bigint(20) unsigned NOT NULL DEFAULT '0',
  `retired_at` bigint(20) unsigned NOT NULL DEFAULT '0',
  `disposition` tinyint(3) unsigned NOT NULL DEFAULT '0',
  `disposition_completed` tinyint(3) unsigned NOT NULL DEFAULT '0',
  `starting_gold` bigint(20) unsigned NOT NULL DEFAULT '0',
  `auction_income` bigint(20) unsigned NOT NULL DEFAULT '0',
  `vendor_income` bigint(20) unsigned NOT NULL DEFAULT '0',
  `auction_fees` bigint(20) unsigned NOT NULL DEFAULT '0',
  `estate_escrow` bigint(20) unsigned NOT NULL DEFAULT '0',
  `estate_final` bigint(20) unsigned NOT NULL DEFAULT '0',
  `sink_percent` tinyint(3) unsigned NOT NULL DEFAULT '0',
  `gold_sunk` bigint(20) unsigned NOT NULL DEFAULT '0',
  `inheritance_planned` bigint(20) unsigned NOT NULL DEFAULT '0',
  `gold_distributed` bigint(20) unsigned NOT NULL DEFAULT '0',
  `estate_prepared` tinyint(3) unsigned NOT NULL DEFAULT '0',
  `auction_attempts` int(10) unsigned NOT NULL DEFAULT '0',
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

CREATE TABLE IF NOT EXISTS `ai_playerbot_lifecycle_inheritance` (
  `retiring_guid` int(10) unsigned NOT NULL,
  `recipient_guid` int(10) unsigned NOT NULL,
  `amount` bigint(20) unsigned NOT NULL,
  `delivered_amount` bigint(20) unsigned NOT NULL DEFAULT '0',
  `delivered` tinyint(3) unsigned NOT NULL DEFAULT '0',
  `created_at` bigint(20) unsigned NOT NULL,
  `updated_at` bigint(20) unsigned NOT NULL,
  PRIMARY KEY (`retiring_guid`,`recipient_guid`),
  KEY `delivery_queue` (`delivered`,`created_at`),
  KEY `recipient_guid` (`recipient_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS `ai_playerbot_lifecycle_item` (
  `retiring_guid` int(10) unsigned NOT NULL,
  `item_guid` int(10) unsigned NOT NULL,
  `item_entry` int(10) unsigned NOT NULL,
  `action` tinyint(3) unsigned NOT NULL,
  `proceeds` bigint(20) unsigned NOT NULL DEFAULT '0',
  `status` tinyint(3) unsigned NOT NULL DEFAULT '0',
  `updated_at` bigint(20) unsigned NOT NULL,
  PRIMARY KEY (`retiring_guid`,`item_guid`),
  KEY `pending_items` (`retiring_guid`,`status`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;
