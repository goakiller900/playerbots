#pragma once

#include "Common.h"
#include <memory>

class AuctionEntry;
class WorldSession;

// World-thread bridge between the normal AH and durable estate journals. It
// intercepts only auction IDs registered in the estate schema (or a registered
// inherited bid claim); every other auction follows the unmodified core path.
class RandomBotEstateAuctionMgr
{
public:
    static RandomBotEstateAuctionMgr& instance();
    void Initialize();
    void Update();
    bool IsTracked(uint32 auctionId) const;
    bool PublishListing(uint32 auctionId);
    void FenceAdoption(uint32 auctionId);
    bool HandleBid(WorldSession* session, AuctionEntry* auction, uint32 price);
    // True means the tracked auction is either queued or already being settled;
    // the core update loop must leave it alone until acknowledgement.
    bool HandleExpiration(AuctionEntry* auction, time_t now);

private:
    RandomBotEstateAuctionMgr();
    ~RandomBotEstateAuctionMgr();
    RandomBotEstateAuctionMgr(RandomBotEstateAuctionMgr const&) = delete;
    RandomBotEstateAuctionMgr& operator=(RandomBotEstateAuctionMgr const&) = delete;
    struct Impl;
    std::unique_ptr<Impl> impl;
};

#define sRandomBotEstateAuctionMgr RandomBotEstateAuctionMgr::instance()
