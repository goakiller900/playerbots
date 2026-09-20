#include "RandomBotEstateStore.h"
#include "Database/DatabaseEnv.h"
#include <cassert>

TestDatabase CharacterDatabase;

namespace
{
RandomBotEstateAuctionResolution Request(uint32 kind)
{
    RandomBotEstateAuctionResolution r;
    r.kind=kind; r.auctionId=77; r.owner=101; r.itemEntry=400; r.itemCount=2;
    r.deposit=40; r.expiresAt=5000; r.mailId=500+kind;
    if(kind==ESTATE_AUCTION_EXPIRED) r.itemGuid=900;
    else { r.bidder=302; r.bid=1000; r.cut=50; r.payoutAt=6000; }
    if(kind==ESTATE_AUCTION_WON) r.itemGuid=900;
    return r;
}
void PostQueries(RandomBotEstateAuctionResolution const& r)
{
    auto& q=CharacterDatabase.connection.queries;
    q.push_back({"SELECT COUNT(*) FROM ai_playerbot_estate_auction",{{1}}});
    q.push_back({"SELECT COUNT(*) FROM mail",{{1}}});
    if(r.kind==ESTATE_AUCTION_WON)
    {
        q.push_back({"SELECT COUNT(*) FROM auction",{{1}}});
        q.push_back({"SELECT COUNT(*) FROM item_instance",{{1}}});
        if(r.claimWin)
            q.push_back({"SELECT COUNT(*) FROM ai_playerbot_estate_bid_claim",{{1}}});
    }
    else
    {
        q.push_back({"SELECT COUNT(*) FROM auction",{{0}}});
        if(r.kind==ESTATE_AUCTION_EXPIRED)
            q.push_back({"SELECT COUNT(*) FROM item_instance",{{1}}});
    }
}
void Setup(RandomBotEstateAuctionResolution const& r)
{
    CharacterDatabase.connection={}; auto& q=CharacterDatabase.connection.queries;
    q.push_back({"information_schema.tables",{{6}}});
    q.push_back({"SELECT broker_guid,item_guid",{{r.owner},{r.kind==ESTATE_AUCTION_PAYOUT?900u:r.itemGuid},{r.itemEntry},{r.itemCount},
        {r.kind==ESTATE_AUCTION_PAYOUT?1u:0u},{r.bidder},{r.bid},{r.deposit},
        {r.kind==ESTATE_AUCTION_WON?0u:r.cut},{r.expiresAt},{r.kind==ESTATE_AUCTION_WON?0u:r.payoutAt},{0}}});
    if(r.claimWin)
        q.push_back({"SELECT COUNT(*) FROM ai_playerbot_estate_bid_claim",{{1}}});
    q.push_back({"SELECT itemowner,itemguid",{{r.owner},{r.itemGuid},{r.itemEntry},{r.itemCount},
        {r.bidder},{r.bid},{r.deposit},{r.expiresAt},{r.kind==ESTATE_AUCTION_WON?0u:r.payoutAt}}});
    PostQueries(r);
    CharacterDatabase.queuedSave=[](SqlConnection& c){return c.Execute("queued auction and mail mutation");};
}
}

int main()
{
    for(uint32 kind=ESTATE_AUCTION_EXPIRED;kind<=ESTATE_AUCTION_PAYOUT;++kind)
    {
        auto r=Request(kind);
        for(unsigned fail=0;fail<=2;++fail)
        {
            Setup(r); CharacterDatabase.connection.failExecute=fail;
            assert(RandomBotEstateStore::CommitAuctionResolution(r).get()==(fail==0));
            assert(!CharacterDatabase.connection.mismatch);
            assert(CharacterDatabase.connection.durable.size()==(fail?0u:2u));
        }
        Setup(r); CharacterDatabase.connection.loseCommitAck=true;
        assert(!RandomBotEstateStore::CommitAuctionResolution(r).get());
        assert(CharacterDatabase.connection.durable.size()==2);
        CharacterDatabase.connection={}; PostQueries(r);
        assert(RandomBotEstateStore::ConfirmAuctionResolution(r).get());
    }

    auto combined=Request(ESTATE_AUCTION_WON);
    combined.claimWin=true;
    for(unsigned fail=0;fail<=3;++fail)
    {
        Setup(combined); CharacterDatabase.connection.failExecute=fail;
        assert(RandomBotEstateStore::CommitAuctionResolution(combined).get()==(fail==0));
        assert(!CharacterDatabase.connection.mismatch);
        assert(CharacterDatabase.connection.durable.size()==(fail?0u:3u));
    }
    Setup(combined); CharacterDatabase.connection.loseCommitAck=true;
    assert(!RandomBotEstateStore::CommitAuctionResolution(combined).get());
    assert(CharacterDatabase.connection.durable.size()==3);
    CharacterDatabase.connection={}; PostQueries(combined);
    assert(RandomBotEstateStore::ConfirmAuctionResolution(combined).get());
}
