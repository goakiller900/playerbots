# Background retirement estate service

This is the authoritative implementation note for the Azeroth Retirement
Organization design. `RandomBotLifecycleMgr::ExecutionAllowed()` remains the
literal `false`; configuration and SQL cannot activate retirement before the
isolated lifecycle acceptance gates pass.

## Implemented architecture

Character and estate lifetimes are independent:

```
random bot -> pending -> intake -> assets detached -> archive/delete -> replacement once
                                      |
                                      `-> estate lots -> AH/vendor -> sink/inheritance
```

Four explicitly provisioned offline service characters are required: Alliance
and Horde seller brokers plus distinct Alliance and Horde inherited-bid brokers.
The split prevents a transferred bid becoming a self-bid. Startup validates exact
GUID/account/faction/role, offline state, dedicated permanently banned accounts,
and no random-account overlap. Service identities are excluded from login, AI,
brackets, inheritance, retirement, factory cleanup, and core deletion.

The normal AH stays visible. Clients see the seller broker's real character name.
Ordinary validation, bid/buyout rules, duration, deposit/cut, and auction mail
formats remain. Only durable tracked IDs enter the estate coordinator. A bidder
on one is fenced from further opcodes/saves/logout only through DB acknowledgement
or receipt reconciliation; all other player/AH behavior uses the original path.

`ai_playerbot_estate.sql` adds eight InnoDB tables: broker registry, estate,
exact-GUID lots, operation receipts, auction mapping, inherited bid claims,
immutable bid revisions, and inheritance ledger. Historical auction/mail/player
reservations raise core ID generators after restart. Original GUIDs remain audit
keys after deletion; item lots are never merged across estates.

Intake escrows initial copper, consumes non-COD asset mail, adopts seller auctions
and bidder obligations under a temporary fence, and transfers equipment,
backpack/bag, bank/bank-bag leaf items without cloning or changing item identity.
Detachment proves zero remaining money, owned items, inventory links, asset mail,
and auction obligations. COD mail is held and logged, never discarded.

Detached lots use `ItemUsageValue`, `AhOverVendorItemIds`/
`VendorOverAHItemIds`, normal core deposits/durations, bounded retries/discounts,
and exact returned/proceeds mail mappings. Vendor fallback uses the shared core
sell-value rule. Unsellable retain and explicit destroy are distinct policies.

Estate escrow is 64-bit copper. Deposits, lost fees, cuts, AH income, vendor
income, sink, and inheritance are separate. Sink is at least vendor-generated
currency. Weighted capped shares are persisted before delivery; each credit
rechecks eligibility under a lock and cap/rounding excess becomes sink.

Archive is permanently excluded. Delete first commits `DELETE_PENDING`, invokes
the pinned core `Player::DeleteFromDB(..., true)`, and completes only after absence
is proven. Replacement GUID/name/account/race/class/gender are reserved durably
before the existing factory path. Recovery recognizes the exact created identity;
the original need not exist while its estate continues liquidating.

## Crash boundary

Economic stores use the pinned worker-owned acknowledged transaction: locks,
immutable preimages, queued core writes, receipt/mapping updates, and postconditions
share one connection/transaction. False is ambiguous and triggers durable
confirmation before live money, auction maps, or mailboxes change. DB callbacks
capture no world objects. Active queues are indexed and bounded.

The core patch adds queue-only auction-mail persistence and post-commit publication,
tracked auction reload, ID high-water methods, shared vendor valuation, stale-login
revisions, broker deletion guards, and the narrow pending-bid fence. It targets
mangos-wotlk `1cd9d566ae83c1a88f1b057514697055055f419c` only.

## Gate status

The full new module/core surface has not compiled on Windows because CMake/Boost
are unavailable. Fake-connection tests do not emulate MariaDB, live mail ownership,
sessions, or character deletion. The exact Linux build and every isolated crash
case remain mandatory. Until they pass, enabling retirement must log once and
perform zero retirement mutations. See `lifecycle-validation.md`.
