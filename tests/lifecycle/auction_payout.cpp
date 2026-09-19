// Compiles the production payout store against a scripted connection double.
// Checks its validation/SQL/acknowledgement branches, not MySQL SQL semantics.
#include "RandomBotLifecycleStore.h"
#include "Database/DatabaseEnv.h"
#include <cassert>
#include <limits>

TestDatabase CharacterDatabase;

RandomBotLifecycleStore::AuctionPayout Request()
{
    RandomBotLifecycleStore::AuctionPayout request;
    request.auctionId = 77;
    request.owner = 100;
    request.account = 200;
    request.bidder = 300;
    request.itemEntry = 400;
    request.bid = 100;
    request.deposit = 30;
    request.cut = 10;
    request.expiresAt = 500;
    request.payoutAt = 600;
    request.eligibleAccounts = {200};
    return request;
}

void Setup(bool receipt = false, bool auction = true)
{
    CharacterDatabase.connection = {};
    auto& q = CharacterDatabase.connection.queries;
    q.push_back({"information_schema.tables", {{4}}});
    q.push_back({"SELECT account,status", {{200}, {4}, {0}, {0}, {0}, {0}}});
    q.push_back({"SELECT COUNT(*) FROM ai_playerbot_lifecycle_auction", {{receipt ? 1u : 0u}}});
    q.push_back({"SELECT COUNT(*) FROM auction", {{auction ? 1u : 0u}}});
    if (receipt && !auction)
        q.push_back({"AND owner=100 AND bidder=300", {{1}}});
    if (!receipt && auction)
    {
        q.push_back({"SELECT account,online", {{200}, {0}}});
        q.push_back({"SELECT itemowner,buyguid", {{100}, {300}, {400}, {100}, {30}, {500}, {600}, {0}, {1}}});
    }
}

int main()
{
    for (unsigned fail = 0; fail <= 3; ++fail)
    {
        Setup();
        auto& db = CharacterDatabase.connection;
        db.failExecute = fail;
        assert(RandomBotLifecycleStore::SettleAuctionPayout(Request()).get() == (fail == 0));
        assert(!db.mismatch && db.queries.empty());
        assert(db.durable.size() == (fail == 0 ? 3u : 0u));
        if (!fail)
        {
            assert(db.durable[0].find("INSERT INTO ai_playerbot_lifecycle_auction") == 0);
            assert(db.durable[1].find("estate_escrow=estate_escrow+120") != std::string::npos);
            assert(db.durable[1].find("auction_fees=auction_fees+10") != std::string::npos);
            assert(db.durable[2] == "DELETE FROM auction WHERE id=77");
        }
    }
    Setup();
    CharacterDatabase.connection.loseCommitAck = true;
    assert(!RandomBotLifecycleStore::SettleAuctionPayout(Request()).get());
    assert(CharacterDatabase.connection.durable.size() == 3);
    Setup(true, false); // Restart/reconciliation observes committed receipt, no auction.
    assert(RandomBotLifecycleStore::SettleAuctionPayout(Request()).get());
    assert(CharacterDatabase.connection.durable.empty());

    Setup(true, true); // Reused ID must not be acknowledged, even if snapshot matches.
    assert(!RandomBotLifecycleStore::SettleAuctionPayout(Request()).get());
    assert(CharacterDatabase.connection.durable.empty());
    Setup(false, false); // Missing row is not proof of settlement.
    assert(!RandomBotLifecycleStore::SettleAuctionPayout(Request()).get());
    Setup(true, false);
    CharacterDatabase.connection.queries.back().row = {{0}}; // Mismatched receipt.
    assert(!RandomBotLifecycleStore::SettleAuctionPayout(Request()).get());
    Setup();
    CharacterDatabase.connection.queries[1].row[3].value = std::numeric_limits<uint64>::max();
    assert(!RandomBotLifecycleStore::SettleAuctionPayout(Request()).get());
    Setup();
    CharacterDatabase.connection.queries[4].row[1].value = 1; // Online seller.
    assert(!RandomBotLifecycleStore::SettleAuctionPayout(Request()).get());
    Setup();
    CharacterDatabase.connection.queries.back().row[7].value = 999; // Item not delivered.
    assert(!RandomBotLifecycleStore::SettleAuctionPayout(Request()).get());
    Setup();
    CharacterDatabase.connection.queries.back().row[8].value = 0; // Delay not elapsed.
    assert(!RandomBotLifecycleStore::SettleAuctionPayout(Request()).get());
    Setup();
    CharacterDatabase.connection.queries[2].fail = true; // Query failure is not no receipt.
    assert(!RandomBotLifecycleStore::SettleAuctionPayout(Request()).get());
    assert(CharacterDatabase.connection.durable.empty());
    Setup();
    auto invalid = Request();
    invalid.eligibleAccounts.clear();
    assert(!RandomBotLifecycleStore::SettleAuctionPayout(invalid).get());
    assert(CharacterDatabase.connection.durable.empty());
}
