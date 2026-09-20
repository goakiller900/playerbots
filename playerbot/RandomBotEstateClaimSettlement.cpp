#include "RandomBotEstateStore.h"
#include "Database/DatabaseEnv.h"

namespace
{
using R=RandomBotEstateClaimWin;
bool Valid(R const&r){return r.estateId&&r.auctionId&&r.owner&&r.brokerGuid&&r.itemGuid&&r.itemEntry&&r.itemCount&&r.bid&&r.mailId&&r.expiresAt&&r.payoutAt;}
#if defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) && CMANGOS_ASYNC_TRANSACTION_CALLBACK >= 4
bool Post(SqlConnection& c,R const&r)
{
    auto claim=c.Query(("SELECT COUNT(*) FROM ai_playerbot_estate_bid_claim WHERE auction_id="+std::to_string(r.auctionId)+
        " AND estate_id="+std::to_string(r.estateId)+" AND broker_guid="+std::to_string(r.brokerGuid)+" AND status=2 AND mail_id="+
        std::to_string(r.mailId)+" AND item_guid="+std::to_string(r.itemGuid)).c_str());
    auto auction=c.Query(("SELECT COUNT(*) FROM auction WHERE id="+std::to_string(r.auctionId)+" AND itemguid=0 AND buyguid="+
        std::to_string(r.brokerGuid)+" AND lastbid="+std::to_string(r.bid)+" AND moneyTime="+std::to_string(r.payoutAt)).c_str());
    auto mail=c.Query(("SELECT COUNT(*) FROM mail m INNER JOIN mail_items mi ON mi.mail_id=m.id INNER JOIN item_instance i ON i.guid=mi.item_guid "
        "WHERE m.id="+std::to_string(r.mailId)+" AND m.receiver="+std::to_string(r.brokerGuid)+" AND mi.item_guid="+
        std::to_string(r.itemGuid)+" AND i.owner_guid="+std::to_string(r.brokerGuid)).c_str());
    return claim&&claim->Fetch()[0].GetUInt32()==1&&auction&&auction->Fetch()[0].GetUInt32()==1&&mail&&mail->Fetch()[0].GetUInt32()==1;
}
#endif
}
std::future<bool> RandomBotEstateStore::CommitClaimWin(R r)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 4
    CharacterDatabase.RollbackTransaction();std::promise<bool> p;auto f=p.get_future();p.set_value(false);return f;
#else
    return CharacterDatabase.CommitTransactionAcknowledged([r](SqlConnection& c)
    {
        if(!Valid(r))return false;
        auto claim=c.Query(("SELECT estate_id,broker_guid,bid,status,item_guid,item_entry,item_count FROM ai_playerbot_estate_bid_claim WHERE auction_id="+
            std::to_string(r.auctionId)+" FOR UPDATE").c_str());if(!claim)return false;Field* f=claim->Fetch();
        if(f[0].GetUInt64()!=r.estateId||f[1].GetUInt32()!=r.brokerGuid||f[2].GetUInt32()!=r.bid||f[3].GetUInt32()!=0||
            f[4].GetUInt32()!=r.itemGuid||f[5].GetUInt32()!=r.itemEntry||f[6].GetUInt32()!=r.itemCount)return false;
        auto a=c.Query(("SELECT itemowner,itemguid,item_template,item_count,buyguid,lastbid,time,moneyTime FROM auction WHERE id="+
            std::to_string(r.auctionId)+" FOR UPDATE").c_str());if(!a)return false;Field*x=a->Fetch();
        return x[0].GetUInt32()==r.owner&&x[1].GetUInt32()==r.itemGuid&&x[2].GetUInt32()==r.itemEntry&&x[3].GetUInt32()==r.itemCount&&
            x[4].GetUInt32()==r.brokerGuid&&x[5].GetUInt32()==r.bid&&x[6].GetUInt64()==r.expiresAt&&!x[7].GetUInt64();
    },[r](SqlConnection& c)
    {
        return c.Execute(("UPDATE ai_playerbot_estate_bid_claim SET status=2,mail_id="+std::to_string(r.mailId)+
            ",updated_at=UNIX_TIMESTAMP() WHERE auction_id="+std::to_string(r.auctionId)).c_str())&&Post(c,r);
    });
#endif
}
std::future<bool> RandomBotEstateStore::ConfirmClaimWin(R r)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 4
    std::promise<bool> p;auto f=p.get_future();p.set_value(false);return f;
#else
    return CharacterDatabase.QueueTransaction([r](SqlConnection& c){return Valid(r)&&Post(c,r);});
#endif
}
