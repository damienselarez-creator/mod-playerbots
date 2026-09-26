#ifndef PLAYERBOT_SELFBOT_PROFESSIONS_H
#define PLAYERBOT_SELFBOT_PROFESSIONS_H

#include "Define.h"

class PlayerbotAI;
bool ExecuteSelfbotProfessions(PlayerbotAI* ai);
void UpdateSelfbotProfessions(PlayerbotAI* ai, uint32 elapsed);
bool IsSelfbotProfessionMaterial(PlayerbotAI* ai, uint32 item);
bool IsSelfbotPrimaryProfession(uint32 skill);

#endif
