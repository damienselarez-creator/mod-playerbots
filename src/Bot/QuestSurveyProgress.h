#ifndef PLAYERBOT_QUEST_SURVEY_PROGRESS_H
#define PLAYERBOT_QUEST_SURVEY_PROGRESS_H

#include <cstdint>
#include <cmath>
#include <istream>
#include <ostream>
#include <map>
#include <limits>
#include <vector>

namespace SelfbotSurvey
{
    enum class Outcome : uint32_t { Visited = 1, Empty = 2, Blocked = 3 };
    struct Visit
    {
        uint32_t map = 0, zone = 0;
        uint64_t stamp = 0, retry = 0;
        Outcome outcome = Outcome::Visited;
    };
    struct Node
    {
        uint64_t key = 0;
        uint32_t entry = 0;
        float x = 0, y = 0, z = 0;
        std::pair<int32_t, int32_t> Sector() const
        {
            return {int32_t(std::floor(x / 400)), int32_t(std::floor(y / 400))};
        }
    };
    struct Progress
    {
        uint64_t clock = 0;
        std::map<uint64_t, Visit> visits;

        bool Pending(uint64_t key, uint64_t stamp) const
        {
            auto it = visits.find(key);
            if (it == visits.end())
                return true;
            if (it->second.outcome == Outcome::Blocked)
                return clock >= it->second.retry;
            return it->second.stamp != stamp;
        }

        void Mark(uint64_t key, uint32_t map, uint32_t zone, uint64_t stamp, Outcome outcome)
        {
            if (!visits.count(key) && visits.size() >= 4096)
                visits.erase(visits.begin());
            visits[key] = {map, zone, stamp, clock + 1800000, outcome};
        }

        void Reset(uint32_t map, uint32_t zone)
        {
            for (auto it = visits.begin(); it != visits.end();)
                if (it->second.map == map && it->second.zone == zone)
                    it = visits.erase(it);
                else
                    ++it;
        }

        bool Read(std::istream& in)
        {
            Progress candidate;
            uint32_t count = 0;
            if (!(in >> candidate.clock >> count) || count > 4096 || candidate.clock > (uint64_t(1) << 60))
                return false;
            for (uint32_t i = 0; i < count; ++i)
            {
                uint64_t key = 0;
                uint32_t outcome = 0;
                Visit visit;
                if (!(in >> key >> visit.map >> visit.zone >> visit.stamp >> visit.retry >> outcome) ||
                    !key || outcome < 1 || outcome > 3 || candidate.visits.count(key) ||
                    visit.retry > candidate.clock + 1800000)
                    return false;
                visit.outcome = Outcome(outcome);
                candidate.visits.emplace(key, visit);
            }
            *this = std::move(candidate);
            return true;
        }

        void Write(std::ostream& out) const
        {
            out << clock << ' ' << visits.size() << '\n';
            for (auto const& [key, visit] : visits)
                out << key << ' ' << visit.map << ' ' << visit.zone << ' ' << visit.stamp << ' '
                    << visit.retry << ' ' << uint32_t(visit.outcome) << '\n';
        }
    };

    inline std::size_t Select(std::vector<Node> const& nodes, Progress const& progress, uint64_t stamp,
                              float x, float y, bool keepSector, std::pair<int32_t, int32_t> sector)
    {
        std::size_t best = nodes.size();
        double bestDistance = std::numeric_limits<double>::max();
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            auto const& node = nodes[i];
            if (!progress.Pending(node.key, stamp) || (keepSector && node.Sector() != sector))
                continue;
            double distance = double(node.x - x) * (node.x - x) + double(node.y - y) * (node.y - y);
            if (best == nodes.size() || distance < bestDistance ||
                (distance == bestDistance && node.key < nodes[best].key))
            {
                best = i;
                bestDistance = distance;
            }
        }
        if (best == nodes.size() && keepSector)
            return Select(nodes, progress, stamp, x, y, false, sector);
        return best;
    }
}

#endif
