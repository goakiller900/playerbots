#include "RandomBotLifecycleStore.h"
#include "Database/DatabaseEnv.h"
#include <algorithm>
#include <limits>

std::future<bool> RandomBotLifecycleStore::SettleAuctionPayout(AuctionPayout request)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 2
    std::promise<bool> unavailable;
    auto result = unavailable.get_future();
    unavailable.set_value(false);
    return result;
#else
    return CharacterDatabase.QueueTransaction([request](SqlConnection& connection)
    {
        if (!request.owner || !request.auctionId || !request.payoutAt || !request.bid ||
            std::find(request.eligibleAccounts.begin(), request.eligibleAccounts.end(), request.account) ==
                request.eligibleAccounts.end())
            return false;
        const uint64 gross = uint64(request.bid) + request.deposit;
        if (request.cut > gross)
            return false;
        const uint64 proceeds = gross - request.cut;
        const std::string owner = std::to_string(request.owner);
        const std::string auction = std::to_string(request.auctionId);
        auto engines = connection.Query(
            "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE() "
            "AND table_name IN ('auction','characters','ai_playerbot_lifecycle',"
            "'ai_playerbot_lifecycle_auction') AND engine='InnoDB'");
        if (!engines || engines->Fetch()[0].GetUInt32() != 4)
            return false;

        auto lifecycle = connection.Query(("SELECT account,status,estate_prepared,estate_escrow,"
            "auction_income,auction_fees FROM ai_playerbot_lifecycle WHERE guid=" + owner + " FOR UPDATE").c_str());
        if (!lifecycle || lifecycle->Fetch()[0].GetUInt32() != request.account)
            return false;
        auto receipt = connection.Query(("SELECT COUNT(*) FROM ai_playerbot_lifecycle_auction WHERE auction_id=" +
            auction).c_str());
        auto exists = connection.Query(("SELECT COUNT(*) FROM auction WHERE id=" + auction).c_str());
        if (!receipt || !exists)
            return false;
        if (receipt->Fetch()[0].GetUInt32())
        {
            // Auction IDs are reusable across restart in the pinned core. A
            // receipt PLUS a live auction row is a conflict, never success.
            if (exists->Fetch()[0].GetUInt32())
                return false;
            auto matched = connection.Query(("SELECT COUNT(*) FROM ai_playerbot_lifecycle_auction WHERE auction_id=" +
                auction + " AND owner=" + owner + " AND bidder=" + std::to_string(request.bidder) +
                " AND item_entry=" + std::to_string(request.itemEntry) +
                " AND bid=" + std::to_string(request.bid) + " AND deposit=" + std::to_string(request.deposit) +
                " AND cut=" + std::to_string(request.cut) + " AND expires_at=" + std::to_string(request.expiresAt) +
                " AND payout_at=" + std::to_string(request.payoutAt)).c_str());
            return matched && matched->Fetch()[0].GetUInt32() == 1;
        }
        if (!exists->Fetch()[0].GetUInt32())
            return false; // Neither auction nor receipt: missing data, not completion.
        Field* state = lifecycle->Fetch();
        if (state[1].GetUInt32() < 2 || state[1].GetUInt32() > 5 || state[2].GetBool())
            return false;
        if (state[3].GetUInt64() > std::numeric_limits<uint64>::max() - proceeds ||
            state[4].GetUInt64() > std::numeric_limits<uint64>::max() - proceeds ||
            state[5].GetUInt64() > std::numeric_limits<uint64>::max() - request.cut)
            return false;
        auto character = connection.Query(("SELECT account,online FROM characters WHERE guid=" + owner + " FOR UPDATE").c_str());
        if (!character || character->Fetch()[0].GetUInt32() != request.account || character->Fetch()[1].GetBool())
            return false;
        auto current = connection.Query(("SELECT itemowner,buyguid,item_template,lastbid,deposit,time,moneyTime,itemguid,"
            "moneyTime<UNIX_TIMESTAMP() FROM auction WHERE id=" + auction + " FOR UPDATE").c_str());
        if (!current)
            return false;
        Field* fields = current->Fetch();
        if (fields[0].GetUInt32() != request.owner || fields[1].GetUInt32() != request.bidder ||
            fields[2].GetUInt32() != request.itemEntry || fields[3].GetUInt32() != request.bid ||
            fields[4].GetUInt32() != request.deposit || fields[5].GetUInt64() != request.expiresAt ||
            fields[6].GetUInt64() != request.payoutAt || fields[7].GetUInt32() || !fields[8].GetBool())
            return false;
        if (!connection.Execute(("INSERT INTO ai_playerbot_lifecycle_auction "
            "(auction_id,owner,bidder,item_entry,bid,deposit,cut,proceeds,expires_at,payout_at,settled_at) VALUES (" +
            auction + "," + owner + "," + std::to_string(request.bidder) + "," + std::to_string(request.itemEntry) +
            "," + std::to_string(request.bid) + "," + std::to_string(request.deposit) + "," +
            std::to_string(request.cut) + "," + std::to_string(proceeds) + "," +
            std::to_string(request.expiresAt) + "," + std::to_string(request.payoutAt) +
            ",UNIX_TIMESTAMP())").c_str()))
            return false;
        if (!connection.Execute(("UPDATE ai_playerbot_lifecycle SET estate_escrow=estate_escrow+" +
            std::to_string(proceeds) + ",auction_income=auction_income+" + std::to_string(proceeds) +
            ",auction_fees=auction_fees+" + std::to_string(request.cut) +
            ",updated_at=UNIX_TIMESTAMP() WHERE guid=" + owner).c_str()))
            return false;
        // Same operation as AuctionEntry::DeleteFromDB, on THIS transaction's
        // connection. Do not call the queued core method from the worker.
        return connection.Execute(("DELETE FROM auction WHERE id=" + auction).c_str());
    });
#endif
}
