#include "playerbot/RandomBotLifecycleMath.h"

#if defined(GenerateBotTests) || defined(PLAYERBOT_LIFECYCLE_STANDALONE_TEST)

#include <limits>

namespace
{
    bool Require(bool condition, const char* message, std::string& error)
    {
        if (condition)
            return true;
        error = message;
        return false;
    }
}

bool RunRandomBotLifecycleMathTests(std::string& error)
{
    std::vector<RandomBotLevelBracket> brackets;
    if (!Require(RandomBotLifecycleMath::ParseLevelBrackets(
            "1-10,11-79,80-80", "10,70,20", 80, brackets, error),
            "valid brackets were rejected", error) ||
        !Require(brackets.size() == 3 && brackets[2].Contains(80),
            "max-level-only bracket was not parsed", error))
        return false;

    std::string parseError;
    if (!Require(!RandomBotLifecycleMath::ParseLevelBrackets(
            "1-10,10-20", "50,50", 80, brackets, parseError),
            "overlapping brackets were accepted", error) ||
        !Require(!RandomBotLifecycleMath::ParseLevelBrackets(
            "1-10,11-20", "40,40", 80, brackets, parseError),
            "percentages not totaling 100 were accepted", error) ||
        !Require(!RandomBotLifecycleMath::ParseLevelBrackets(
            "1-81", "100", 80, brackets, parseError),
            "out-of-expansion level was accepted", error))
        return false;

    RandomBotLifecycleMath::ParseLevelBrackets("1-10,11-20", "50,50", 80, brackets, parseError);
    if (!Require(RandomBotLifecycleMath::CalculateBracketTargets(brackets, 0) == std::vector<std::uint32_t>({0, 0}),
            "zero-bot targets are incorrect", error) ||
        !Require(RandomBotLifecycleMath::CalculateBracketTargets(brackets, 1) == std::vector<std::uint32_t>({1, 0}),
            "single-bot target rounding is incorrect", error) ||
        !Require(RandomBotLifecycleMath::CalculateBracketDeficits({10, 10}, {12, 4}) ==
            std::vector<std::uint32_t>({0, 6}), "bracket deficits are incorrect", error))
        return false;

    if (!Require(RandomBotLifecycleMath::CalculateGoldSink(0, 0, 50) == 0,
            "zero estate sink is incorrect", error) ||
        !Require(RandomBotLifecycleMath::CalculateGoldSink(1000, 700, 40) == 700,
            "vendor income did not floor the sink", error) ||
        !Require(RandomBotLifecycleMath::CalculateGoldSink(1000, 1500, 40) == 1000,
            "sink was not clamped to the estate", error))
        return false;

    RandomBotInheritanceResult allocation = RandomBotLifecycleMath::CalculateInheritanceShares(1000, {}, {});
    if (!Require(allocation.distributed == 0 && allocation.remainder == 1000,
            "empty recipient allocation is incorrect", error))
        return false;

    allocation = RandomBotLifecycleMath::CalculateInheritanceShares(
        std::numeric_limits<std::uint32_t>::max(), {100, 200, 300}, {1, 4, 9});
    if (!Require(allocation.distributed == 600 && allocation.remainder ==
            std::numeric_limits<std::uint32_t>::max() - std::uint64_t(600),
            "large capped allocation is incorrect", error) ||
        !Require(allocation.shares[0] <= 100 && allocation.shares[1] <= 200 && allocation.shares[2] <= 300,
            "recipient cap was exceeded", error))
        return false;

    if (!Require(RandomBotLifecycleMath::CanTransition(RandomBotLifecycleStatus::MAX_LEVEL,
            RandomBotLifecycleStatus::RETIREMENT_PENDING), "valid lifecycle transition was rejected", error) ||
        !Require(!RandomBotLifecycleMath::CanTransition(RandomBotLifecycleStatus::ACTIVE,
            RandomBotLifecycleStatus::DELETED), "destructive lifecycle transition was accepted", error) ||
        !Require(!RandomBotLifecycleMath::CanTransition(RandomBotLifecycleStatus::DELETED,
            RandomBotLifecycleStatus::ACTIVE), "deleted bot could return to active", error))
        return false;

    return true;
}

#endif
