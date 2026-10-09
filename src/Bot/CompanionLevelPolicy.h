/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */
#ifndef PLAYERBOTS_COMPANIONLEVELPOLICY_H
#define PLAYERBOTS_COMPANIONLEVELPOLICY_H

#include <algorithm>
#include <cstdint>

class CompanionLevelPolicy
{
public:
    static constexpr uint8_t CatchUp(uint8_t current, uint8_t master)
    {
        return std::max(current, master);
    }

    static constexpr uint32_t LimitXP(uint8_t current, uint8_t master,
        uint32_t xp, uint32_t nextLevelXP, uint32_t amount)
    {
        // Lower-level bots wait for the safe catch-up instead of crossing several
        // levels inside GiveXP. At equal level they can fill, but not cross, the bar.
        if (current != master || !nextLevelXP || xp >= nextLevelXP - 1)
            return 0;
        return std::min(amount, nextLevelXP - xp - 1);
    }
};

#endif
