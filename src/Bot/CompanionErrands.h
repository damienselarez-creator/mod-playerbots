#ifndef PLAYERBOT_COMPANION_ERRANDS_H
#define PLAYERBOT_COMPANION_ERRANDS_H

#include "MovementActions.h"
#include "ObjectGuid.h"

struct CompanionErrandState;
class Item;

bool IsCompanionInventoryManaged(PlayerbotAI* ai);
bool CanSellCompanionItem(PlayerbotAI* ai, Item* item);

bool IsManagedCompanion(PlayerbotAI* ai);
void UpdateCompanionErrands(PlayerbotAI* ai, uint32 elapsed);
void CancelCompanionErrands(PlayerbotAI* ai);
bool IsCompanionGatherLoot(PlayerbotAI* ai, ObjectGuid guid);
void FinishCompanionGather(PlayerbotAI* ai, ObjectGuid guid);

class CompanionErrandAction : public MovementAction
{
public:
    explicit CompanionErrandAction(PlayerbotAI* ai) : MovementAction(ai, "companion errands") { }
    bool isUseful() override;
    bool Execute(Event event) override;
};
#endif
