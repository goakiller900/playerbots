#include "RandomBotEstateStore.h"
#include "Database/DatabaseEnv.h"
#include <cassert>

TestDatabase CharacterDatabase;

namespace
{
RandomBotEstateAuctionBid Request(bool buyout = false)
{
    RandomBotEstateAuctionBid r;
    r.auctionId = 77; r.owner = 101; r.itemGuid = 900; r.itemEntry = 400; r.itemCount = 2;
    r.oldBidder = 301; r.oldBid = 100; r.bidder = 302; r.bidderAccount = 402;
    r.bid = buyout ? 1000 : 200; r.startBid = 100; r.buyout = 1000; r.moneyBefore = 1200;
    r.refundMailId = 501; r.winnerMailId = buyout ? 502 : 0;
    r.expiresAt = 5000; r.payoutAt = buyout ? 6000 : 0;
    return r;
}

void Setup(RandomBotEstateAuctionBid const& r)
{
    CharacterDatabase.connection = {};
    auto& q = CharacterDatabase.connection.queries;
    q.push_back({"information_schema.tables", {{8}}});
    q.push_back({"SELECT (SELECT COUNT(*) FROM ai_playerbot_estate_auction", {{1}}});
    q.push_back({"SELECT COUNT(*) FROM ai_playerbot_estate_auction_bid", {{0}}});
    q.push_back({"SELECT itemowner,itemguid", {{r.owner},{r.itemGuid},{r.itemEntry},{r.itemCount},
        {r.oldBidder},{r.oldBid},{r.startBid},{r.buyout},{r.expiresAt},{0}}});
    q.push_back({"SELECT account,money FROM characters", {{r.bidderAccount},{r.moneyBefore}}});
    q.push_back({"SELECT COUNT(*) FROM auction", {{1}}});
    q.push_back({"SELECT COUNT(*) FROM characters", {{1}}});
    q.push_back({"SELECT COUNT(*) FROM mail", {{1}}});
    if (r.winnerMailId)
        q.push_back({"SELECT COUNT(*) FROM mail m", {{1}}});
    CharacterDatabase.queuedSave = [](SqlConnection& c)
    {
        return c.Execute("queued core auction/buyer/mail mutation");
    };
}
}

int main()
{
    for (bool buyout : {false, true})
    {
        RandomBotEstateAuctionBid r = Request(buyout);
        for (unsigned fail = 0; fail <= 4; ++fail)
        {
            Setup(r);
            auto& db = CharacterDatabase.connection;
            db.failExecute = fail;
            assert(RandomBotEstateStore::CommitAuctionBid(r).get() == (fail == 0));
            assert(!db.mismatch);
            assert(db.durable.size() == (fail ? 0u : 4u));
        }
        Setup(r);
        CharacterDatabase.connection.loseCommitAck = true;
        assert(!RandomBotEstateStore::CommitAuctionBid(r).get());
        assert(CharacterDatabase.connection.durable.size() == 4);

        CharacterDatabase.connection = {};
        auto& q = CharacterDatabase.connection.queries;
        q.push_back({"SELECT COUNT(*) FROM ai_playerbot_estate_auction_bid", {{1}}});
        q.push_back({"SELECT COUNT(*) FROM auction", {{1}}});
        q.push_back({"SELECT COUNT(*) FROM characters", {{1}}});
        q.push_back({"SELECT COUNT(*) FROM mail", {{1}}});
        if (buyout)
            q.push_back({"SELECT COUNT(*) FROM mail m", {{1}}});
        assert(RandomBotEstateStore::ConfirmAuctionBid(r).get());
        assert(!CharacterDatabase.connection.mismatch);
    }
}
