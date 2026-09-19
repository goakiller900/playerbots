#include "RandomBotLifecycleStore.h"
#include "Database/DatabaseEnv.h"
#include <algorithm>

namespace
{
    using Bid = RandomBotLifecycleStore::AuctionBid;

    bool Valid(const Bid& r)
    {
        if (!r.auctionId || !r.owner || !r.itemGuid || !r.itemEntry || !r.itemCount ||
            !r.bidder || !r.bidderAccount || r.bidder == r.owner || r.bidderAccount == r.account ||
            !r.expiresAt || r.bid <= r.oldBid || r.bid < r.startBid || r.bid > r.moneyBefore ||
            std::find(r.eligibleAccounts.begin(), r.eligibleAccounts.end(), r.account) == r.eligibleAccounts.end())
            return false;
        const bool won = r.buyout && r.bid == r.buyout;
        if ((r.buyout && r.bid > r.buyout) || won != bool(r.winnerMailId) || won != bool(r.payoutAt))
            return false;
        // Match the pinned handler's funds and increment checks, including its
        // full-price funds check for a bidder raising their own previous bid.
        if (!won && uint64(r.bid) < uint64(r.oldBid) + std::max(uint32(1), (r.oldBid / 100) * 5))
            return false;
        const bool refund = r.oldBidder && r.oldBidder != r.bidder;
        return refund == bool(r.refundMailId) && (!r.oldBidder || r.oldBid) &&
            (!r.refundMailId || r.refundMailId != r.winnerMailId);
    }

    uint32 Debit(const Bid& r)
    {
        return r.bid - (r.oldBidder == r.bidder ? r.oldBid : 0);
    }

    std::string Key(const Bid& r)
    {
        return "auction_id=" + std::to_string(r.auctionId) + " AND bid=" + std::to_string(r.bid);
    }

    std::string Match(const Bid& r)
    {
        return Key(r) + " AND owner=" + std::to_string(r.owner) +
            " AND account=" + std::to_string(r.account) +
            " AND item_guid=" + std::to_string(r.itemGuid) +
            " AND item_entry=" + std::to_string(r.itemEntry) +
            " AND item_count=" + std::to_string(r.itemCount) +
            " AND old_bidder=" + std::to_string(r.oldBidder) + " AND old_bid=" + std::to_string(r.oldBid) +
            " AND bidder=" + std::to_string(r.bidder) + " AND bidder_account=" + std::to_string(r.bidderAccount) +
            " AND money_before=" + std::to_string(r.moneyBefore) +
            " AND start_bid=" + std::to_string(r.startBid) + " AND buyout=" + std::to_string(r.buyout) +
            " AND expires_at=" + std::to_string(r.expiresAt) + " AND payout_at=" + std::to_string(r.payoutAt) +
            " AND refund_mail_id=" + std::to_string(r.refundMailId) +
            " AND winner_mail_id=" + std::to_string(r.winnerMailId);
    }

#if defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) && CMANGOS_ASYNC_TRANSACTION_CALLBACK >= 3
    bool Postconditions(SqlConnection& c, const Bid& r)
    {
        auto auction = c.Query(("SELECT COUNT(*) FROM auction WHERE id=" + std::to_string(r.auctionId) +
            " AND itemowner=" + std::to_string(r.owner) + " AND buyguid=" + std::to_string(r.bidder) +
            " AND lastbid=" + std::to_string(r.bid) + " AND moneyTime=" + std::to_string(r.payoutAt) +
            " AND itemguid=" + std::to_string(r.winnerMailId ? 0 : r.itemGuid) +
            " AND time=" + std::to_string(r.expiresAt)).c_str());
        if (!auction || auction->Fetch()[0].GetUInt32() != 1)
            return false;
        auto money = c.Query(("SELECT COUNT(*) FROM characters WHERE guid=" + std::to_string(r.bidder) +
            " AND account=" + std::to_string(r.bidderAccount) +
            " AND money=" + std::to_string(r.moneyBefore - Debit(r))).c_str());
        if (!money || money->Fetch()[0].GetUInt32() != 1)
            return false;
        if (r.refundMailId)
        {
            auto refund = c.Query(("SELECT COUNT(*) FROM mail WHERE id=" + std::to_string(r.refundMailId) +
                " AND receiver=" + std::to_string(r.oldBidder) + " AND messageType=2 AND cod=0 AND has_items=0"
                " AND money=" + std::to_string(r.oldBid)).c_str());
            if (!refund || refund->Fetch()[0].GetUInt32() != 1)
                return false;
        }
        if (r.winnerMailId)
        {
            auto winner = c.Query(("SELECT COUNT(*) FROM mail m INNER JOIN mail_items mi ON mi.mail_id=m.id "
                "INNER JOIN item_instance i ON i.guid=mi.item_guid WHERE m.id=" + std::to_string(r.winnerMailId) +
                " AND m.receiver=" + std::to_string(r.bidder) + " AND mi.receiver=m.receiver AND i.owner_guid=m.receiver"
                " AND m.messageType=2 AND m.money=0 AND m.cod=0 AND m.has_items=1 AND i.guid=" +
                std::to_string(r.itemGuid) + " AND i.itemEntry=" + std::to_string(r.itemEntry) +
                " AND mi.item_template=i.itemEntry AND i.count=" + std::to_string(r.itemCount)).c_str());
            auto inventory = c.Query(("SELECT COUNT(*) FROM character_inventory WHERE item=" + std::to_string(r.itemGuid)).c_str());
            if (!winner || winner->Fetch()[0].GetUInt32() != 1 || !inventory || inventory->Fetch()[0].GetUInt32())
                return false;
        }
        return true;
    }
#endif
}

std::future<bool> RandomBotLifecycleStore::CommitAuctionBid(AuctionBid r)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 3
    CharacterDatabase.RollbackTransaction();
    std::promise<bool> unavailable;
    auto result = unavailable.get_future();
    unavailable.set_value(false);
    return result;
#else
    return CharacterDatabase.CommitTransactionAcknowledged([r](SqlConnection& c)
    {
        if (!Valid(r))
            return false;
        auto engines = c.Query("SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE() "
            "AND table_name IN ('characters','auction','mail','mail_items','item_instance',"
            "'character_inventory','ai_playerbot_lifecycle','ai_playerbot_lifecycle_auction_bid') AND engine='InnoDB'");
        if (!engines || engines->Fetch()[0].GetUInt32() != 8)
            return false;
        auto state = c.Query(("SELECT account,status,estate_prepared FROM ai_playerbot_lifecycle WHERE guid=" +
            std::to_string(r.owner) + " FOR UPDATE").c_str());
        if (!state || state->Fetch()[0].GetUInt32() != r.account || state->Fetch()[1].GetUInt32() < 2 ||
            state->Fetch()[1].GetUInt32() > 5 || state->Fetch()[2].GetBool())
            return false;
        auto duplicate = c.Query(("SELECT COUNT(*) FROM ai_playerbot_lifecycle_auction_bid WHERE " + Key(r)).c_str());
        if (!duplicate || duplicate->Fetch()[0].GetUInt32())
            return false; // Never replay queued serialization, even after lost COMMIT acknowledgement.
        auto auction = c.Query(("SELECT itemowner,itemguid,item_template,item_count,buyguid,lastbid,startbid,"
            "buyoutprice,time,moneyTime FROM auction WHERE id=" + std::to_string(r.auctionId) + " FOR UPDATE").c_str());
        if (!auction)
            return false;
        Field* f = auction->Fetch();
        if (f[0].GetUInt32() != r.owner || f[1].GetUInt32() != r.itemGuid || f[2].GetUInt32() != r.itemEntry ||
            f[3].GetUInt32() != r.itemCount || f[4].GetUInt32() != r.oldBidder || f[5].GetUInt32() != r.oldBid ||
            f[6].GetUInt32() != r.startBid || f[7].GetUInt32() != r.buyout || f[8].GetUInt64() != r.expiresAt ||
            f[9].GetUInt64())
            return false;
        auto buyer = c.Query(("SELECT account,money FROM characters WHERE guid=" + std::to_string(r.bidder) + " FOR UPDATE").c_str());
        if (!buyer || buyer->Fetch()[0].GetUInt32() != r.bidderAccount || buyer->Fetch()[1].GetUInt32() != r.moneyBefore)
            return false;
        return c.Execute(("INSERT INTO ai_playerbot_lifecycle_auction_bid "
            "(auction_id,owner,account,item_guid,item_entry,item_count,old_bidder,old_bid,bidder,bidder_account,"
            "bid,start_bid,buyout,money_before,expires_at,payout_at,refund_mail_id,winner_mail_id) VALUES (" +
            std::to_string(r.auctionId) + "," + std::to_string(r.owner) + "," + std::to_string(r.account) + "," +
            std::to_string(r.itemGuid) + "," + std::to_string(r.itemEntry) + "," + std::to_string(r.itemCount) + "," +
            std::to_string(r.oldBidder) + "," + std::to_string(r.oldBid) + "," + std::to_string(r.bidder) + "," +
            std::to_string(r.bidderAccount) + "," + std::to_string(r.bid) + "," + std::to_string(r.startBid) + "," +
            std::to_string(r.buyout) + "," + std::to_string(r.moneyBefore) + "," + std::to_string(r.expiresAt) + "," +
            std::to_string(r.payoutAt) + "," + std::to_string(r.refundMailId) + "," + std::to_string(r.winnerMailId) + ")").c_str());
    }, [r](SqlConnection& c) { return Postconditions(c, r); });
#endif
}

std::future<bool> RandomBotLifecycleStore::ConfirmAuctionBid(AuctionBid r)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 3
    std::promise<bool> unavailable;
    auto result = unavailable.get_future();
    unavailable.set_value(false);
    return result;
#else
    return CharacterDatabase.QueueTransaction([r](SqlConnection& c)
    {
        if (!Valid(r))
            return false;
        auto receipt = c.Query(("SELECT COUNT(*) FROM ai_playerbot_lifecycle_auction_bid WHERE " + Match(r)).c_str());
        // The caller must retain its fences through confirmation. After restart,
        // recovery reads the ledger and persisted balances, not stale live Player data.
        return receipt && receipt->Fetch()[0].GetUInt32() == 1 && Postconditions(c, r);
    });
#endif
}
