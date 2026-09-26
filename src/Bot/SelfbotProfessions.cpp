#include "SelfbotInventory.h"
#include "SelfbotProfessions.h"

#include "EventMap.h"
#include "LootObjectStack.h"
#include "MotionMaster.h"
#include "NewRpgBaseAction.h"
#include "Playerbots.h"
#include "PlayerbotSpellRepository.h"
#include "ProfessionPlan.h"
#include "Trainer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace
{
    constexpr float SearchRadius = 6000.0f;
    enum ProfessionEvent : uint32 { Decision = 1, SourceExpired, MissingSpawn, RetryTraining };
    enum class SourceKind : uint8 { Vendor, Creature, Skinning, Gathering, Trainer, Focus };

    struct Source
    {
        SourceKind kind = SourceKind::Vendor;
        uint32 entry = 0;
        uint32 spawn = 0;
        WorldPosition position;
        uint32 spell = 0;
        double cost = 0;

        uint64 Key() const { return (uint64(kind) << 56) | spawn; }
    };

    bool IsProfession(uint32 skill)
    {
        return IsSelfbotPrimaryProfession(skill) || skill == SKILL_COOKING ||
               skill == SKILL_FIRST_AID || skill == SKILL_FISHING;
    }

    bool FriendlyTemplate(Player* bot, CreatureTemplate const* creature)
    {
        auto const* faction = sFactionTemplateStore.LookupEntry(creature->faction);
        return faction && !bot->GetFactionTemplateEntry()->IsHostileTo(*faction);
    }

    uint32 SpellProfession(uint32 spellId, uint32 depth = 0)
    {
        if (depth > 3)
            return 0;
        auto const* skill = PlayerbotSpellRepository::Instance().GetSkillLine(spellId);
        if (skill && IsProfession(skill->SkillLine))
            return skill->SkillLine;
        auto const* spell = sSpellMgr->GetSpellInfo(spellId);
        if (!spell)
            return 0;
        for (auto const& effect : spell->Effects)
        {
            if (effect.Effect == SPELL_EFFECT_SKILL && IsProfession(effect.MiscValue))
                return effect.MiscValue;
            if (effect.Effect == SPELL_EFFECT_LEARN_SPELL)
                if (uint32 result = SpellProfession(effect.TriggerSpell, depth + 1))
                    return result;
        }
        return 0;
    }

    void SetObjective(PlayerbotAI* ai, std::string const& text)
    {
        if (ai->selfbotCycle.objective == text)
            return;
        ai->selfbotCycle.objective = text;
        ChatHandler(ai->GetBot()->GetSession()).SendSysMessage(("[Metiers] " + text).c_str());
        LOG_INFO("playerbots", "[SelfbotProfessions] {}: {}", ai->GetBot()->GetName(), text);
    }
}

struct SelfbotProfessionState
{
    EventMap events;
    bool ready = true;
    bool trainingDue = true;
    bool hasSource = false;
    bool awaitingCraft = false;
    uint32 outputBefore = 0;
    uint32 skillBefore = 0;
    uint32 moneyFloor = 0;
    uint32 skill = 0;
    uint32 baseline = 0;
    uint32 obtained = 0;
    uint32 castFailures = 0;
    uint32 rootSpell = 0;
    uint32 map = 0;
    Source source;
    std::vector<SelfbotCraft::Step> steps;
    std::size_t step = 0;
    std::unordered_set<uint64> rejectedSources;
    std::unordered_set<uint32> rejectedRecipes;
    std::unordered_set<uint32> materials;
    std::unordered_map<uint32, std::vector<Source>> sourceCache;
    std::unordered_map<uint32, uint32> toolCache;
    std::vector<uint32> nearbyCreatures;
    std::vector<uint32> nearbyObjects;

    explicit SelfbotProfessionState(Player* player)
        : moneyFloor(player->GetMoney() / 5), map(player->GetMapId()) { IndexSources(player); }

    void IndexSources(Player* player)
    {
        nearbyCreatures.clear();
        nearbyObjects.clear();
        for (auto const& pair : sObjectMgr->GetAllCreatureData())
        {
            auto const& data = pair.second;
            if (data.mapid == player->GetMapId() &&
                player->GetDistance(data.posX, data.posY, data.posZ) < SearchRadius)
                nearbyCreatures.push_back(uint32(pair.first));
        }
        for (auto const& pair : sObjectMgr->GetAllGOData())
        {
            auto const& data = pair.second;
            if (data.mapid == player->GetMapId() &&
                player->GetDistance(data.posX, data.posY, data.posZ) < SearchRadius)
                nearbyObjects.push_back(uint32(pair.first));
        }
    }
};

namespace
{
    class ProfessionAction : public NewRpgBaseAction
    {
    public:
        explicit ProfessionAction(PlayerbotAI* ai) : NewRpgBaseAction(ai, "selfbot professions"),
            state(*ai->selfbotCycle.professions) { }

        bool Execute(Event) override
        {
            if (!state.ready)
                return false;
            state.ready = false;
            state.events.ScheduleEvent(Decision, Milliseconds(1000));
            if (!bot->IsAlive() || bot->IsInCombat() || bot->IsNonMeleeSpellCast(false) ||
                bot->IsInFlight() || bot->GetLootGUID())
                return false;
            if (bot->GetMap()->IsDungeon() || bot->InBattleground())
            {
                SetObjective(botAI, "Metiers suspendus : rejoignez une zone exterieure.");
                return false;
            }
            if (state.map != bot->GetMapId())
            {
                ResetPlan();
                state.sourceCache.clear();
                state.toolCache.clear();
                state.rejectedSources.clear();
                state.map = bot->GetMapId();
                state.IndexSources(bot);
            }

            if (state.hasSource && state.source.kind == SourceKind::Trainer)
                return VisitTrainer();
            if (state.steps.empty() && state.trainingDue)
            {
                state.trainingDue = false;
                state.events.RescheduleEvent(RetryTraining, Milliseconds(60000));
                if (FindTrainer())
                    return VisitTrainer();
            }
            if (botAI->selfbotCycle.crafts >= 200)
            {
                SetObjective(botAI, "Limite de 200 fabrications de test atteinte; plage suivante.");
                botAI->selfbotCycle.remaining = 0;
                return true;
            }
            if (state.steps.empty())
            {
                if (state.hasSource && state.source.kind == SourceKind::Gathering)
                    return WorkSource();
                if (!MakePlan())
                {
                    if (FindGatheringPractice())
                        return WorkSource();
                    SetObjective(botAI, "Aucune progression accessible : metier, budget, recette ou source manquante.");
                    botAI->selfbotCycle.remaining = 0;
                    return true;
                }
            }
            if (state.step >= state.steps.size())
            {
                ResetPlan();
                state.trainingDue = true;
                return true;
            }
            auto const& next = state.steps[state.step];
            if (next.spell)
                return Craft(next);

            uint32 count = bot->GetItemCount(next.item, false);
            if (count >= state.baseline + next.quantity)
            {
                Advance();
                return true;
            }
            if (count > state.obtained)
            {
                state.obtained = count;
                state.events.RescheduleEvent(SourceExpired, Milliseconds(900000));
                botAI->selfbotCycle.stalled = 0;
            }
            if (!state.hasSource && !SelectSource(next.item))
            {
                RejectRecipe("Plus de source accessible pour le composant " + std::to_string(next.item));
                return true;
            }
            return WorkSource();
        }

    private:
        SelfbotProfessionState& state;

        bool WantedProfession(uint32 id) const
        {
            return id && IsProfession(id) && (bot->HasSkill(id) ||
                id == botAI->selfbotCycle.profession1 || id == botAI->selfbotCycle.profession2);
        }

        void ResetPlan()
        {
            state.steps.clear();
            state.step = 0;
            state.hasSource = false;
            state.awaitingCraft = false;
            state.castFailures = 0;
            state.materials.clear();
            state.events.CancelEvent(SourceExpired);
            state.events.CancelEvent(MissingSpawn);
            bot->StopMoving();
            botAI->rpgInfo.ChangeToRest();
        }

        void Advance()
        {
            ++state.step;
            state.hasSource = false;
            state.awaitingCraft = false;
            state.castFailures = 0;
            state.events.CancelEvent(SourceExpired);
            state.events.CancelEvent(MissingSpawn);
            if (state.step < state.steps.size())
            {
                state.baseline = bot->GetItemCount(state.steps[state.step].item, false);
                state.obtained = state.baseline;
            }
        }

        void RejectRecipe(std::string const& reason)
        {
            SetObjective(botAI, reason + "; recherche d'une autre recette.");
            state.rejectedRecipes.insert(state.rootSpell);
            ResetPlan();
        }

        void UseSource(Source const& source)
        {
            state.source = source;
            state.hasSource = true;
            state.events.RescheduleEvent(SourceExpired, Milliseconds(900000));
            state.events.CancelEvent(MissingSpawn);
            botAI->rpgInfo.ChangeToRest();
            bot->StopMoving();
        }

        bool Near(float map, float x, float y, float z) const
        {
            return uint32(map) == bot->GetMapId() && bot->GetDistance(x, y, z) < SearchRadius;
        }

        bool CanGather(GameObjectTemplate const* object) const
        {
            if (!object || object->type != GAMEOBJECT_TYPE_CHEST)
                return false;
            auto const* lock = sLockStore.LookupEntry(object->GetLockId());
            if (!lock)
                return false;
            for (uint32 i = 0; i < 8; ++i)
            {
                if (lock->Type[i] != LOCK_KEY_SKILL)
                    continue;
                uint32 id = SkillByLockType(LockType(lock->Index[i]));
                if ((id == SKILL_MINING || id == SKILL_HERBALISM) && bot->HasSkill(id) &&
                    bot->GetSkillValue(id) >= lock->Skill[i])
                    return true;
            }
            return false;
        }

        std::vector<Source>& Sources(uint32 item)
        {
            auto cached = state.sourceCache.find(item);
            if (cached != state.sourceCache.end())
                return cached->second;
            std::vector<Source> found;
            std::unordered_map<uint32, uint8> creatureKinds;
            for (uint32 spawn : state.nearbyCreatures)
            {
                auto const* stored = sObjectMgr->GetCreatureData(spawn);
                if (!stored)
                    continue;
                auto const& data = *stored;
                if (!Near(data.mapid, data.posX, data.posY, data.posZ) || !(data.phaseMask & bot->GetPhaseMask()))
                    continue;
                auto const* creature = sObjectMgr->GetCreatureTemplate(data.id);
                if (!creature)
                    continue;
                uint8 kinds = 0;
                auto known = creatureKinds.find(data.id);
                if (known != creatureKinds.end())
                    kinds = known->second;
                else
                {
                    auto const* vendor = sObjectMgr->GetNpcVendorItemList(data.id);
                    if (vendor && FriendlyTemplate(bot, creature))
                        for (auto const* offer : vendor->m_items)
                            if (offer->item == item && !offer->ExtendedCost)
                                kinds |= 1;
                    if (creature->rank == CREATURE_ELITE_NORMAL && creature->maxlevel <= bot->GetLevel() + 1)
                    {
                        auto const* faction = sFactionTemplateStore.LookupEntry(creature->faction);
                        if (faction && !bot->GetFactionTemplateEntry()->IsFriendlyTo(*faction))
                        {
                            auto const* loot = LootTemplates_Creature.GetLootFor(creature->lootid);
                            if (loot && loot->HasNonQuestItem(item))
                                kinds |= 2;
                            uint32 required = creature->GetRequiredLootSkill();
                            uint32 rank = creature->maxlevel < 10 ? 0 :
                                (creature->maxlevel < 20 ? (creature->maxlevel - 10) * 10 : creature->maxlevel * 5);
                            loot = LootTemplates_Skinning.GetLootFor(creature->SkinLootId);
                            if (bot->HasSkill(required) && bot->GetSkillValue(required) >= rank &&
                                loot && loot->HasNonQuestItem(item))
                                kinds |= 4;
                        }
                    }
                    creatureKinds[data.id] = kinds;
                }
                if (!kinds)
                    continue;
                WorldPosition position(data.mapid, data.posX, data.posY, data.posZ);
                double distance = bot->GetDistance(position);
                if (kinds & 1)
                    found.push_back({SourceKind::Vendor, data.id, spawn, position, 0, 1 + distance / 100});
                if (kinds & 2)
                    found.push_back({SourceKind::Creature, data.id, spawn, position, 0, 10 + distance / 100});
                if (kinds & 4)
                    found.push_back({SourceKind::Skinning, data.id, spawn, position, 0, 12 + distance / 100});
            }
            std::unordered_map<uint32, bool> gatherEntries;
            for (uint32 spawn : state.nearbyObjects)
            {
                auto const* stored = sObjectMgr->GetGameObjectData(spawn);
                if (!stored)
                    continue;
                auto const& data = *stored;
                if (!Near(data.mapid, data.posX, data.posY, data.posZ) || !(data.phaseMask & bot->GetPhaseMask()))
                    continue;
                auto known = gatherEntries.find(data.id);
                bool useful = false;
                if (known != gatherEntries.end())
                    useful = known->second;
                else
                {
                    auto const* object = sObjectMgr->GetGameObjectTemplate(data.id);
                    auto const* loot = object ? LootTemplates_Gameobject.GetLootFor(object->GetLootId()) : nullptr;
                    useful = CanGather(object) && loot && loot->HasNonQuestItem(item);
                    gatherEntries[data.id] = useful;
                }
                if (useful)
                {
                    WorldPosition position(data.mapid, data.posX, data.posY, data.posZ);
                    found.push_back({SourceKind::Gathering, data.id, spawn, position, 0,
                                     5 + bot->GetDistance(position) / 100});
                }
            }
            std::sort(found.begin(), found.end(), [](Source const& a, Source const& b) { return a.cost < b.cost; });
            return state.sourceCache.emplace(item, std::move(found)).first->second;
        }

        double SourceCost(uint32 item)
        {
            for (auto const& source : Sources(item))
                if (!state.rejectedSources.count(source.Key()))
                    return source.cost;
            return -1;
        }

        bool SelectSource(uint32 item)
        {
            for (auto const& source : Sources(item))
            {
                if (state.rejectedSources.count(source.Key()))
                    continue;
                UseSource(source);
                SetObjective(botAI, "Composant " + std::to_string(item) + ", source " +
                             std::to_string(source.entry) + " a " +
                             std::to_string(uint32(bot->GetDistance(source.position))) + " m.");
                return true;
            }
            return false;
        }

        bool FindTrainer()
        {
            double best = std::numeric_limits<double>::max();
            Source selected;
            bool found = false;
            for (uint32 spawn : state.nearbyCreatures)
            {
                auto const* stored = sObjectMgr->GetCreatureData(spawn);
                if (!stored)
                    continue;
                auto const& data = *stored;
                if (!Near(data.mapid, data.posX, data.posY, data.posZ) || !(data.phaseMask & bot->GetPhaseMask()))
                    continue;
                auto const* creature = sObjectMgr->GetCreatureTemplate(data.id);
                auto* trainer = sObjectMgr->GetTrainer(data.id);
                if (!creature || !trainer || trainer->GetTrainerType() != Trainer::Type::Tradeskill ||
                    !FriendlyTemplate(bot, creature) || !trainer->IsTrainerValidForPlayer(bot))
                    continue;
                Source source{SourceKind::Trainer, data.id, spawn,
                    WorldPosition(data.mapid, data.posX, data.posY, data.posZ)};
                if (state.rejectedSources.count(source.Key()))
                    continue;
                for (auto const& spell : trainer->GetSpells())
                {
                    uint32 profession = IsProfession(spell.ReqSkillLine) ? spell.ReqSkillLine :
                                        SpellProfession(spell.SpellId);
                    if (!WantedProfession(profession) || !trainer->CanTeachSpell(bot, &spell) ||
                        uint64(spell.MoneyCost) + state.moneyFloor > bot->GetMoney())
                        continue;
                    double score = bot->GetDistance(source.position) + spell.MoneyCost / 100.0;
                    if (!bot->HasSkill(profession) ||
                        bot->GetSkillValue(profession) == bot->GetMaxSkillValue(profession))
                        score -= SearchRadius;
                    if (score < best)
                    {
                        best = score;
                        selected = source;
                        selected.spell = spell.SpellId;
                        found = true;
                    }
                }
            }
            if (found)
            {
                UseSource(selected);
                SetObjective(botAI, "Apprentissage " + std::to_string(selected.spell) + " : maitre " +
                             std::to_string(selected.entry));
            }
            return found;
        }

        bool VisitTrainer()
        {
            auto const& source = state.source;
            Creature* npc = ObjectAccessor::GetSpawnedCreatureByDBGUID(bot->GetMapId(), source.spawn);
            if (!npc || !bot->GetNPCIfCanInteractWith(npc->GetGUID(), UNIT_NPC_FLAG_TRAINER))
                return Approach(npc);
            auto* trainer = sObjectMgr->GetTrainer(npc->GetEntry());
            auto const* lesson = trainer ? trainer->GetSpell(source.spell) : nullptr;
            if (lesson && trainer->CanTeachSpell(bot, lesson) &&
                uint64(lesson->MoneyCost) + state.moneyFloor <= bot->GetMoney())
            {
                bot->StopMoving();
                trainer->TeachSpell(npc, bot, source.spell);
                SetObjective(botAI, "Apprentissage demande au maitre : " + std::to_string(source.spell));
                state.sourceCache.clear();
                state.rejectedRecipes.clear();
                state.trainingDue = true;
            }
            state.hasSource = false;
            state.events.CancelEvent(SourceExpired);
            return true;
        }

        bool MakePlan()
        {
            std::vector<SelfbotCraft::Recipe> recipes;
            std::vector<uint32> roots;
            std::map<uint32, uint32> inventory;
            for (auto const& entry : bot->GetSpellMap())
            {
                if (entry.second->State == PLAYERSPELL_REMOVED || !entry.second->Active)
                    continue;
                auto const* skill = PlayerbotSpellRepository::Instance().GetSkillLine(entry.first);
                auto const* spell = sSpellMgr->GetSpellInfo(entry.first);
                if (!skill || !spell || !IsProfession(skill->SkillLine) || !bot->HasSkill(skill->SkillLine))
                    continue;
                SelfbotCraft::Recipe recipe;
                recipe.spell = entry.first;
                for (auto const& effect : spell->Effects)
                    if (effect.Effect == SPELL_EFFECT_CREATE_ITEM && effect.ItemType)
                    {
                        recipe.item = effect.ItemType;
                        recipe.output = std::max(1, effect.CalcValue(bot));
                        break;
                    }
                if (!recipe.item)
                    continue;
                for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
                    if (spell->Reagent[i] > 0 && spell->ReagentCount[i])
                    {
                        recipe.reagents[spell->Reagent[i]] = spell->ReagentCount[i];
                        inventory[spell->Reagent[i]] = bot->GetItemCount(spell->Reagent[i], false);
                    }
                if (recipe.reagents.empty())
                    continue;
                for (uint32 tool : spell->Totem)
                    if (tool)
                    {
                        recipe.tools[tool] = 1;
                        inventory[tool] = bot->GetItemCount(tool, false);
                    }
                bool toolsAvailable = true;
                for (uint32 category : spell->TotemCategory)
                {
                    if (!category || bot->HasItemTotemCategory(category))
                        continue;
                    uint32 tool = FindTool(category);
                    if (!tool)
                    {
                        toolsAvailable = false;
                        break;
                    }
                    recipe.tools[tool] = 1;
                    inventory[tool] = bot->GetItemCount(tool, false);
                }
                if (!toolsAvailable)
                    continue;
                inventory[recipe.item] = bot->GetItemCount(recipe.item, false);
                if (bot->GetSkillValue(skill->SkillLine) < bot->GetMaxSkillValue(skill->SkillLine) &&
                    bot->GetSkillValue(skill->SkillLine) < skill->TrivialSkillLineRankHigh &&
                    !state.rejectedRecipes.count(recipe.spell))
                    roots.push_back(uint32(recipes.size()));
                recipes.push_back(std::move(recipe));
            }
            SelfbotCraft::Planner planner(recipes, [this](uint32 item) { return SourceCost(item); });
            SelfbotCraft::Plan best;
            uint32 chosen = 0;
            // Prefer recipes requiring fewer different reagents; bound discovery work per decision.
            std::sort(roots.begin(), roots.end(), [&recipes](uint32 a, uint32 b)
            {
                return recipes[a].reagents.size() < recipes[b].reagents.size();
            });
            uint32 tested = 0;
            for (uint32 index : roots)
            {
                if (++tested > 24)
                    break;
                auto plan = planner.Build(recipes[index], inventory);
                if (plan.valid && (!best.valid || plan.cost < best.cost))
                {
                    best = std::move(plan);
                    chosen = recipes[index].spell;
                }
            }
            if (!best.valid || best.steps.empty())
                return false;
            state.steps = std::move(best.steps);
            state.rootSpell = chosen;
            state.step = 0;
            state.baseline = bot->GetItemCount(state.steps[0].item, false);
            state.obtained = state.baseline;
            state.materials.clear();
            for (auto const& step : state.steps)
                state.materials.insert(step.item);
            SetObjective(botAI, "Plan de fabrication " + std::to_string(chosen) + " : " +
                         std::to_string(state.steps.size()) + " etapes (composants inclus).");
            return true;
        }

        uint32 FindTool(uint32 category)
        {
            auto known = state.toolCache.find(category);
            if (known != state.toolCache.end())
                return known->second;
            double best = std::numeric_limits<double>::max();
            uint32 selected = 0;
            for (auto const& pair : *sObjectMgr->GetItemTemplateStore())
            {
                auto const& item = pair.second;
                if (!bot->IsTotemCategoryCompatiableWith(&item, category))
                    continue;
                double cost = SourceCost(item.ItemId);
                if (cost >= 0 && cost < best)
                {
                    best = cost;
                    selected = item.ItemId;
                }
            }
            state.toolCache[category] = selected;
            return selected;
        }

        bool Approach(WorldObject* object)
        {
            if (object && object->GetPhaseMask() & bot->GetPhaseMask())
            {
                state.events.CancelEvent(MissingSpawn);
                return MoveWorldObjectTo(object->GetGUID(), INTERACTION_DISTANCE - 1);
            }
            if (bot->GetDistance(state.source.position) < 15.0f)
            {
                if (state.events.GetTimeUntilEvent(MissingSpawn).count() == 0)
                    state.events.ScheduleEvent(MissingSpawn, Milliseconds(30000));
                return false;
            }
            return MoveFarTo(state.source.position);
        }

        bool WorkSource()
        {
            if (!state.hasSource)
                return false;
            auto const& source = state.source;
            if (source.kind == SourceKind::Gathering)
            {
                GameObject* object = ObjectAccessor::GetSpawnedGameObjectByDBGUID(bot->GetMapId(), source.spawn);
                if (!object || !object->isSpawned())
                    return Approach(nullptr);
                if (bot->GetDistance(object) > INTERACTION_DISTANCE)
                    return Approach(object);
                LootObject loot(bot, object->GetGUID());
                if (loot.IsLootPossible(bot))
                {
                    context->GetValue<LootObjectStack*>("available loot")->Get()->Add(object->GetGUID());
                    return false; // Let the ordinary loot strategy perform the interaction.
                }
            }
            else
            {
                Creature* creature = ObjectAccessor::GetSpawnedCreatureByDBGUID(bot->GetMapId(), source.spawn);
                if (!creature)
                    return Approach(nullptr);
                if (source.kind == SourceKind::Vendor)
                {
                    if (!bot->GetNPCIfCanInteractWith(creature->GetGUID(), UNIT_NPC_FLAG_VENDOR))
                        return Approach(creature);
                    uint32 item = state.steps[state.step].item;
                    auto const* goods = creature->GetVendorItems();
                    auto const* proto = sObjectMgr->GetItemTemplate(item);
                    if (goods && proto)
                        for (uint32 slot = 0; slot < goods->GetItemCount(); ++slot)
                        {
                            auto const* offer = goods->GetItem(slot);
                            if (offer->item != item || offer->ExtendedCost)
                                continue;
                            uint32 price = uint32(std::floor(
                                proto->BuyPrice * bot->GetReputationPriceDiscount(creature)));
                            if (uint64(price) + state.moneyFloor > bot->GetMoney())
                                break;
                            bot->StopMoving();
                            if (bot->BuyItemFromVendorSlot(creature->GetGUID(), slot, item, 1, NULL_BAG, NULL_SLOT))
                                return true;
                            break;
                        }
                }
                else if (creature->IsAlive())
                {
                    if (!bot->IsValidAttackTarget(creature) || creature->GetLevel() > bot->GetLevel() + 1)
                        return RejectSource();
                    if (bot->GetDistance(creature) > 25.0f || !bot->IsWithinLOSInMap(creature))
                        return Approach(creature);
                    bot->SetSelection(creature->GetGUID());
                    return botAI->DoSpecificAction("attack my target", Event(), true);
                }
                else
                {
                    LootObject loot(bot, creature->GetGUID());
                    if (loot.IsLootPossible(bot))
                    {
                        context->GetValue<LootObjectStack*>("available loot")->Get()->Add(creature->GetGUID());
                        return false;
                    }
                }
            }
            return RejectSource();
        }

        bool RejectSource()
        {
            state.rejectedSources.insert(state.source.Key());
            state.hasSource = false;
            state.events.CancelEvent(SourceExpired);
            state.events.CancelEvent(MissingSpawn);
            bot->StopMoving();
            return true;
        }

        bool FindFocus(uint32 focus)
        {
            float best = SearchRadius;
            Source selected;
            bool found = false;
            for (uint32 spawn : state.nearbyObjects)
            {
                auto const* stored = sObjectMgr->GetGameObjectData(spawn);
                if (!stored)
                    continue;
                auto const& data = *stored;
                if (!Near(data.mapid, data.posX, data.posY, data.posZ) || !(data.phaseMask & bot->GetPhaseMask()))
                    continue;
                auto const* object = sObjectMgr->GetGameObjectTemplate(data.id);
                if (!object || object->type != GAMEOBJECT_TYPE_SPELL_FOCUS || object->spellFocus.focusId != focus)
                    continue;
                Source source{SourceKind::Focus, data.id, spawn,
                    WorldPosition(data.mapid, data.posX, data.posY, data.posZ)};
                float distance = bot->GetDistance(source.position);
                if (distance < best && !state.rejectedSources.count(source.Key()))
                {
                    selected = source;
                    best = distance;
                    found = true;
                }
            }
            if (found)
                UseSource(selected);
            return found;
        }

        bool Craft(SelfbotCraft::Step const& next)
        {
            auto const* spell = sSpellMgr->GetSpellInfo(next.spell);
            if (!spell || !bot->HasSpell(next.spell))
            {
                RejectRecipe("Recette devenue indisponible");
                return true;
            }
            if (state.awaitingCraft)
            {
                state.awaitingCraft = false;
                if (bot->GetItemCount(next.item, false) > state.outputBefore)
                {
                    RecordSelfbotCraftOutput(botAI, next.item);
                    ++botAI->selfbotCycle.crafts;
                    auto& completed = state.steps[state.step];
                    if (--completed.quantity == 0)
                        Advance();
                    state.castFailures = 0;
                    return true;
                }
                ++state.castFailures;
            }
            if (state.hasSource && state.source.kind == SourceKind::Focus &&
                bot->GetDistance(state.source.position) > INTERACTION_DISTANCE - 1)
            {
                SetObjective(botAI, "Trajet vers l'atelier " + std::to_string(state.source.entry));
                return MoveFarTo(state.source.position);
            }
            bot->StopMoving();
            bot->SetStandState(UNIT_STAND_STATE_STAND);
            if (bot->IsMounted())
            {
                botAI->DoSpecificAction("dismount", Event(), true);
                return true;
            }
            if (!botAI->CanCastSpell(next.spell, bot))
            {
                if (spell->RequiresSpellFocus && !state.hasSource && FindFocus(spell->RequiresSpellFocus))
                    return true;
                if (++state.castFailures >= 5)
                    RejectRecipe("Fabrication impossible (atelier, outil, place ou precondition)");
                return false;
            }
            BeginSelfbotCraftOutput(botAI, next.item);
            state.outputBefore = bot->GetItemCount(next.item, false);
            if (botAI->CastSpell(next.spell, bot))
            {
                state.awaitingCraft = true;
                SetObjective(botAI, "Fabrication de " + std::to_string(next.item) + " (sort " +
                             std::to_string(next.spell) + ")");
                return true;
            }
            if (++state.castFailures >= 5)
                RejectRecipe("Echecs repetes de fabrication");
            return false;
        }

        bool FindGatheringPractice()
        {
            if (state.hasSource)
                return true;
            float best = SearchRadius;
            Source selected;
            bool found = false;
            for (uint32 spawn : state.nearbyObjects)
            {
                auto const* stored = sObjectMgr->GetGameObjectData(spawn);
                if (!stored)
                    continue;
                auto const& data = *stored;
                if (!Near(data.mapid, data.posX, data.posY, data.posZ) || !(data.phaseMask & bot->GetPhaseMask()))
                    continue;
                auto const* object = sObjectMgr->GetGameObjectTemplate(data.id);
                if (!CanGather(object))
                    continue;
                Source source{SourceKind::Gathering, data.id, spawn,
                    WorldPosition(data.mapid, data.posX, data.posY, data.posZ)};
                float distance = bot->GetDistance(source.position);
                if (distance < best && !state.rejectedSources.count(source.Key()))
                {
                    selected = source;
                    best = distance;
                    found = true;
                }
            }
            if (found)
            {
                UseSource(selected);
                SetObjective(botAI, "Entrainement par recolte : ressource " + std::to_string(selected.entry));
            }
            return found;
        }
    };
}

bool IsSelfbotPrimaryProfession(uint32 skill)
{
    switch (skill)
    {
        case SKILL_ALCHEMY:
        case SKILL_BLACKSMITHING:
        case SKILL_ENCHANTING:
        case SKILL_ENGINEERING:
        case SKILL_HERBALISM:
        case SKILL_INSCRIPTION:
        case SKILL_JEWELCRAFTING:
        case SKILL_LEATHERWORKING:
        case SKILL_MINING:
        case SKILL_SKINNING:
        case SKILL_TAILORING:
            return true;
        default:
            return false;
    }
}

void UpdateSelfbotProfessions(PlayerbotAI* ai, uint32 elapsed)
{
    if (ai->selfbotCycle.phase != 1)
        return;
    auto& stored = ai->selfbotCycle.professions;
    if (!stored)
        stored = std::make_shared<SelfbotProfessionState>(ai->GetBot());
    auto& state = *stored;
    state.events.Update(elapsed);
    while (uint32 event = state.events.ExecuteEvent())
    {
        if (event == Decision)
            state.ready = true;
        else if (event == RetryTraining)
            state.trainingDue = true;
        else if ((event == SourceExpired || event == MissingSpawn) && state.hasSource)
        {
            state.rejectedSources.insert(state.source.Key());
            state.hasSource = false;
            state.events.CancelEvent(SourceExpired);
            state.events.CancelEvent(MissingSpawn);
            ai->GetBot()->StopMoving();
            SetObjective(ai, "Source inaccessible ou sans progres; recherche d'une autre source.");
        }
    }
}

bool ExecuteSelfbotProfessions(PlayerbotAI* ai)
{
    if (!ai->selfbotCycle.professions)
        UpdateSelfbotProfessions(ai, 0);
    return ProfessionAction(ai).Execute(Event());
}

bool IsSelfbotProfessionMaterial(PlayerbotAI* ai, uint32 item)
{
    return ai && IsSelfBot(ai->GetBot()) && ai->selfbotCycle.enabled && ai->selfbotCycle.phase == 1 &&
        ai->selfbotCycle.professions && ai->selfbotCycle.professions->materials.count(item);
}
