#include "RandomBotLifecycleMath.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>

namespace
{
    // floor(remainder * multiplier / divisor), without overflowing uint64.
    // Precondition: remainder < divisor; multiplier has at most 32 bits.
    std::uint64_t FractionProduct(std::uint64_t remainder, std::uint32_t multiplier,
        std::uint64_t divisor)
    {
        std::uint64_t quotient = 0;
        std::uint64_t residue = 0;
        for (int bit = 31; bit >= 0; --bit)
        {
            quotient *= 2;
            if (residue >= divisor - residue)
            {
                residue -= divisor - residue;
                ++quotient;
            }
            else
                residue *= 2;
            if (multiplier & (std::uint32_t(1) << bit))
            {
                if (residue >= divisor - remainder)
                {
                    residue -= divisor - remainder;
                    ++quotient;
                }
                else
                    residue += remainder;
            }
        }
        return quotient;
    }

    std::string Trim(const std::string& value)
    {
        size_t first = 0;
        while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])))
            ++first;

        size_t last = value.size();
        while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])))
            --last;

        return value.substr(first, last - first);
    }

    bool ParseUInt(const std::string& text, std::uint32_t& value)
    {
        const std::string trimmed = Trim(text);
        if (trimmed.empty() || !std::all_of(trimmed.begin(), trimmed.end(), [](char c)
            { return std::isdigit(static_cast<unsigned char>(c)); }))
            return false;

        try
        {
            const unsigned long parsed = std::stoul(trimmed);
            if (parsed > std::numeric_limits<std::uint32_t>::max())
                return false;
            value = static_cast<std::uint32_t>(parsed);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    std::vector<std::string> SplitComma(const std::string& text)
    {
        std::vector<std::string> values;
        std::stringstream stream(text);
        std::string value;
        while (std::getline(stream, value, ','))
            values.push_back(Trim(value));
        if (!text.empty() && text.back() == ',')
            values.emplace_back();
        return values;
    }

}

bool RandomBotLifecycleMath::ParseLevelBrackets(const std::string& ranges, const std::string& percentages,
    std::uint32_t maxPlayerLevel, std::vector<RandomBotLevelBracket>& brackets, std::string& error)
{
    brackets.clear();
    error.clear();
    const std::vector<std::string> rangeValues = SplitComma(ranges);
    const std::vector<std::string> percentValues = SplitComma(percentages);

    if (rangeValues.empty() || (rangeValues.size() == 1 && rangeValues.front().empty()))
    {
        error = "AiPlayerbot.LevelBrackets is empty";
        return false;
    }
    if (rangeValues.size() != percentValues.size())
    {
        error = "AiPlayerbot.LevelBracketBalance must contain exactly one percentage per bracket";
        return false;
    }

    std::uint64_t totalPercent = 0;
    for (size_t i = 0; i < rangeValues.size(); ++i)
    {
        const size_t dash = rangeValues[i].find('-');
        if (dash == std::string::npos || rangeValues[i].find('-', dash + 1) != std::string::npos)
        {
            error = "invalid level bracket '" + rangeValues[i] + "' (expected min-max)";
            brackets.clear();
            return false;
        }

        RandomBotLevelBracket bracket;
        if (!ParseUInt(rangeValues[i].substr(0, dash), bracket.minLevel) ||
            !ParseUInt(rangeValues[i].substr(dash + 1), bracket.maxLevel) ||
            !ParseUInt(percentValues[i], bracket.percentage))
        {
            error = "invalid numeric value in level bracket configuration at position " + std::to_string(i + 1);
            brackets.clear();
            return false;
        }
        if (!bracket.minLevel || bracket.minLevel > bracket.maxLevel)
        {
            error = "level bracket minimum must be at least 1 and no greater than its maximum";
            brackets.clear();
            return false;
        }
        if (bracket.maxLevel > maxPlayerLevel)
        {
            error = "level bracket " + rangeValues[i] + " exceeds the configured core maximum level " +
                std::to_string(maxPlayerLevel);
            brackets.clear();
            return false;
        }
        if (bracket.percentage > 100)
        {
            error = "level bracket percentages must be between 0 and 100";
            brackets.clear();
            return false;
        }

        for (const RandomBotLevelBracket& previous : brackets)
        {
            if (bracket.minLevel <= previous.maxLevel && bracket.maxLevel >= previous.minLevel)
            {
                error = "level brackets overlap at " + rangeValues[i];
                brackets.clear();
                return false;
            }
        }

        totalPercent += bracket.percentage;
        brackets.push_back(bracket);
    }

    if (totalPercent != 100)
    {
        error = "AiPlayerbot.LevelBracketBalance percentages must total 100 (configured total is " +
            std::to_string(totalPercent) + ")";
        brackets.clear();
        return false;
    }

    std::sort(brackets.begin(), brackets.end(), [](const RandomBotLevelBracket& left, const RandomBotLevelBracket& right)
        { return left.minLevel < right.minLevel; });
    return true;
}

std::int32_t RandomBotLifecycleMath::FindLevelBracket(const std::vector<RandomBotLevelBracket>& brackets, std::uint32_t level)
{
    for (size_t i = 0; i < brackets.size(); ++i)
        if (brackets[i].Contains(level))
            return static_cast<std::int32_t>(i);
    return -1;
}

std::vector<std::uint32_t> RandomBotLifecycleMath::CalculateBracketTargets(
    const std::vector<RandomBotLevelBracket>& brackets, std::uint32_t total)
{
    std::vector<std::uint32_t> targets(brackets.size(), 0);
    std::vector<std::pair<std::uint32_t, size_t>> remainders;
    std::uint64_t assigned = 0;
    for (size_t i = 0; i < brackets.size(); ++i)
    {
        const std::uint64_t scaled = std::uint64_t(total) * brackets[i].percentage;
        targets[i] = static_cast<std::uint32_t>(scaled / 100);
        assigned += targets[i];
        remainders.emplace_back(static_cast<std::uint32_t>(scaled % 100), i);
    }

    std::stable_sort(remainders.begin(), remainders.end(), [](const auto& left, const auto& right)
        { return left.first > right.first; });
    for (std::uint64_t i = assigned; i < total && !remainders.empty(); ++i)
        ++targets[remainders[(i - assigned) % remainders.size()].second];
    return targets;
}

std::vector<std::uint32_t> RandomBotLifecycleMath::CalculateBracketDeficits(const std::vector<std::uint32_t>& targets,
    const std::vector<std::uint32_t>& current)
{
    std::vector<std::uint32_t> deficits(targets.size(), 0);
    for (size_t i = 0; i < targets.size(); ++i)
    {
        const std::uint32_t have = i < current.size() ? current[i] : 0;
        deficits[i] = targets[i] > have ? targets[i] - have : 0;
    }
    return deficits;
}

std::uint64_t RandomBotLifecycleMath::CalculateGoldSink(std::uint64_t finalEstate, std::uint64_t vendorGeneratedGold, std::uint32_t sinkPercent)
{
    sinkPercent = std::min<std::uint32_t>(sinkPercent, 100);
    const std::uint64_t percentageSink = finalEstate / 100 * sinkPercent +
        (finalEstate % 100) * sinkPercent / 100;
    return std::min(finalEstate, std::max(percentageSink, vendorGeneratedGold));
}

RandomBotInheritanceResult RandomBotLifecycleMath::CalculateInheritanceShares(std::uint64_t pool,
    const std::vector<std::uint64_t>& recipientCaps, const std::vector<std::uint32_t>& weights)
{
    RandomBotInheritanceResult result;
    result.shares.assign(recipientCaps.size(), 0);
    std::uint64_t remaining = pool;

    while (remaining)
    {
        std::uint64_t totalWeight = 0;
        for (size_t i = 0; i < recipientCaps.size(); ++i)
            if (result.shares[i] < recipientCaps[i])
                totalWeight += i < weights.size() && weights[i] ? weights[i] : 1;
        if (!totalWeight)
            break;

        bool progressed = false;
        const std::uint64_t roundStart = remaining;
        for (size_t i = 0; i < recipientCaps.size() && remaining; ++i)
        {
            if (result.shares[i] >= recipientCaps[i])
                continue;
            const std::uint64_t weight = i < weights.size() && weights[i] ? weights[i] : 1;
            std::uint64_t share = std::max<std::uint64_t>(1, roundStart / totalWeight * weight +
                FractionProduct(roundStart % totalWeight, static_cast<std::uint32_t>(weight), totalWeight));
            share = std::min(share, recipientCaps[i] - result.shares[i]);
            share = std::min(share, remaining);
            result.shares[i] += share;
            remaining -= share;
            progressed |= share != 0;
        }
        if (!progressed)
            break;
    }

    result.distributed = pool - remaining;
    result.remainder = remaining;
    return result;
}

bool RandomBotLifecycleMath::CanTransition(RandomBotLifecycleStatus from, RandomBotLifecycleStatus to)
{
    switch (from)
    {
        case RandomBotLifecycleStatus::ACTIVE:
            return to == RandomBotLifecycleStatus::MAX_LEVEL;
        case RandomBotLifecycleStatus::MAX_LEVEL:
            return to == RandomBotLifecycleStatus::ACTIVE || to == RandomBotLifecycleStatus::RETIREMENT_PENDING;
        case RandomBotLifecycleStatus::RETIREMENT_PENDING:
            return to == RandomBotLifecycleStatus::LIQUIDATING ||
                to == RandomBotLifecycleStatus::RETIREMENT_ASSETS_ESCROWED;
        case RandomBotLifecycleStatus::RETIREMENT_ASSETS_ESCROWED:
            return to == RandomBotLifecycleStatus::RETIRED;
        case RandomBotLifecycleStatus::LIQUIDATING:
            return to == RandomBotLifecycleStatus::WAITING_AUCTIONS || to == RandomBotLifecycleStatus::SETTLING_MAIL;
        case RandomBotLifecycleStatus::WAITING_AUCTIONS:
            return to == RandomBotLifecycleStatus::SETTLING_MAIL;
        case RandomBotLifecycleStatus::SETTLING_MAIL:
            return to == RandomBotLifecycleStatus::LIQUIDATING || to == RandomBotLifecycleStatus::DISTRIBUTING_ESTATE;
        case RandomBotLifecycleStatus::DISTRIBUTING_ESTATE:
            return to == RandomBotLifecycleStatus::RETIRED;
        case RandomBotLifecycleStatus::RETIRED:
            return to == RandomBotLifecycleStatus::DELETE_PENDING;
        case RandomBotLifecycleStatus::DELETE_PENDING:
            return to == RandomBotLifecycleStatus::DELETED;
        case RandomBotLifecycleStatus::DELETED:
            return false;
    }
    return false;
}
