#include "RandomBotEstateStore.h"
#include "Database/DatabaseEnv.h"
#include <cassert>

TestDatabase CharacterDatabase;

RandomBotEstateAuctionListing Request()
{
    RandomBotEstateAuctionListing r;
    r.estateId=1; r.lotId=2; r.auctionId=77; r.brokerGuid=101; r.brokerAccount=201; r.houseId=1;
    r.itemGuid=900; r.itemEntry=400; r.itemCount=2; r.randomPropertyId=-3; r.attempt=1;
    r.startBid=900; r.buyout=1000; r.deposit=40; r.expiresAt=5000;
    return r;
}

void Setup()
{
    CharacterDatabase.connection = {};
    auto& q=CharacterDatabase.connection.queries;
    q.push_back({"information_schema.tables",{{10}}});
    q.push_back({"SELECT COUNT(*) FROM ai_playerbot_estate_auction",{{0}}});
    q.push_back({"SELECT broker_guid,broker_account",{{101},{201},{2},{1},{1000},{0}}});
    q.push_back({"SELECT c.account,c.online,b.enabled,b.role",{{201},{0},{1},{0}}});
    q.push_back({"SELECT l.item_guid",{{900},{400},{2},{0},{0},{0},{uint64(uint32(-3))}}});
    q.push_back({"SELECT (SELECT COUNT(*) FROM auction",{{0}}});
}

int main()
{
    for(unsigned fail=0;fail<=5;++fail)
    {
        Setup(); auto& db=CharacterDatabase.connection; db.failExecute=fail;
        assert(RandomBotEstateStore::CreateAuctionListing(Request()).get()==(fail==0));
        assert(!db.mismatch && db.durable.size()==(fail?0u:5u));
    }
    Setup(); CharacterDatabase.connection.loseCommitAck=true;
    assert(!RandomBotEstateStore::CreateAuctionListing(Request()).get());
    assert(CharacterDatabase.connection.durable.size()==5);
    CharacterDatabase.connection={};
    CharacterDatabase.connection.queries.push_back({"SELECT COUNT(*) FROM ai_playerbot_estate_auction ea",{{1}}});
    assert(RandomBotEstateStore::ConfirmAuctionListing(Request()).get());
    assert(CharacterDatabase.connection.durable.empty());
    Setup(); CharacterDatabase.connection.queries[2].row[4].value=39;
    assert(!RandomBotEstateStore::CreateAuctionListing(Request()).get());
    Setup(); CharacterDatabase.connection.queries[3].row[3].value=1;
    assert(!RandomBotEstateStore::CreateAuctionListing(Request()).get());
    Setup(); CharacterDatabase.connection.queries[4].row[4].value=1;
    assert(!RandomBotEstateStore::CreateAuctionListing(Request()).get());
    Setup(); CharacterDatabase.connection.queries[5].row[0].value=1;
    assert(!RandomBotEstateStore::CreateAuctionListing(Request()).get());
}
