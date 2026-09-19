#include "RandomBotEstateStore.h"
#include "Database/DatabaseEnv.h"
#include <limits>

std::future<bool> RandomBotEstateStore::CommitLotLiquidation(RandomBotEstateLiquidation r)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 3
    CharacterDatabase.RollbackTransaction();
    std::promise<bool> unavailable;
    auto result = unavailable.get_future();
    unavailable.set_value(false);
    return result;
#else
    const std::string estate = std::to_string(r.estateId), lot = std::to_string(r.lotId), item = std::to_string(r.itemGuid);
    return CharacterDatabase.CommitTransactionAcknowledged([r, estate, lot, item](SqlConnection& connection)
    {
        if (!r.estateId || !r.lotId || !r.brokerGuid || !r.brokerAccount || !r.itemGuid || !r.itemEntry || !r.itemCount ||
            (r.destroy && (!r.destroyAllowed || r.proceeds)) || r.proceeds > std::numeric_limits<uint32>::max())
            return false;
        auto engines = connection.Query("SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE() "
            "AND table_name IN ('characters','item_instance','character_inventory','character_gifts','item_loot',"
            "'ai_playerbot_estate','ai_playerbot_estate_lot','ai_playerbot_estate_broker','ai_playerbot_estate_operation') "
            "AND engine='InnoDB'");
        if (!engines || engines->Fetch()[0].GetUInt32() != 9)
            return false;
        auto state = connection.Query(("SELECT broker_guid,broker_account,status,assets_detached,escrow,vendor_income "
            "FROM ai_playerbot_estate WHERE estate_id=" + estate + " FOR UPDATE").c_str());
        if (!state)
            return false;
        Field* f = state->Fetch();
        if (f[0].GetUInt32() != r.brokerGuid || f[1].GetUInt32() != r.brokerAccount || f[2].GetUInt32() != 2 ||
            !f[3].GetBool() || f[4].GetUInt64() > std::numeric_limits<uint64>::max() - r.proceeds ||
            f[5].GetUInt64() > std::numeric_limits<uint64>::max() - r.proceeds)
            return false;
        auto service = connection.Query(("SELECT c.account,c.online,b.enabled FROM characters c INNER JOIN "
            "ai_playerbot_estate_broker b ON b.guid=c.guid AND b.account=c.account WHERE c.guid=" +
            std::to_string(r.brokerGuid) + " FOR UPDATE").c_str());
        if (!service || service->Fetch()[0].GetUInt32() != r.brokerAccount || service->Fetch()[1].GetBool() ||
            !service->Fetch()[2].GetBool())
            return false;
        auto asset = connection.Query(("SELECT i.itemEntry,i.count,i.charges,i.flags FROM item_instance i INNER JOIN "
            "ai_playerbot_estate_lot l ON l.item_guid=i.guid WHERE l.lot_id=" + lot + " AND l.estate_id=" + estate +
            " AND l.status=3 AND l.auction_id=0 AND i.guid=" + item + " AND i.owner_guid=" + std::to_string(r.brokerGuid) +
            " FOR UPDATE").c_str());
        if (!asset || asset->Fetch()[0].GetUInt32() != r.itemEntry || asset->Fetch()[1].GetUInt32() != r.itemCount ||
            asset->Fetch()[2].GetCppString() != r.charges || (asset->Fetch()[3].GetUInt32() & 8))
            return false; // Wrapped item flag: do not price/destroy the wrapper's contents.
        auto unsafe = connection.Query(("SELECT (SELECT COUNT(*) FROM character_inventory WHERE item=" + item +
            " OR bag=" + item + ") + (SELECT COUNT(*) FROM mail_items WHERE item_guid=" + item +
            ") + (SELECT COUNT(*) FROM auction WHERE itemguid=" + item +
            ") + (SELECT COUNT(*) FROM character_gifts WHERE item_guid=" + item +
            ") + (SELECT COUNT(*) FROM item_loot WHERE guid=" + item + ")").c_str());
        if (!unsafe || unsafe->Fetch()[0].GetUInt64())
            return false;
        return connection.Execute(("INSERT INTO ai_playerbot_estate_operation "
            "(estate_id,kind,asset_id,item_entry,item_count,amount,completed_at) VALUES (" + estate +
            (r.destroy ? ",3," : ",2,") + lot + "," + std::to_string(r.itemEntry) + "," +
            std::to_string(r.itemCount) + "," + std::to_string(r.proceeds) + ",UNIX_TIMESTAMP())").c_str());
    }, [r, estate, lot, item](SqlConnection& connection)
    {
        auto removed = connection.Query(("SELECT COUNT(*) FROM item_instance WHERE guid=" + item).c_str());
        if (!removed || removed->Fetch()[0].GetUInt32())
            return false;
        if (!connection.Execute(("UPDATE ai_playerbot_estate SET escrow=escrow+" + std::to_string(r.proceeds) +
            ",vendor_income=vendor_income+" + std::to_string(r.proceeds) +
            ",updated_at=UNIX_TIMESTAMP() WHERE estate_id=" + estate).c_str()))
            return false;
        return connection.Execute(("UPDATE ai_playerbot_estate_lot SET status=" + std::to_string(r.destroy ? 7 : 6) +
            ",updated_at=UNIX_TIMESTAMP() WHERE lot_id=" + lot + " AND estate_id=" + estate).c_str());
    });
#endif
}

std::future<bool> RandomBotEstateStore::ConfirmLotLiquidation(RandomBotEstateLiquidation r)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 3
    std::promise<bool> unavailable;
    auto result = unavailable.get_future();
    unavailable.set_value(false);
    return result;
#else
    return CharacterDatabase.QueueTransaction([r](SqlConnection& connection)
    {
        if (!r.estateId || !r.lotId || !r.itemGuid || !r.brokerGuid || !r.brokerAccount ||
            (r.destroy && (!r.destroyAllowed || r.proceeds)))
            return false;
        auto receipt = connection.Query(("SELECT COUNT(*) FROM ai_playerbot_estate_operation o "
            "INNER JOIN ai_playerbot_estate_lot l ON l.estate_id=o.estate_id AND l.lot_id=o.asset_id "
            "INNER JOIN ai_playerbot_estate e ON e.estate_id=o.estate_id WHERE o.estate_id=" +
            std::to_string(r.estateId) + " AND o.asset_id=" + std::to_string(r.lotId) +
            " AND o.kind=" + std::to_string(r.destroy ? 3 : 2) +
            " AND o.item_entry=" + std::to_string(r.itemEntry) + " AND o.item_count=" + std::to_string(r.itemCount) +
            " AND o.amount=" + std::to_string(r.proceeds) + " AND l.item_guid=" + std::to_string(r.itemGuid) +
            " AND l.status=" + std::to_string(r.destroy ? 7 : 6) +
            " AND e.broker_guid=" + std::to_string(r.brokerGuid) +
            " AND e.broker_account=" + std::to_string(r.brokerAccount)).c_str());
        if (!receipt || receipt->Fetch()[0].GetUInt32() != 1)
            return false;
        auto removed = connection.Query(("SELECT COUNT(*) FROM item_instance WHERE guid=" + std::to_string(r.itemGuid)).c_str());
        return removed && !removed->Fetch()[0].GetUInt32();
    });
#endif
}
