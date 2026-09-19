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

class RandomBotEstateStore
{
public:
    // Offline inventory/bank leaf transfer, not liquidation. No stack merging or
    // copied item. Caller holds original+broker login/save leases; wrapped items,
    // saved loot, nonempty bags and auction/mail-linked items fail closed.
    // An ambiguous result can retry this SAME immutable request: the receipt
    // branch only verifies the committed transfer and never transfers twice.
    static std::future<bool> TransferInventoryLot(RandomBotEstateIntakeLot request);
    // Consumes queued core Item removal, credits independent estate escrow and
    // writes a receipt atomically. Caller must validate quote using the core's
    // GetVendorSellValue with overflow checks and retain exact charge preimage.
    static std::future<bool> CommitLotLiquidation(RandomBotEstateLiquidation request);
    static std::future<bool> ConfirmLotLiquidation(RandomBotEstateLiquidation request);
    static bool QuoteLotLiquidation(Item const& item, RandomBotEstateLiquidation& request);
};
