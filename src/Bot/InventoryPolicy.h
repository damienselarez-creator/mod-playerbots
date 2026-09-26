#ifndef PLAYERBOT_INVENTORY_POLICY_H
#define PLAYERBOT_INVENTORY_POLICY_H

#include <algorithm>
#include <cstdint>

namespace SelfbotInventoryPolicy
{
    inline bool NeedsSpace(uint32_t free, uint32_t total)
    {
        return total && (free <= 2 || uint64_t(free) * 100 <= uint64_t(total) * 15);
    }

    inline uint32_t SaleQuantity(bool protectedItem, uint32_t quality, uint32_t count,
                                 uint32_t produced, uint32_t original, bool vendorUsage, uint32_t keep = 1)
    {
        if (protectedItem || !count)
            return 0;
        if (quality == 0)
            return count;
        if (quality != 1 || !vendorUsage || count <= original)
            return 0;
        // Keep the requested sample and never sell the original stock.
        uint32_t ownedProduction = std::min(produced, count - original);
        return ownedProduction > keep ? ownedProduction - keep : 0;
    }

    inline bool CanRepair(uint32_t money, uint32_t reserve, uint64_t cost)
    {
        return cost && cost + reserve <= money;
    }

    inline bool NeedsRepair(uint32_t maximum, uint32_t current)
    {
        return maximum && uint64_t(current) * 5 <= maximum;
    }
}

#endif
