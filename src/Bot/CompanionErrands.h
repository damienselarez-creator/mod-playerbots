#ifndef PLAYERBOT_COMPANION_ERRANDS_H
#define PLAYERBOT_COMPANION_ERRANDS_H

#include "MovementActions.h"
#include "ObjectGuid.h"
#include <string>

struct CompanionTrainingSnapshot
{
    std::string status;
    uint32 trainer = 0;
    uint32 spell = 0;
    uint32 learned = 0;
    uint32 money = 0;
    uint32 ageSeconds = 0;
};
CompanionTrainingSnapshot GetCompanionTrainingSnapshot(ObjectGuid guid);

struct CompanionErrandState;
class Item;

bool IsCompanionInventoryManaged(PlayerbotAI* ai);
bool CanSellCompanionItem(PlayerbotAI* ai, Item* item);

bool IsManagedCompanion(PlayerbotAI* ai);
void UpdateCompanionErrands(PlayerbotAI* ai, uint32 elapsed);
void CancelCompanionErrands(PlayerbotAI* ai);
void RequestCompanionTraining(PlayerbotAI* ai);
void SetCompanionNarrativeFocus(ObjectGuid guid, std::string const& focus);
std::string GetCompanionNarrativeFocus(ObjectGuid guid);
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
