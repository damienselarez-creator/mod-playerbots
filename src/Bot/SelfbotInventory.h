#ifndef PLAYERBOT_SELFBOT_INVENTORY_H
#define PLAYERBOT_SELFBOT_INVENTORY_H

#include "Define.h"

class PlayerbotAI;
bool IsSelfbotInventoryManaged(PlayerbotAI* ai);
bool UpdateSelfbotInventory(PlayerbotAI* ai, uint32 elapsed);
bool SelfbotInventoryPauseRequested(PlayerbotAI* ai);
void BeginSelfbotCraftOutput(PlayerbotAI* ai, uint32 item);
void RecordSelfbotCraftOutput(PlayerbotAI* ai, uint32 item);

#endif
