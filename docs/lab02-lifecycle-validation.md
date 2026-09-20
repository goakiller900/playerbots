# LAB02 isolated lifecycle acceptance

Never crash-test the populated live realm. Use the same host and MariaDB/core
versions with cloned DBs, separate realm/config/ports, and the exact pinned source.

## Build order

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

Retain production dependency flags. Treat every warning/error in new estate/core
files as a failure.

## Schema and disabled startup

Apply both character scripts twice to a disposable clone:

```sh
mysql --login-path=lab02-lifecycle lab02_lifecycle_characters \
 < src/modules/PlayerBots/sql/characters/ai_playerbot_lifecycle.sql
mysql --login-path=lab02-lifecycle lab02_lifecycle_characters \
 < src/modules/PlayerBots/sql/characters/ai_playerbot_estate.sql
# repeat both, then verify all participating tables are InnoDB
```

Before/after character checksums must match. With defaults, upstream behavior is
unchanged. With `Retirement.Enabled=1`, the hard gate must log once and create no
lifecycle/estate rows or economic/disposition changes.

## Broker fixture

Using normal admin/core tools, create four dedicated offline characters/accounts:
Alliance/Horde seller and Alliance/Horde bidder. Permanently ban accounts using
the pinned realmd predicate (`active=1`, `expires_at=banned_at`), verify rejected
login, register exact faction/role rows, and configure all eight GUID/account
options. Never reuse human/random accounts. Any mismatch must fail closed.

Only after build/schema/disabled tests pass may a separate reviewed LAB branch
change the literal gate. Restore the clone before every crash case.

## Crash matrix

Inject stops before BEGIN, after locks, after each queued core write, before
COMMIT, after durable COMMIT/before acknowledgement, and before live publication.
At every point verify:

- equipment, bags/contents, bank, mail gold/items, seller auctions and inherited
  bids are wholly detached once or wholly intact;
- COD, wrapped, saved-loot, corrupt, nonempty-bag and unsupported assets hold
  finalization rather than disappear;
- real players/bots see and bid/buy normal AH listings; debit, refunds, winner
  item, auction state and receipt are atomic and never duplicate;
- expiry, returned mail, repost limit/discount, deposit loss, proceeds/cut, and
  vendor/destroy reconcile once with exact lot GUID ownership;
- two estates holding the same item entry never merge accounting;
- archive never logs in; delete happens only after detachment and preserves the
  independent estate/audit; broker identities cannot be deleted;
- crashes around replacement save yield exactly the one reserved low-level bot;
- final estate equals sink plus delivered inheritance, vendor income is fully
  neutralized at minimum, and caps/rounding/overflow cannot mint copper;
- humans and non-random alts outside a tracked bid are unchanged.

Run one estate first, then cross-faction/inherited bids, then bounded thousands-bot
load tests. Preserve SQL snapshots, logs, config, binary hashes, and failpoint
results. Production activation is rejected until all cases pass and the gate gets
a separate review.
