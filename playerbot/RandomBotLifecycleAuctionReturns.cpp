#include "RandomBotLifecycleStore.h"
#include "Database/DatabaseEnv.h"
#include <algorithm>

namespace
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 3
    std::future<bool> NoReturnSupport()
    {
        std::promise<bool> promise;
        auto future = promise.get_future();
        promise.set_value(false);
        return future;
    }
#endif
#if defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) && CMANGOS_ASYNC_TRANSACTION_CALLBACK >= 3
    std::string ReturnKey(const RandomBotLifecycleStore::AuctionReturn& request)
    {
        return "auction_id=" + std::to_string(request.auctionId) + " AND owner=" +
            std::to_string(request.owner) + " AND item_guid=" + std::to_string(request.itemGuid) +
            " AND item_entry=" + std::to_string(request.itemEntry) +
            " AND item_count=" + std::to_string(request.itemCount) +
            " AND expires_at=" + std::to_string(request.expiresAt) +
            " AND mail_id=" + std::to_string(request.mailId);
    }

    bool ReturnDelivered(SqlConnection& connection, const RandomBotLifecycleStore::AuctionReturn& request)
    {
        auto result = connection.Query(("SELECT COUNT(*) FROM mail m "
            "INNER JOIN mail_items mi ON mi.mail_id=m.id "
            "INNER JOIN item_instance i ON i.guid=mi.item_guid "
            "WHERE m.id=" + std::to_string(request.mailId) + " AND m.receiver=" + std::to_string(request.owner) +
            " AND m.messageType=2 AND m.has_items=1 AND m.money=0 AND m.cod=0 AND mi.receiver=m.receiver "
            "AND mi.item_guid=" + std::to_string(request.itemGuid) + " AND mi.item_template=" +
            std::to_string(request.itemEntry) + " AND i.owner_guid=m.receiver AND i.itemEntry=mi.item_template "
            "AND i.count=" + std::to_string(request.itemCount)).c_str());
        return result && result->Fetch()[0].GetUInt32() == 1;
    }
#endif
}

std::future<bool> RandomBotLifecycleStore::CommitAuctionReturn(AuctionReturn request)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 3
    CharacterDatabase.RollbackTransaction();
    return NoReturnSupport();
#else
    return CharacterDatabase.CommitTransactionAcknowledged(
        [request](SqlConnection& connection)
        {
            if (!request.auctionId || !request.owner || !request.itemGuid || !request.itemCount || !request.mailId ||
                std::find(request.eligibleAccounts.begin(), request.eligibleAccounts.end(), request.account) ==
                    request.eligibleAccounts.end())
                return false;
            const std::string guid = std::to_string(request.owner);
            const std::string auction = std::to_string(request.auctionId);
            auto engines = connection.Query(
                "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE() AND table_name IN "
                "('auction','characters','item_instance','mail','mail_items','ai_playerbot_lifecycle',"
                "'ai_playerbot_lifecycle_auction_return') AND engine='InnoDB'");
            if (!engines || engines->Fetch()[0].GetUInt32() != 7)
                return false;
            auto state = connection.Query(("SELECT account,status,estate_prepared FROM ai_playerbot_lifecycle "
                "WHERE guid=" + guid + " FOR UPDATE").c_str());
            if (!state || state->Fetch()[0].GetUInt32() != request.account || state->Fetch()[1].GetUInt32() < 2 ||
                state->Fetch()[1].GetUInt32() > 5 || state->Fetch()[2].GetBool())
                return false;
            auto character = connection.Query(("SELECT account,online FROM characters WHERE guid=" + guid + " FOR UPDATE").c_str());
            if (!character || character->Fetch()[0].GetUInt32() != request.account || character->Fetch()[1].GetBool())
                return false;
            auto receipt = connection.Query(("SELECT COUNT(*) FROM ai_playerbot_lifecycle_auction_return WHERE auction_id=" +
                auction).c_str());
            if (!receipt || receipt->Fetch()[0].GetUInt32())
                return false; // Confirm separately; don't replay serialized mail INSERTs.
            auto current = connection.Query(("SELECT itemowner,itemguid,item_template,item_count,time,lastbid,moneyTime,"
                "time<UNIX_TIMESTAMP() FROM auction WHERE id=" + auction + " FOR UPDATE").c_str());
            if (!current)
                return false;
            Field* f = current->Fetch();
            if (f[0].GetUInt32() != request.owner || f[1].GetUInt32() != request.itemGuid ||
                f[2].GetUInt32() != request.itemEntry || f[3].GetUInt32() != request.itemCount ||
                f[4].GetUInt64() != request.expiresAt || f[5].GetUInt32() || f[6].GetUInt64() || !f[7].GetBool())
                return false;
            // PK collisions on reserved mail IDs also abort the whole transaction.
            return connection.Execute(("INSERT INTO ai_playerbot_lifecycle_auction_return "
                "(auction_id,owner,item_guid,item_entry,item_count,expires_at,mail_id,settled_at) VALUES (" +
                auction + "," + guid + "," + std::to_string(request.itemGuid) + "," +
                std::to_string(request.itemEntry) + "," + std::to_string(request.itemCount) + "," +
                std::to_string(request.expiresAt) + "," + std::to_string(request.mailId) +
                ",UNIX_TIMESTAMP())").c_str());
        },
        [request](SqlConnection& connection)
        {
            return ReturnDelivered(connection, request) &&
                connection.Execute(("DELETE FROM auction WHERE id=" + std::to_string(request.auctionId)).c_str());
        });
#endif
}

std::future<bool> RandomBotLifecycleStore::ConfirmAuctionReturn(AuctionReturn request)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 3
    return NoReturnSupport();
#else
    return CharacterDatabase.QueueTransaction([request](SqlConnection& connection)
    {
        auto receipt = connection.Query(("SELECT COUNT(*) FROM ai_playerbot_lifecycle_auction_return WHERE " +
            ReturnKey(request)).c_str());
        auto auction = connection.Query(("SELECT COUNT(*) FROM auction WHERE id=" + std::to_string(request.auctionId)).c_str());
        return receipt && auction && receipt->Fetch()[0].GetUInt32() == 1 &&
            !auction->Fetch()[0].GetUInt32() && ReturnDelivered(connection, request);
    });
#endif
}
