-- DEVELOPMENT ONLY: additive broker-estate schema. Does not provision accounts,
-- select bots for retirement, transfer assets, or enable the hard-disabled worker.
-- No FK cascades to characters: original character deletion MUST preserve estates.
-- Apply alongside ai_playerbot_lifecycle.sql only to isolated cloned test DBs.

-- Admin-attested dedicated identities. Runtime never silently registers a player.
-- faction: 0 Alliance, 1 Horde. No logged-in Player is required by the service.
CREATE TABLE IF NOT EXISTS `ai_playerbot_estate_broker` (
  `guid` int(10) unsigned NOT NULL,
  `account` int(10) unsigned NOT NULL,
  `faction` tinyint(3) unsigned NOT NULL,
  `enabled` tinyint(3) unsigned NOT NULL DEFAULT '0',
  PRIMARY KEY (`guid`),
  KEY `account` (`account`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS `ai_playerbot_estate` (
  `estate_id` bigint(20) unsigned NOT NULL AUTO_INCREMENT,
  `original_guid` int(10) unsigned NOT NULL,
  `original_account` int(10) unsigned NOT NULL,
  `broker_guid` int(10) unsigned NOT NULL,
  `broker_account` int(10) unsigned NOT NULL,
  `status` tinyint(3) unsigned NOT NULL DEFAULT '0',
  `assets_detached` tinyint(3) unsigned NOT NULL DEFAULT '0',
  `escrow` bigint(20) unsigned NOT NULL DEFAULT '0',
  `starting_gold` bigint(20) unsigned NOT NULL DEFAULT '0',
  `vendor_income` bigint(20) unsigned NOT NULL DEFAULT '0',
  `auction_income` bigint(20) unsigned NOT NULL DEFAULT '0',
  `auction_deposits` bigint(20) unsigned NOT NULL DEFAULT '0',
  `auction_cuts` bigint(20) unsigned NOT NULL DEFAULT '0',
  `gold_sunk` bigint(20) unsigned NOT NULL DEFAULT '0',
  `gold_distributed` bigint(20) unsigned NOT NULL DEFAULT '0',
  `next_action_at` bigint(20) unsigned NOT NULL DEFAULT '0',
  `updated_at` bigint(20) unsigned NOT NULL DEFAULT '0',
  PRIMARY KEY (`estate_id`),
  UNIQUE KEY `original_guid` (`original_guid`),
  KEY `work_queue` (`status`,`next_action_at`,`estate_id`),
  KEY `broker` (`broker_guid`,`status`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- Lots retain exact item GUIDs; no cross-estate merging or inventory auto-stacking.
-- item_guid remains an audit identity after the item is sold/destroyed.
CREATE TABLE IF NOT EXISTS `ai_playerbot_estate_lot` (
  `lot_id` bigint(20) unsigned NOT NULL AUTO_INCREMENT,
  `estate_id` bigint(20) unsigned NOT NULL,
  `item_guid` int(10) unsigned NOT NULL,
  `item_entry` int(10) unsigned NOT NULL,
  `item_count` int(10) unsigned NOT NULL,
  `status` tinyint(3) unsigned NOT NULL DEFAULT '0',
  `attempt` int(10) unsigned NOT NULL DEFAULT '0',
  `auction_id` int(10) unsigned NOT NULL DEFAULT '0',
  `next_action_at` bigint(20) unsigned NOT NULL DEFAULT '0',
  `updated_at` bigint(20) unsigned NOT NULL DEFAULT '0',
  PRIMARY KEY (`lot_id`),
  UNIQUE KEY `estate_item` (`estate_id`,`item_guid`),
  KEY `work_queue` (`status`,`next_action_at`,`lot_id`),
  KEY `item_guid` (`item_guid`),
  KEY `auction_id` (`auction_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- One immutable outcome per operation identity. No receipt is written separately
-- from the ownership/money mutation that it confirms.
CREATE TABLE IF NOT EXISTS `ai_playerbot_estate_operation` (
  `estate_id` bigint(20) unsigned NOT NULL,
  `kind` tinyint(3) unsigned NOT NULL,
  `asset_id` bigint(20) unsigned NOT NULL,
  `item_entry` int(10) unsigned NOT NULL DEFAULT '0',
  `item_count` int(10) unsigned NOT NULL DEFAULT '0',
  `amount` bigint(20) unsigned NOT NULL DEFAULT '0',
  `completed_at` bigint(20) unsigned NOT NULL,
  PRIMARY KEY (`estate_id`,`kind`,`asset_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;
