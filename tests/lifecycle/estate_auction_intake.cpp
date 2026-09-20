#include "RandomBotEstateStore.h"
#include "Database/DatabaseEnv.h"
#include <cassert>

TestDatabase CharacterDatabase;

namespace
{
RandomBotEstateAuctionIntake Request(bool payout)
{
    RandomBotEstateAuctionIntake r;
    r.estateId=1; r.originalGuid=100; r.brokerGuid=101; r.auctionId=77; r.houseId=6;
    r.itemGuid=payout?0:900; r.itemEntry=400; r.itemCount=2; r.startBid=800; r.buyout=1000;
    r.bidder=payout?302:0; r.bid=payout?1000:0; r.deposit=40; r.cut=payout?50:0;
    r.expiresAt=5000; r.payoutAt=payout?6000:0;
    return r;
}

void Setup(RandomBotEstateAuctionIntake const& r)
{
    CharacterDatabase.connection={};
    auto& q=CharacterDatabase.connection.queries;
    q.push_back({"SELECT houseid,itemguid",{{r.houseId},{r.itemGuid},{r.itemEntry},{r.itemCount},{r.originalGuid},
        {r.buyout},{r.expiresAt},{r.payoutAt},{r.bidder},{r.bid},{r.startBid},{r.deposit}}});
    if(!r.payoutAt)
        q.push_back({"SELECT COUNT(*) FROM item_instance",{{1}}});
}
}

int main()
{
    for(bool payout:{false,true})
    {
        auto r=Request(payout);
        const unsigned writes=payout?3:5;
        for(unsigned fail=0;fail<=writes;++fail)
        {
            Setup(r);
            CharacterDatabase.connection.failExecute=fail;
            assert(RandomBotEstateStore::AdoptAuction(r).get()==(fail==0));
            assert(!CharacterDatabase.connection.mismatch);
            assert(CharacterDatabase.connection.durable.size()==(fail?0u:writes));
        }
    }

    auto invalid=Request(true);
    invalid.itemGuid=900;
    assert(!RandomBotEstateStore::AdoptAuction(invalid).get());
}
