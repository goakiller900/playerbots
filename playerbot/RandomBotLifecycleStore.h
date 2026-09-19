#pragma once

#include "Common.h"
#include <future>
#include <vector>

// Inputs are immutable snapshots: database worker callbacks never access Player,
// configuration singletons, the world registry, or login caches.
struct RandomBotEstateRequest
{
    uint32 guid = 0;
    uint32 account = 0;
    uint32 sinkPercent = 100;
    uint32 recipientCount = 0;
    uint64 recipientCap = 0;
    uint64 moneyCap = 0;
    std::vector<uint32> eligibleAccounts;
    std::vector<uint32> weights;
};

enum class RandomBotAssetOperation : uint8
{
    VENDOR_ITEM = 1,
    DESTROY_ITEM = 2,
    COLLECT_MAIL_GOLD = 3
};

// One full stack or one delivered mail's money. The idempotency key is
// (guid, kind, assetGuid); it is never regenerated after uncertain completion.
// Values must come from a quiescent character and core item rules. The caller
// must hold an offline lease and drain prior saves before preparing serialization.
struct RandomBotAssetSave
{
    uint32 guid = 0;
    uint32 account = 0;
    RandomBotAssetOperation kind = RandomBotAssetOperation::VENDOR_ITEM;
    uint32 assetGuid = 0;
    uint32 itemEntry = 0;
    uint32 itemCount = 0;
    uint32 moneyBefore = 0;
    uint64 proceeds = 0;
    std::vector<uint32> eligibleAccounts;
};

class RandomBotLifecycleStore
{
public:
    static bool Supported();
    // Consumes core-serialized bid/debit/refund/winner-mail writes. The caller
    // must hold bidder-save, mailbox and auction mutation fences until receipt
    // reconciliation. No Player or AuctionEntry pointer crosses the worker.
    struct AuctionBid
    {
        uint32 auctionId = 0, owner = 0, account = 0;
        uint32 itemGuid = 0, itemEntry = 0, itemCount = 0;
        uint32 oldBidder = 0, oldBid = 0, bidder = 0, bidderAccount = 0;
        uint32 bid = 0, startBid = 0, buyout = 0, moneyBefore = 0;
        uint32 refundMailId = 0, winnerMailId = 0;
        uint64 expiresAt = 0, payoutAt = 0;
        std::vector<uint32> eligibleAccounts;
    };
    static std::future<bool> CommitAuctionBid(AuctionBid request);
    static std::future<bool> ConfirmAuctionBid(AuctionBid request);
    // Experimental, unconnected AH store primitives: NOT an approved runtime
    // integration. Real-player bidding/persistence must remain unchanged.
    // Terminal seller payout only. The item must already have left the auction
    // and the normal core payout delay must have elapsed. No fake sale or bid.
    struct AuctionPayout
    {
        uint32 auctionId = 0, owner = 0, account = 0, bidder = 0;
        uint32 itemEntry = 0, bid = 0, deposit = 0, cut = 0;
        uint64 expiresAt = 0, payoutAt = 0;
        std::vector<uint32> eligibleAccounts;
    };
    static std::future<bool> SettleAuctionPayout(AuctionPayout request);
    struct AuctionReturn
    {
        uint32 auctionId = 0, owner = 0, account = 0, itemGuid = 0;
        uint32 itemEntry = 0, itemCount = 0, mailId = 0;
        uint64 expiresAt = 0;
        std::vector<uint32> eligibleAccounts;
    };
    // Consumes queued MailDraft persistence. Confirmation never queues mail.
    static std::future<bool> CommitAuctionReturn(AuctionReturn request);
    static std::future<bool> ConfirmAuctionReturn(AuctionReturn request);
    // Consumes an EXISTING transaction containing core-serialized asset saves.
    // Proceeds enter estate escrow, never the capped Player money field.
    // Caller must prevent every ordinary mutation/save through acknowledgement.
    // False/exception (including duplicate journal key) requires reconciliation
    // and reload, never repeating DestroyItem/ModifyMoney on the live object.
    // Not called by the manager until quiescence/recovery is implemented.
    static std::future<bool> CommitAssetSave(RandomBotAssetSave request);
    // Read-only receipt/postcondition check after lost acknowledgement. False
    // means keep the character quarantined, not permission to replay a save.
    static std::future<bool> ConfirmAssetSave(RandomBotAssetSave request);
    // The caller must hold an offline lease until the returned future is ready.
    // All false/exception results are ambiguous: retry using durable ledger state.
    static std::future<bool> PrepareEstate(RandomBotEstateRequest request);
    static std::future<bool> DeliverInheritance(uint32 retiringGuid, uint32 recipientGuid,
        uint64 moneyCap, std::vector<uint32> eligibleAccounts);
};
