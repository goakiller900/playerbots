# Random-bot lifecycle development checkpoint

The broker/estate design in [retirement-estate-service.md](retirement-estate-service.md)
supersedes character-owned liquidation assumptions below. This document also
records historical development findings; it is not a completion claim.

This is **not a production-ready retirement implementation**. Retirement execution
is deliberately unavailable, including when Retirement.Enabled=1. No retirement
inventory removal, estate delivery, deletion, or replacement is scheduled by the
manager. Do not remove that gate just because the transaction foundation compiles.
Do not install the development schema on the running server yet.

## Compatibility target and source findings

Inspected locally against CMaNGOS mangos-wotlk
`1cd9d566ae83c1a88f1b057514697055055f419c`, with PlayerBots base
`4120aa9d3abf0afc1836f6ba90596c978b1161f5`.
The supplied world database revision is
`bd8d187f1fc5eafa13e9e4b6498a02422b78444b`; it was not loaded or tested here.

The authoritative core paths below are relative to that pinned core:

| Mechanism | Finding and consequence |
| --- | --- |
| shared/Database/Database.cpp, Database.h | BeginTransaction creates a thread-local statement queue. PExecute appends writes, but PQuery uses a pooled synchronous connection. A PQuery FOR UPDATE cannot protect the queued writes. |
| shared/Database/SqlOperations.cpp, SqlDelayThread.cpp | SqlTransaction runs on the worker's connection. CommitTransaction acknowledges enqueue, not durable commit. CommitTransactionDirect discards Execute's result and returns true. |
| game/Entities/Player.cpp | SaveInventoryAndGoldToDB calls inventory and gold persistence without starting a transaction. SaveToDB performs ordinary character saves; a stale online Player can overwrite an offline money update. |
| game/Entities/CharacterHandler.cpp | LoginQueryHolder caches character data asynchronously. Blocking new login requests alone cannot prevent a previously cached money value from loading later. The completion path owns/deletes the holder. |
| game/Mails/MailHandler.cpp, Mail.cpp | Take-money/item uses inventory/gold/mail saves. Returning mail and COD processing involve additional transactions. Ordinary actions cannot simply be treated as acknowledged, crash-atomic liquidation. |
| game/AuctionHouse/AuctionHouseHandler.cpp, AuctionHouseMgr.cpp | Listings use real auction entries, deposits, inventory removal and persistence. Sold auctions may remain pending payout. Both owner listings and bidder obligations matter. Ledger accounting must join the corresponding durable operation. |
| game/Entities/Player.cpp: DeleteFromDB | A core permanent-deletion API exists. Its broad cleanup is not authorization to delete before asset settlement. Completion must be reconciled from durable state; never replace it with ad-hoc character-table deletes. |

PlayerBots uses both RandomPlayerbotMgr's legacy selection and PlayerbotLoginMgr's
asynchronous selection. Both need exclusions and bracket handling. Random-bot
event values have expiration semantics and are not a transactional estate journal.
RandomPlayerbotFactory owns character creation. ItemUsageValue, AhAction,
SellAction and RPG/mail actions remain authoritative for classification, pricing,
vendor semantics and interaction; they have not been replaced by fake sales.

## Files and architecture

- PlayerbotAIConfig, RandomPlayerbotMgr, PlayerbotLoginMgr and PlayerbotFactory:
  opt-in bracket/natural-progression draft and lifecycle selection hooks.
- RandomBotLifecycleMath.h/.cpp: independent parsing, target/deficit, sink,
  weighted inheritance and transition calculations.
- RandomBotLifecycle.h/.cpp: durable-exclusion snapshot and login revision/lease
  foundation. Partial/missing auxiliary schema does not erase existing exclusions.
  Exclusions apply only to matching character/account rows in configured random
  accounts. No periodic economic processing is enabled.
- RandomBotLifecycleStore.h/.cpp and RandomBotLifecycleAssets.cpp: **unwired, experimental** acknowledged estate
  preparation and delivery transactions. They use only the worker connection.
  Inputs are immutable snapshots; worker callbacks never access Player objects.
- core-patches/0001-lifecycle-transaction-and-login-foundation.patch:
  QueueTransaction with future completion, query-holder revision and login
  completion checks, including rejected-holder/session cleanup.
  Version 2 also consumes existing core save queues through
  CommitTransactionAcknowledged, reports inventory serialization errors, and adds
  SaveInventoryMoneyAndMailToDB (including bank updates). Legacy save callers
  still use their existing paths.
- sql/characters/ai_playerbot_lifecycle.sql: non-destructive CREATE TABLE IF NOT
  EXISTS draft for lifecycle, inheritance, item journals and atomic asset receipts. No existing table
  is dropped, converted, emptied or populated. No automatic migration.
- Three config-dist variants and standalone/component calculation tests.

The estate-store design clears the donor and creates the sink/share ledger in
one transaction. Delivery locks a ledger row, revalidates an offline recipient,
updates money and marks the delivery in one transaction. An already delivered
row is a no-op. Cap/eligibility excess is recorded as sink. Query failure is not
interpreted as an empty recipient population. Existing table engines are checked
for InnoDB; the code does not silently convert an operator's tables.

This is **not yet an end-to-end atomicity guarantee**: its caller must retain an
offline lease through completion/reconciliation, fence character saves and late
mail/auction operations, and validate liquidated inventory. False/exception
completion may include an ambiguous COMMIT, so retry must inspect journal state.
The manager currently does not call this store.

The asset-save boundary validates matching character/account, lifecycle phase,
money, item owner/entry/full-stack count or delivered non-COD mail money on the
worker connection. Core-serialized removal/collection, the operation receipt,
and escrow/vendor/AH accounting share one transaction. Post-save checks reject
silently skipped removals. Duplicate receipts reject replay of the save queue;
the future caller must reconcile and reload. Escrow avoids silently truncating
proceeds at the Player money cap. This boundary is not yet connected to any
in-memory liquidation action or quiescence controller.

Additional AH finding: AuctionBidWinning saves winning state before calling
SendAuctionWonMail. AuctionHouseObject::Update sends successful/expired mail
before a separate DeleteFromDB. MailDraft::SendMailTo commits independently and
updates the live mailbox without acknowledgement. Retirement cannot safely
reuse these sequences unchanged: listing, winning-item transfer, payouts and
expiry returns all need durable, idempotent coordination and deferred publication.
The user superseded the vendor-only restriction: retirement must participate in
the normal AH, including visible listings, bids, buyouts and ordinary delivery
semantics. Narrow persistence fences for retirement-owned auctions are allowed.
Permanent AH blocking is not an acceptable finished implementation. The previously
removed runtime hooks must not simply be restored: the buyer debit, outbid refund
and winner delivery gaps must be closed first.

Vendor/destroy handling is a fallback for unsuitable or unsold assets after the
configured retry policy. The current asset-save boundary conservatively rejects
outstanding auction obligations and online characters; it is not yet a complete
liquidation scheduler.

The two experimental AH store primitives and their receipt tables remain
unconnected development code, tested only with scripted database doubles. They
are not an approved or complete AH integration. The development schema still has
six tables. Normal AH settlement retains its upstream crash characteristics;
this feature does not claim to repair those for real-player transactions.

Asset-save reconciliation now checks a matching durable operation receipt and
the saved asset postcondition on the worker connection. Failure remains a hold,
not permission to replay serialization. A caller must retain the operation
identity and offline lease; complete restart orchestration is still missing.

All-state recovery, disposition, exactly-once replacement and complete offline
inventory/mail liquidation remain unfinished. Do not remove the hard gate.

The estate store now refuses remaining character_inventory, item_instance
ownership, mail attachments and auction owner/bidder obligations. Retaining
unsellable items needs an explicit audited exemption before archive finalization;
there is intentionally no bypass for it yet.

## Configuration

All three top-level feature switches default off. Existing options are retained.

| New option suffix after AiPlayerbot. | Meaning |
| --- | --- |
| NaturalLeveling.Enabled | New-character initial setup using existing randombotStartingLevel; adopt progressed random bots without level reset. DisableRandomLevels remains unchanged. |
| LevelBracketBalancing.Enabled | Prefer online-selection deficits; no quota-driven kicks or de-leveling. |
| LevelBracketInitialRandomization.Enabled | Prefer underrepresented brackets for initial randomization, separately from online selection. Natural starting level takes precedence. |
| LevelBrackets / LevelBracketBalance | Ranges and percentages; invalid input disables bracket features. Core configured maximum is used for validation. |
| Retirement.Enabled | Reserved; currently logs unavailability and performs no retirement. |
| Retirement.CheckInterval / MinMaxLevelTime | Intended check interval/endgame residence, seconds. |
| Retirement.MaxPerCycle / MaxLevelPopulationPercent | Intended rate limit and maximum-level population threshold. |
| Retirement.AuctionAttempts / AuctionDiscountPercentPerAttempt | Intended bounded listing retries and discount. |
| Retirement.GoldSinkMinPercent / GoldSinkMaxPercent | Intended per-estate sink range; vendor income is the minimum sink, capped to estate. |
| Retirement.InheritanceMinRecipients / InheritanceMaxRecipients | Intended randomized recipient count. |
| Retirement.MaxInheritancePerBot | Copper cap per recipient; zero means only the core money cap. |
| Retirement.CreateReplacement | Intended one new generation per finalized estate. No replacement creation is currently implemented. |
| Retirement.CharacterDisposition | archive by default; delete explicitly opts into irreversible core deletion after durable asset detachment. Neither finalization path is currently implemented. |
| Retirement.UnsellableItemPolicy | retain in broker custody or explicitly destroy. Neither policy bypasses complete intake. |

## Work still required before enabling retirement

1. Acknowledged inventory/mail/auction persistence with accounting in the same
   transaction, plus an explicit policy for in-memory rollback/quarantine.
   Do not resume adventuring or save stale state after an ambiguous failure.
2. Quiescence and asset handling for equipment, containers, contents, bank,
   mail gold/attachments/COD, existing listings, bids and delayed proceeds.
   Unsupported assets must hold the lifecycle, not silently disappear.
3. Gradual eligibility/start and restart recovery, protected/group/combat/instance
   guards, and lifecycle-owned login distinct from ordinary login exclusion.
4. Wire offline estate/delivery leases through acknowledged logout/save and
   reconcile failures. Prove no preloaded query holder or late save can restore
   old money. Reject real-player account sessions and account reassignment.
5. Archive finalization and optional core deletion after complete settlement,
   with retained audit records and late-delivery protection.
6. Exactly-once factory replacement: durable reserved identity and creation
   acknowledgement/reconciliation. Name lookup alone is insufficient.
7. Full bracket/class/race interaction and natural-initialization review, including
   async cached levels and default-off behavioral regression coverage.
8. Database fault-injection tests and staged Linux build/runtime validation.

## Validation performed here

- Checked the reference checkout's exact core commit and inspected the APIs above.
- Whitespace/diff checks and reverse-apply check of the core patch.
- Added standalone C++ calculation tests and GenerateBotTests integration.
- Cross-checked the overflow-free fractional arithmetic against arbitrary-
  precision arithmetic for 20,000 deterministic inputs. This does not execute C++.
- A temporary approved Zig 0.13.0 compiler was installed under ignored
  .validation-tools, not as a project dependency. Standalone C++14 calculation
  tests and the production acknowledged-transaction wrapper tests compiled and
  passed on Windows. Wrapper tests cover BEGIN failure, statement failure,
  COMMIT failure, exceptions, throwing rollback and lost COMMIT acknowledgement
  with receipt-based non-replay in a fake transactional asset model.
- Five Python source/deployment guard tests passed. These verify disabled config,
  the scheduler gate, additive schema, connection-only accounting and patch parity.
- No full CMaNGOS build, actual SQL migration, live database or server restart
  tests ran. CMake and WSL remain unavailable. Zig's initial runtime-library
  build emitted DLL-attribute warnings; the standalone source builds succeeded.
- The production payout and expiry-return store files compiled and passed
  scripted-connection tests: write failure rollback, ambiguous COMMIT, receipt
  replay/collision, missing receipt/row, overflow, online seller, incomplete item
  transfer, payout delay and attachment verification. These do not execute MySQL
  or the actual MailDraft serializer.
- The complete currently available Windows suite is reproducible with:
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/lifecycle/run_windows.ps1
  (requires the temporary compiler already installed in .validation-tools).
- No production data was accessed, and nothing was committed or pushed.

## Linux checks next (staging source/database only)

Keep the running server untouched. First place the reviewed working-tree files
in a staging copy matching the paths below; do not use git reset on the server.
These commands build only: no install, daemon restart, or production SQL import.

```sh
cd /srv/cmangos/src/mangos-wotlk
test "$(git rev-parse HEAD)" = 1cd9d566ae83c1a88f1b057514697055055f419c
git diff --check
git apply --check src/modules/PlayerBots/core-patches/0001-lifecycle-transaction-and-login-foundation.patch
# Apply only to a reviewed staging copy, once:
git apply src/modules/PlayerBots/core-patches/0001-lifecycle-transaction-and-login-foundation.patch

cmake -S src/modules/PlayerBots/tests/lifecycle -B /tmp/playerbots-lifecycle-math \
  -DCMANGOS_CORE_SOURCE_DIR=/srv/cmangos/src/mangos-wotlk
cmake --build /tmp/playerbots-lifecycle-math --parallel 2
ctest --test-dir /tmp/playerbots-lifecycle-math --output-on-failure

cmake -S . -B /tmp/cmangos-lifecycle-build \
  -DBUILD_PLAYERBOTS=ON -DPCH=OFF \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=/srv/cmangos/server
cmake --build /tmp/cmangos-lifecycle-build --parallel 2
```

Use the production build's additional dependency/configuration flags if needed.
Build an unpatched-core configuration too: bracket features should compile and
the unsupported economic store must remain inactive.

Once the missing pipeline is implemented, use disposable cloned character/login
databases. Verify every participating table is InnoDB. Apply the schema twice
and compare existing data. Inject failure before/after each statement and COMMIT,
kill/restart at every phase, and repeat every delivery/replacement request.
Check estate = sunk + distributed + still-pending shares, item ownership,
preloaded login rejection, money caps, zero recipients, late mail and bids,
archive exclusion, deletion cleanup, occupied replacement accounts, and that
real-player and non-random-alt records remain unchanged. Run default-off
regressions before considering any production deployment.
