/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COMPANIONRECOVERYPOLICY_H
#define PLAYERBOTS_COMPANIONRECOVERYPOLICY_H

#include <cstdint>

class CompanionRecoveryPolicy
{
public:
    static constexpr uint32_t WipeDelay = 10;
    static constexpr uint32_t NormalDelay = 20;
    static constexpr uint32_t ResurrectionDelay = 60;

    static constexpr bool ShouldRelease(uint32_t elapsed, bool combat, bool resurrectionPending,
        bool livingAlly, bool resurrectionClass)
    {
        if (combat || resurrectionPending)
            return false;

        uint32_t delay = !livingAlly ? WipeDelay :
            (resurrectionClass ? ResurrectionDelay : NormalDelay);
        return elapsed >= delay;
    }
};

#endif
