#include "playerbot/playerbot.h"
#include "RandomBotEstateAuctionMgr.h"
#include "RandomBotEstateService.h"
#include "RandomBotEstateStore.h"
#include "RandomBotLifecycle.h"
#include "PlayerbotAIConfig.h"
#include "AuctionHouse/AuctionHouseMgr.h"
#include "Entities/Player.h"
#include "Entities/Bag.h"
#include "Globals/ObjectMgr.h"
#include "Mails/Mail.h"
#include "Server/WorldSession.h"
#include "strategy/values/ItemUsageValue.h"
#include <chrono>
#include <list>
#include <sstream>
#include <unordered_set>
#include <unordered_map>

namespace
{
std::unique_ptr<Item> LoadDetachedItem(uint32 itemGuid,uint32 owner)
{
    auto result=CharacterDatabase.PQuery("SELECT itemEntry,creatorGuid,giftCreatorGuid,count,duration,charges,flags,enchantments,"
        "randomPropertyId,durability,playedTime,text FROM item_instance WHERE guid=%u AND owner_guid=%u",itemGuid,owner);
    if(!result)return {};
    ItemPrototype const* proto=ObjectMgr::GetItemPrototype(result->Fetch()[0].GetUInt32());if(!proto)return {};
    std::unique_ptr<Item> item(NewItemOrBag(proto));
    if(!item||!item->LoadFromDB(itemGuid,result->Fetch(),ObjectGuid(HIGHGUID_PLAYER,owner)))return {};
    return item;
}
std::unique_ptr<MailDraft> OutbidDraft(AuctionEntry const& a)
{
    std::ostringstream subject;
    subject << a.itemTemplate << ':' << a.itemRandomPropertyId << ':' << AUCTION_OUTBIDDED;
    std::ostringstream body;
    body.width(16); body << std::right << std::hex << a.owner;
    body << std::dec << ':' << a.bid << ':' << a.buyout << ':' << a.deposit << ':' << a.GetAuctionCut();
    return std::unique_ptr<MailDraft>(new MailDraft(subject.str(), body.str()));
}

std::unique_ptr<MailDraft> WinnerDraft(AuctionEntry const& a, Item* item)
{
    std::ostringstream subject;
    subject << a.itemTemplate << ':' << a.itemRandomPropertyId << ':' << AUCTION_WON;
    std::ostringstream body;
    body.width(16); body << std::right << std::hex << a.owner;
    body << std::dec << ':' << a.buyout << ':' << a.buyout;
    std::unique_ptr<MailDraft> draft(new MailDraft(subject.str(), body.str()));
    draft->AddItem(item);
    return draft;
}

std::unique_ptr<MailDraft> ReturnDraft(AuctionEntry const& a, Item* item)
{
    std::ostringstream subject;
    subject << a.itemTemplate << ':' << a.itemRandomPropertyId << ':' << AUCTION_EXPIRED << ':' << a.Id << ':' << a.itemCount;
    std::unique_ptr<MailDraft> draft(new MailDraft(subject.str(), ""));
    draft->AddItem(item);
    return draft;
}

std::unique_ptr<MailDraft> PayoutDraft(AuctionEntry const& a, uint32 money)
{
    std::ostringstream subject;
    subject << a.itemTemplate << ':' << a.itemRandomPropertyId << ':' << AUCTION_SUCCESSFUL;
    std::ostringstream body;
    body.width(16); body << std::right << std::hex << a.bidder;
    body << std::dec << ':' << a.bid << ':' << a.buyout << ':' << a.deposit << ':' << a.GetAuctionCut();
    std::unique_ptr<MailDraft> draft(new MailDraft(subject.str(), body.str()));
    draft->SetMoney(money);
    return draft;
}
}

struct RandomBotEstateAuctionMgr::Impl
{
    enum Phase { COMMITTING, CONFIRMING };
    struct Pending
    {
        RandomBotEstateAuctionBid request;
        Phase phase = COMMITTING;
        std::future<bool> result;
        std::unique_ptr<MailDraft> refund;
        std::unique_ptr<MailDraft> winner;
        time_t deliverTime = 0;
    };
    struct Settlement
    {
        RandomBotEstateAuctionResolution request;
        Phase phase = COMMITTING;
        std::future<bool> result;
        std::unique_ptr<MailDraft> mail;
        time_t deliverTime = 0;
    };
    struct MailWork
    {
        RandomBotEstateAuctionMail request;
        Phase phase = COMMITTING;
        std::future<bool> result;
    };
    struct ClaimWin
    {
        RandomBotEstateClaimWin request;
        Phase phase = COMMITTING;
        std::future<bool> result;
        std::unique_ptr<MailDraft> mail;
        time_t deliverTime = 0;
    };
    struct ListingWork
    {
        RandomBotEstateAuctionListing request;
        Phase phase=COMMITTING;
        std::future<bool> result;
    };
    struct ClaimMailWork
    {
        RandomBotEstateClaimMail request;
        Phase phase=COMMITTING;
        std::future<bool> result;
    };
    struct LiquidationWork
    {
        RandomBotEstateLiquidation request;
        Phase phase=COMMITTING;
        std::future<bool> result;
        std::unique_ptr<Item> item;
    };
    bool initialized = false;
    std::unordered_set<uint32> tracked;
    std::unordered_set<uint32> sellerAuctions;
    std::unordered_set<uint32> claimAuctions;
    std::list<Pending> pending;
    std::list<Settlement> settlements;
    std::unordered_set<uint32> resolving;
    std::unordered_map<uint32,time_t> adoptionHolds;
    std::list<MailWork> mailWork;
    std::list<ClaimWin> claimWins;
    std::list<ListingWork> listingWork;
    std::list<LiquidationWork> liquidationWork;
    std::list<ClaimMailWork> claimMailWork;
    std::unordered_set<uint64> busyLots;
    std::unordered_set<uint32> consumingMail;
    time_t nextMailScan = 0;
    time_t nextLotScan = 0;
    time_t nextAuctionRecovery = 0;
};

RandomBotEstateAuctionMgr& RandomBotEstateAuctionMgr::instance()
{
    static RandomBotEstateAuctionMgr manager;
    return manager;
}
RandomBotEstateAuctionMgr::RandomBotEstateAuctionMgr() : impl(new Impl) {}
RandomBotEstateAuctionMgr::~RandomBotEstateAuctionMgr() = default;

void RandomBotEstateAuctionMgr::Initialize()
{
    if (impl->initialized)
        return;
    impl->initialized = true;
    if (!RandomBotLifecycleMgr::ExecutionAllowed() || !sRandomBotEstateService.Ready())
        return;
    auto sellers=CharacterDatabase.Query("SELECT auction_id FROM ai_playerbot_estate_auction WHERE status IN (0,1)");
    if(sellers)do{uint32 id=sellers->Fetch()[0].GetUInt32();impl->tracked.insert(id);impl->sellerAuctions.insert(id);}while(sellers->NextRow());
    auto claims=CharacterDatabase.Query("SELECT auction_id FROM ai_playerbot_estate_bid_claim WHERE status=0");
    if(claims)do{uint32 id=claims->Fetch()[0].GetUInt32();impl->tracked.insert(id);impl->claimAuctions.insert(id);}while(claims->NextRow());
}

bool RandomBotEstateAuctionMgr::IsTracked(uint32 auctionId) const
{
    return RandomBotLifecycleMgr::ExecutionAllowed() && impl->tracked.count(auctionId);
}

bool RandomBotEstateAuctionMgr::PublishListing(uint32 auctionId)
{
    Initialize();
    if (!RandomBotLifecycleMgr::ExecutionAllowed() || !sAuctionMgr.PublishBrokerAuctionFromDB(auctionId))
        return false;
    impl->tracked.insert(auctionId);
    impl->sellerAuctions.insert(auctionId);
    return true;
}

void RandomBotEstateAuctionMgr::FenceAdoption(uint32 auctionId)
{
    if(!auctionId)return;
    impl->tracked.insert(auctionId);
    impl->resolving.insert(auctionId);
    impl->adoptionHolds[auctionId]=time(nullptr)+10;
}

bool RandomBotEstateAuctionMgr::HandleBid(WorldSession* session, AuctionEntry* auction, uint32 price)
{
    Initialize();
    if (!session || !auction || !IsTracked(auction->Id))
        return false;
    if(impl->resolving.count(auction->Id))
    {
        session->SendAuctionCommandResult(auction,AUCTION_BID_PLACED,AUCTION_ERR_DATABASE);
        return true;
    }
    Player* bidder = session->GetPlayer();
    if (!bidder || bidder->IsEstateAuctionPending())
    {
        session->SendAuctionCommandResult(auction, AUCTION_BID_PLACED, AUCTION_ERR_DATABASE);
        return true;
    }
    const uint32 bid = auction->buyout && price > auction->buyout ? auction->buyout : price;
    Item* item = sAuctionMgr.GetAItem(auction->itemGuidLow);
    if (!item)
    {
        session->SendAuctionCommandResult(auction, AUCTION_BID_PLACED, AUCTION_ERR_DATABASE);
        return true;
    }

    Impl::Pending pending;
    pending.deliverTime=time(nullptr);
    auto& r = pending.request;
    r.auctionId=auction->Id; r.owner=auction->owner; r.itemGuid=auction->itemGuidLow;
    r.itemEntry=auction->itemTemplate; r.itemCount=auction->itemCount; r.oldBidder=auction->bidder;
    r.oldBid=auction->bid; r.bidder=bidder->GetGUIDLow(); r.bidderAccount=session->GetAccountId();
    r.bid=bid; r.startBid=auction->startbid; r.buyout=auction->buyout; r.moneyBefore=bidder->GetMoney();
    r.expiresAt=auction->expireTime;
    const bool won = auction->buyout && bid == auction->buyout;
    if (r.oldBidder && r.oldBidder != r.bidder)
        r.refundMailId=sObjectMgr.GenerateMailID();
    if (won)
    {
        r.winnerMailId=sObjectMgr.GenerateMailID();
        r.payoutAt=time(nullptr)+HOUR;
    }

    bidder->SetEstateAuctionPending(true);
    session->SendNotification("Auction bid is being finalized.");
    CharacterDatabase.BeginTransaction();
    bool mailQueued=true;
    const uint32 debit = r.bid - (r.oldBidder == r.bidder ? r.oldBid : 0);
    CharacterDatabase.PExecute("UPDATE characters SET money=%u WHERE guid=%u AND account=%u AND money=%u",
        r.moneyBefore-debit, r.bidder, r.bidderAccount, r.moneyBefore);
    if (r.refundMailId)
    {
        pending.refund=OutbidDraft(*auction);
        pending.refund->SetMoney(r.oldBid);
        mailQueued=pending.refund->QueueAuctionDelivery(r.refundMailId, ObjectGuid(HIGHGUID_PLAYER,r.oldBidder),
            MailSender(auction), pending.deliverTime);
    }
    if (won)
    {
        CharacterDatabase.PExecute("UPDATE item_instance SET owner_guid=%u WHERE guid=%u AND owner_guid=%u",
            r.bidder,r.itemGuid,r.owner);
        CharacterDatabase.PExecute("UPDATE auction SET itemguid=0,moneyTime='" UI64FMTD
            "',buyguid=%u,lastbid=%u WHERE id=%u",r.payoutAt,r.bidder,r.bid,r.auctionId);
        pending.winner=WinnerDraft(*auction,item);
        mailQueued=mailQueued&&pending.winner->QueueAuctionDelivery(r.winnerMailId,ObjectGuid(HIGHGUID_PLAYER,r.bidder),
            MailSender(auction),pending.deliverTime);
    }
    else
        CharacterDatabase.PExecute("UPDATE auction SET buyguid=%u,lastbid=%u WHERE id=%u",r.bidder,r.bid,r.auctionId);
    if(!mailQueued)
    {
        CharacterDatabase.RollbackTransaction();bidder->SetEstateAuctionPending(false);
        session->SendAuctionCommandResult(auction,AUCTION_BID_PLACED,AUCTION_ERR_DATABASE);return true;
    }
    pending.result=RandomBotEstateStore::CommitAuctionBid(r);
    impl->pending.emplace_back(std::move(pending));
    return true;
}

bool RandomBotEstateAuctionMgr::HandleExpiration(AuctionEntry* auction, time_t now)
{
    Initialize();
    if (!auction || !IsTracked(auction->Id))
        return false;
    if (impl->resolving.count(auction->Id))
        return true;
    if(impl->claimAuctions.count(auction->Id)&&!impl->sellerAuctions.count(auction->Id))
    {
        if(auction->moneyDeliveryTime)return false;
        if(now<=auction->expireTime)return true;
        if(!auction->bid||!auction->itemGuidLow)return true;
        auto claim=CharacterDatabase.PQuery("SELECT estate_id,broker_guid,bid,item_guid,item_entry,item_count FROM "
            "ai_playerbot_estate_bid_claim WHERE auction_id=%u AND status=0",auction->Id);
        if(!claim)return true;
        Field* f=claim->Fetch();
        if(f[1].GetUInt32()!=auction->bidder||f[2].GetUInt32()!=auction->bid)return true;
        Item* item=sAuctionMgr.GetAItem(auction->itemGuidLow);if(!item)return true;
        Impl::ClaimWin work;auto&r=work.request;r.estateId=f[0].GetUInt64();r.auctionId=auction->Id;r.owner=auction->owner;
        r.brokerGuid=f[1].GetUInt32();r.bid=f[2].GetUInt32();r.itemGuid=f[3].GetUInt32();r.itemEntry=f[4].GetUInt32();
        r.itemCount=f[5].GetUInt32();r.expiresAt=auction->expireTime;r.payoutAt=uint64(now)+HOUR;r.mailId=sObjectMgr.GenerateMailID();
        work.deliverTime=now;work.mail=WinnerDraft(*auction,item);
        CharacterDatabase.BeginTransaction();
        CharacterDatabase.PExecute("UPDATE item_instance SET owner_guid=%u WHERE guid=%u AND owner_guid=%u",r.brokerGuid,r.itemGuid,r.owner);
        CharacterDatabase.PExecute("UPDATE auction SET itemguid=0,moneyTime='" UI64FMTD "' WHERE id=%u AND itemguid=%u",
            r.payoutAt,r.auctionId,r.itemGuid);
        if(!work.mail->QueueAuctionDelivery(r.mailId,ObjectGuid(HIGHGUID_PLAYER,r.brokerGuid),MailSender(auction),now))
        {CharacterDatabase.RollbackTransaction();return true;}
        work.result=RandomBotEstateStore::CommitClaimWin(r);impl->resolving.insert(r.auctionId);
        impl->claimWins.emplace_back(std::move(work));return true;
    }
    RandomBotEstateAuctionResolution r;
    if (auction->moneyDeliveryTime)
    {
        if (now <= auction->moneyDeliveryTime) return true;
        r.kind=ESTATE_AUCTION_PAYOUT;
    }
    else
    {
        if (now <= auction->expireTime) return true;
        r.kind=auction->bid?ESTATE_AUCTION_WON:ESTATE_AUCTION_EXPIRED;
    }
    r.auctionId=auction->Id; r.owner=auction->owner; r.itemGuid=auction->itemGuidLow;
    r.itemEntry=auction->itemTemplate; r.itemCount=auction->itemCount; r.bidder=auction->bidder;
    r.bid=auction->bid; r.deposit=auction->deposit; r.cut=auction->GetAuctionCut();
    r.expiresAt=auction->expireTime; r.payoutAt=auction->moneyDeliveryTime;
    if (r.kind==ESTATE_AUCTION_WON) r.payoutAt=uint64(now)+HOUR;
    r.mailId=sObjectMgr.GenerateMailID();
    Item* item=r.itemGuid?sAuctionMgr.GetAItem(r.itemGuid):nullptr;
    if ((r.kind==ESTATE_AUCTION_EXPIRED||r.kind==ESTATE_AUCTION_WON)&&!item)
    {
        sLog.outError("Retirement estate auction %u has no in-memory item; settlement blocked.",r.auctionId);
        return true;
    }
    Impl::Settlement pending;
    pending.deliverTime=now;
    pending.request=r;
    CharacterDatabase.BeginTransaction();
    if(r.kind==ESTATE_AUCTION_EXPIRED)
    {
        auction->DeleteFromDB();
        pending.mail=ReturnDraft(*auction,item);
        if(!pending.mail->QueueAuctionDelivery(r.mailId,ObjectGuid(HIGHGUID_PLAYER,r.owner),MailSender(auction),now))
        {CharacterDatabase.RollbackTransaction();return true;}
    }
    else if(r.kind==ESTATE_AUCTION_WON)
    {
        CharacterDatabase.PExecute("UPDATE item_instance SET owner_guid=%u WHERE guid=%u AND owner_guid=%u",
            r.bidder,r.itemGuid,r.owner);
        CharacterDatabase.PExecute("UPDATE auction SET itemguid=0,moneyTime='" UI64FMTD
            "' WHERE id=%u AND itemguid=%u",r.payoutAt,r.auctionId,r.itemGuid);
        pending.mail=WinnerDraft(*auction,item);
        if(!pending.mail->QueueAuctionDelivery(r.mailId,ObjectGuid(HIGHGUID_PLAYER,r.bidder),MailSender(auction),now))
        {CharacterDatabase.RollbackTransaction();return true;}
    }
    else
    {
        auction->DeleteFromDB();
        const uint64 gross=uint64(r.bid)+r.deposit;
        if(gross<r.cut||gross-r.cut>UINT32_MAX)
        {
            CharacterDatabase.RollbackTransaction();
            sLog.outError("Retirement estate auction %u payout exceeds the core mail-money width; settlement held.",r.auctionId);
            impl->resolving.insert(r.auctionId);
            return true;
        }
        const uint32 profit=uint32(gross-r.cut);
        pending.mail=PayoutDraft(*auction,profit);
        if(!pending.mail->QueueAuctionDelivery(r.mailId,ObjectGuid(HIGHGUID_PLAYER,r.owner),MailSender(auction),now))
        {CharacterDatabase.RollbackTransaction();return true;}
    }
    pending.result=RandomBotEstateStore::CommitAuctionResolution(r);
    impl->resolving.insert(r.auctionId);
    impl->settlements.emplace_back(std::move(pending));
    return true;
}

void RandomBotEstateAuctionMgr::Update()
{
    Initialize();
    for (auto it=impl->pending.begin(); it!=impl->pending.end();)
    {
        if (it->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        { ++it; continue; }
        bool committed=false;
        try { committed=it->result.get(); } catch (...) { committed=false; }
        if (!committed && it->phase==Impl::COMMITTING)
        {
            it->phase=Impl::CONFIRMING;
            it->result=RandomBotEstateStore::ConfirmAuctionBid(it->request);
            ++it; continue;
        }
        RandomBotEstateAuctionBid const& r=it->request;
        Player* bidder=sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER,r.bidder),false);
        AuctionEntry* auction=sAuctionMgr.FindAuction(r.auctionId);
        if (committed && auction)
        {
            const uint32 debit=r.bid-(r.oldBidder==r.bidder?r.oldBid:0);
            if (bidder) bidder->SetMoney(r.moneyBefore-debit);
            auction->bidder=r.bidder; auction->bid=r.bid;
            if (r.winnerMailId)
            {
                sAuctionMgr.RemoveAItem(r.itemGuid);
                auction->itemGuidLow=0;
                auction->moneyDeliveryTime=time_t(r.payoutAt);
            }
            if (it->refund)
                it->refund->PublishQueuedAuctionDelivery(r.refundMailId,ObjectGuid(HIGHGUID_PLAYER,r.oldBidder),
                    MailSender(auction),it->deliverTime);
            if (it->winner)
                it->winner->PublishQueuedAuctionDelivery(r.winnerMailId,ObjectGuid(HIGHGUID_PLAYER,r.bidder),
                    MailSender(auction),it->deliverTime);
            if (bidder && bidder->GetSession())
            {
                bidder->GetSession()->SendAuctionCommandResult(auction,AUCTION_BID_PLACED,AUCTION_OK);
                bidder->GetAchievementMgr().UpdateAchievementCriteria(
                    ACHIEVEMENT_CRITERIA_TYPE_HIGHEST_AUCTION_BID,r.bid);
                if(r.winnerMailId)
                {
                    bidder->GetSession()->SendAuctionBidderNotification(auction);
                    bidder->GetAchievementMgr().UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_WON_AUCTIONS,1);
                }
            }
            if(Player* owner=sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER,r.owner),false))
                if(owner->GetSession())owner->GetSession()->SendAuctionOwnerNotification(auction);
        }
        else if (bidder && bidder->GetSession())
            bidder->GetSession()->SendAuctionCommandResult(auction,AUCTION_BID_PLACED,AUCTION_ERR_DATABASE);
        if (bidder) bidder->SetEstateAuctionPending(false);
        if(committed&&impl->claimAuctions.count(r.auctionId)&&r.oldBidder)
        {
            impl->claimAuctions.erase(r.auctionId);
            if(!impl->sellerAuctions.count(r.auctionId))impl->tracked.erase(r.auctionId);
        }
        it=impl->pending.erase(it);
    }
    for(auto it=impl->settlements.begin();it!=impl->settlements.end();)
    {
        if(it->result.wait_for(std::chrono::seconds(0))!=std::future_status::ready){++it;continue;}
        bool committed=false; try{committed=it->result.get();}catch(...){committed=false;}
        if(!committed&&it->phase==Impl::COMMITTING)
        {
            it->phase=Impl::CONFIRMING;
            it->result=RandomBotEstateStore::ConfirmAuctionResolution(it->request);
            ++it;continue;
        }
        auto const r=it->request;
        AuctionEntry* auction=sAuctionMgr.FindAuction(r.auctionId);
        if(committed&&auction)
        {
            if(r.kind==ESTATE_AUCTION_WON)
            {
                sAuctionMgr.RemoveAItem(r.itemGuid);
                auction->itemGuidLow=0;
                auction->moneyDeliveryTime=time_t(r.payoutAt);
                it->mail->PublishQueuedAuctionDelivery(r.mailId,ObjectGuid(HIGHGUID_PLAYER,r.bidder),MailSender(auction),it->deliverTime);
            }
            else
            {
                if(r.kind==ESTATE_AUCTION_EXPIRED)
                {
                    sAuctionMgr.RemoveAItem(r.itemGuid);
                    it->mail->PublishQueuedAuctionDelivery(r.mailId,ObjectGuid(HIGHGUID_PLAYER,r.owner),MailSender(auction),it->deliverTime);
                }
                else
                    it->mail->PublishQueuedAuctionDelivery(r.mailId,ObjectGuid(HIGHGUID_PLAYER,r.owner),MailSender(auction),it->deliverTime);
                sAuctionMgr.GetAuctionsMap(auction->auctionHouseEntry)->RemoveAuction(r.auctionId);
                delete auction;
                impl->sellerAuctions.erase(r.auctionId);
                impl->tracked.erase(r.auctionId);
            }
        }
        if(!committed)
            sLog.outError("Retirement estate auction %u settlement failed durable reconciliation; it remains retryable.",r.auctionId);
        impl->resolving.erase(r.auctionId);
        it=impl->settlements.erase(it);
    }
    for(auto it=impl->claimWins.begin();it!=impl->claimWins.end();)
    {
        if(it->result.wait_for(std::chrono::seconds(0))!=std::future_status::ready){++it;continue;}
        bool committed=false;try{committed=it->result.get();}catch(...){committed=false;}
        if(!committed&&it->phase==Impl::COMMITTING){it->phase=Impl::CONFIRMING;it->result=RandomBotEstateStore::ConfirmClaimWin(it->request);++it;continue;}
        auto r=it->request;AuctionEntry* auction=sAuctionMgr.FindAuction(r.auctionId);
        if(committed&&auction)
        {
            sAuctionMgr.RemoveAItem(r.itemGuid);auction->itemGuidLow=0;auction->moneyDeliveryTime=time_t(r.payoutAt);
            it->mail->PublishQueuedAuctionDelivery(r.mailId,ObjectGuid(HIGHGUID_PLAYER,r.brokerGuid),MailSender(auction),it->deliverTime);
            if(Player* owner=sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER,r.owner),false))
                if(owner->GetSession())owner->GetSession()->SendAuctionOwnerNotification(auction);
            impl->claimAuctions.erase(r.auctionId);if(!impl->sellerAuctions.count(r.auctionId))impl->tracked.erase(r.auctionId);
        }
        if(!committed)sLog.outError("Retirement inherited bid %u could not reconcile; held for retry.",r.auctionId);
        impl->resolving.erase(r.auctionId);it=impl->claimWins.erase(it);
    }
    for(auto it=impl->mailWork.begin();it!=impl->mailWork.end();)
    {
        if(it->result.wait_for(std::chrono::seconds(0))!=std::future_status::ready){++it;continue;}
        bool committed=false;try{committed=it->result.get();}catch(...){committed=false;}
        if(!committed&&it->phase==Impl::COMMITTING)
        {
            it->phase=Impl::CONFIRMING;
            it->result=RandomBotEstateStore::ConfirmAuctionMail(it->request);
            ++it;continue;
        }
        if(committed)
            sLog.outString("Retirement estate " UI64FMTD " collected auction %u %s.",it->request.estateId,
                it->request.auctionId,it->request.status==3?"proceeds":"return");
        else
            sLog.outError("Retirement estate auction %u mail collection failed reconciliation; retained for retry.",
                it->request.auctionId);
        impl->consumingMail.erase(it->request.auctionId);
        it=impl->mailWork.erase(it);
    }
    for(auto it=impl->listingWork.begin();it!=impl->listingWork.end();)
    {
        if(it->result.wait_for(std::chrono::seconds(0))!=std::future_status::ready){++it;continue;}
        bool committed=false;try{committed=it->result.get();}catch(...){committed=false;}
        if(!committed&&it->phase==Impl::COMMITTING){it->phase=Impl::CONFIRMING;it->result=RandomBotEstateStore::ConfirmAuctionListing(it->request);++it;continue;}
        if(committed&&!PublishListing(it->request.auctionId))
            sLog.outError("Retirement estate auction %u committed but could not publish; startup recovery will load it.",it->request.auctionId);
        impl->busyLots.erase(it->request.lotId);it=impl->listingWork.erase(it);
    }
    for(auto it=impl->liquidationWork.begin();it!=impl->liquidationWork.end();)
    {
        if(it->result.wait_for(std::chrono::seconds(0))!=std::future_status::ready){++it;continue;}
        bool committed=false;try{committed=it->result.get();}catch(...){committed=false;}
        if(!committed&&it->phase==Impl::COMMITTING){it->phase=Impl::CONFIRMING;it->result=RandomBotEstateStore::ConfirmLotLiquidation(it->request);++it;continue;}
        if(!committed)sLog.outError("Retirement estate lot " UI64FMTD " liquidation held after failed reconciliation.",it->request.lotId);
        impl->busyLots.erase(it->request.lotId);it=impl->liquidationWork.erase(it);
    }
    for(auto it=impl->claimMailWork.begin();it!=impl->claimMailWork.end();)
    {
        if(it->result.wait_for(std::chrono::seconds(0))!=std::future_status::ready){++it;continue;}
        bool committed=false;try{committed=it->result.get();}catch(...){committed=false;}
        if(!committed&&it->phase==Impl::COMMITTING){it->phase=Impl::CONFIRMING;it->result=RandomBotEstateStore::ConfirmClaimMail(it->request);++it;continue;}
        impl->consumingMail.erase(it->request.auctionId);it=impl->claimMailWork.erase(it);
    }
    const time_t now=time(nullptr);
    for(auto it=impl->adoptionHolds.begin();it!=impl->adoptionHolds.end();)
    {
        if(now<it->second){++it;continue;}
        auto mapped=CharacterDatabase.PQuery("SELECT (SELECT COUNT(*) FROM ai_playerbot_estate_auction WHERE auction_id=%u AND status=0),"
            "(SELECT COUNT(*) FROM ai_playerbot_estate_bid_claim WHERE auction_id=%u AND status=0)",it->first,it->first);
        if(mapped&&(mapped->Fetch()[0].GetUInt32()||mapped->Fetch()[1].GetUInt32())&&sAuctionMgr.PublishBrokerAuctionFromDB(it->first))
        {
            if(mapped->Fetch()[0].GetUInt32())impl->sellerAuctions.insert(it->first);
            if(mapped->Fetch()[1].GetUInt32())impl->claimAuctions.insert(it->first);
        }
        else impl->tracked.erase(it->first);
        impl->resolving.erase(it->first);it=impl->adoptionHolds.erase(it);
    }
    if(!RandomBotLifecycleMgr::ExecutionAllowed()||now<impl->nextMailScan)return;
    if(now>=impl->nextAuctionRecovery)
    {
        impl->nextAuctionRecovery=now+30;
        auto active=CharacterDatabase.PQuery("SELECT auction_id FROM ai_playerbot_estate_auction WHERE status IN (0,1) ORDER BY auction_id LIMIT %u",
            sPlayerbotAIConfig.retirementEstateBatchSize);
        if(active)do{uint32 id=active->Fetch()[0].GetUInt32();if(sAuctionMgr.PublishBrokerAuctionFromDB(id))
            {impl->tracked.insert(id);impl->sellerAuctions.insert(id);}}while(active->NextRow());
    }
    impl->nextMailScan=now+5;
    auto rows=CharacterDatabase.PQuery("SELECT estate_id,lot_id,auction_id,broker_guid,item_guid,item_entry,item_count,"
        "status,IF(status=2,return_mail_id,seller_mail_id),bid,deposit,auction_cut FROM ai_playerbot_estate_auction "
        "WHERE status IN (2,3) ORDER BY updated_at,auction_id LIMIT %u",sPlayerbotAIConfig.retirementEstateBatchSize);
    if(rows)
    { do
    {
        Field* f=rows->Fetch(); const uint32 auctionId=f[2].GetUInt32();
        if(impl->consumingMail.count(auctionId))continue;
        Impl::MailWork work; auto& r=work.request;
        r.estateId=f[0].GetUInt64();r.lotId=f[1].GetUInt64();r.auctionId=auctionId;r.brokerGuid=f[3].GetUInt32();
        r.itemGuid=f[4].GetUInt32();r.itemEntry=f[5].GetUInt32();r.itemCount=f[6].GetUInt32();r.status=f[7].GetUInt32();
        r.mailId=f[8].GetUInt32();r.bid=f[9].GetUInt32();r.deposit=f[10].GetUInt32();r.cut=f[11].GetUInt32();
        r.nextActionAt=uint64(now)+sPlayerbotAIConfig.retirementAuctionRepostDelay;
        work.result=RandomBotEstateStore::ConsumeAuctionMail(r);
        impl->consumingMail.insert(auctionId);
        impl->mailWork.emplace_back(std::move(work));
    }while(rows->NextRow()); }
    auto claims=CharacterDatabase.PQuery("SELECT estate_id,auction_id,broker_guid,bid,status,mail_id,item_guid,item_entry,item_count "
        "FROM ai_playerbot_estate_bid_claim WHERE status IN (1,2) ORDER BY updated_at,auction_id LIMIT %u",
        sPlayerbotAIConfig.retirementEstateBatchSize);
    if(claims){do{Field*f=claims->Fetch();uint32 id=f[1].GetUInt32();if(impl->consumingMail.count(id))continue;
        Impl::ClaimMailWork work;auto&r=work.request;r.estateId=f[0].GetUInt64();r.auctionId=id;r.brokerGuid=f[2].GetUInt32();
        r.bid=f[3].GetUInt32();r.status=f[4].GetUInt32();r.mailId=f[5].GetUInt32();r.itemGuid=f[6].GetUInt32();
        r.itemEntry=f[7].GetUInt32();r.itemCount=f[8].GetUInt32();work.result=RandomBotEstateStore::ConsumeClaimMail(r);
        impl->consumingMail.insert(id);impl->claimMailWork.emplace_back(std::move(work));}while(claims->NextRow());}

    if(now<impl->nextLotScan)return;
    impl->nextLotScan=now+5;
    auto lots=CharacterDatabase.PQuery("SELECT l.estate_id,l.lot_id,e.broker_guid,e.broker_account,b.faction,l.item_guid,"
        "l.item_entry,l.item_count,l.status,l.attempt,i.randomPropertyId,e.escrow FROM ai_playerbot_estate_lot l INNER JOIN "
        "ai_playerbot_estate e ON e.estate_id=l.estate_id INNER JOIN ai_playerbot_estate_broker b ON b.guid=e.broker_guid "
        "INNER JOIN item_instance i ON i.guid=l.item_guid AND i.owner_guid=e.broker_guid WHERE e.status=2 AND "
        "l.status IN (0,2,3) AND l.next_action_at<=UNIX_TIMESTAMP() ORDER BY l.next_action_at,l.lot_id LIMIT %u",
        sPlayerbotAIConfig.retirementEstateBatchSize);
    if(!lots)return;
    do
    {
        Field* f=lots->Fetch();const uint64 lotId=f[1].GetUInt64();if(impl->busyLots.count(lotId))continue;
        const uint32 broker=f[2].GetUInt32(),itemGuid=f[5].GetUInt32(),entry=f[6].GetUInt32(),count=f[7].GetUInt32();
        const uint32 status=f[8].GetUInt32(),attempt=f[9].GetUInt32();
        std::unique_ptr<Item> item=LoadDetachedItem(itemGuid,broker);if(!item)continue;
        ItemPrototype const* proto=item->GetProto();
        const bool ah=status!=3&&sPlayerbotAIConfig.retirementAuctionEnabled&&attempt<sPlayerbotAIConfig.retirementAuctionMaxAttempts&&
            item->CanBeTraded()&&ItemUsageValue::IsMoreProfitableToSellToAHThanToVendor(proto,nullptr);
        if(ah)
        {
            const uint32 house=f[4].GetUInt32()?6:1;AuctionHouseEntry const* houseEntry=sAuctionHouseStore.LookupEntry(house);
            if(!houseEntry)continue;
            const uint32 nextAttempt=attempt+1;const uint32 discount=std::min(100u,(nextAttempt-1)*sPlayerbotAIConfig.retirementAuctionDiscountPercentPerAttempt);
            uint64 per=ItemUsageValue::GetBotAHSellMaxPrice(proto);per=std::max<uint64>(1,per*(100-discount)/100);
            uint64 total=per*count;
            Impl::ListingWork work;auto&r=work.request;r.estateId=f[0].GetUInt64();r.lotId=lotId;r.auctionId=sObjectMgr.GenerateAuctionID();
            r.brokerGuid=broker;r.brokerAccount=f[3].GetUInt32();r.houseId=house;r.itemGuid=itemGuid;r.itemEntry=entry;r.itemCount=count;
            r.deposit=AuctionHouseMgr::GetAuctionDeposit(houseEntry,sPlayerbotAIConfig.retirementAuctionDuration,proto,count);
            if(f[11].GetUInt64()<r.deposit)
            {CharacterDatabase.PExecute("UPDATE ai_playerbot_estate_lot SET status=3,updated_at=UNIX_TIMESTAMP() WHERE lot_id='" UI64FMTD "' AND status IN (0,2)",lotId);continue;}
            total=std::min<uint64>(total,uint64(UINT32_MAX)-r.deposit);
            r.randomPropertyId=f[10].GetInt32();r.attempt=nextAttempt;r.buyout=std::max(1u,uint32(total));r.startBid=std::max(1u,uint32(total*80/100));
            r.expiresAt=uint64(now)+sPlayerbotAIConfig.retirementAuctionDuration;work.result=RandomBotEstateStore::CreateAuctionListing(r);
            impl->busyLots.insert(lotId);impl->listingWork.emplace_back(std::move(work));continue;
        }
        if(status!=3)
        {
            CharacterDatabase.PExecute("UPDATE ai_playerbot_estate_lot SET status=3,updated_at=UNIX_TIMESTAMP() WHERE lot_id='" UI64FMTD "' AND status IN (0,2)",lotId);
            continue;
        }
        Impl::LiquidationWork work;auto&r=work.request;r.estateId=f[0].GetUInt64();r.lotId=lotId;r.brokerGuid=broker;
        r.brokerAccount=f[3].GetUInt32();
        if(!RandomBotEstateStore::QuoteLotLiquidation(*item,r))
        {
            if(sPlayerbotAIConfig.retirementUnsellableItemPolicy==RandomBotUnsellableItemPolicy::RETAIN)
            {CharacterDatabase.PExecute("UPDATE ai_playerbot_estate_lot SET status=4,updated_at=UNIX_TIMESTAMP() WHERE lot_id='" UI64FMTD "' AND status=3",lotId);continue;}
            r.destroy=true;r.destroyAllowed=true;if(!RandomBotEstateStore::QuoteLotLiquidation(*item,r))continue;
        }
        CharacterDatabase.BeginTransaction();item->DeleteFromDB();work.item=std::move(item);work.result=RandomBotEstateStore::CommitLotLiquidation(r);
        impl->busyLots.insert(lotId);impl->liquidationWork.emplace_back(std::move(work));
    }while(lots->NextRow());
}
