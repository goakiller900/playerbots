#include "RandomBotEstateStore.h"
#include "Database/DatabaseEnv.h"

namespace
{
    using Listing = RandomBotEstateAuctionListing;
    bool Valid(const Listing& r)
    {
        return r.estateId && r.lotId && r.auctionId && r.brokerGuid && r.brokerAccount && r.houseId &&
            r.itemGuid && r.itemEntry && r.itemCount && r.attempt && r.startBid && r.buyout >= r.startBid &&
            r.expiresAt;
    }

#if defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) && CMANGOS_ASYNC_TRANSACTION_CALLBACK >= 4
    bool Confirm(SqlConnection& c, const Listing& r)
    {
        auto result = c.Query(("SELECT COUNT(*) FROM ai_playerbot_estate_auction ea "
            "INNER JOIN auction a ON a.id=ea.auction_id INNER JOIN ai_playerbot_estate_lot l ON l.lot_id=ea.lot_id "
            "INNER JOIN item_instance i ON i.guid=ea.item_guid WHERE ea.auction_id=" + std::to_string(r.auctionId) +
            " AND ea.estate_id=" + std::to_string(r.estateId) + " AND ea.lot_id=" + std::to_string(r.lotId) +
            " AND ea.broker_guid=" + std::to_string(r.brokerGuid) + " AND ea.house_id=" + std::to_string(r.houseId) +
            " AND ea.item_guid=" + std::to_string(r.itemGuid) + " AND ea.item_entry=" + std::to_string(r.itemEntry) +
            " AND ea.item_count=" + std::to_string(r.itemCount) + " AND ea.attempt=" + std::to_string(r.attempt) +
            " AND ea.status=0 AND ea.start_bid=" + std::to_string(r.startBid) + " AND ea.buyout=" +
            std::to_string(r.buyout) + " AND ea.deposit=" + std::to_string(r.deposit) + " AND ea.expires_at=" +
            std::to_string(r.expiresAt) + " AND a.houseid=ea.house_id AND a.itemguid=ea.item_guid "
            "AND a.itemowner=ea.broker_guid AND a.buyguid=0 AND a.lastbid=0 AND a.moneyTime=0 "
            "AND a.startbid=ea.start_bid AND a.buyoutprice=ea.buyout AND a.deposit=ea.deposit AND a.time=ea.expires_at "
            "AND l.estate_id=ea.estate_id AND l.item_guid=ea.item_guid AND l.status=1 AND l.auction_id=ea.auction_id "
            "AND i.owner_guid=ea.broker_guid AND i.itemEntry=ea.item_entry AND i.count=ea.item_count").c_str());
        return result && result->Fetch()[0].GetUInt32() == 1;
    }
#endif
}

std::future<bool> RandomBotEstateStore::CreateAuctionListing(RandomBotEstateAuctionListing r)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 4
    std::promise<bool> unavailable;
    auto result = unavailable.get_future();
    unavailable.set_value(false);
    return result;
#else
    return CharacterDatabase.QueueTransaction([r](SqlConnection& c)
    {
        if (!Valid(r))
            return false;
        auto engines = c.Query("SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE() "
            "AND table_name IN ('auction','characters','item_instance','character_inventory','mail_items',"
            "'ai_playerbot_estate','ai_playerbot_estate_lot','ai_playerbot_estate_broker',"
            "'ai_playerbot_estate_auction','ai_playerbot_estate_operation') AND engine='InnoDB'");
        if (!engines || engines->Fetch()[0].GetUInt32() != 10)
            return false;
        const std::string estate = std::to_string(r.estateId), lot = std::to_string(r.lotId);
        const std::string auction = std::to_string(r.auctionId), item = std::to_string(r.itemGuid);
        auto existing = c.Query(("SELECT COUNT(*) FROM ai_playerbot_estate_auction WHERE auction_id=" + auction).c_str());
        if (!existing || existing->Fetch()[0].GetUInt32())
            return false; // Same request is reconciled, never replayed.
        auto state = c.Query(("SELECT broker_guid,broker_account,status,assets_detached,escrow,auction_deposits "
            "FROM ai_playerbot_estate WHERE estate_id=" + estate + " FOR UPDATE").c_str());
        if (!state)
            return false;
        Field* f = state->Fetch();
        if (f[0].GetUInt32() != r.brokerGuid || f[1].GetUInt32() != r.brokerAccount || f[2].GetUInt32() != 2 ||
            !f[3].GetBool() || f[4].GetUInt64() < r.deposit)
            return false;
        auto service = c.Query(("SELECT c.account,c.online,b.enabled,b.role FROM characters c INNER JOIN "
            "ai_playerbot_estate_broker b ON b.guid=c.guid AND b.account=c.account WHERE c.guid=" +
            std::to_string(r.brokerGuid) + " FOR UPDATE").c_str());
        if (!service || service->Fetch()[0].GetUInt32() != r.brokerAccount || service->Fetch()[1].GetBool() ||
            !service->Fetch()[2].GetBool() || service->Fetch()[3].GetUInt32() != 0)
            return false;
        auto asset = c.Query(("SELECT l.item_guid,l.item_entry,l.item_count,l.status,l.attempt,l.auction_id,"
            "i.randomPropertyId FROM ai_playerbot_estate_lot l INNER JOIN item_instance i ON i.guid=l.item_guid "
            "WHERE l.lot_id=" + lot + " AND l.estate_id=" + estate + " AND i.owner_guid=" +
            std::to_string(r.brokerGuid) + " FOR UPDATE").c_str());
        if (!asset)
            return false;
        Field* a = asset->Fetch();
        if (a[0].GetUInt32() != r.itemGuid || a[1].GetUInt32() != r.itemEntry || a[2].GetUInt32() != r.itemCount ||
            (a[3].GetUInt32() != 0 && a[3].GetUInt32() != 2) || a[4].GetUInt32() + 1 != r.attempt ||
            a[5].GetUInt32() || a[6].GetInt32() != r.randomPropertyId)
            return false;
        auto refs = c.Query(("SELECT (SELECT COUNT(*) FROM auction WHERE id=" + auction + " OR itemguid=" + item +
            ") + (SELECT COUNT(*) FROM character_inventory WHERE item=" + item +
            ") + (SELECT COUNT(*) FROM mail_items WHERE item_guid=" + item + ")").c_str());
        if (!refs || refs->Fetch()[0].GetUInt32())
            return false;
        const std::string values = auction + "," + std::to_string(r.houseId) + "," + item + "," +
            std::to_string(r.itemEntry) + "," + std::to_string(r.itemCount) + "," +
            std::to_string(r.randomPropertyId) + "," + std::to_string(r.brokerGuid) + "," +
            std::to_string(r.buyout) + "," + std::to_string(r.expiresAt) + ",0,0,0," +
            std::to_string(r.startBid) + "," + std::to_string(r.deposit);
        if (!c.Execute(("INSERT INTO auction (id,houseid,itemguid,item_template,item_count,item_randompropertyid,"
            "itemowner,buyoutprice,time,moneyTime,buyguid,lastbid,startbid,deposit) VALUES (" + values + ")").c_str()) ||
            !c.Execute(("INSERT INTO ai_playerbot_estate_auction (auction_id,estate_id,lot_id,broker_guid,house_id,"
                "item_guid,item_entry,item_count,attempt,status,start_bid,buyout,deposit,expires_at,updated_at) VALUES (" +
                auction + "," + estate + "," + lot + "," + std::to_string(r.brokerGuid) + "," +
                std::to_string(r.houseId) + "," + item + "," + std::to_string(r.itemEntry) + "," +
                std::to_string(r.itemCount) + "," + std::to_string(r.attempt) + ",0," +
                std::to_string(r.startBid) + "," + std::to_string(r.buyout) + "," +
                std::to_string(r.deposit) + "," + std::to_string(r.expiresAt) + ",UNIX_TIMESTAMP())").c_str()) ||
            !c.Execute(("UPDATE ai_playerbot_estate_lot SET status=1,attempt=" + std::to_string(r.attempt) +
                ",auction_id=" + auction + ",updated_at=UNIX_TIMESTAMP() WHERE lot_id=" + lot).c_str()) ||
            !c.Execute(("UPDATE ai_playerbot_estate SET escrow=escrow-" + std::to_string(r.deposit) +
                ",auction_deposits=auction_deposits+" + std::to_string(r.deposit) +
                ",updated_at=UNIX_TIMESTAMP() WHERE estate_id=" + estate).c_str()))
            return false;
        return c.Execute(("INSERT INTO ai_playerbot_estate_operation "
            "(estate_id,kind,asset_id,item_entry,item_count,amount,completed_at) VALUES (" + estate + ",4," +
            auction + "," + std::to_string(r.itemEntry) + "," + std::to_string(r.itemCount) + "," +
            std::to_string(r.deposit) + ",UNIX_TIMESTAMP())").c_str());
    });
#endif
}

std::future<bool> RandomBotEstateStore::ConfirmAuctionListing(RandomBotEstateAuctionListing r)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 4
    std::promise<bool> unavailable;
    auto result = unavailable.get_future();
    unavailable.set_value(false);
    return result;
#else
    return CharacterDatabase.QueueTransaction([r](SqlConnection& c) { return Valid(r) && Confirm(c, r); });
#endif
}
