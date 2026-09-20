#include "RandomBotEstateStore.h"
#include "Database/DatabaseEnv.h"

namespace
{
using R=RandomBotEstateAuctionMail;
bool Valid(R const& r)
{
    return r.estateId&&r.lotId&&r.auctionId&&r.brokerGuid&&r.itemEntry&&r.itemCount&&r.mailId&&
        ((r.status==2&&r.itemGuid&&!r.bid&&!r.cut)||(r.status==3&&!r.itemGuid&&r.bid&&r.cut<=uint64(r.bid)+r.deposit));
}
uint64 Payout(R const& r){return uint64(r.bid)+r.deposit-r.cut;}
#if defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) && CMANGOS_ASYNC_TRANSACTION_CALLBACK >= 4
bool Post(SqlConnection& c,R const& r)
{
    auto op=c.Query(("SELECT COUNT(*) FROM ai_playerbot_estate_operation WHERE estate_id="+std::to_string(r.estateId)+
        " AND kind=7 AND asset_id="+std::to_string(r.auctionId)+" AND amount="+
        std::to_string(r.status==3?Payout(r):r.deposit)).c_str());
    auto map=c.Query(("SELECT COUNT(*) FROM ai_playerbot_estate_auction WHERE auction_id="+
        std::to_string(r.auctionId)+" AND status=4").c_str());
    auto mail=c.Query(("SELECT COUNT(*) FROM mail WHERE id="+std::to_string(r.mailId)).c_str());
    return op&&op->Fetch()[0].GetUInt32()==1&&map&&map->Fetch()[0].GetUInt32()==1&&mail&&!mail->Fetch()[0].GetUInt32();
}
#endif
}

std::future<bool> RandomBotEstateStore::ConsumeAuctionMail(R r)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 4
    std::promise<bool> p;auto f=p.get_future();p.set_value(false);return f;
#else
    return CharacterDatabase.QueueTransaction([r](SqlConnection& c)
    {
        if(!Valid(r))return false;
        auto engines=c.Query("SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE() AND "
            "table_name IN ('mail','mail_items','item_instance','ai_playerbot_estate','ai_playerbot_estate_lot',"
            "'ai_playerbot_estate_auction','ai_playerbot_estate_operation') AND engine='InnoDB'");
        if(!engines||engines->Fetch()[0].GetUInt32()!=7)return false;
        auto a=c.Query(("SELECT ea.estate_id,ea.lot_id,ea.broker_guid,ea.item_guid,ea.item_entry,ea.item_count,ea.status,"
            "ea.bid,ea.deposit,ea.auction_cut,"+(r.status==2?std::string("ea.return_mail_id"):std::string("ea.seller_mail_id"))+
            " FROM ai_playerbot_estate_auction ea WHERE ea.auction_id="+std::to_string(r.auctionId)+" FOR UPDATE").c_str());
        if(!a)return false;Field* f=a->Fetch();
        if(f[0].GetUInt64()!=r.estateId||f[1].GetUInt64()!=r.lotId||f[2].GetUInt32()!=r.brokerGuid||
            f[3].GetUInt32()!=r.itemGuid||f[4].GetUInt32()!=r.itemEntry||f[5].GetUInt32()!=r.itemCount||
            f[6].GetUInt32()!=r.status||f[7].GetUInt32()!=r.bid||f[8].GetUInt32()!=r.deposit||
            f[9].GetUInt32()!=r.cut||f[10].GetUInt32()!=r.mailId)return false;
        auto m=c.Query(("SELECT receiver,money,cod,has_items FROM mail WHERE id="+std::to_string(r.mailId)+" FOR UPDATE").c_str());
        if(!m)return false;Field* x=m->Fetch();
        if(x[0].GetUInt32()!=r.brokerGuid||x[2].GetUInt32()||x[1].GetUInt64()!=(r.status==3?Payout(r):0)||
            x[3].GetBool()!=(r.status==2))return false;
        if(r.status==2)
        {
            auto item=c.Query(("SELECT COUNT(*) FROM mail_items mi INNER JOIN item_instance i ON i.guid=mi.item_guid WHERE mi.mail_id="+
                std::to_string(r.mailId)+" AND mi.item_guid="+std::to_string(r.itemGuid)+" AND mi.receiver="+
                std::to_string(r.brokerGuid)+" AND i.owner_guid="+std::to_string(r.brokerGuid)+" AND i.itemEntry="+
                std::to_string(r.itemEntry)+" AND i.count="+std::to_string(r.itemCount)+" FOR UPDATE").c_str());
            if(!item||item->Fetch()[0].GetUInt32()!=1)return false;
            if(!c.Execute(("DELETE FROM mail_items WHERE mail_id="+std::to_string(r.mailId)+" AND item_guid="+
                std::to_string(r.itemGuid)).c_str())||!c.Execute(("UPDATE ai_playerbot_estate_lot SET status=2,auction_id=0,next_action_at="+
                std::to_string(r.nextActionAt)+",updated_at=UNIX_TIMESTAMP() WHERE lot_id="+std::to_string(r.lotId)).c_str())||
                !c.Execute(("UPDATE ai_playerbot_estate SET auction_deposits=auction_deposits-"+std::to_string(r.deposit)+
                ",auction_fees=auction_fees+"+std::to_string(r.deposit)+",updated_at=UNIX_TIMESTAMP() WHERE estate_id="+
                std::to_string(r.estateId)+" AND auction_deposits>="+std::to_string(r.deposit)).c_str()))return false;
        }
        else if(!c.Execute(("UPDATE ai_playerbot_estate_lot SET status=5,auction_id=0,updated_at=UNIX_TIMESTAMP() WHERE lot_id="+
            std::to_string(r.lotId)).c_str())||!c.Execute(("UPDATE ai_playerbot_estate SET escrow=escrow+"+
            std::to_string(Payout(r))+",auction_income=auction_income+"+std::to_string(r.bid)+
            ",auction_deposits=auction_deposits-"+std::to_string(r.deposit)+",auction_cuts=auction_cuts+"+
            std::to_string(r.cut)+",updated_at=UNIX_TIMESTAMP() WHERE estate_id="+std::to_string(r.estateId)+
            " AND auction_deposits>="+std::to_string(r.deposit)).c_str()))return false;
        return c.Execute(("DELETE FROM mail WHERE id="+std::to_string(r.mailId)).c_str())&&
            c.Execute(("UPDATE ai_playerbot_estate_auction SET status=4,updated_at=UNIX_TIMESTAMP() WHERE auction_id="+
            std::to_string(r.auctionId)).c_str())&&c.Execute(("INSERT INTO ai_playerbot_estate_operation "
            "(estate_id,kind,asset_id,item_entry,item_count,amount,completed_at) VALUES ("+std::to_string(r.estateId)+
            ",7,"+std::to_string(r.auctionId)+","+std::to_string(r.itemEntry)+","+std::to_string(r.itemCount)+","+
            std::to_string(r.status==3?Payout(r):r.deposit)+",UNIX_TIMESTAMP())").c_str());
    });
#endif
}

std::future<bool> RandomBotEstateStore::ConfirmAuctionMail(R r)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 4
    std::promise<bool> p;auto f=p.get_future();p.set_value(false);return f;
#else
    return CharacterDatabase.QueueTransaction([r](SqlConnection& c){return Valid(r)&&Post(c,r);});
#endif
}
