#pragma once

#include "Common.h"
#include <mutex>
#include <unordered_set>

// Identity/provisioning boundary only. Economic execution remains behind the
// lifecycle manager's immutable development gate.
class RandomBotEstateService
{
public:
    static RandomBotEstateService& instance()
    {
        static RandomBotEstateService service;
        return service;
    }
    void Initialize();
    bool IsServiceCharacter(uint32 guid) const;
    bool IsServiceAccount(uint32 account) const;
    bool HasServiceAccounts() const;
    uint32 SellerBroker(uint32 faction) const { return faction ? hordeSeller : allianceSeller; }
    uint32 BidderBroker(uint32 faction) const { return faction ? hordeBidder : allianceBidder; }
    bool Ready() const { return ready; }

private:
    bool ValidateBroker(uint32 guid, uint32 account, uint32 faction, uint32 role);
    bool initialized = false;
    bool ready = false;
    mutable std::mutex mutex;
    std::unordered_set<uint32> characters;
    std::unordered_set<uint32> accounts;
    uint32 allianceSeller = 0, hordeSeller = 0, allianceBidder = 0, hordeBidder = 0;
};

#define sRandomBotEstateService RandomBotEstateService::instance()
