#include "RandomBotEstateStore.h"
#include "Database/DatabaseEnv.h"
#include <algorithm>

std::future<bool> RandomBotEstateStore::BeginIntake(RandomBotEstateIntake r)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 4
    std::promise<bool> p;auto f=p.get_future();p.set_value(false);return f;
#else
    return CharacterDatabase.QueueTransaction([r](SqlConnection& c)
    {
        if(!r.originalGuid||!r.originalAccount||!r.brokerGuid||!r.brokerAccount||r.originalGuid==r.brokerGuid||
            std::find(r.eligibleAccounts.begin(),r.eligibleAccounts.end(),r.originalAccount)==r.eligibleAccounts.end()||
            std::find(r.eligibleAccounts.begin(),r.eligibleAccounts.end(),r.brokerAccount)!=r.eligibleAccounts.end())return false;
        auto engines=c.Query("SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE() AND table_name IN "
            "('characters','ai_playerbot_lifecycle','ai_playerbot_estate','ai_playerbot_estate_broker') AND engine='InnoDB'");
        if(!engines||engines->Fetch()[0].GetUInt32()!=4)return false;
        auto existing=c.Query(("SELECT broker_guid,broker_account,status FROM ai_playerbot_estate WHERE original_guid="+
            std::to_string(r.originalGuid)+" FOR UPDATE").c_str());
        if(existing)return existing->Fetch()[0].GetUInt32()==r.brokerGuid&&existing->Fetch()[1].GetUInt32()==r.brokerAccount;
        auto life=c.Query(("SELECT account,status,protected FROM ai_playerbot_lifecycle WHERE guid="+
            std::to_string(r.originalGuid)+" FOR UPDATE").c_str());
        auto ch=c.Query(("SELECT account,online,money FROM characters WHERE guid="+std::to_string(r.originalGuid)+" FOR UPDATE").c_str());
        auto broker=c.Query(("SELECT c.account,c.online,b.enabled,b.role FROM characters c INNER JOIN ai_playerbot_estate_broker b "
            "ON b.guid=c.guid AND b.account=c.account WHERE c.guid="+std::to_string(r.brokerGuid)+" FOR UPDATE").c_str());
        if(!life||life->Fetch()[0].GetUInt32()!=r.originalAccount||life->Fetch()[1].GetUInt32()!=2||life->Fetch()[2].GetBool()||
            !ch||ch->Fetch()[0].GetUInt32()!=r.originalAccount||ch->Fetch()[1].GetBool()||!broker||
            broker->Fetch()[0].GetUInt32()!=r.brokerAccount||broker->Fetch()[1].GetBool()||!broker->Fetch()[2].GetBool()||
            broker->Fetch()[3].GetUInt32()!=0)return false;
        const uint64 gold=ch->Fetch()[2].GetUInt64();
        if(!c.Execute(("UPDATE characters SET money=0 WHERE guid="+std::to_string(r.originalGuid)+" AND money="+
            std::to_string(gold)).c_str())||!c.Execute(("INSERT INTO ai_playerbot_estate (original_guid,original_account,broker_guid,"
            "broker_account,status,escrow,starting_gold,updated_at) VALUES ("+std::to_string(r.originalGuid)+","+
            std::to_string(r.originalAccount)+","+std::to_string(r.brokerGuid)+","+std::to_string(r.brokerAccount)+",0,"+
            std::to_string(gold)+","+std::to_string(gold)+",UNIX_TIMESTAMP())").c_str()))return false;
        return c.Execute(("UPDATE ai_playerbot_lifecycle SET disposition="+std::to_string(r.disposition)+",replacement_required="+
            std::to_string(r.replacementRequired?1:0)+",updated_at=UNIX_TIMESTAMP() WHERE guid="+
            std::to_string(r.originalGuid)).c_str());
    });
#endif
}

std::future<bool> RandomBotEstateStore::TransferMail(RandomBotEstateMailIntake r)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 4
    std::promise<bool> p;auto f=p.get_future();p.set_value(false);return f;
#else
    return CharacterDatabase.QueueTransaction([r](SqlConnection& c)
    {
        if(!r.estateId||!r.originalGuid||!r.brokerGuid||!r.mailId||(!r.money&&r.items.empty())||r.items.size()>12)return false;
        auto state=c.Query(("SELECT original_guid,original_account,broker_guid,broker_account,status,assets_detached FROM "
            "ai_playerbot_estate WHERE estate_id="+std::to_string(r.estateId)+" FOR UPDATE").c_str());
        if(!state)return false;Field* s=state->Fetch();
        if(s[0].GetUInt32()!=r.originalGuid||s[1].GetUInt32()!=r.originalAccount||s[2].GetUInt32()!=r.brokerGuid||
            s[3].GetUInt32()!=r.brokerAccount||s[4].GetUInt32()!=0||s[5].GetBool())return false;
        auto mail=c.Query(("SELECT receiver,money,cod,has_items,"
            "IF(messageType=2 AND SUBSTRING_INDEX(subject,':',-1)='2',CAST(SUBSTRING_INDEX(SUBSTRING_INDEX(body,':',2),':',-1) AS UNSIGNED),0),"
            "IF(messageType=2 AND SUBSTRING_INDEX(subject,':',-1)='2',CAST(SUBSTRING_INDEX(SUBSTRING_INDEX(body,':',4),':',-1) AS UNSIGNED),0),"
            "IF(messageType=2 AND SUBSTRING_INDEX(subject,':',-1)='2',CAST(SUBSTRING_INDEX(body,':',-1) AS UNSIGNED),0) FROM mail WHERE id="+
            std::to_string(r.mailId)+" FOR UPDATE").c_str());
        if(!mail)return false;Field* m=mail->Fetch();
        if(m[0].GetUInt32()!=r.originalGuid||m[1].GetUInt64()!=r.money||m[2].GetUInt32()||
            m[3].GetBool()!=!r.items.empty()||m[4].GetUInt32()!=r.auctionBid||m[5].GetUInt32()!=r.auctionDeposit||
            m[6].GetUInt32()!=r.auctionCut)return false;
        if((r.auctionBid||r.auctionDeposit||r.auctionCut)&&(!r.auctionBid||r.auctionCut>uint64(r.auctionBid)+r.auctionDeposit||
            r.money!=uint64(r.auctionBid)+r.auctionDeposit-r.auctionCut))return false;
        for(auto const& item:r.items)
        {
            auto q=c.Query(("SELECT i.itemEntry,i.count FROM mail_items mi INNER JOIN item_instance i ON i.guid=mi.item_guid WHERE "
                "mi.mail_id="+std::to_string(r.mailId)+" AND mi.item_guid="+std::to_string(item.itemGuid)+" AND mi.receiver="+
                std::to_string(r.originalGuid)+" AND i.owner_guid="+std::to_string(r.originalGuid)+" FOR UPDATE").c_str());
            if(!q||q->Fetch()[0].GetUInt32()!=item.itemEntry||q->Fetch()[1].GetUInt32()!=item.itemCount)return false;
            if(!c.Execute(("UPDATE item_instance SET owner_guid="+std::to_string(r.brokerGuid)+" WHERE guid="+
                std::to_string(item.itemGuid)+" AND owner_guid="+std::to_string(r.originalGuid)).c_str())||
                !c.Execute(("INSERT INTO ai_playerbot_estate_lot (estate_id,item_guid,item_entry,item_count,status,updated_at) VALUES ("+
                std::to_string(r.estateId)+","+std::to_string(item.itemGuid)+","+std::to_string(item.itemEntry)+","+
                std::to_string(item.itemCount)+",0,UNIX_TIMESTAMP())").c_str()))return false;
        }
        return c.Execute(("DELETE FROM mail_items WHERE mail_id="+std::to_string(r.mailId)).c_str())&&
            c.Execute(("DELETE FROM mail WHERE id="+std::to_string(r.mailId)).c_str())&&
            c.Execute(("UPDATE ai_playerbot_estate SET escrow=escrow+"+std::to_string(r.money)+
                (r.auctionBid?",auction_income=auction_income+"+std::to_string(r.auctionBid)+
                    ",auction_cuts=auction_cuts+"+std::to_string(r.auctionCut):std::string())+
                ",updated_at=UNIX_TIMESTAMP() WHERE estate_id="+
                std::to_string(r.estateId)).c_str())&&c.Execute(("INSERT INTO ai_playerbot_estate_operation "
                "(estate_id,kind,asset_id,amount,completed_at) VALUES ("+std::to_string(r.estateId)+",5,"+
                std::to_string(r.mailId)+","+std::to_string(r.money)+",UNIX_TIMESTAMP())").c_str());
    });
#endif
}

std::future<bool> RandomBotEstateStore::AdoptAuction(RandomBotEstateAuctionIntake r)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 4
    std::promise<bool> p;auto f=p.get_future();p.set_value(false);return f;
#else
    return CharacterDatabase.QueueTransaction([r](SqlConnection& c)
    {
        const bool pendingPayout=r.payoutAt!=0;
        if(!r.estateId||!r.originalGuid||!r.brokerGuid||!r.auctionId||!r.itemEntry||!r.itemCount||
            (pendingPayout?(!r.bidder||!r.bid||r.itemGuid||r.cut>uint64(r.bid)+r.deposit):!r.itemGuid))return false;
        auto a=c.Query(("SELECT houseid,itemguid,item_template,item_count,itemowner,buyoutprice,time,moneyTime,buyguid,lastbid,startbid,deposit "
            "FROM auction WHERE id="+std::to_string(r.auctionId)+" FOR UPDATE").c_str());if(!a)return false;Field* f=a->Fetch();
        if(f[0].GetUInt32()!=r.houseId||f[1].GetUInt32()!=r.itemGuid||f[2].GetUInt32()!=r.itemEntry||f[3].GetUInt32()!=r.itemCount||
            f[4].GetUInt32()!=r.originalGuid||f[5].GetUInt32()!=r.buyout||f[6].GetUInt64()!=r.expiresAt||f[7].GetUInt64()!=r.payoutAt||
            f[8].GetUInt32()!=r.bidder||f[9].GetUInt32()!=r.bid||f[10].GetUInt32()!=r.startBid||f[11].GetUInt32()!=r.deposit)return false;
        if(pendingPayout)
        {
            // The normal core has already detached the sold item to winner
            // mail and deliberately zeroed auction.itemguid.  Preserve the
            // template/count and auction ID as the payout asset identity; no
            // item is recreated or transferred here.
            return c.Execute(("UPDATE auction SET itemowner="+std::to_string(r.brokerGuid)+" WHERE id="+
                std::to_string(r.auctionId)+" AND itemowner="+std::to_string(r.originalGuid)).c_str())&&
                c.Execute(("INSERT INTO ai_playerbot_estate_auction (auction_id,estate_id,broker_guid,house_id,item_guid,item_entry,item_count,"
                "attempt,status,start_bid,buyout,bidder,bid,deposit,auction_cut,expires_at,payout_at,updated_at) VALUES ("+
                std::to_string(r.auctionId)+","+std::to_string(r.estateId)+","+std::to_string(r.brokerGuid)+","+
                std::to_string(r.houseId)+",0,"+std::to_string(r.itemEntry)+","+std::to_string(r.itemCount)+","+
                std::to_string(r.auctionId)+",1,"+std::to_string(r.startBid)+","+std::to_string(r.buyout)+","+
                std::to_string(r.bidder)+","+std::to_string(r.bid)+","+std::to_string(r.deposit)+","+
                std::to_string(r.cut)+","+std::to_string(r.expiresAt)+","+std::to_string(r.payoutAt)+",UNIX_TIMESTAMP())").c_str())&&
                c.Execute(("UPDATE ai_playerbot_estate SET auction_deposits=auction_deposits+"+std::to_string(r.deposit)+
                ",updated_at=UNIX_TIMESTAMP() WHERE estate_id="+std::to_string(r.estateId)).c_str());
        }
        auto item=c.Query(("SELECT COUNT(*) FROM item_instance WHERE guid="+std::to_string(r.itemGuid)+" AND owner_guid="+
            std::to_string(r.originalGuid)+" AND itemEntry="+std::to_string(r.itemEntry)+" AND count="+
            std::to_string(r.itemCount)+" FOR UPDATE").c_str());if(!item||item->Fetch()[0].GetUInt32()!=1)return false;
        if(!c.Execute(("UPDATE item_instance SET owner_guid="+std::to_string(r.brokerGuid)+" WHERE guid="+std::to_string(r.itemGuid)).c_str())||
            !c.Execute(("UPDATE auction SET itemowner="+std::to_string(r.brokerGuid)+" WHERE id="+std::to_string(r.auctionId)).c_str())||
            !c.Execute(("INSERT INTO ai_playerbot_estate_lot (estate_id,item_guid,item_entry,item_count,status,attempt,auction_id,updated_at) VALUES ("+
            std::to_string(r.estateId)+","+std::to_string(r.itemGuid)+","+std::to_string(r.itemEntry)+","+std::to_string(r.itemCount)+
            ",1,1,"+std::to_string(r.auctionId)+",UNIX_TIMESTAMP())").c_str())||!c.Execute(("INSERT INTO ai_playerbot_estate_auction "
            "(auction_id,estate_id,lot_id,broker_guid,house_id,item_guid,item_entry,item_count,attempt,status,start_bid,buyout,bidder,bid,deposit,"
            "expires_at,updated_at) SELECT "+std::to_string(r.auctionId)+","+std::to_string(r.estateId)+",lot_id,"+
            std::to_string(r.brokerGuid)+","+std::to_string(r.houseId)+","+std::to_string(r.itemGuid)+","+std::to_string(r.itemEntry)+","+
            std::to_string(r.itemCount)+",1,0,"+std::to_string(r.startBid)+","+std::to_string(r.buyout)+","+
            std::to_string(r.bidder)+","+std::to_string(r.bid)+","+std::to_string(r.deposit)+","+
            std::to_string(r.expiresAt)+",UNIX_TIMESTAMP() FROM ai_playerbot_estate_lot WHERE estate_id="+
            std::to_string(r.estateId)+" AND item_guid="+std::to_string(r.itemGuid)).c_str()))return false;
        return c.Execute(("UPDATE ai_playerbot_estate SET auction_deposits=auction_deposits+"+std::to_string(r.deposit)+
            ",updated_at=UNIX_TIMESTAMP() WHERE estate_id="+std::to_string(r.estateId)).c_str());
    });
#endif
}

std::future<bool> RandomBotEstateStore::AdoptBid(RandomBotEstateBidIntake r)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 4
    std::promise<bool> p;auto f=p.get_future();p.set_value(false);return f;
#else
    return CharacterDatabase.QueueTransaction([r](SqlConnection& c)
    {
        if(!r.estateId||!r.originalGuid||!r.bidderBrokerGuid||!r.auctionId||!r.bid)return false;
        auto a=c.Query(("SELECT itemowner,buyguid,lastbid,moneyTime,itemguid,item_template,item_count FROM auction WHERE id="+std::to_string(r.auctionId)+" FOR UPDATE").c_str());
        if(!a||a->Fetch()[0].GetUInt32()==r.bidderBrokerGuid||a->Fetch()[1].GetUInt32()!=r.originalGuid||
            a->Fetch()[2].GetUInt32()!=r.bid||a->Fetch()[3].GetUInt64()||a->Fetch()[4].GetUInt32()!=r.itemGuid||
            a->Fetch()[5].GetUInt32()!=r.itemEntry||a->Fetch()[6].GetUInt32()!=r.itemCount)return false;
        return c.Execute(("UPDATE auction SET buyguid="+std::to_string(r.bidderBrokerGuid)+" WHERE id="+
            std::to_string(r.auctionId)+" AND buyguid="+std::to_string(r.originalGuid)).c_str())&&
            c.Execute(("INSERT INTO ai_playerbot_estate_bid_claim (auction_id,estate_id,broker_guid,bid,status,item_guid,item_entry,item_count,updated_at) VALUES ("+
            std::to_string(r.auctionId)+","+std::to_string(r.estateId)+","+std::to_string(r.bidderBrokerGuid)+","+
            std::to_string(r.bid)+",0,"+std::to_string(r.itemGuid)+","+std::to_string(r.itemEntry)+","+
            std::to_string(r.itemCount)+",UNIX_TIMESTAMP())").c_str());
    });
#endif
}

std::future<bool> RandomBotEstateStore::FinalizeIntake(uint64 estateId,uint32 originalGuid,uint32 originalAccount)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 4
    std::promise<bool> p;auto f=p.get_future();p.set_value(false);return f;
#else
    return CharacterDatabase.QueueTransaction([estateId,originalGuid,originalAccount](SqlConnection& c)
    {
        if(!estateId||!originalGuid||!originalAccount)return false;
        auto e=c.Query(("SELECT original_guid,original_account,status,assets_detached FROM ai_playerbot_estate WHERE estate_id="+
            std::to_string(estateId)+" FOR UPDATE").c_str());
        if(!e||e->Fetch()[0].GetUInt32()!=originalGuid||e->Fetch()[1].GetUInt32()!=originalAccount)
            return false;
        if(e->Fetch()[3].GetBool())return true;
        auto remaining=c.Query(("SELECT (SELECT COUNT(*) FROM characters WHERE guid="+std::to_string(originalGuid)+" AND (money<>0 OR online<>0)) + "
            "(SELECT COUNT(*) FROM character_inventory WHERE guid="+std::to_string(originalGuid)+") + "
            "(SELECT COUNT(*) FROM item_instance WHERE owner_guid="+std::to_string(originalGuid)+") + "
            "(SELECT COUNT(*) FROM auction WHERE itemowner="+std::to_string(originalGuid)+" OR buyguid="+std::to_string(originalGuid)+") + "
            "(SELECT COUNT(*) FROM mail WHERE receiver="+std::to_string(originalGuid)+" AND (money<>0 OR cod<>0 OR has_items<>0)) + "
            "(SELECT COUNT(*) FROM mail_items WHERE receiver="+std::to_string(originalGuid)+")").c_str());
        if(!remaining||remaining->Fetch()[0].GetUInt64())return false;
        return c.Execute(("UPDATE ai_playerbot_estate SET status=2,assets_detached=1,updated_at=UNIX_TIMESTAMP() WHERE estate_id="+
            std::to_string(estateId)).c_str())&&c.Execute(("UPDATE ai_playerbot_lifecycle SET status=10,updated_at=UNIX_TIMESTAMP() WHERE guid="+
            std::to_string(originalGuid)+" AND account="+std::to_string(originalAccount)+" AND status=2").c_str());
    });
#endif
}
