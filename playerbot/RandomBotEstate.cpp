#include "RandomBotEstate.h"

bool RandomBotEstateRules::CanTransition(RandomBotEstateStatus from, RandomBotEstateStatus to)
{
    using S = RandomBotEstateStatus;
    if (to == S::HELD)
        return from != S::COMPLETE && from != S::HELD;
    switch (from)
    {
        case S::INTAKE: return to == S::ESCROWED;
        case S::ESCROWED: return to == S::LIQUIDATING;
        case S::LIQUIDATING: return to == S::DISTRIBUTING;
        case S::DISTRIBUTING: return to == S::COMPLETE;
        default: return false; // HELD requires explicit evidence-based recovery.
    }
}

bool RandomBotEstateRules::CanTransitionLot(RandomBotEstateLotStatus from, RandomBotEstateLotStatus to)
{
    using S = RandomBotEstateLotStatus;
    if (to == S::HELD)
        return from == S::READY || from == S::LISTED || from == S::RETURNED || from == S::VENDOR_PENDING;
    switch (from)
    {
        case S::READY: return to == S::LISTED || to == S::VENDOR_PENDING || to == S::RETAINED;
        case S::LISTED: return to == S::SOLD || to == S::RETURNED;
        case S::RETURNED: return to == S::READY || to == S::VENDOR_PENDING || to == S::RETAINED;
        case S::VENDOR_PENDING: return to == S::VENDORED || to == S::DESTROYED || to == S::RETAINED;
        default: return false;
    }
}

bool RandomBotEstateRules::CanFinalizeCharacter(RandomBotEstateStatus status, bool detached,
    bool remainingCharacterAssets, bool unresolvedIntake)
{
    return detached && !remainingCharacterAssets && !unresolvedIntake &&
        (status == RandomBotEstateStatus::ESCROWED || status == RandomBotEstateStatus::LIQUIDATING ||
         status == RandomBotEstateStatus::DISTRIBUTING || status == RandomBotEstateStatus::COMPLETE);
}

bool RandomBotEstateRules::CanDistribute(RandomBotEstateStatus status, std::uint64_t unsettledLots,
    std::uint64_t unsettledAuctions, std::uint64_t unsettledMail, bool ambiguousOperation)
{
    return status == RandomBotEstateStatus::LIQUIDATING && !unsettledLots && !unsettledAuctions &&
        !unsettledMail && !ambiguousOperation;
}
