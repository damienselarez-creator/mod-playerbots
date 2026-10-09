/*
 * This file is part of the mod-playerbots module for AzerothCore.
 * Released under GNU GPL v2 or later; see AUTHORS.
 */
#ifndef COMPANION_VOCATION_H
#define COMPANION_VOCATION_H

#include <array>
#include <atomic>
#include <cstdint>

class Player;

struct CompanionVocation
{
    bool managed = false;
    int tab = -1;
    std::array<uint32_t, 2> professions{};
    using Provider = CompanionVocation (*)(Player*);
    inline static std::atomic<Provider> provider{nullptr};

    static CompanionVocation Get(Player* bot)
    {
        auto function = provider.load();
        return function ? function(bot) : CompanionVocation{};
    }
};
#endif
