-- DEVELOPMENT ONLY: additive broker-estate schema. Does not provision accounts
-- or enable the hard-disabled worker.
-- No FK cascades to characters: original character deletion MUST preserve estates.
-- Apply alongside ai_playerbot_lifecycle.sql only to isolated cloned test DBs.

-- Admin-attested dedicated identities. Runtime never silently registers a player.
-- faction: 0 Alliance, 1 Horde. No logged-in Player is required by the service.
CREATE TABLE IF NOT EXISTS `ai_playerbot_estate_broker` (
  `guid` int(10) unsigned NOT NULL,
  `account` int(10) unsigned NOT NULL,
  `faction` tinyint(3) unsigned NOT NULL,
  `role` tinyint(3) unsigned NOT NULL DEFAULT '0',
  `enabled` tinyint(3) unsigned NOT NULL DEFAULT '0',
  PRIMARY KEY (`guid`),
  UNIQUE KEY `faction_role` (`faction`,`role`),
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
  `auction_fees` bigint(20) unsigned NOT NULL DEFAULT '0',
  `gold_sunk` bigint(20) unsigned NOT NULL DEFAULT '0',
  `gold_distributed` bigint(20) unsigned NOT NULL DEFAULT '0',
  `final_estate` bigint(20) unsigned NOT NULL DEFAULT '0',
  `sink_percent` tinyint(3) unsigned NOT NULL DEFAULT '0',
  `next_action_at` bigint(20) unsigned NOT NULL DEFAULT '0',
  `updated_at` bigint(20) unsigned NOT NULL DEFAULT '0',
  PRIMARY KEY (`estate_id`),
  UNIQUE KEY `original_guid` (`original_guid`),
  KEY `work_queue` (`status`,`next_action_at`,`estate_id`),
  KEY `broker` (`broker_guid`,`status`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS `ai_playerbot_estate_inheritance` (
  `estate_id` bigint(20) unsigned NOT NULL,
  `recipient_guid` int(10) unsigned NOT NULL,
  `amount` bigint(20) unsigned NOT NULL,
  `delivered_amount` bigint(20) unsigned NOT NULL DEFAULT '0',
  `delivered` tinyint(3) unsigned NOT NULL DEFAULT '0',
  `created_at` bigint(20) unsigned NOT NULL,
  `updated_at` bigint(20) unsigned NOT NULL,
  PRIMARY KEY (`estate_id`,`recipient_guid`),
  KEY `delivery_queue` (`delivered`,`estate_id`)
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

-- Permanent mapping between a normal core auction and the originating estate.
-- History is retained so ObjectMgr can keep auction IDs above all prior receipts.
CREATE TABLE IF NOT EXISTS `ai_playerbot_estate_auction` (
  `auction_id` int(10) unsigned NOT NULL,
  `estate_id` bigint(20) unsigned NOT NULL,
  `lot_id` bigint(20) unsigned NOT NULL DEFAULT '0',
  `broker_guid` int(10) unsigned NOT NULL,
  `house_id` int(10) unsigned NOT NULL,
  `item_guid` int(10) unsigned NOT NULL DEFAULT '0',
  `item_entry` int(10) unsigned NOT NULL,
  `item_count` int(10) unsigned NOT NULL,
  `attempt` int(10) unsigned NOT NULL,
  `status` tinyint(3) unsigned NOT NULL DEFAULT '0',
  `start_bid` int(10) unsigned NOT NULL,
  `buyout` int(10) unsigned NOT NULL,
  `bidder` int(10) unsigned NOT NULL DEFAULT '0',
  `bid` int(10) unsigned NOT NULL DEFAULT '0',
  `deposit` int(10) unsigned NOT NULL,
  `auction_cut` int(10) unsigned NOT NULL DEFAULT '0',
  `expires_at` bigint(20) unsigned NOT NULL,
  `payout_at` bigint(20) unsigned NOT NULL DEFAULT '0',
  `winner_mail_id` int(10) unsigned NOT NULL DEFAULT '0',
  `seller_mail_id` int(10) unsigned NOT NULL DEFAULT '0',
  `return_mail_id` int(10) unsigned NOT NULL DEFAULT '0',
  `updated_at` bigint(20) unsigned NOT NULL DEFAULT '0',
  PRIMARY KEY (`auction_id`),
  UNIQUE KEY `lot_attempt` (`lot_id`,`attempt`),
  KEY `work_queue` (`status`,`payout_at`,`auction_id`),
  KEY `estate_status` (`estate_id`,`status`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- Claims inherited from the retiring character's existing bids. The bidder
-- broker is distinct from the listing broker, preventing service self-bids.
CREATE TABLE IF NOT EXISTS `ai_playerbot_estate_bid_claim` (
  `auction_id` int(10) unsigned NOT NULL,
  `estate_id` bigint(20) unsigned NOT NULL,
  `broker_guid` int(10) unsigned NOT NULL,
  `bid` int(10) unsigned NOT NULL,
  `status` tinyint(3) unsigned NOT NULL DEFAULT '0',
  `mail_id` int(10) unsigned NOT NULL DEFAULT '0',
  `item_guid` int(10) unsigned NOT NULL DEFAULT '0',
  `item_entry` int(10) unsigned NOT NULL DEFAULT '0',
  `item_count` int(10) unsigned NOT NULL DEFAULT '0',
  `updated_at` bigint(20) unsigned NOT NULL DEFAULT '0',
  PRIMARY KEY (`auction_id`),
  KEY `work_queue` (`status`,`auction_id`),
  KEY `estate_status` (`estate_id`,`status`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- Immutable bid revisions for service-associated auctions. Buyer debit, old
-- bidder refund, winner delivery and auction mutation share this receipt's txn.
CREATE TABLE IF NOT EXISTS `ai_playerbot_estate_auction_bid` (
  `auction_id` int(10) unsigned NOT NULL,
  `bid` int(10) unsigned NOT NULL,
  `old_bidder` int(10) unsigned NOT NULL,
  `old_bid` int(10) unsigned NOT NULL,
  `bidder` int(10) unsigned NOT NULL,
  `bidder_account` int(10) unsigned NOT NULL,
  `money_before` int(10) unsigned NOT NULL,
  `refund_mail_id` int(10) unsigned NOT NULL DEFAULT '0',
  `winner_mail_id` int(10) unsigned NOT NULL DEFAULT '0',
  `payout_at` bigint(20) unsigned NOT NULL DEFAULT '0',
  `completed_at` bigint(20) unsigned NOT NULL,
  PRIMARY KEY (`auction_id`,`bid`),
  KEY `bidder` (`bidder`,`completed_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;
