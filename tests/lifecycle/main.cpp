#include "playerbot/RandomBotLifecycleMath.h"
#include <iostream>
#include <limits>

bool RunRandomBotLifecycleMathTests(std::string& error);

int main()
{
    std::string error;
    if (!RunRandomBotLifecycleMathTests(error))
    {
        std::cerr << error << '\n';
        return 1;
    }
    std::vector<RandomBotLevelBracket> brackets;
    for (const auto& range : {"1-10,", ",1-10", "10-1", "0-10", "1-10,5-20", "1-99999999999999999999"})
        if (RandomBotLifecycleMath::ParseLevelBrackets(range, "100", 80, brackets, error))
            return 2;
    if (RandomBotLifecycleMath::ParseLevelBrackets("1-10", "100,", 80, brackets, error))
        return 3;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto weight = std::numeric_limits<std::uint32_t>::max();
    for (const auto pool : {std::uint64_t(0), std::uint64_t(1), maximum - 1, maximum})
    {
        const auto allocation = RandomBotLifecycleMath::CalculateInheritanceShares(
            pool, {maximum, maximum, maximum}, {weight, weight - 1, weight});
        std::uint64_t sum = 0;
        for (auto share : allocation.shares)
        {
            if (share > pool - sum)
                return 4;
            sum += share;
        }
        if (sum != allocation.distributed || pool - sum != allocation.remainder)
            return 5;
        if (RandomBotLifecycleMath::CalculateGoldSink(pool, maximum, 40) != pool)
            return 6;
    }
    std::cout << "Lifecycle math tests passed\n";
    return 0;
}
