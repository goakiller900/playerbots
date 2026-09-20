#include "RandomBotEstateStore.h"
#include "Database/DatabaseEnv.h"
#include <algorithm>

namespace
{
    using Bid = RandomBotEstateAuctionBid;
    uint32 Debit(const Bid& r) { return r.bid - (r.oldBidder == r.bidder ? r.oldBid : 0); }
    bool Won(const Bid& r) { return r.buyout && r.bid == r.buyout; }
    bool Valid(const Bid& r)
    {
        if (!r.auctionId || !r.owner || !r.itemGuid || !r.itemEntry || !r.itemCount || !r.bidder || !r.bidderAccount ||
            r.bidder == r.owner || !r.expiresAt || r.bid <= r.oldBid || r.bid < r.startBid || r.bid > r.moneyBefore ||
            (r.buyout && r.bid > r.buyout) || Won(r) != bool(r.winnerMailId) || Won(r) != bool(r.payoutAt))
            return false;
        if (!Won(r) && uint64(r.bid) < uint64(r.oldBid) + std::max(uint32(1), (r.oldBid / 100) * 5))
            return false;
        const bool refund = r.oldBidder && r.oldBidder != r.bidder;
        return refund == bool(r.refundMailId) && (!r.oldBidder || r.oldBid) &&
            (!r.refundMailId || r.refundMailId != r.winnerMailId) && Debit(r) <= r.moneyBefore;
    }

    std::string Receipt(const Bid& r)
    {
        return "auction_id=" + std::to_string(r.auctionId) + " AND bid=" + std::to_string(r.bid) +
            " AND old_bidder=" + std::to_string(r.oldBidder) + " AND old_bid=" + std::to_string(r.oldBid) +
            " AND bidder=" + std::to_string(r.bidder) + " AND bidder_account=" + std::to_string(r.bidderAccount) +
            " AND money_before=" + std::to_string(r.moneyBefore) + " AND refund_mail_id=" +
            std::to_string(r.refundMailId) + " AND winner_mail_id=" + std::to_string(r.winnerMailId) +
            " AND payout_at=" + std::to_string(r.payoutAt);
    }

#if defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) && CMANGOS_ASYNC_TRANSACTION_CALLBACK >= 4
    bool Postconditions(SqlConnection& c, const Bid& r)
    {
        auto auction = c.Query(("SELECT COUNT(*) FROM auction WHERE id=" + std::to_string(r.auctionId) +
            " AND itemowner=" + std::to_string(r.owner) + " AND buyguid=" + std::to_string(r.bidder) +
            " AND lastbid=" + std::to_string(r.bid) + " AND moneyTime=" + std::to_string(r.payoutAt) +
            " AND itemguid=" + std::to_string(Won(r) ? 0 : r.itemGuid) +
            " AND time=" + std::to_string(r.expiresAt)).c_str());
        auto buyer = c.Query(("SELECT COUNT(*) FROM characters WHERE guid=" + std::to_string(r.bidder) +
            " AND account=" + std::to_string(r.bidderAccount) +
            " AND money=" + std::to_string(r.moneyBefore - Debit(r))).c_str());
        if (!auction || auction->Fetch()[0].GetUInt32() != 1 || !buyer || buyer->Fetch()[0].GetUInt32() != 1)
            return false;
        if (r.refundMailId)
        {
            auto refund = c.Query(("SELECT COUNT(*) FROM mail WHERE id=" + std::to_string(r.refundMailId) +
                " AND receiver=" + std::to_string(r.oldBidder) + " AND messageType=2 AND money=" +
                std::to_string(r.oldBid) + " AND cod=0 AND has_items=0").c_str());
            if (!refund || refund->Fetch()[0].GetUInt32() != 1)
                return false;
        }
        if (Won(r))
        {
            auto winner = c.Query(("SELECT COUNT(*) FROM mail m INNER JOIN mail_items mi ON mi.mail_id=m.id "
                "INNER JOIN item_instance i ON i.guid=mi.item_guid WHERE m.id=" + std::to_string(r.winnerMailId) +
                " AND m.receiver=" + std::to_string(r.bidder) + " AND m.messageType=2 AND m.money=0 AND m.cod=0 "
                "AND m.has_items=1 AND mi.receiver=m.receiver AND mi.item_guid=" + std::to_string(r.itemGuid) +
                " AND mi.item_template=" + std::to_string(r.itemEntry) + " AND i.owner_guid=m.receiver AND i.itemEntry=" +
                std::to_string(r.itemEntry) + " AND i.count=" + std::to_string(r.itemCount)).c_str());
            if (!winner || winner->Fetch()[0].GetUInt32() != 1)
                return false;
        }
        return true;
    }
#endif
}

std::future<bool> RandomBotEstateStore::CommitAuctionBid(RandomBotEstateAuctionBid r)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 4
    CharacterDatabase.RollbackTransaction();
    std::promise<bool> unavailable; auto result=unavailable.get_future(); unavailable.set_value(false); return result;
#else
    return CharacterDatabase.CommitTransactionAcknowledged([r](SqlConnection& c)
    {
        if (!Valid(r))
            return false;
        auto engines = c.Query("SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE() "
            "AND table_name IN ('characters','auction','mail','mail_items','item_instance','ai_playerbot_estate_auction',"
            "'ai_playerbot_estate_bid_claim','ai_playerbot_estate_auction_bid') AND engine='InnoDB'");
        if (!engines || engines->Fetch()[0].GetUInt32()!=8)
            return false;
        const std::string id=std::to_string(r.auctionId);
        auto tracked=c.Query(("SELECT (SELECT COUNT(*) FROM ai_playerbot_estate_auction WHERE auction_id="+id+
            " AND status=0) + (SELECT COUNT(*) FROM ai_playerbot_estate_bid_claim WHERE auction_id="+id+
            " AND status=0 AND broker_guid="+std::to_string(r.oldBidder)+")").c_str());
        if(!tracked || !tracked->Fetch()[0].GetUInt32())
            return false;
        auto duplicate=c.Query(("SELECT COUNT(*) FROM ai_playerbot_estate_auction_bid WHERE auction_id="+id+
            " AND bid="+std::to_string(r.bid)).c_str());
        if(!duplicate || duplicate->Fetch()[0].GetUInt32())
            return false;
        auto auction=c.Query(("SELECT itemowner,itemguid,item_template,item_count,buyguid,lastbid,startbid,buyoutprice,"
            "time,moneyTime FROM auction WHERE id="+id+" FOR UPDATE").c_str());
        if(!auction)
            return false;
        Field* f=auction->Fetch();
        if(f[0].GetUInt32()!=r.owner || f[1].GetUInt32()!=r.itemGuid || f[2].GetUInt32()!=r.itemEntry ||
            f[3].GetUInt32()!=r.itemCount || f[4].GetUInt32()!=r.oldBidder || f[5].GetUInt32()!=r.oldBid ||
            f[6].GetUInt32()!=r.startBid || f[7].GetUInt32()!=r.buyout || f[8].GetUInt64()!=r.expiresAt || f[9].GetUInt64())
            return false;
        auto buyer=c.Query(("SELECT account,money FROM characters WHERE guid="+std::to_string(r.bidder)+" FOR UPDATE").c_str());
        if(!buyer || buyer->Fetch()[0].GetUInt32()!=r.bidderAccount || buyer->Fetch()[1].GetUInt32()!=r.moneyBefore)
            return false;
        return c.Execute(("INSERT INTO ai_playerbot_estate_auction_bid (auction_id,bid,old_bidder,old_bid,bidder,"
            "bidder_account,money_before,refund_mail_id,winner_mail_id,payout_at,completed_at) VALUES ("+id+","+
            std::to_string(r.bid)+","+std::to_string(r.oldBidder)+","+std::to_string(r.oldBid)+","+
            std::to_string(r.bidder)+","+std::to_string(r.bidderAccount)+","+std::to_string(r.moneyBefore)+","+
            std::to_string(r.refundMailId)+","+std::to_string(r.winnerMailId)+","+
            std::to_string(r.payoutAt)+",UNIX_TIMESTAMP())").c_str());
    }, [r](SqlConnection& c)
    {
        if(!Postconditions(c,r))
            return false;
        const std::string id=std::to_string(r.auctionId);
        if(!c.Execute(("UPDATE ai_playerbot_estate_auction SET bidder="+std::to_string(r.bidder)+",bid="+
            std::to_string(r.bid)+",status="+std::to_string(Won(r)?1:0)+",payout_at="+
            std::to_string(r.payoutAt)+",winner_mail_id="+std::to_string(r.winnerMailId)+
            ",updated_at=UNIX_TIMESTAMP() WHERE auction_id="+id).c_str()))
            return false;
        if(r.refundMailId && !c.Execute(("UPDATE ai_playerbot_estate_bid_claim SET status=1,mail_id="+
            std::to_string(r.refundMailId)+",updated_at=UNIX_TIMESTAMP() WHERE auction_id="+id+
            " AND broker_guid="+std::to_string(r.oldBidder)+" AND status=0").c_str()))
            return false;
        return true;
    });
#endif
}

std::future<bool> RandomBotEstateStore::ConfirmAuctionBid(RandomBotEstateAuctionBid r)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 4
    std::promise<bool> unavailable; auto result=unavailable.get_future(); unavailable.set_value(false); return result;
#else
    return CharacterDatabase.QueueTransaction([r](SqlConnection& c)
    {
        if(!Valid(r)) return false;
        auto receipt=c.Query(("SELECT COUNT(*) FROM ai_playerbot_estate_auction_bid WHERE "+Receipt(r)).c_str());
        return receipt && receipt->Fetch()[0].GetUInt32()==1 && Postconditions(c,r);
    });
#endif
}
