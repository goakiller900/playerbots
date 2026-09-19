# LAB02 validation gates (development checkpoint)

The background broker architecture in [retirement-estate-service.md](retirement-estate-service.md)
is authoritative. Provision only against isolated cloned databases; LAB02's live
populated realm is not a crash-test target. Character deletion/replacement must
be tested while an independent broker estate still has auctions outstanding.

The implementation is NOT internally complete. Only gates A and B below can be
run now. Gate C is the required runtime acceptance matrix, not an executable
claim that the missing lifecycle/fault-injection harness exists.
Never remove the scheduler gate to make a test run.

Scope correction: normal AH participation is REQUIRED. Retirement listings must
remain visible and accept normal bids/buyouts. Narrow persistence fences are
permitted for retirement-owned auctions, but normal rules, prices and delivery
semantics must be preserved. Permanent AH blocking is not an acceptance option.
The earlier vendor-only scope and source-parity restriction are superseded.
Retirement remains hard-disabled while acknowledged bid/item/mail settlement,
all-state recovery, final disposition and replacement are implemented. Those are
implementation work, not merely Linux validation.

The Windows suite comprises seven C++ executables (math, transaction runner,
estate intake/liquidation, asset save/reconciliation, experimental payout and return stores) and eight Python
safety-contract tests. Store tests use doubles, not MySQL or actual core item/mail
serialization. AH tests do not imply an approved runtime integration.

Latest Windows checkpoint: all seven executables and eight Python tests passed;
both repository whitespace checks passed. The exported patch reverse-applies to
the pinned local reference. This is not full-core compilation or a database test.

## A. Isolated source and build

Use LAB02, never the running server's install/database configuration. Place a
copy of the pinned core at /srv/cmangos/lab02-lifecycle/src/mangos-wotlk and the
reviewed PlayerBots working tree at its src/modules/PlayerBots directory.
Exclude .core-reference and .validation-tools when transferring PlayerBots.
The core copy must be clean at the pinned revision before applying the patch.
These commands do not install or start a daemon:

```sh
set -eu
cd /srv/cmangos/lab02-lifecycle/src/mangos-wotlk
test "$(git rev-parse HEAD)" = 1cd9d566ae83c1a88f1b057514697055055f419c
test -z "$(git status --porcelain --untracked-files=no)"
git apply --check src/modules/PlayerBots/core-patches/0001-lifecycle-transaction-and-login-foundation.patch
git apply src/modules/PlayerBots/core-patches/0001-lifecycle-transaction-and-login-foundation.patch
git diff --check

cmake -S src/modules/PlayerBots/tests/lifecycle \
  -B /srv/cmangos/lab02-lifecycle/build-tests \
  -DCMANGOS_CORE_SOURCE_DIR=/srv/cmangos/lab02-lifecycle/src/mangos-wotlk
cmake --build /srv/cmangos/lab02-lifecycle/build-tests --parallel 2
ctest --test-dir /srv/cmangos/lab02-lifecycle/build-tests --output-on-failure
python3 src/modules/PlayerBots/tests/lifecycle/test_safety_contract.py -v

cmake -S . -B /srv/cmangos/lab02-lifecycle/build-core \
  -DBUILD_PLAYERBOTS=ON -DPCH=OFF -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_INSTALL_PREFIX=/srv/cmangos/lab02-lifecycle/server
cmake --build /srv/cmangos/lab02-lifecycle/build-core --parallel 2
```

Retain other required dependency flags from the existing Linux build. Run the
math tests and build PlayerBots against an unpatched pinned core copy as well:
the optional asset APIs must compile out. The Python patch-parity test skips if
the Windows-only .core-reference folder is absent; the explicit git checks above
cover the LAB02 core patch.

## B. Additive schema and disabled startup

Use a disposable clone named lab02_lifecycle_characters. Restrict the test
database user's grants to the LAB02 schemas; do not use production credentials.
Back up the clone before testing and retain before/after row counts/checksums.
Do not start a second server against a database used by the running server.

```sh
set -eu
cd /srv/cmangos/lab02-lifecycle/src/mangos-wotlk
mysql --login-path=lab02-lifecycle lab02_lifecycle_characters \
  < src/modules/PlayerBots/sql/characters/ai_playerbot_lifecycle.sql
mysql --login-path=lab02-lifecycle lab02_lifecycle_characters \
  < src/modules/PlayerBots/sql/characters/ai_playerbot_lifecycle.sql
mysql --login-path=lab02-lifecycle lab02_lifecycle_characters \
  < src/modules/PlayerBots/sql/characters/ai_playerbot_estate.sql
mysql --login-path=lab02-lifecycle lab02_lifecycle_characters \
  < src/modules/PlayerBots/sql/characters/ai_playerbot_estate.sql
mysql --login-path=lab02-lifecycle lab02_lifecycle_characters -e \
  "SELECT table_name,engine FROM information_schema.tables WHERE table_schema=DATABASE() AND (table_name LIKE 'ai_playerbot_lifecycle%' OR table_name LIKE 'ai_playerbot_estate%' OR table_name IN ('characters','character_inventory','item_instance','character_gifts','item_loot','mail','mail_items','auction'));"
```

All participating tables must be InnoDB. Do not auto-convert a production table
to make this check pass. Existing character data must remain unchanged after
both schema imports.
There are six legacy lifecycle and four broker-estate development tables; none has seed rows or automatic
character backfill. Include both auction receipt tables in before/after checks.

Configure an isolated LAB02 realm with its own ports, realm ID, login database,
character database and world database clone at the supplied WotLK-DB revision.
Never copy production connection strings into its active configuration.
After a successful full build, use the LAB02-only binary/configuration to check:

- All feature switches off: upstream login/randomization behavior is preserved.
- Retirement.Enabled=1: one clear unavailable diagnostic; zero lifecycle
  economic operations, deletions or replacements. This is the required current
  behavior, not a failed activation test.
- Bracket invalid syntax/overlap/percent totals: diagnostics and disabled balancing.
- Natural mode: progressed characters retain level and XP; ordinary non-random
  alts and real players are unchanged.

Record the exact binary revision, config, schema checks and log for each run.
Do not activate retirement or run the destructive matrix below on this draft.

## C. Required runtime acceptance matrix after implementation

The missing harness must expose deterministic failpoints before BEGIN, after
validation/receipt insertion, after each core save statement, before COMMIT,
after durable COMMIT but before acknowledgement, and before in-memory publication.
Repeat each scenario from a restored disposable fixture and kill/restart the
server at each failpoint. Ordinary graceful logout alone is not a crash test.

| Scenario | Required post-restart invariant |
| --- | --- |
| Vendor full stack in equipment/backpack/bag/bank/bank bag | Exactly one durable removal and one receipt/escrow credit, or neither. Vendor total matches the core's charges-adjusted copper value. |
| Nonempty bag, wrapped item, saved item loot | No contents or embedded assets disappear; unsupported category holds processing explicitly. |
| Delivered mail gold, attached items, delayed mail, COD | No duplicate gold/attachment; original mailbox preserved on failed save; COD waits until atomic sender settlement exists. |
| Broker-owned listing, bidder obligation, delayed seller proceeds | Listings are visible and purchasable normally. Buyer debit, refund/winner mail, auction state and estate mapping are atomic. No fake sale gold. |
| Real player bids/buyouts during settlement | Narrow temporary persistence fences must not change AH rules or permanently block bids. Verify no double charge, lost debit or duplicate winner/refund mail. |
| Asset save acknowledgement lost | Matching receipt and cleared asset postcondition confirm completion without replay; absent/conflicting/query-failed evidence holds the bot offline. |
| Vendor fallback | AH-suitable lots first use normal listings with bounded retries. Unsold/unsuitable lots use validated core sell-value semantics; destruction requires explicit policy. |
| DB error, deadlock, connection loss at COMMIT | Frozen state stays quarantined; durable receipt decides reload/resume. Ordinary autosave/logout cannot overwrite the recovered assets. |
| Grouped/combat/instance/protected/real-player/non-random alt | Retirement does not start or mutate the character. |
| Estate preparation and each recipient delivery | Estate equals sink plus delivered plus pending shares. Duplicate request is not a second credit. Caps and uint64 extremes cannot overflow. |
| Cached and in-flight login holders | Old-money snapshot is rejected after a lease/revision change; holder/session ownership has no leak or double free. |
| Archive | Audit persists; every ordinary selection path rejects the retired character after restart. Retained-item rules are explicit. |
| Delete with broker auctions still active | After complete asset detachment only the original eligible random character is deleted by the core API; independent estate, auctions and audit survive; late mail cannot orphan assets. |
| Two estates holding identical item entries | Lot GUIDs and per-estate accounting remain distinct through listing, return, repost and sale. No untracked stack merge. |
| Broker account overlaps random prefix, becomes online, or is deleted | Fail closed before provisioning/economic work; core deletion guards protect registered identities. No ordinary player is repurposed. |
| Replacement creation, full account, lost save acknowledgement | One reserved new identity per retirement, exactly one persisted replacement, low-level initialization once. No duplicated account/character on retry. |
| Disable retirement mid-cycle/restart | Explicit recovery policy prevents stranded or replayed estates. |

Compare human/non-random records and all economic totals before/after. Run
thousands-bot load tests only after single-character invariants pass. No
production enablement until all rows pass and the safety gate is deliberately
reviewed in a separate change.
