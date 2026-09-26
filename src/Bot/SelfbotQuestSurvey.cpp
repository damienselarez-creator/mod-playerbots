#include "SelfbotQuestSurvey.h"

#include "Chat.h"
#include "EventMap.h"
#include "MotionMaster.h"
#include "NewRpgBaseAction.h"
#include "Playerbots.h"
#include "QuestSurveyProgress.h"

#include <set>

namespace
{
    enum SurveyEvent : uint32 { Decision = 1, NoProgress, TripLimit };

    uint64 Stamp(Player* player)
    {
        uint64 stamp = player->GetLevel();
        stamp = stamp * 1099511628211ULL + player->GetPhaseMask();
        stamp = stamp * 1099511628211ULL + player->GetRewardedQuestCount();
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 quest = player->GetQuestSlotQuestId(slot);
            if (quest && player->GetQuestStatus(quest) == QUEST_STATUS_COMPLETE)
                stamp = stamp * 1099511628211ULL + quest;
        }
        return stamp;
    }

    void Message(PlayerbotAI* ai, std::string const& text)
    {
        ai->selfbotCycle.objective = text;
        ChatHandler(ai->GetBot()->GetSession()).SendSysMessage(("[Exploration] " + text).c_str());
        LOG_INFO("playerbots", "[SelfbotQuestSurvey] {}: {}", ai->GetBot()->GetName(), text);
    }
}

struct SelfbotQuestSurveyState
{
    uint32 map = 0, zone = 0, phaseMask = 0;
    std::vector<SelfbotSurvey::Node> nodes;
    EventMap events;
    bool ready = true, travelling = false, keepSector = false, timeout = false;
    std::pair<int32, int32> sector;
    SelfbotSurvey::Node target;
    float bestDistance = 0;
};

namespace
{
    class SurveyAction : public NewRpgBaseAction
    {
    public:
        explicit SurveyAction(PlayerbotAI* ai) : NewRpgBaseAction(ai, "selfbot quest survey") { }

        void Prepare()
        {
            auto& stored = botAI->selfbotCycle.surveyRuntime;
            if (stored && stored->map == bot->GetMapId() && stored->phaseMask == bot->GetPhaseMask() &&
                (stored->travelling || stored->zone == bot->GetZoneId()))
                return;
            stored = std::make_shared<SelfbotQuestSurveyState>();
            auto& state = *stored;
            state.map = bot->GetMapId();
            state.zone = bot->GetZoneId();
            state.phaseMask = bot->GetPhaseMask();
            if (bot->GetMap()->IsDungeon() || bot->InBattleground())
                return;
            for (auto const& [spawn, data] : sObjectMgr->GetAllCreatureData())
            {
                if (data.mapid != state.map || !(data.phaseMask & state.phaseMask))
                    continue;
                auto const* proto = sObjectMgr->GetCreatureTemplate(data.id);
                if (!proto || !((data.npcflag ? data.npcflag : proto->npcflag) & UNIT_NPC_FLAG_QUESTGIVER))
                    continue;
                auto const* faction = sFactionTemplateStore.LookupEntry(proto->faction);
                if (!faction || bot->GetFactionTemplateEntry()->IsHostileTo(*faction))
                    continue;
                if (bot->GetMap()->GetZoneId(state.phaseMask, data.posX, data.posY, data.posZ) != state.zone)
                    continue;
                state.nodes.push_back({uint64(spawn), data.id, data.posX, data.posY, data.posZ});
            }
            for (auto const& [spawn, data] : sObjectMgr->GetAllGOData())
            {
                if (data.mapid != state.map || !(data.phaseMask & state.phaseMask))
                    continue;
                auto const* proto = sObjectMgr->GetGameObjectTemplate(data.id);
                auto starters = sObjectMgr->GetGOQuestRelationBounds(data.id);
                auto enders = sObjectMgr->GetGOQuestInvolvedRelationBounds(data.id);
                if (!proto || (proto->type != GAMEOBJECT_TYPE_QUESTGIVER &&
                    starters.first == starters.second && enders.first == enders.second))
                    continue;
                if (bot->GetMap()->GetZoneId(state.phaseMask, data.posX, data.posY, data.posZ) != state.zone)
                    continue;
                state.nodes.push_back({(uint64(1) << 32) | spawn, data.id, data.posX, data.posY, data.posZ});
            }
        }

        bool Execute(Event) override
        {
            Prepare();
            auto& state = *botAI->selfbotCycle.surveyRuntime;
            auto& progress = botAI->selfbotCycle.surveyProgress;
            if (!state.ready)
                return true;
            state.ready = false;
            state.events.RescheduleEvent(Decision, Milliseconds(1000));
            if (state.timeout && state.travelling)
            {
                Finish(SelfbotSurvey::Outcome::Blocked, "Donneur inaccessible : reporte pour 30 minutes actives.");
                return true;
            }
            if (!state.travelling)
            {
                uint32 free = 0;
                for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
                    free += bot->GetQuestSlotQuestId(slot) == 0;
                std::vector<SelfbotSurvey::Node> candidates;
                for (auto const& node : state.nodes)
                    if (free || HasReward(node))
                        candidates.push_back(node);
                std::size_t selected = SelfbotSurvey::Select(candidates, progress, Stamp(bot),
                    bot->GetPositionX(), bot->GetPositionY(), state.keepSector, state.sector);
                if (selected == candidates.size())
                    return false;
                state.target = candidates[selected];
                auto const& node = state.target;
                state.sector = node.Sector();
                state.keepSector = true;
                state.travelling = true;
                state.timeout = false;
                state.bestDistance = bot->GetDistance(node.x, node.y, node.z);
                state.events.RescheduleEvent(NoProgress, Milliseconds(120000));
                state.events.RescheduleEvent(TripLimit, Milliseconds(600000));
                bot->GetMotionMaster()->Clear();
                bot->StopMoving();
                botAI->rpgInfo.ChangeToRest();
                Message(botAI, "Secteur " + std::to_string(state.sector.first) + "," +
                    std::to_string(state.sector.second) + " : visite du donneur " + std::to_string(node.entry));
            }
            auto const& node = state.target;
            WorldObject* object = node.key >> 32 ?
                static_cast<WorldObject*>(ObjectAccessor::GetSpawnedGameObjectByDBGUID(state.map, uint32(node.key))) :
                static_cast<WorldObject*>(ObjectAccessor::GetSpawnedCreatureByDBGUID(state.map, uint32(node.key)));
            if (object && object->IsInWorld() && object->GetPhaseMask() & bot->GetPhaseMask())
            {
                if (bot->CanInteractWithQuestGiver(object))
                {
                    uint32 before = QuestCount();
                    uint32 rewards = uint32(bot->GetRewardedQuestCount());
                    bool available = HasQuestToAcceptOrReward(object);
                    bool interacted = InteractWithNpcOrGameObjectForQuest(object->GetGUID());
                    if (!interacted)
                    {
                        Finish(SelfbotSurvey::Outcome::Blocked, "Interaction impossible : donneur reporte.");
                        return true;
                    }
                    bool changed = before != QuestCount() || rewards != bot->GetRewardedQuestCount();
                    Finish(changed ? SelfbotSurvey::Outcome::Visited :
                        (available ? SelfbotSurvey::Outcome::Blocked : SelfbotSurvey::Outcome::Empty),
                        changed ? "Donneur visite : journal de quetes mis a jour." :
                        (available ? "Interaction sans progression : donneur reporte." :
                                     "Donneur visite : aucune quete adaptee disponible."));
                    return true;
                }
                MoveWorldObjectTo(object->GetGUID());
            }
            else
                MoveFarTo(WorldPosition(state.map, node.x, node.y, node.z));
            float distance = object ? bot->GetDistance(object) : bot->GetDistance(node.x, node.y, node.z);
            if (distance + 10 < state.bestDistance)
            {
                state.bestDistance = distance;
                state.events.RescheduleEvent(NoProgress, Milliseconds(120000));
            }
            return true;
        }

    private:
        bool HasReward(SelfbotSurvey::Node const& node)
        {
            auto bounds = node.key >> 32 ? sObjectMgr->GetGOQuestInvolvedRelationBounds(node.entry) :
                sObjectMgr->GetCreatureQuestInvolvedRelationBounds(node.entry);
            for (auto it = bounds.first; it != bounds.second; ++it)
                if (bot->GetQuestStatus(it->second) == QUEST_STATUS_COMPLETE)
                    return true;
            return false;
        }

        uint32 QuestCount()
        {
            uint32 count = 0;
            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
                count += bot->GetQuestSlotQuestId(slot) != 0;
            return count;
        }

        void Finish(SelfbotSurvey::Outcome outcome, std::string const& message)
        {
            auto& state = *botAI->selfbotCycle.surveyRuntime;
            auto const& node = state.target;
            botAI->selfbotCycle.surveyProgress.Mark(node.key, state.map, state.zone, Stamp(bot), outcome);
            state.travelling = false;
            state.timeout = false;
            state.events.CancelEvent(NoProgress);
            state.events.CancelEvent(TripLimit);
            bot->GetMotionMaster()->Clear();
            bot->StopMoving();
            botAI->rpgInfo.ChangeToIdle();
            botAI->selfbotCycle.decisionElapsed = 15000;
            botAI->selfbotCycle.stalled = 0;
            Message(botAI, message);
            SaveSelfbotCycle(botAI);
        }
    };
}

void UpdateSelfbotQuestSurvey(PlayerbotAI* ai, uint32 elapsed)
{
    auto& cycle = ai->selfbotCycle;
    if (cycle.phase == 1 || !ai->GetBot()->IsAlive() || ai->GetBot()->IsInCombat() ||
        ai->GetBot()->IsNonMeleeSpellCast(false) || ai->GetBot()->IsInFlight())
        return;
    cycle.surveyProgress.clock += elapsed;
    auto& stored = cycle.surveyRuntime;
    if (!stored)
        return;
    if (stored->travelling && ai->rpgInfo.GetStatus() != RPG_REST)
    {
        stored->travelling = false;
        stored->events.CancelEvent(NoProgress);
        stored->events.CancelEvent(TripLimit);
    }
    stored->events.Update(elapsed);
    while (uint32 event = stored->events.ExecuteEvent())
        if (event == Decision)
            stored->ready = true;
        else if (stored->travelling)
        {
            stored->timeout = true;
            stored->ready = true;
        }
}

bool ExecuteSelfbotQuestSurvey(PlayerbotAI* ai)
{
    return SurveyAction(ai).Execute(Event());
}

bool ContinueSelfbotQuestSurvey(PlayerbotAI* ai)
{
    return ai->selfbotCycle.surveyRuntime && ai->selfbotCycle.surveyRuntime->travelling &&
        ai->rpgInfo.GetStatus() == RPG_REST && ExecuteSelfbotQuestSurvey(ai);
}

std::string SelfbotQuestSurveyStatus(PlayerbotAI* ai)
{
    SurveyAction(ai).Prepare();
    auto const& state = *ai->selfbotCycle.surveyRuntime;
    auto const& progress = ai->selfbotCycle.surveyProgress;
    std::map<std::pair<int32, int32>, uint32> sectors;
    uint32 pending = 0, visited = 0, blocked = 0;
    uint64 stamp = Stamp(ai->GetBot());
    for (auto const& node : state.nodes)
    {
        uint32& outstanding = sectors[node.Sector()];
        if (progress.Pending(node.key, stamp))
        {
            ++pending;
            ++outstanding;
        }
        else if (progress.visits.at(node.key).outcome == SelfbotSurvey::Outcome::Blocked)
        {
            ++blocked;
            ++outstanding;
        }
        else
            ++visited;
    }
    uint32 complete = 0;
    for (auto const& pair : sectors)
        complete += pair.second == 0;
    return "Zone " + std::to_string(state.zone) + ": secteurs verifies " + std::to_string(complete) + "/" +
        std::to_string(sectors.size()) + "; donneurs visites " + std::to_string(visited) +
        ", a visiter " + std::to_string(pending) + ", inaccessibles/reportes " + std::to_string(blocked);
}

void ResetSelfbotQuestSurvey(PlayerbotAI* ai)
{
    ai->selfbotCycle.surveyProgress.Reset(ai->GetBot()->GetMapId(), ai->GetBot()->GetZoneId());
    ai->selfbotCycle.surveyRuntime.reset();
    ai->GetBot()->GetMotionMaster()->Clear();
    ai->GetBot()->StopMoving();
    ai->rpgInfo.ChangeToIdle();
    SaveSelfbotCycle(ai);
}
