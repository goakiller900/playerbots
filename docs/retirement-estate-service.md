# Background retirement estate service (authoritative redesign)

This supersedes the retiring-character-as-AH-seller design in earlier checkpoint
documents. **Implementation is incomplete; retirement remains hard-disabled.**
Neither the broker schema nor an enabled configuration bypasses that gate.

## Durable separation

Character lifecycle and estate accounting have independent state machines:

```
original: pending -> assets escrowed -> archive/delete -> replacement
estate:   intake  -> escrowed -> liquidation -> distribution -> complete
lot:      ready -> listed -> sold
                    | returned -> ready (bounded retry)
                    | fallback -> vendor/destroy/retain
```

The estate ID is an independent 64-bit database identity. Original GUID/account
are immutable historical references, not foreign keys to characters. A lot keeps
its exact item GUID, entry and count. No cross-estate stack merging is permitted.
The broker's Player money is never estate escrow. Retained unsellable items belong
to the service estate, not to a character about to be deleted.

Completed intake must prove there are no unaccounted assets or unresolved intake
operations before permitting character finalization. Financial completion waits
for every lot, auction and tracked mail obligation, independent of whether the
original character still exists. HELD never authorizes destruction or replay.

## Exact-core findings

Reference: mangos-wotlk `1cd9d566ae83c1a88f1b057514697055055f419c`.

- `AuctionHouseMgr::GetAuctionsMap` separates Alliance/Horde/neutral markets unless
  `CONFIG_BOOL_ALLOW_TWO_SIDE_INTERACTION_AUCTION` merges them. Two configurable
  faction brokers are the conservative initial provisioning requirement, even on
  a merged realm. No fabricated seller name is needed.
- `AuctionHouseObject::AddAuction(..., Player*)` assigns owner zero for a null
  Player. It cannot be reused as-is for an offline service identity. Its in-memory
  publication also precedes acknowledgement. An acknowledged offline construction
  path must reuse its normal item/deposit/duration rules before publication.
- `SendAuctionWonMail`, `SendAuctionSuccessfulMail`, `SendAuctionExpiredMail` and
  `MailDraft::SendMailTo` support offline receivers. No walking or service login
  is intrinsically required for delivery. The existing multi-transaction winner
  path still needs retirement-auction-specific atomic persistence.
- `BuildAuctionInfo` sends the auction owner's real character GUID. The client
  resolves the real broker name. Auction mail uses `MAIL_AUCTION` and the house ID
  as sender; winner body encodes seller GUID, bid and buyout. Normal formats must
  remain intact. Returned items/proceeds are addressed to the broker. Attribution
  to an estate must be a separate durable mapping, never mailbox order or parsing
  display text.
- `Player::DeleteFromDB(..., deleteFinally=true)` removes item instances owned by
  the original GUID, inventory links and its received mail. Thus transfer of item
  ownership and removal of original placement/mail links must precede deletion.
  The core API must be retained, not replaced by an ad-hoc SQL deletion list.
- `HandleSellItemOpcode` starts with SellPrice times stack count and scales the
  value for the first negative-charge spell using float arithmetic. Background
  valuation must preserve that rule with checked arithmetic, and refuse corrupt
  or unsupported assets. A generic item entry price alone is insufficient.

## Implemented foundation

- Independent estate/lot states and testable finalization/distribution predicates.
- Four additive InnoDB tables: estate, broker registry, lots, operation receipts.
  Indexed active-work queues avoid scans of historical completed estates.
- Explicit broker configuration and registry-based exclusion from character login
  and random-bot classification. Startup validates account binding, faction,
  offline state, non-random account, dedicated-account contents and permanent auth
  ban. Disabled registry identities remain reserved. The core refuses deletion of
  registered broker characters/accounts. Random-bot provisioning aborts before its
  account-deletion paths if its prefix includes a registered broker account.
- Offline inventory/bank leaf transfer: ownership change, removal of original
  placement, exact lot registration and receipt commit together. The original item
  is not cloned, merged, vendored or destroyed. Tests cover rollback at every write,
  lost commit acknowledgement, conservative replay checks and unsupported assets.
- Existing acknowledged worker transactions, escrow math, login revisions and
  receipt work are retained. The core mail serializer has a queue-only AH entry
  point; it does not independently commit or publish live mail.
- Background vendor/destroy transaction boundary, using core-serialized item
  removal and independent estate escrow. A shared `Item::GetVendorSellValue`
  preserves ordinary vendor arithmetic; the retirement quote rejects overflow,
  corrupt charges, nonempty bags and wrapped/saved-loot items. Durable receipt
  confirmation does not repeat removal or credit after lost acknowledgement.

These are foundations, not a functioning background estate worker. The previous
character-owned AH bid/payout/return stores remain unconnected experimental code
and are NOT the broker settlement implementation. They must be refactored to use
estate/lot/listing identities rather than treating auction owner as original GUID.

## Provisioning (isolated test realm only at this stage)

1. Use the ordinary core account/character creation flow to create dedicated
   service characters: one Alliance, one Horde. Choose valid names yourself.
   Never use random-bot accounts or accounts containing human characters.
2. Log both out. Record exact character/account GUIDs. Explicitly register each in
   `ai_playerbot_estate_broker` with matching account and faction 0/1, enabled 1.
   This is an administrator attestation; runtime does not insert registry rows.
3. Prevent account authentication. The pinned `realmd/AuthSocket.cpp` recognizes
   a permanent active ban when `expires_at=banned_at`, not merely `expires_at=0`.
   Inspect actual ban records and verify a login is rejected. Do not assume a ban
   command generated the intended representation on this exact revision.
4. Configure `Retirement.Broker.Alliance.Guid/Account` and
   `Retirement.Broker.Horde.Guid/Account`. Defaults are zero, unprovisioned.
   Apply both additive development schemas to cloned databases only.
5. Restart the isolated realm before attempting broker validation. Account reuse,
   GUID/account mismatch, an online broker, missing table or invalid faction must
   leave processing unavailable. No service account/character is auto-created.

Do not unregister a broker that owns pending lots or tracked mail. A production
provisioning/retirement command is not implemented. Registry/schema read failures
and startup ordering still need a complete login-path review before enablement.

## Remaining implementation gates

1. Intake orchestration, monetary intake, mail attachments/gold/COD/templates,
   existing auction/bid obligations, gifts/saved loot and late-mail races. Current
   leaf transfer deliberately holds nonempty bags, gifts, saved loot and items
   referenced by mail/auctions; those assets are not silently discarded.
2. Background normal AH listing, stable auction/estate mapping, bounded reposts,
   normal buyer debit/refund/winner delivery under acknowledged persistence and
   narrow temporary fences; restart-safe ID allocation and publication.
3. Virtual vendor/destroy orchestration, pricing/classification reuse, broker mail routing,
   estate fees/deposit accounting and independent inheritance settlement.
4. Complete all-state recovery, safe archive/delete and factory-based exactly-once
   replacement after detached intake. Delete plus retain is a valid configuration
   only because assets must first belong to the broker estate; there is no bypass
   for incomplete intake.
5. Full consistency/diff review and isolated Linux/MariaDB crash acceptance.

Do not purge receipts, mappings or GUID-reservation history while any related
operation can be replayed. No automatic audit-history deletion is implemented.
