#pragma once

#include <cstdint>

// Independent from character lifecycle/disposition: these records survive the
// original character and its replacement. Numeric values are persisted in SQL.
enum class RandomBotEstateStatus : std::uint8_t
{
    INTAKE = 0,
    ESCROWED = 1,
    LIQUIDATING = 2,
    DISTRIBUTING = 3,
    COMPLETE = 4,
    HELD = 5
};

enum class RandomBotEstateLotStatus : std::uint8_t
{
    READY = 0,
    LISTED = 1,
    RETURNED = 2,
    VENDOR_PENDING = 3,
    RETAINED = 4,
    SOLD = 5,
    VENDORED = 6,
    DESTROYED = 7,
    HELD = 8
};

class RandomBotEstateRules
{
public:
    static bool CanTransition(RandomBotEstateStatus from, RandomBotEstateStatus to);
    static bool CanTransitionLot(RandomBotEstateLotStatus from, RandomBotEstateLotStatus to);
    // Detached assets need not be liquidated before archive/delete/replacement.
    // A HELD estate never authorizes deletion, even if some assets moved already.
    static bool CanFinalizeCharacter(RandomBotEstateStatus status, bool detached,
        bool remainingCharacterAssets, bool unresolvedIntake);
    static bool CanDistribute(RandomBotEstateStatus status, std::uint64_t unsettledLots,
        std::uint64_t unsettledAuctions, std::uint64_t unsettledMail, bool ambiguousOperation);
};
