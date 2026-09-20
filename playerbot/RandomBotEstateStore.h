#pragma once

#include "Common.h"
#include <future>
#include <string>
#include <vector>
class Item;

struct RandomBotEstateIntakeLot
{
    uint64 estateId = 0;
    uint32 originalGuid = 0, originalAccount = 0;
    uint32 brokerGuid = 0, brokerAccount = 0;
    uint32 itemGuid = 0, itemEntry = 0, itemCount = 0;
    std::vector<uint32> eligibleAccounts;
};

struct RandomBotEstateIntake
{
    uint32 originalGuid = 0, originalAccount = 0;
    uint32 brokerGuid = 0, brokerAccount = 0;
    uint32 disposition = 0;
    bool replacementRequired = false;
    std::vector<uint32> eligibleAccounts;
};

struct RandomBotEstateMailItem
{
    uint32 itemGuid = 0, itemEntry = 0, itemCount = 0;
};

struct RandomBotEstateMailIntake
{
    uint64 estateId = 0;
    uint32 originalGuid = 0, originalAccount = 0, brokerGuid = 0, brokerAccount = 0, mailId = 0;
    uint64 money = 0;
    std::vector<RandomBotEstateMailItem> items;
};

struct RandomBotEstateAuctionIntake
{
    uint64 estateId = 0;
    uint32 originalGuid = 0, brokerGuid = 0, auctionId = 0, houseId = 0;
    uint32 itemGuid = 0, itemEntry = 0, itemCount = 0, startBid = 0, buyout = 0;
    uint32 bidder = 0, bid = 0, deposit = 0;
    uint64 expiresAt = 0;
};

struct RandomBotEstateBidIntake
{
    uint64 estateId = 0;
    uint32 originalGuid = 0, bidderBrokerGuid = 0, auctionId = 0, bid = 0;
    uint32 itemGuid = 0, itemEntry = 0, itemCount = 0;
};

struct RandomBotEstateLiquidation
{
    uint64 estateId = 0, lotId = 0;
    uint32 brokerGuid = 0, brokerAccount = 0;
    uint32 itemGuid = 0, itemEntry = 0, itemCount = 0;
    uint64 proceeds = 0;
    std::string charges;
    bool destroy = false;
    bool destroyAllowed = false;
};

struct RandomBotEstateAuctionListing
{
    uint64 estateId = 0, lotId = 0;
    uint32 auctionId = 0, brokerGuid = 0, brokerAccount = 0, houseId = 0;
    uint32 itemGuid = 0, itemEntry = 0, itemCount = 0;
    int32 randomPropertyId = 0;
    uint32 attempt = 0, startBid = 0, buyout = 0, deposit = 0;
    uint64 expiresAt = 0;
};

struct RandomBotEstateAuctionBid
{
    uint32 auctionId = 0, owner = 0, itemGuid = 0, itemEntry = 0, itemCount = 0;
    uint32 oldBidder = 0, oldBid = 0, bidder = 0, bidderAccount = 0;
    uint32 bid = 0, startBid = 0, buyout = 0, moneyBefore = 0;
    uint32 refundMailId = 0, winnerMailId = 0;
    uint64 expiresAt = 0, payoutAt = 0;
};

enum RandomBotEstateAuctionResolutionKind
{
    ESTATE_AUCTION_EXPIRED = 1,
    ESTATE_AUCTION_WON = 2,
    ESTATE_AUCTION_PAYOUT = 3
};

struct RandomBotEstateAuctionResolution
{
    uint32 kind = 0, auctionId = 0, owner = 0, itemGuid = 0, itemEntry = 0, itemCount = 0;
    uint32 bidder = 0, bid = 0, deposit = 0, cut = 0;
    uint32 mailId = 0;
    uint64 expiresAt = 0, payoutAt = 0;
};

struct RandomBotEstateAuctionMail
{
    uint64 estateId = 0, lotId = 0;
    uint32 auctionId = 0, brokerGuid = 0, itemGuid = 0, itemEntry = 0, itemCount = 0;
    uint32 status = 0, mailId = 0, bid = 0, deposit = 0, cut = 0;
    uint64 nextActionAt = 0;
};

struct RandomBotEstateClaimWin
{
    uint64 estateId = 0;
    uint32 auctionId = 0, owner = 0, brokerGuid = 0, itemGuid = 0, itemEntry = 0, itemCount = 0;
    uint32 bid = 0, mailId = 0;
    uint64 expiresAt = 0, payoutAt = 0;
};

struct RandomBotEstateClaimMail
{
    uint64 estateId=0;
    uint32 auctionId=0,brokerGuid=0,bid=0,status=0,mailId=0,itemGuid=0,itemEntry=0,itemCount=0;
};

struct RandomBotEstateDistribution
{
    uint64 estateId=0,moneyCap=0,recipientCap=0;
    uint32 sinkMin=0,sinkMax=0,minRecipients=0,maxRecipients=0;
    std::vector<uint32> eligibleAccounts;
};

struct RandomBotReplacementReservation
{
    uint32 retiringGuid=0,guid=0,account=0;
    std::string name;
    uint8 race=0,cls=0,gender=0;
    std::vector<uint32> eligibleAccounts;
};

class RandomBotEstateStore
{
public:
    // Offline inventory/bank leaf transfer, not liquidation. No stack merging or
    // copied item. Caller holds original+broker login/save leases; wrapped items,
    // saved loot, nonempty bags and auction/mail-linked items fail closed.
    // An ambiguous result can retry this SAME immutable request: the receipt
    // branch only verifies the committed transfer and never transfers twice.
    static std::future<bool> TransferInventoryLot(RandomBotEstateIntakeLot request);
    static std::future<bool> BeginIntake(RandomBotEstateIntake request);
    static std::future<bool> TransferMail(RandomBotEstateMailIntake request);
    static std::future<bool> AdoptAuction(RandomBotEstateAuctionIntake request);
    static std::future<bool> AdoptBid(RandomBotEstateBidIntake request);
    static std::future<bool> FinalizeIntake(uint64 estateId, uint32 originalGuid, uint32 originalAccount);
    // Consumes queued core Item removal, credits independent estate escrow and
    // writes a receipt atomically. Caller must validate quote using the core's
    // GetVendorSellValue with overflow checks and retain exact charge preimage.
    static std::future<bool> CommitLotLiquidation(RandomBotEstateLiquidation request);
    static std::future<bool> ConfirmLotLiquidation(RandomBotEstateLiquidation request);
    static bool QuoteLotLiquidation(Item const& item, RandomBotEstateLiquidation& request);
    static std::future<bool> CreateAuctionListing(RandomBotEstateAuctionListing request);
    static std::future<bool> ConfirmAuctionListing(RandomBotEstateAuctionListing request);
    // Consumes a queued buyer debit, refund/winner mail and auction mutation.
    // Applicable only when owner or current bidder is a registered estate role.
    static std::future<bool> CommitAuctionBid(RandomBotEstateAuctionBid request);
    static std::future<bool> ConfirmAuctionBid(RandomBotEstateAuctionBid request);
    static std::future<bool> CommitAuctionResolution(RandomBotEstateAuctionResolution request);
    static std::future<bool> ConfirmAuctionResolution(RandomBotEstateAuctionResolution request);
    static std::future<bool> ConsumeAuctionMail(RandomBotEstateAuctionMail request);
    static std::future<bool> ConfirmAuctionMail(RandomBotEstateAuctionMail request);
    static std::future<bool> CommitClaimWin(RandomBotEstateClaimWin request);
    static std::future<bool> ConfirmClaimWin(RandomBotEstateClaimWin request);
    static std::future<bool> ConsumeClaimMail(RandomBotEstateClaimMail request);
    static std::future<bool> ConfirmClaimMail(RandomBotEstateClaimMail request);
    static std::future<bool> PrepareDistribution(RandomBotEstateDistribution request);
    static std::future<bool> DeliverInheritance(uint64 estateId,uint32 recipientGuid,uint64 moneyCap,
        std::vector<uint32> eligibleAccounts);
    static std::future<bool> CompleteDistribution(uint64 estateId);
    static std::future<bool> FinalizeArchive(uint64 estateId,uint32 guid,uint32 account);
    static std::future<bool> MarkDeletePending(uint64 estateId,uint32 guid,uint32 account);
    static std::future<bool> ConfirmDeleted(uint32 guid,uint32 account);
    static std::future<bool> ReserveReplacement(RandomBotReplacementReservation request);
    static std::future<bool> ConfirmReplacement(RandomBotReplacementReservation request);
};
