#ifndef PLAYERBOT_SELFBOT_CYCLE_H
#define PLAYERBOT_SELFBOT_CYCLE_H

#include <cstdint>
#include <string>
#include <istream>
#include <ostream>
#include <memory>
#include "QuestSurveyProgress.h"

class PlayerbotAI;
class ChatHandler;
struct SelfbotProfessionState;
struct SelfbotInventoryState;
struct SelfbotQuestSurveyState;

inline bool SelfbotManualSessionAllowed(bool botSession, bool socketOpen, bool loggingOut)
{
    return !botSession && socketOpen && !loggingOut;
}

// Only active AI ticks count. A phase boundary waits for a safe action boundary.
struct SelfbotCycle
{
    bool enabled = false;
    bool loaded = false;
    bool configured = false;
    uint32_t phase = 0;
    uint32_t duration = 7200000;
    uint32_t remaining = 7200000;
    uint32_t crafts = 0;
    uint32_t saveElapsed = 0;
    uint32_t decisionElapsed = 15000;
    uint32_t stalled = 0;
    uint64_t progress = 0;
    uint32_t profession1 = 0;
    uint32_t profession2 = 0;
    std::shared_ptr<SelfbotProfessionState> professions;
    std::shared_ptr<SelfbotInventoryState> inventory;
    std::shared_ptr<SelfbotQuestSurveyState> surveyRuntime;
    SelfbotSurvey::Progress surveyProgress;
    std::string objective = "En attente";

    bool Read(std::istream& input)
    {
        uint32_t version = 0;
        SelfbotCycle candidate;
        if (!(input >> version >> candidate.enabled >> candidate.phase >> candidate.duration >>
              candidate.remaining >> candidate.crafts) || (version < 1 || version > 3) || candidate.phase > 2 ||
            (candidate.duration != 7200000 && candidate.duration != 14400000) ||
            candidate.remaining > candidate.duration || candidate.crafts > (version == 1 ? 20u : 200u))
            return false;
        if (version >= 2 && !(input >> candidate.profession1 >> candidate.profession2))
            return false;
        if (version == 3 && !candidate.surveyProgress.Read(input))
            return false;
        candidate.loaded = true;
        *this = candidate;
        return true;
    }

    void Write(std::ostream& output) const
    {
        output << 3 << ' ' << enabled << ' ' << phase << ' ' << duration << ' '
               << remaining << ' ' << crafts << ' ' << profession1 << ' ' << profession2 << '\n';
        surveyProgress.Write(output);
    }

    bool Tick(uint32_t elapsed, bool safe)
    {
        if (!enabled)
            return false;
        remaining = elapsed >= remaining ? 0 : remaining - elapsed;
        if (remaining || !safe)
            return false;
        phase = (phase + 1) % 3;
        remaining = duration;
        crafts = 0;
        stalled = 0;
        progress = 0;
        professions.reset();
        surveyRuntime.reset();
        return true;
    }
};

void UpdateSelfbotCycle(PlayerbotAI* ai, uint32_t elapsed);
void SaveSelfbotCycle(PlayerbotAI* ai);
void ResumeSelfbotCycle(PlayerbotAI* ai);
bool ExecuteSelfbotCycle(PlayerbotAI* ai);
bool HandleSelfbotCycleCommand(ChatHandler* handler, char const* args);

#endif
