#include "RandomBotEstateStore.h"
#include "Database/DatabaseEnv.h"

namespace
{
using R = RandomBotEstateAuctionResolution;
bool Valid(R const& r)
{
    if (r.kind < ESTATE_AUCTION_EXPIRED || r.kind > ESTATE_AUCTION_PAYOUT || !r.auctionId ||
        !r.owner || !r.itemEntry || !r.itemCount || !r.mailId)
        return false;
    if (r.kind == ESTATE_AUCTION_EXPIRED)
        return r.itemGuid && !r.bidder && !r.bid && !r.payoutAt;
    if (r.kind == ESTATE_AUCTION_WON)
        return r.itemGuid && r.bidder && r.bid && r.payoutAt;
    return !r.itemGuid && r.bidder && r.bid && r.payoutAt && r.cut <= uint64(r.bid) + r.deposit;
}

uint32 Status(R const& r) { return r.kind == ESTATE_AUCTION_WON ? 1 : (r.kind == ESTATE_AUCTION_EXPIRED ? 2 : 3); }
const char* MailColumn(R const& r)
{
    return r.kind == ESTATE_AUCTION_WON ? "winner_mail_id" :
        (r.kind == ESTATE_AUCTION_EXPIRED ? "return_mail_id" : "seller_mail_id");
}

#if defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) && CMANGOS_ASYNC_TRANSACTION_CALLBACK >= 4
bool Post(SqlConnection& c, R const& r)
{
    auto map=c.Query(("SELECT COUNT(*) FROM ai_playerbot_estate_auction WHERE auction_id="+
        std::to_string(r.auctionId)+" AND status="+std::to_string(Status(r))+" AND "+MailColumn(r)+"="+
        std::to_string(r.mailId)).c_str());
    if(!map || map->Fetch()[0].GetUInt32()!=1) return false;
    auto mail=c.Query(("SELECT COUNT(*) FROM mail WHERE id="+std::to_string(r.mailId)+" AND messageType=2 AND cod=0").c_str());
    if(!mail || mail->Fetch()[0].GetUInt32()!=1) return false;
    if(r.kind==ESTATE_AUCTION_WON)
    {
        auto a=c.Query(("SELECT COUNT(*) FROM auction WHERE id="+std::to_string(r.auctionId)+" AND itemguid=0 AND buyguid="+
            std::to_string(r.bidder)+" AND lastbid="+std::to_string(r.bid)+" AND moneyTime="+
            std::to_string(r.payoutAt)).c_str());
        auto i=c.Query(("SELECT COUNT(*) FROM item_instance i INNER JOIN mail_items mi ON mi.item_guid=i.guid WHERE mi.mail_id="+
            std::to_string(r.mailId)+" AND i.guid="+std::to_string(r.itemGuid)+" AND i.owner_guid="+
            std::to_string(r.bidder)).c_str());
        return a&&a->Fetch()[0].GetUInt32()==1&&i&&i->Fetch()[0].GetUInt32()==1;
    }
    auto gone=c.Query(("SELECT COUNT(*) FROM auction WHERE id="+std::to_string(r.auctionId)).c_str());
    if(!gone || gone->Fetch()[0].GetUInt32()) return false;
    if(r.kind==ESTATE_AUCTION_EXPIRED)
    {
        auto i=c.Query(("SELECT COUNT(*) FROM item_instance i INNER JOIN mail_items mi ON mi.item_guid=i.guid WHERE mi.mail_id="+
            std::to_string(r.mailId)+" AND i.guid="+std::to_string(r.itemGuid)+" AND i.owner_guid="+
            std::to_string(r.owner)).c_str());
        return i&&i->Fetch()[0].GetUInt32()==1;
    }
    return true;
}
#endif
}

std::future<bool> RandomBotEstateStore::CommitAuctionResolution(R r)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 4
    CharacterDatabase.RollbackTransaction(); std::promise<bool> p; auto f=p.get_future(); p.set_value(false); return f;
#else
    return CharacterDatabase.CommitTransactionAcknowledged([r](SqlConnection& c)
    {
        if(!Valid(r)) return false;
        auto engines=c.Query("SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE() AND "
            "table_name IN ('auction','mail','mail_items','item_instance','ai_playerbot_estate_auction') AND engine='InnoDB'");
        if(!engines||engines->Fetch()[0].GetUInt32()!=5) return false;
        auto map=c.Query(("SELECT broker_guid,item_guid,item_entry,item_count,status,bidder,bid,deposit,auction_cut,expires_at,payout_at,"+
            std::string(MailColumn(r))+" FROM ai_playerbot_estate_auction WHERE auction_id="+std::to_string(r.auctionId)+" FOR UPDATE").c_str());
        if(!map) return false;
        Field* f=map->Fetch();
        const uint32 expectedStatus=r.kind==ESTATE_AUCTION_PAYOUT?1:0;
        if(f[0].GetUInt32()!=r.owner||f[1].GetUInt32()!=r.itemGuid||f[2].GetUInt32()!=r.itemEntry||
            f[3].GetUInt32()!=r.itemCount||f[4].GetUInt32()!=expectedStatus||f[5].GetUInt32()!=r.bidder||
            f[6].GetUInt32()!=r.bid||f[7].GetUInt32()!=r.deposit||
            f[8].GetUInt32()!=(r.kind==ESTATE_AUCTION_WON?0:r.cut)||
            f[9].GetUInt64()!=r.expiresAt||f[10].GetUInt64()!=(r.kind==ESTATE_AUCTION_WON?0:r.payoutAt)||
            f[11].GetUInt32()) return false;
        auto a=c.Query(("SELECT itemowner,itemguid,item_template,item_count,buyguid,lastbid,deposit,time,moneyTime FROM auction WHERE id="+
            std::to_string(r.auctionId)+" FOR UPDATE").c_str());
        if(!a) return false;
        Field* x=a->Fetch();
        return x[0].GetUInt32()==r.owner&&x[1].GetUInt32()==r.itemGuid&&x[2].GetUInt32()==r.itemEntry&&
            x[3].GetUInt32()==r.itemCount&&x[4].GetUInt32()==r.bidder&&x[5].GetUInt32()==r.bid&&
            x[6].GetUInt32()==r.deposit&&x[7].GetUInt64()==r.expiresAt&&
            x[8].GetUInt64()==(r.kind==ESTATE_AUCTION_WON?0:r.payoutAt);
    },[r](SqlConnection& c)
    {
        if(!c.Execute(("UPDATE ai_playerbot_estate_auction SET status="+std::to_string(Status(r))+","+MailColumn(r)+"="+
            std::to_string(r.mailId)+",auction_cut="+std::to_string(r.cut)+",updated_at=UNIX_TIMESTAMP() WHERE auction_id="+
            std::to_string(r.auctionId)).c_str())) return false;
        return Post(c,r);
    });
#endif
}

std::future<bool> RandomBotEstateStore::ConfirmAuctionResolution(R r)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 4
    std::promise<bool> p; auto f=p.get_future(); p.set_value(false); return f;
#else
    return CharacterDatabase.QueueTransaction([r](SqlConnection& c){return Valid(r)&&Post(c,r);});
#endif
}
