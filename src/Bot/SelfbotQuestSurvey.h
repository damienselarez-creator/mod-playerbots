#ifndef PLAYERBOT_QUEST_SURVEY_H
#define PLAYERBOT_QUEST_SURVEY_H

#include "Define.h"
#include <string>

class PlayerbotAI;
void UpdateSelfbotQuestSurvey(PlayerbotAI* ai, uint32 elapsed);
bool ExecuteSelfbotQuestSurvey(PlayerbotAI* ai);
bool ContinueSelfbotQuestSurvey(PlayerbotAI* ai);
std::string SelfbotQuestSurveyStatus(PlayerbotAI* ai);
void ResetSelfbotQuestSurvey(PlayerbotAI* ai);

#endif
