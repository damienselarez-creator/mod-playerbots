#ifndef PLAYERBOT_COMPANION_ERRAND_POLICY_H
#define PLAYERBOT_COMPANION_ERRAND_POLICY_H

#include <array>
#include <algorithm>
#include <cstdint>
#include <map>
#include <sstream>
#include <string>

namespace CompanionErrands
{
    // Names are the canonical item_template names, also used by RogueActions.
    inline int PoisonFamily(std::string const& name)
    {
        std::array<std::string, 2> const names = {"Instant Poison", "Deadly Poison"};
        for (size_t family = 0; family < names.size(); ++family)
            for (std::string const suffix : {"", " II", " III", " IV", " V", " VI", " VII", " VIII", " IX"})
                if (name == names[family] + suffix)
                    return int(family);
        return -1;
    }

    inline uint32_t PoisonBatches(uint32_t held, uint32_t pack, uint32_t price,
        uint32_t money, uint32_t reserve)
    {
        if (held >= 3 || !pack || pack > 10 || money < reserve)
            return 0;
        uint32_t desired = (10 - held + pack - 1) / pack;
        return price ? std::min(desired, (money - reserve) / price) : desired;
    }

    inline bool CanSellGrey(uint32_t quality, bool protectedItem, bool unused)
    {
        return quality == 0 && !protectedItem && unused;
    }

    using Plans = std::map<std::string, std::array<uint32_t, 2>>;

    inline bool IsPrimary(uint32_t skill)
    {
        for (uint32_t id : {164u, 165u, 171u, 182u, 186u, 197u, 202u, 333u, 393u, 755u, 773u})
            if (skill == id)
                return true;
        return false;
    }

    inline Plans ParsePlans(std::string const& text)
    {
        Plans result;
        std::istringstream input(text);
        std::string entry;
        while (std::getline(input, entry, ';'))
        {
            auto const first = entry.find(':');
            if (first == std::string::npos || !first || first > 12)
                continue;
            std::string values = entry.substr(first + 1);
            for (char& c : values)
                if (c == ':')
                    c = ' ';
            std::istringstream numbers(values);
            uint32_t a = 0, b = 0;
            std::string extra;
            if (!(numbers >> a >> b) || (numbers >> extra) || !IsPrimary(a) || !IsPrimary(b) || a == b)
                continue;
            result.emplace(entry.substr(0, first), std::array<uint32_t, 2>{a, b});
        }
        return result;
    }

    inline bool CanSpend(uint32_t money, uint32_t cost, uint32_t reserve)
    {
        return uint64_t(cost) + reserve <= money;
    }

    inline bool HerbWithinLeash(float botToHerb, float masterToHerb, float botToMaster)
    {
        return botToHerb <= 40.0f && masterToHerb <= 60.0f && botToMaster <= 60.0f;
    }
}
#endif
