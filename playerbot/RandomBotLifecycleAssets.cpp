#include "RandomBotLifecycleStore.h"
#include "RandomBotLifecycleMath.h"
#include "Database/DatabaseEnv.h"
#include <algorithm>
#include <limits>

std::future<bool> RandomBotLifecycleStore::CommitAssetSave(RandomBotAssetSave request)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 2
    CharacterDatabase.RollbackTransaction();
    std::promise<bool> unsupported;
    auto result = unsupported.get_future();
    unsupported.set_value(false);
    return result;
#else
    const std::string guid = std::to_string(request.guid);
    const std::string asset = std::to_string(request.assetGuid);
    const std::string key = "guid=" + guid + " AND kind=" +
        std::to_string(uint32(request.kind)) + " AND asset_guid=" + asset;
    return CharacterDatabase.CommitTransactionAcknowledged(
        [request, guid, asset, key](SqlConnection& connection)
        {
            if (!request.guid || !request.assetGuid ||
                std::find(request.eligibleAccounts.begin(), request.eligibleAccounts.end(), request.account) ==
                    request.eligibleAccounts.end())
                return false;
            if (request.kind != RandomBotAssetOperation::VENDOR_ITEM &&
                request.kind != RandomBotAssetOperation::DESTROY_ITEM &&
                request.kind != RandomBotAssetOperation::COLLECT_MAIL_GOLD)
                return false;
            if (request.kind == RandomBotAssetOperation::DESTROY_ITEM && request.proceeds)
                return false;
            // Core serializers touch these tables, including wrapped-item/loot
            // cleanup. Refuse a mixed transactional/nontransactional schema.
            auto engines = connection.Query(
                "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE() "
                "AND table_name IN ('characters','character_inventory','item_instance','character_gifts',"
                "'item_loot','mail','mail_items','ai_playerbot_lifecycle',"
                "'ai_playerbot_lifecycle_operation') AND engine='InnoDB'");
            if (!engines || engines->Fetch()[0].GetUInt32() != 9)
                return false;
            auto lifecycle = connection.Query(("SELECT account,status,estate_prepared,estate_escrow,"
                "vendor_income,auction_income FROM ai_playerbot_lifecycle WHERE guid=" + guid + " FOR UPDATE").c_str());
            if (!lifecycle)
                return false;
            Field* fields = lifecycle->Fetch();
            const auto required = request.kind == RandomBotAssetOperation::COLLECT_MAIL_GOLD ?
                RandomBotLifecycleStatus::SETTLING_MAIL : RandomBotLifecycleStatus::LIQUIDATING;
            if (fields[0].GetUInt32() != request.account || fields[1].GetUInt32() != uint32(required) ||
                fields[2].GetBool())
                return false;
            for (size_t i = 3; i < 6; ++i)
                if (fields[i].GetUInt64() > std::numeric_limits<uint64>::max() - request.proceeds)
                    return false;
            auto character = connection.Query(("SELECT account,money,online FROM characters WHERE guid=" + guid +
                " FOR UPDATE").c_str());
            if (!character || character->Fetch()[0].GetUInt32() != request.account ||
                character->Fetch()[1].GetUInt32() != request.moneyBefore ||
                character->Fetch()[2].GetBool())
                return false;
            // No retirement auction integration: leave ordinary bidding and
            // settlement untouched. The caller must also hold the offline login
            // lease; the persisted online flag alone is not a login fence.
            auto auctions = connection.Query(("SELECT COUNT(*) FROM auction WHERE itemowner=" + guid +
                " OR buyguid=" + guid).c_str());
            if (!auctions || auctions->Fetch()[0].GetUInt64())
                return false;
            auto done = connection.Query(("SELECT COUNT(*) FROM ai_playerbot_lifecycle_operation WHERE " + key).c_str());
            if (!done || done->Fetch()[0].GetUInt32())
                return false; // Reconcile committed operation; never replay the save queue.

            uint32 sourceType = 0;
            if (request.kind == RandomBotAssetOperation::COLLECT_MAIL_GOLD)
            {
                auto mail = connection.Query(("SELECT money,cod,messageType,deliver_time<=UNIX_TIMESTAMP() "
                    "FROM mail WHERE id=" + asset + " AND receiver=" + guid + " FOR UPDATE").c_str());
                if (!mail || mail->Fetch()[0].GetUInt64() != request.proceeds ||
                    mail->Fetch()[1].GetUInt32() || !mail->Fetch()[3].GetBool())
                    return false; // COD needs its own atomic sender-payment path.
                sourceType = mail->Fetch()[2].GetUInt32();
            }
            else
            {
                auto item = connection.Query(("SELECT i.itemEntry,i.count FROM item_instance i "
                    "INNER JOIN character_inventory v ON v.item=i.guid AND v.guid=i.owner_guid "
                    "WHERE i.guid=" + asset + " AND i.owner_guid=" + guid + " FOR UPDATE").c_str());
                if (!item || item->Fetch()[0].GetUInt32() != request.itemEntry ||
                    item->Fetch()[1].GetUInt32() != request.itemCount || !request.itemCount)
                    return false;
            }
            // Insert belongs to the SAME transaction as the following serialized
            // core saves and escrow update: no visible "completed" half-operation.
            return connection.Execute(("INSERT INTO ai_playerbot_lifecycle_operation "
                "(guid,kind,asset_guid,item_entry,item_count,proceeds,source_type,completed_at) VALUES (" +
                guid + "," + std::to_string(uint32(request.kind)) + "," + asset + "," +
                std::to_string(request.itemEntry) + "," + std::to_string(request.itemCount) + "," +
                std::to_string(request.proceeds) + "," + std::to_string(sourceType) +
                ",UNIX_TIMESTAMP())").c_str());
        },
        [request, guid, asset, key](SqlConnection& connection)
        {
            // Verify core serialization actually removed the full stack or
            // collected the mail gold. A silent serializer no-op rolls back.
            std::string check;
            if (request.kind == RandomBotAssetOperation::COLLECT_MAIL_GOLD)
                check = "SELECT COUNT(*) FROM mail WHERE id=" + asset +
                    " AND receiver=" + guid + " AND money=0 AND cod=0";
            else
                check = "SELECT (SELECT COUNT(*) FROM item_instance WHERE guid=" + asset +
                    ") + (SELECT COUNT(*) FROM character_inventory WHERE item=" + asset +
                    ") + (SELECT COUNT(*) FROM mail_items WHERE item_guid=" + asset + ")";
            auto verified = connection.Query(check.c_str());
            const uint32 expected = request.kind == RandomBotAssetOperation::COLLECT_MAIL_GOLD ? 1 : 0;
            if (!verified || verified->Fetch()[0].GetUInt64() != expected)
                return false;
            auto cash = connection.Query(("SELECT money FROM characters WHERE guid=" + guid).c_str());
            if (!cash || cash->Fetch()[0].GetUInt32() != request.moneyBefore)
                return false; // No cap truncation and no accidental double credit.
            const std::string proceeds = std::to_string(request.proceeds);
            std::string accounting = "estate_escrow=estate_escrow+" + proceeds;
            if (request.kind == RandomBotAssetOperation::VENDOR_ITEM)
                accounting += ",vendor_income=vendor_income+" + proceeds;
            else if (request.kind == RandomBotAssetOperation::COLLECT_MAIL_GOLD)
                accounting += ",auction_income=auction_income+(SELECT IF(source_type=2,proceeds,0) "
                    "FROM ai_playerbot_lifecycle_operation WHERE " + key + ")";
            return connection.Execute(("UPDATE ai_playerbot_lifecycle SET " + accounting +
                ",updated_at=UNIX_TIMESTAMP() WHERE guid=" + guid).c_str());
        });
#endif
}

std::future<bool> RandomBotLifecycleStore::ConfirmAssetSave(RandomBotAssetSave request)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 2
    std::promise<bool> unsupported;
    auto result = unsupported.get_future();
    unsupported.set_value(false);
    return result;
#else
    return CharacterDatabase.QueueTransaction([request](SqlConnection& connection)
    {
        if (!request.guid || !request.assetGuid ||
            std::find(request.eligibleAccounts.begin(), request.eligibleAccounts.end(), request.account) ==
                request.eligibleAccounts.end())
            return false;
        if (request.kind != RandomBotAssetOperation::VENDOR_ITEM &&
            request.kind != RandomBotAssetOperation::DESTROY_ITEM &&
            request.kind != RandomBotAssetOperation::COLLECT_MAIL_GOLD)
            return false;
        const std::string guid = std::to_string(request.guid);
        const std::string asset = std::to_string(request.assetGuid);
        auto receipt = connection.Query(("SELECT COUNT(*) FROM ai_playerbot_lifecycle_operation o "
            "INNER JOIN ai_playerbot_lifecycle l ON l.guid=o.guid "
            "WHERE o.guid=" + guid + " AND l.account=" + std::to_string(request.account) +
            " AND o.kind=" + std::to_string(uint32(request.kind)) + " AND o.asset_guid=" + asset +
            " AND o.item_entry=" + std::to_string(request.itemEntry) +
            " AND o.item_count=" + std::to_string(request.itemCount) +
            " AND o.proceeds=" + std::to_string(request.proceeds)).c_str());
        if (!receipt || receipt->Fetch()[0].GetUInt64() != 1)
            return false;
        // Verify the saved postcondition too. This does not authorize processing
        // a reused ID: the caller must retain the original operation identity.
        const std::string query = request.kind == RandomBotAssetOperation::COLLECT_MAIL_GOLD ?
            "SELECT COUNT(*) FROM mail WHERE id=" + asset +
                " AND receiver=" + guid + " AND money=0 AND cod=0" :
            "SELECT (SELECT COUNT(*) FROM item_instance WHERE guid=" + asset +
                ") + (SELECT COUNT(*) FROM character_inventory WHERE item=" + asset +
                ") + (SELECT COUNT(*) FROM mail_items WHERE item_guid=" + asset + ")";
        auto cleared = connection.Query(query.c_str());
        const uint64 expected = request.kind == RandomBotAssetOperation::COLLECT_MAIL_GOLD ? 1 : 0;
        return cleared && cleared->Fetch()[0].GetUInt64() == expected;
    });
#endif
}
