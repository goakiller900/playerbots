#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct RandomBotLevelBracket
{
    std::uint32_t minLevel = 0;
    std::uint32_t maxLevel = 0;
    std::uint32_t percentage = 0;

    bool Contains(std::uint32_t level) const { return level >= minLevel && level <= maxLevel; }
};

enum class RandomBotLifecycleStatus : std::uint8_t
{
    ACTIVE = 0,
    AT_MAX_LEVEL = 1,
    RETIREMENT_PENDING = 2,
    LIQUIDATING = 3,
    WAITING_AUCTIONS = 4,
    SETTLING_MAIL = 5,
    DISTRIBUTING_ESTATE = 6,
    RETIRED = 7,
    DELETE_PENDING = 8,
    DELETED = 9,
    // Append; never renumber persisted pre-broker development states.
    RETIREMENT_ASSETS_ESCROWED = 10
};

enum class RandomBotCharacterDisposition : std::uint8_t
{
    ARCHIVE = 0,
    DELETE_CHARACTER = 1
};

enum class RandomBotUnsellableItemPolicy : std::uint8_t
{
    RETAIN = 0,
    DESTROY = 1
};

struct RandomBotInheritanceResult
{
    std::vector<std::uint64_t> shares;
    std::uint64_t distributed = 0;
    std::uint64_t remainder = 0;
};

class RandomBotLifecycleMath
{
public:
    static bool ParseLevelBrackets(const std::string& ranges, const std::string& percentages,
        std::uint32_t maxPlayerLevel, std::vector<RandomBotLevelBracket>& brackets, std::string& error);
    static std::int32_t FindLevelBracket(const std::vector<RandomBotLevelBracket>& brackets, std::uint32_t level);
    static std::vector<std::uint32_t> CalculateBracketTargets(const std::vector<RandomBotLevelBracket>& brackets,
        std::uint32_t total);
    static std::vector<std::uint32_t> CalculateBracketDeficits(const std::vector<std::uint32_t>& targets,
        const std::vector<std::uint32_t>& current);
    static std::uint64_t CalculateGoldSink(std::uint64_t finalEstate, std::uint64_t vendorGeneratedGold, std::uint32_t sinkPercent);
    static RandomBotInheritanceResult CalculateInheritanceShares(std::uint64_t pool,
        const std::vector<std::uint64_t>& recipientCaps, const std::vector<std::uint32_t>& weights);
    static bool CanTransition(RandomBotLifecycleStatus from, RandomBotLifecycleStatus to);
};
