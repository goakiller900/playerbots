#include "RandomBotEstateStore.h"
#include "Database/DatabaseEnv.h"
#include <algorithm>

std::future<bool> RandomBotEstateStore::TransferInventoryLot(RandomBotEstateIntakeLot r)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 3
    std::promise<bool> unavailable;
    auto result = unavailable.get_future();
    unavailable.set_value(false);
    return result;
#else
    return CharacterDatabase.QueueTransaction([r](SqlConnection& connection)
    {
        if (!r.estateId || !r.originalGuid || !r.brokerGuid || r.originalGuid == r.brokerGuid ||
            !r.brokerAccount || r.originalAccount == r.brokerAccount || !r.itemGuid || !r.itemEntry || !r.itemCount ||
            std::find(r.eligibleAccounts.begin(), r.eligibleAccounts.end(), r.originalAccount) == r.eligibleAccounts.end() ||
            std::find(r.eligibleAccounts.begin(), r.eligibleAccounts.end(), r.brokerAccount) != r.eligibleAccounts.end())
            return false;
        const std::string estate = std::to_string(r.estateId), item = std::to_string(r.itemGuid);
        const std::string original = std::to_string(r.originalGuid), broker = std::to_string(r.brokerGuid);
        auto engines = connection.Query("SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE() "
            "AND table_name IN ('characters','character_inventory','item_instance','ai_playerbot_estate',"
            "'ai_playerbot_estate_lot','ai_playerbot_estate_operation','ai_playerbot_estate_broker',"
            "'ai_playerbot_lifecycle') AND engine='InnoDB'");
        if (!engines || engines->Fetch()[0].GetUInt32() != 8)
            return false;
        auto state = connection.Query(("SELECT original_guid,original_account,broker_guid,broker_account,status,assets_detached "
            "FROM ai_playerbot_estate WHERE estate_id=" + estate + " FOR UPDATE").c_str());
        if (!state)
            return false;
        Field* f = state->Fetch();
        if (f[0].GetUInt32() != r.originalGuid || f[1].GetUInt32() != r.originalAccount ||
            f[2].GetUInt32() != r.brokerGuid || f[3].GetUInt32() != r.brokerAccount)
            return false;
        const std::string key = "estate_id=" + estate + " AND kind=1 AND asset_id=" + item;
        auto receipt = connection.Query(("SELECT COUNT(*) FROM ai_playerbot_estate_operation WHERE " + key +
            " AND item_entry=" + std::to_string(r.itemEntry) + " AND item_count=" + std::to_string(r.itemCount) +
            " AND amount=0").c_str());
        if (!receipt)
            return false;
        if (receipt->Fetch()[0].GetUInt32())
        {
            // Conservative acknowledgement: if liquidation has moved the lot on,
            // its recovery coordinator must examine that later operation instead.
            auto completed = connection.Query(("SELECT COUNT(*) FROM ai_playerbot_estate_lot l "
                "INNER JOIN item_instance i ON i.guid=l.item_guid WHERE l.estate_id=" + estate +
                " AND l.item_guid=" + item + " AND l.status=0 AND i.owner_guid=" + broker +
                " AND i.itemEntry=" + std::to_string(r.itemEntry) + " AND i.count=" + std::to_string(r.itemCount) +
                " AND NOT EXISTS (SELECT 1 FROM character_inventory WHERE item=" + item + ")").c_str());
            return completed && completed->Fetch()[0].GetUInt32() == 1;
        }
        if (f[4].GetUInt32() != 0 || f[5].GetBool())
            return false;
        auto lifecycle = connection.Query(("SELECT account,status FROM ai_playerbot_lifecycle WHERE guid=" +
            original + " FOR UPDATE").c_str());
        if (!lifecycle || lifecycle->Fetch()[0].GetUInt32() != r.originalAccount ||
            lifecycle->Fetch()[1].GetUInt32() != 2)
            return false;
        auto donor = connection.Query(("SELECT account,online FROM characters WHERE guid=" + original + " FOR UPDATE").c_str());
        auto service = connection.Query(("SELECT c.account,c.online,b.enabled FROM characters c "
            "INNER JOIN ai_playerbot_estate_broker b ON b.guid=c.guid AND b.account=c.account "
            "WHERE c.guid=" + broker + " FOR UPDATE").c_str());
        if (!donor || donor->Fetch()[0].GetUInt32() != r.originalAccount || donor->Fetch()[1].GetBool() ||
            !service || service->Fetch()[0].GetUInt32() != r.brokerAccount || service->Fetch()[1].GetBool() ||
            !service->Fetch()[2].GetBool())
            return false;
        auto asset = connection.Query(("SELECT i.itemEntry,i.count FROM item_instance i "
            "INNER JOIN character_inventory v ON v.item=i.guid AND v.guid=i.owner_guid WHERE i.guid=" +
            item + " AND i.owner_guid=" + original + " FOR UPDATE").c_str());
        if (!asset || asset->Fetch()[0].GetUInt32() != r.itemEntry || asset->Fetch()[1].GetUInt32() != r.itemCount)
            return false;
        auto unsafe = connection.Query(("SELECT (SELECT COUNT(*) FROM character_inventory WHERE bag=" + item +
            ") + (SELECT COUNT(*) FROM character_gifts WHERE item_guid=" + item +
            ") + (SELECT COUNT(*) FROM item_loot WHERE guid=" + item +
            ") + (SELECT COUNT(*) FROM mail_items WHERE item_guid=" + item +
            ") + (SELECT COUNT(*) FROM auction WHERE itemguid=" + item +
            ") + (SELECT COUNT(*) FROM ai_playerbot_estate_lot WHERE item_guid=" + item + ")").c_str());
        if (!unsafe || unsafe->Fetch()[0].GetUInt64())
            return false;
        // Only ownership and placement change. Item flags, enchantments, charges,
        // durability, count and GUID remain intact; no inventory auto-stacking.
        if (!connection.Execute(("DELETE FROM character_inventory WHERE guid=" + original + " AND item=" + item).c_str()) ||
            !connection.Execute(("UPDATE item_instance SET owner_guid=" + broker + " WHERE guid=" + item +
                " AND owner_guid=" + original).c_str()) ||
            !connection.Execute(("INSERT INTO ai_playerbot_estate_lot "
                "(estate_id,item_guid,item_entry,item_count,status,updated_at) VALUES (" + estate + "," + item + "," +
                std::to_string(r.itemEntry) + "," + std::to_string(r.itemCount) + ",0,UNIX_TIMESTAMP())").c_str()))
            return false;
        return connection.Execute(("INSERT INTO ai_playerbot_estate_operation "
            "(estate_id,kind,asset_id,item_entry,item_count,amount,completed_at) VALUES (" + estate + ",1," + item + "," +
            std::to_string(r.itemEntry) + "," + std::to_string(r.itemCount) + ",0,UNIX_TIMESTAMP())").c_str());
    });
#endif
}
