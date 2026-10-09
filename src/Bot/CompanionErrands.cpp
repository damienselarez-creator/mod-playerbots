#include "CompanionErrands.h"
#include "CompanionVocation.h"

#include "Bag.h"
#include "CellImpl.h"
#include "CompanionErrandPolicy.h"
#include "EventMap.h"
#include "InventoryPolicy.h"
#include "ItemPackets.h"
#include "ItemUsageValue.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "LootObjectStack.h"
#include "MotionMaster.h"
#include "Playerbots.h"
#include "PlayerbotSpellRepository.h"
#include "Trainer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <mutex>
#include <map>
#include <chrono>

namespace
{
    std::mutex narrativeFocusMutex;
    std::map<ObjectGuid, std::string> narrativeFocus;

    using TrainingClock = std::chrono::steady_clock;
    std::mutex trainingMutex;
    std::map<ObjectGuid, std::pair<TrainingClock::time_point, CompanionTrainingSnapshot>> trainingSnapshots;

    void RecordTraining(Player* bot, std::string const& status, uint32 trainer, uint32 spell, uint32 learned)
    {
        std::lock_guard<std::mutex> lock(trainingMutex);
        if (trainingSnapshots.size() >= 4096 && !trainingSnapshots.count(bot->GetGUID()))
            trainingSnapshots.erase(trainingSnapshots.begin());
        trainingSnapshots[bot->GetGUID()] = {TrainingClock::now(),
            {status, trainer, spell, learned, bot->GetMoney(), 0}};
    }

    enum Timer : uint32 { Decision = 1, TownScan, HerbScan, Deadline, Progress, Resume, ClearRejected, VerifyLesson };
    enum class Errand { None, Trainer, Herb, Return, PoisonVendor, Bags };
    constexpr float TownRadius = 1500.0f;

    bool Eligible(PlayerbotAI* ai)
    {
        Player* bot = ai->GetBot();
        Player* master = ai->GetMaster();
        if (!IsCompanionInventoryManaged(ai) || !master || !master->IsInWorld() || !master->GetSession() ||
            master->GetSession()->IsLoggingOut() || !master->IsAlive() || !bot->IsAlive() ||
            master->IsBeingTeleported() || bot->IsBeingTeleported() || master->GetMap() != bot->GetMap() ||
            !(master->GetPhaseMask() & bot->GetPhaseMask()) || bot->InBattleground() || bot->GetMap()->IsDungeon() ||
            bot->IsInCombat() || master->IsInCombat() || bot->IsInFlight() || master->IsInFlight() ||
            bot->GetTransport() || master->GetTransport() || bot->GetVehicle() || master->GetVehicle() ||
            !ai->HasStrategy("follow", BOT_STATE_NON_COMBAT) ||
            ai->HasStrategy("stay", BOT_STATE_NON_COMBAT) || ai->HasStrategy("passive", BOT_STATE_NON_COMBAT))
            return false;
        if (Group* group = bot->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource())
                    if (member->IsInCombat())
                        return false;
        return bot->GetGroup() && bot->GetGroup() == master->GetGroup();
    }

    uint32 Settlement(uint32 areaId, uint32 zoneId)
    {
        auto const* zone = sAreaTableStore.LookupEntry(zoneId);
        if (zone && (zone->flags & AREA_FLAG_CAPITAL))
            return zoneId;
        auto const* area = sAreaTableStore.LookupEntry(areaId);
        if (area && (area->flags & (AREA_FLAG_TOWN | AREA_FLAG_CAPITAL)))
            return areaId;
        return 0;
    }

    uint32 Settlement(Player* player)
    {
        return Settlement(player->GetAreaId(), player->GetZoneId());
    }

    bool InTown(Player* player)
    {
        return Settlement(player) || player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_RESTING);
    }

    bool IsSecondaryProfession(uint32 skill)
    {
        return skill == SKILL_FISHING || skill == SKILL_COOKING || skill == SKILL_FIRST_AID;
    }

    bool IsProfession(uint32 skill)
    {
        return CompanionErrands::IsPrimary(skill) || IsSecondaryProfession(skill);
    }

    uint32 ProfessionForSpell(uint32 id, uint32 depth = 0)
    {
        if (depth > 3)
            return 0;
        auto const* line = PlayerbotSpellRepository::Instance().GetSkillLine(id);
        if (line && IsProfession(line->SkillLine))
            return line->SkillLine;
        auto const* spell = sSpellMgr->GetSpellInfo(id);
        if (spell)
            for (auto const& effect : spell->Effects)
            {
                if (effect.Effect == SPELL_EFFECT_SKILL && IsProfession(effect.MiscValue))
                    return effect.MiscValue;
                if (effect.Effect == SPELL_EFFECT_LEARN_SPELL)
                    if (uint32 skill = ProfessionForSpell(effect.TriggerSpell, depth + 1))
                        return skill;
            }
        return 0;
    }

    bool Protected(PlayerbotAI* ai, Item* item)
    {
        Player* player = ai->GetBot();
        auto const* proto = item->GetTemplate();
        if (!Player::IsInventoryPos(item->GetBagSlot(), item->GetSlot()) || item->IsBag() ||
            !proto->SellPrice || proto->StartQuest || proto->Class == ITEM_CLASS_QUEST ||
            proto->Class == ITEM_CLASS_CONSUMABLE || proto->Class == ITEM_CLASS_PROJECTILE ||
            proto->Class == ITEM_CLASS_KEY || proto->TotemCategory || item->IsInTrade() ||
            item->IsWrapped())
            return true;
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(player->GetQuestSlotQuestId(slot));
            if (!quest)
                continue;
            for (uint32 id : quest->RequiredItemId)
                if (id == proto->ItemId)
                    return true;
            if (quest->GetSrcItemId() == proto->ItemId)
                return true;
        }
        // Keep reagents and tools for every known spell, including later profession plans.
        for (auto const& pair : player->GetSpellMap())
        {
            if (pair.second->State == PLAYERSPELL_REMOVED)
                continue;
            SpellInfo const* spell = sSpellMgr->GetSpellInfo(pair.first);
            if (!spell)
                continue;
            for (int32 reagent : spell->Reagent)
                if (reagent > 0 && uint32(reagent) == proto->ItemId)
                    return true;
            for (uint32 tool : spell->Totem)
                if (tool == proto->ItemId)
                    return true;
        }
        for (uint32 slot = 0; slot < MAX_ENCHANTMENT_SLOT; ++slot)
            if (item->GetEnchantmentId(EnchantmentSlot(slot)))
                return true;
        return false;
    }

    bool NeedsBagSpace(Player* bot)
    {
        uint32 free = 0, total = 0;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        {
            ++total;
            if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                ++free;
        }
        for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
            if (Bag* bag = bot->GetBagByPos(slot))
                if (!bag->GetTemplate()->BagFamily)
                {
                    total += bag->GetBagSize();
                    free += bag->GetFreeSlots();
                }
        return SelfbotInventoryPolicy::NeedsSpace(free, total);
    }

    bool NeedsEquipmentRepair(Player* bot)
    {
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (SelfbotInventoryPolicy::NeedsRepair(item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY),
                    item->GetUInt32Value(ITEM_FIELD_DURABILITY)))
                    return true;
        return false;
    }

    bool HasBagRoom(Player* bot)
    {
        uint32 free = 0;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                ++free;
        for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
            if (Bag* bag = bot->GetBagByPos(slot))
                if (!bag->GetTemplate()->BagFamily)
                    free += bag->GetFreeSlots();
        return free >= 2;
    }

    std::array<uint32, 2> PoisonStock(Player* bot)
    {
        std::array<uint32, 2> stock{};
        auto count = [&](Item* item)
        {
            if (!item || bot->CanUseItem(item->GetTemplate()) != EQUIP_ERR_OK)
                return;
            int family = CompanionErrands::PoisonFamily(item->GetTemplate()->Name1);
            if (family >= 0)
                stock[family] += item->GetCount();
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            count(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
            if (Bag* bag = bot->GetBagByPos(slot))
                for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                    count(bag->GetItemByPos(i));
        return stock;
    }

    struct PoisonPurchase
    {
        uint32 item = 0;
        uint32 slot = 0;
        uint32 batches = 0;
    };

    PoisonPurchase ChoosePoison(Player* bot, VendorItemData const* goods, uint32 reserve,
        std::array<uint32, 2> const& stock, Creature* vendor = nullptr)
    {
        if (bot->getClass() != CLASS_ROGUE || !goods || goods->Empty())
            return {};
        std::array<ItemTemplate const*, 2> best{};
        std::array<uint32, 2> slots{};
        for (uint32 slot = 0; slot < goods->GetItemCount(); ++slot)
        {
            auto const* offer = goods->GetItem(slot);
            auto const* item = offer ? sObjectMgr->GetItemTemplate(offer->item) : nullptr;
            if (!item || offer->ExtendedCost || item->BuyPrice < 0 ||
                bot->CanUseItem(item) != EQUIP_ERR_OK)
                continue;
            int family = CompanionErrands::PoisonFamily(item->Name1);
            if (family < 0 || stock[family] >= 3 || (vendor && offer->maxcount &&
                vendor->GetVendorItemCurrentCount(offer) < item->BuyCount))
                continue;
            if (!best[family] || item->RequiredLevel > best[family]->RequiredLevel)
            {
                best[family] = item;
                slots[family] = slot;
            }
        }
        for (size_t family = 0; family < best.size(); ++family)
        {
            auto const* item = best[family];
            if (!item)
                continue;
            float discount = vendor ? bot->GetReputationPriceDiscount(vendor) : 1.0f;
            // Full-price affordability is conservative: the native purchase discounts the entire batch.
            uint32 batches = CompanionErrands::PoisonBatches(stock[family], item->BuyCount,
                item->BuyPrice, bot->GetMoney(), reserve);
            auto const* offer = goods->GetItem(slots[family]);
            if (vendor && offer->maxcount && item->BuyCount)
                batches = std::min(batches, vendor->GetVendorItemCurrentCount(offer) / item->BuyCount);
            ItemPosCountVec dest;
            if (batches && bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, item->ItemId,
                batches * item->BuyCount) == EQUIP_ERR_OK &&
                CompanionErrands::CanSpend(bot->GetMoney(),
                    uint32(std::floor(uint64(item->BuyPrice) * batches * discount)), reserve))
                return {item->ItemId, slots[family], batches};
        }
        return {};
    }

    uint32 RepairEquipment(Player* bot, Creature* vendor, uint32 reserve)
    {
        Creature* repairer = bot->GetNPCIfCanInteractWith(vendor->GetGUID(), UNIT_NPC_FLAG_REPAIR);
        if (!repairer)
            return 0;
        float discount = bot->GetReputationPriceDiscount(repairer);
        uint32 spent = 0;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;
            uint32 maximum = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
            uint32 current = item->GetUInt32Value(ITEM_FIELD_DURABILITY);
            if (!maximum || current >= maximum)
                continue;
            auto const* proto = item->GetTemplate();
            auto const* costs = sDurabilityCostsStore.LookupEntry(proto->ItemLevel);
            auto const* quality = sDurabilityQualityStore.LookupEntry((proto->Quality + 1) * 2);
            if (!costs || !quality)
                continue;
            // Same price calculation as Player::DurabilityRepair in this repository.
            uint32 index = ItemSubClassToDurabilityMultiplierId(proto->Class, proto->SubClass);
            uint32 multiplier = costs->multiplier[index];
            uint32 price = uint32((maximum - current) * multiplier * double(quality->quality_mod));
            price = std::max<uint32>(1, uint32(price * discount * sWorld->getRate(RATE_REPAIRCOST)));
            if (!SelfbotInventoryPolicy::CanRepair(bot->GetMoney(), reserve, price))
                continue;
            uint32 before = bot->GetMoney();
            bot->DurabilityRepair(uint16(INVENTORY_SLOT_BAG_0 << 8) | slot, true, discount, false);
            spent += before - bot->GetMoney();
        }
        return spent;
    }

    bool SafeHerb(Player* bot, GameObject* herb)
    {
        std::list<Unit*> units;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(bot, bot, 75.0f);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> search(bot, units, check);
        Cell::VisitObjects(bot, search, 75.0f);
        float dx = herb->GetPositionX() - bot->GetPositionX();
        float dy = herb->GetPositionY() - bot->GetPositionY();
        float length = dx * dx + dy * dy;
        for (Unit* unit : units)
        {
            if (!unit->IsAlive() || !unit->IsHostileTo(bot) || !bot->CanSeeOrDetect(unit))
                continue;
            float t = length > 0.01f ? std::clamp(((unit->GetPositionX() - bot->GetPositionX()) * dx +
                (unit->GetPositionY() - bot->GetPositionY()) * dy) / length, 0.0f, 1.0f) : 0.0f;
            float x = bot->GetPositionX() + t * dx;
            float y = bot->GetPositionY() + t * dy;
            if (unit->GetDistance(x, y, bot->GetPositionZ()) < 25.0f)
                return false;
        }
        return true;
    }
}

struct CompanionErrandState
{
    EventMap events;
    Errand kind = Errand::None;
    bool ready = true;
    bool townDue = true;
    bool herbDue = true;
    bool paused = false;
    bool gathering = false;
    uint32 spawn = 0;
    uint32 map = 0;
    uint32 zone = 0;
    uint32 settlement = 0;
    uint32 reserve = 0;
    uint32 learned = 0;
    uint32 pendingLesson = 0;
    uint32 pendingTrainer = 0;
    uint32 moneyBeforeLesson = 0;
    bool verificationDue = false;
    ObjectGuid target;
    ObjectGuid owner;
    float x = 0, y = 0, z = 0;
    float closest = std::numeric_limits<float>::max();
    uint32 sold = 0;
    uint32 repairs = 0;
    bool warnedBags = false;
    std::set<ObjectGuid> failedSales;
    std::set<uint32> rejectedBagVendors;
    std::set<uint32> rejectedTrainers;
    std::set<uint32> rejectedLessons;
    std::set<uint32> rejectedVendors;
    std::set<ObjectGuid> rejectedHerbs;
};

CompanionTrainingSnapshot GetCompanionTrainingSnapshot(ObjectGuid guid)
{
    std::lock_guard<std::mutex> lock(trainingMutex);
    auto it = trainingSnapshots.find(guid);
    if (it == trainingSnapshots.end())
        return {};
    auto age = std::chrono::duration_cast<std::chrono::seconds>(TrainingClock::now() - it->second.first).count();
    if (age > 600)
    {
        trainingSnapshots.erase(it);
        return {};
    }
    auto result = it->second.second;
    result.ageSeconds = uint32(age);
    return result;
}

bool IsCompanionInventoryManaged(PlayerbotAI* ai)
{
    return ai && ai->GetBot() && ai->GetMaster() && ai->GetMaster() != ai->GetBot() &&
        !IsSelfBot(ai->GetBot()) && IsRealPlayer(ai->GetMaster());
}

bool CanSellCompanionItem(PlayerbotAI* ai, Item* item)
{
    if (!item || item->GetTemplate()->Quality != ITEM_QUALITY_POOR || Protected(ai, item))
        return false;
    ItemUsage usage = ai->GetAiObjectContext()->GetValue<ItemUsage>("item usage", item->GetEntry())->Get();
    return CompanionErrands::CanSellGrey(item->GetTemplate()->Quality, false,
        usage == ITEM_USAGE_VENDOR || usage == ITEM_USAGE_AH || usage == ITEM_USAGE_NONE);
}

bool IsManagedCompanion(PlayerbotAI* ai)
{
    return ai && ai->GetBot() && ai->GetMaster() && ai->GetMaster() != ai->GetBot() &&
        !IsSelfBot(ai->GetBot()) && (sPlayerbotAIConfig.companionProfessionPlans.count(ai->GetBot()->GetName()) ||
        CompanionVocation::Get(ai->GetBot()).professions[0] != 0);
}

namespace
{
    void StopTrip(PlayerbotAI* ai, bool reject)
    {
        auto& state = *ai->companionErrands;
        if (reject)
        {
            LOG_INFO("playerbots.companion", "[Training] {} abandons errand={} spawn={} distance={} learned={}",
                ai->GetBot()->GetName(), uint32(state.kind), state.spawn,
                ai->GetBot()->GetDistance(state.x, state.y, state.z), state.learned);
            if (state.kind == Errand::Trainer)
            {
                state.rejectedTrainers.insert(state.spawn);
                RecordTraining(ai->GetBot(), "interrupted_or_unreachable", 0, state.pendingLesson, state.learned);
            }
            if (state.kind == Errand::Bags)
                state.rejectedBagVendors.insert(state.spawn);
            if (state.kind == Errand::PoisonVendor)
                state.rejectedVendors.insert(state.spawn);
            if (state.kind == Errand::Herb)
                state.rejectedHerbs.insert(state.target);
        }
        if (state.kind != Errand::None)
        {
            Player* bot = ai->GetBot();
            if (state.kind == Errand::Herb)
            {
                if (state.gathering)
                    bot->InterruptNonMeleeSpells(false);
                auto* context = ai->GetAiObjectContext();
                if (context->GetValue<LootObject>("loot target")->Get().guid == state.target)
                    context->GetValue<LootObject>("loot target")->Set(LootObject());
                context->GetValue<LootObjectStack*>("available loot")->Get()->Remove(state.target);
            }
            bot->StopMoving();
            bot->GetMotionMaster()->Clear();
        }
        state.kind = Errand::None;
        state.pendingLesson = 0;
        state.verificationDue = false;
        state.events.CancelEvent(VerifyLesson);
        state.target.Clear();
        state.gathering = false;
        state.events.CancelEvent(Deadline);
        state.events.CancelEvent(Progress);
    }

    bool HasLearnedLesson(Player* bot, uint32 id)
    {
        if (bot->HasSpell(id))
            return true;
        auto const* info = sSpellMgr->GetSpellInfo(id);
        if (!info)
            return false;
        bool teachesSpell = false;
        for (auto const& effect : info->GetEffects())
        {
            if (!effect.IsEffect(SPELL_EFFECT_LEARN_SPELL))
                continue;
            teachesSpell = true;
            if (!bot->HasSpell(effect.TriggerSpell))
                return false;
        }
        return teachesSpell;
    }

    bool CanVisitTrainer(PlayerbotAI* ai)
    {
        // Class lessons and existing professions do not require a PBC vocation.
        return IsCompanionInventoryManaged(ai) && sPlayerbotAIConfig.allowLearnTrainerSpells;
    }

    void Returning(PlayerbotAI* ai)
    {
        auto& state = *ai->companionErrands;
        StopTrip(ai, false);
        // Continue local errands while the master remains in the same settlement.
        if ((IsManagedCompanion(ai) || CanVisitTrainer(ai)) && Eligible(ai) && state.settlement &&
            Settlement(ai->GetMaster()) == state.settlement)
        {
            state.townDue = true;
            return;
        }
        state.kind = Errand::Return;
        state.closest = std::numeric_limits<float>::max();
        state.events.RescheduleEvent(Deadline, Milliseconds(180000));
        state.events.RescheduleEvent(Progress, Milliseconds(30000));
    }

    Trainer::Spell const* ChooseLesson(PlayerbotAI* ai, Trainer::Trainer* trainer, float discount, bool budget = true)
    {
        Player* bot = ai->GetBot();
        if (!CanVisitTrainer(ai) || !trainer || !trainer->IsTrainerValidForPlayer(bot))
            return nullptr;
        bool const classTrainer = trainer->GetTrainerType() == Trainer::Type::Class;
        auto const focus = GetCompanionNarrativeFocus(bot->GetGUID());
        if ((focus == "training_class" && !classTrainer) || (focus == "training_professions" && classTrainer))
            return nullptr;
        if (!classTrainer && trainer->GetTrainerType() != Trainer::Type::Tradeskill)
            return nullptr;
        auto skills = CompanionVocation::Get(bot).professions;
        if (!skills[0])
            if (auto plan = sPlayerbotAIConfig.companionProfessionPlans.find(bot->GetName());
                plan != sPlayerbotAIConfig.companionProfessionPlans.end())
                skills = plan->second;
        Trainer::Spell const* best = nullptr;
        for (auto const& spell : trainer->GetSpells())
        {
            if (ai->companionErrands->rejectedLessons.count(spell.SpellId))
                continue;
            if (!classTrainer)
            {
                uint32 skill = IsProfession(spell.ReqSkillLine) ? spell.ReqSkillLine :
                    ProfessionForSpell(spell.SpellId);
                if (!skill || (!bot->HasSkill(skill) && skill != skills[0] && skill != skills[1] &&
                    !(IsManagedCompanion(ai) && IsSecondaryProfession(skill))))
                    continue;
            }
            uint32 cost = uint32(std::floor(spell.MoneyCost * discount));
            if (trainer->CanTeachSpell(bot, &spell) &&
                (!budget || CompanionErrands::CanSpend(bot->GetMoney(), cost, ai->companionErrands->reserve)) &&
                (!best || spell.MoneyCost < best->MoneyCost ||
                    (spell.MoneyCost == best->MoneyCost && spell.SpellId < best->SpellId)))
                best = trainer->GetSpell(spell.SpellId);
        }
        return best;
    }
}

void SetCompanionNarrativeFocus(ObjectGuid guid, std::string const& focus)
{
    std::lock_guard<std::mutex> lock(narrativeFocusMutex);
    if (focus == "normal")
        narrativeFocus.erase(guid);
    else if (focus == "follow" || focus == "training" || focus == "training_class" ||
        focus == "training_professions" || focus == "craft")
        narrativeFocus[guid] = focus;
}

std::string GetCompanionNarrativeFocus(ObjectGuid guid)
{
    std::lock_guard<std::mutex> lock(narrativeFocusMutex);
    auto found = narrativeFocus.find(guid);
    return found == narrativeFocus.end() ? "normal" : found->second;
}

void CancelCompanionErrands(PlayerbotAI* ai)
{
    if (!ai->companionErrands)
        return;
    if (ai->companionErrands->kind == Errand::Trainer)
        RecordTraining(ai->GetBot(), "interrupted", 0, 0, ai->companionErrands->learned);
    StopTrip(ai, false);
    auto& state = *ai->companionErrands;
    state.paused = true;
    state.events.RescheduleEvent(Resume, Milliseconds(60000));
}

void RequestCompanionTraining(PlayerbotAI* ai)
{
    if (!IsCompanionInventoryManaged(ai))
        return;
    // Schedule a fresh decision; normal safety and purchasing checks remain authoritative.
    if (!ai->companionErrands)
    {
        ai->companionErrands = std::make_shared<CompanionErrandState>();
        ai->companionErrands->reserve = ai->GetBot()->GetMoney() / 5;
        ai->companionErrands->events.ScheduleEvent(ClearRejected, Milliseconds(300000));
    }
    auto& state = *ai->companionErrands;
    if (state.pendingLesson)
        return;
    StopTrip(ai, false);
    state.paused = false;
    state.townDue = true;
    state.ready = true;
    state.events.CancelEvent(Resume);
    RecordTraining(ai->GetBot(), "requested", 0, 0, 0);
}

void UpdateCompanionErrands(PlayerbotAI* ai, uint32 elapsed)
{
    if (!ai->companionErrands)
        return;
    auto& state = *ai->companionErrands;
    state.events.Update(elapsed);
    while (uint32 event = state.events.ExecuteEvent())
    {
        switch (event)
        {
            case Decision: state.ready = true; break;
            case TownScan: state.townDue = true; break;
            case HerbScan: state.herbDue = true; break;
            case Resume: state.paused = false; break;
            case VerifyLesson: state.verificationDue = true; break;
            case ClearRejected:
                state.rejectedBagVendors.clear();
                state.failedSales.clear();
                state.warnedBags = false;
                state.rejectedTrainers.clear();
                state.rejectedLessons.clear();
                state.rejectedVendors.clear();
                state.rejectedHerbs.clear();
                state.events.ScheduleEvent(ClearRejected, Milliseconds(300000));
                break;
            case Deadline:
            case Progress:
                StopTrip(ai, true);
                state.paused = true;
                state.events.RescheduleEvent(Resume, Milliseconds(15000));
                break;
        }
    }
    if (state.kind == Errand::None)
        return;
    Player* master = ai->GetMaster();
    Player* bot = ai->GetBot();
    if (GetCompanionNarrativeFocus(bot->GetGUID()) == "follow" ||
        !Eligible(ai) || master->GetGUID() != state.owner || bot->GetMapId() != state.map ||
        ((state.kind == Errand::Trainer || state.kind == Errand::PoisonVendor || state.kind == Errand::Bags) &&
            (!InTown(master) || master->GetZoneId() != state.zone ||
            (state.settlement ? Settlement(master) != state.settlement :
                master->GetDistance(state.x, state.y, state.z) > TownRadius))) ||
        (state.kind == Errand::Herb && (bot->GetDistance(master) > 60.0f ||
            master->GetDistance(state.x, state.y, state.z) > 60.0f)))
        CancelCompanionErrands(ai);
}

bool IsCompanionGatherLoot(PlayerbotAI* ai, ObjectGuid guid)
{
    return ai->companionErrands && ai->companionErrands->kind == Errand::Herb &&
        ai->companionErrands->target == guid;
}

void FinishCompanionGather(PlayerbotAI* ai, ObjectGuid guid)
{
    if (IsCompanionGatherLoot(ai, guid))
    {
        ai->companionErrands->gathering = false;
        ai->companionErrands->rejectedHerbs.insert(guid);
        Returning(ai);
    }
}

bool CompanionErrandAction::isUseful()
{
    if (!Eligible(botAI) || GetCompanionNarrativeFocus(bot->GetGUID()) == "follow")
        return false;
    if (!botAI->companionErrands)
    {
        botAI->companionErrands = std::make_shared<CompanionErrandState>();
        botAI->companionErrands->reserve = bot->GetMoney() / 5;
        botAI->companionErrands->events.ScheduleEvent(ClearRejected, Milliseconds(300000));
    }
    auto const& state = *botAI->companionErrands;
    return !state.paused && (state.kind != Errand::None || (state.ready && (state.townDue || state.herbDue)));
}

bool CompanionErrandAction::Execute(Event)
{
    if (!isUseful())
        return false;
    auto& state = *botAI->companionErrands;
    Player* master = botAI->GetMaster();
    if (!state.ready || bot->IsNonMeleeSpellCast(false) || !bot->GetLootGUID().IsEmpty())
        return state.kind != Errand::None;
    state.ready = false;
    state.events.RescheduleEvent(Decision, Milliseconds(1000));

    if (state.kind == Errand::None)
    {
        state.owner = master->GetGUID();
        state.map = bot->GetMapId();
        state.zone = master->GetZoneId();
        state.settlement = Settlement(master);
        if (state.townDue)
        {
            state.townDue = false;
            state.events.RescheduleEvent(TownScan, Milliseconds(60000));
            if (InTown(master) && (IsManagedCompanion(botAI) || CanVisitTrainer(botAI) ||
                (!master->isMoving() && bot->GetDistance(master) < 30.0f)))
            {
                state.reserve = std::max(state.reserve, bot->GetMoney() / 5);
                double best = std::numeric_limits<double>::max();
                auto const stock = PoisonStock(bot);
                bool sell = false;
                for (Item* item : botAI->GetInventoryItems())
                    if (!state.failedSales.count(item->GetGUID()) && CanSellCompanionItem(botAI, item))
                        sell = true;
                bool repair = NeedsEquipmentRepair(bot);
                if (!sell && NeedsBagSpace(bot) && !state.warnedBags)
                {
                    botAI->TellMaster("Mes sacs sont presque pleins. Je conserve les objets proteges; "
                        "votre aide est necessaire.");
                    state.warnedBags = true;
                }
                bool budgetBlocked = false;
                Errand selected = Errand::None;
                for (auto const& [spawn, data] : sObjectMgr->GetAllCreatureData())
                {
                    if (data.mapid != state.map || !(data.phaseMask & bot->GetPhaseMask()))
                        continue;
                    auto const* creature = sObjectMgr->GetCreatureTemplate(data.id);
                    if (!creature || !(creature->npcflag &
                        (UNIT_NPC_FLAG_VENDOR | UNIT_NPC_FLAG_REPAIR | UNIT_NPC_FLAG_TRAINER)))
                        continue;
                    auto const* faction = sFactionTemplateStore.LookupEntry(creature->faction);
                    if (!faction || !bot->GetFactionTemplateEntry()->IsFriendlyTo(*faction))
                        continue;
                    uint32 areaId = bot->GetMap()->GetAreaId(bot->GetPhaseMask(), data.posX, data.posY, data.posZ);
                    uint32 zoneId = bot->GetMap()->GetZoneId(bot->GetPhaseMask(), data.posX, data.posY, data.posZ);
                    if (zoneId != state.zone)
                        continue;
                    if (state.settlement ? Settlement(areaId, zoneId) != state.settlement :
                        master->GetDistance(data.posX, data.posY, data.posZ) > TownRadius)
                        continue;
                    auto* trainer = sObjectMgr->GetTrainer(data.id);
                    Errand candidate = Errand::None;
                    double priority = 0;
                    if (!state.rejectedBagVendors.count(uint32(spawn)) &&
                        ((sell && (creature->npcflag & UNIT_NPC_FLAG_VENDOR)) ||
                        (repair && (creature->npcflag & UNIT_NPC_FLAG_REPAIR))))
                    {
                        candidate = Errand::Bags;
                        priority = -10000.0;
                    }
                    else if (!state.rejectedTrainers.count(uint32(spawn)) && ChooseLesson(botAI, trainer, 1.0f))
                    {
                        candidate = Errand::Trainer;
                        auto focus = GetCompanionNarrativeFocus(bot->GetGUID());
                        bool profession = trainer->GetTrainerType() != Trainer::Type::Class;
                        priority = focus == "craft" ? (profession ? 0.0 : 10000.0) : (profession ? 10000.0 : 0.0);
                    }
                    else if (IsManagedCompanion(botAI) && !state.rejectedVendors.count(uint32(spawn)) &&
                        ChoosePoison(bot, sObjectMgr->GetNpcVendorItemList(data.id), state.reserve, stock).item)
                    {
                        candidate = Errand::PoisonVendor;
                        priority = 20000.0;
                    }
                    if (candidate == Errand::None)
                    {
                        budgetBlocked = budgetBlocked || ChooseLesson(botAI, trainer, 1.0f, false);
                        continue;
                    }
                    double score = bot->GetDistance(data.posX, data.posY, data.posZ) + priority;
                    if (score >= best)
                        continue;
                    best = score;
                    selected = candidate;
                    state.spawn = uint32(spawn);
                    state.x = data.posX;
                    state.y = data.posY;
                    state.z = data.posZ;
                }
                if (best == std::numeric_limits<double>::max() &&
                    GetCompanionNarrativeFocus(bot->GetGUID()).starts_with("training"))
                {
                    auto const previous = GetCompanionTrainingSnapshot(bot->GetGUID());
                    if (previous.status.empty() || previous.status == "requested" ||
                        previous.status == "no_eligible_lesson" || previous.status == "budget_insufficient")
                        RecordTraining(bot, budgetBlocked ? "budget_insufficient" : "no_eligible_lesson", 0, 0, 0);
                }
                if (best != std::numeric_limits<double>::max())
                {
                    state.kind = selected;
                    state.learned = 0;
                    if (selected == Errand::Trainer)
                        RecordTraining(bot, "travelling", 0, 0, 0);
                    state.sold = 0;
                    state.repairs = 0;
                    botAI->TellMaster(selected == Errand::Bags ?
                        "Je vais vendre mes objets gris inutiles et faire reparer mon equipement, puis je reviens." :
                        selected == Errand::Trainer ?
                        "Je vais voir un maitre pour mes apprentissages, puis je vous rejoins." :
                        "Je vais acheter mes poisons, puis je vous rejoins.");
                }
            }
        }
        if (state.kind == Errand::None && state.herbDue)
        {
            state.herbDue = false;
            state.events.RescheduleEvent(HerbScan, Milliseconds(5000));
            if (!GetCompanionNarrativeFocus(bot->GetGUID()).starts_with("training") &&
                IsManagedCompanion(botAI) && bot->HasSkill(SKILL_HERBALISM) && bot->HasSpell(2366) && HasBagRoom(bot) &&
                bot->GetDistance(master) <= 40.0f && !master->IsMounted() && !bot->IsMounted())
            {
                float best = 40.0f;
                for (ObjectGuid guid : context->GetValue<GuidVector>("nearest game objects")->Get())
                {
                    GameObject* herb = botAI->GetGameObject(guid);
                    if (!herb || state.rejectedHerbs.count(guid) || !bot->CanSeeOrDetect(herb) ||
                        !herb->isSpawned() || herb->getLootState() != GO_READY ||
                        !CompanionErrands::HerbWithinLeash(bot->GetDistance(herb), master->GetDistance(herb),
                            bot->GetDistance(master)) || bot->GetDistance(herb) >= best ||
                        !bot->IsWithinLOSInMap(herb))
                        continue;
                    LootObject loot(bot, guid);
                    if (loot.skillId != SKILL_HERBALISM || !loot.IsLootPossible(bot) || !SafeHerb(bot, herb))
                        continue;
                    best = bot->GetDistance(herb);
                    state.target = guid;
                    state.x = herb->GetPositionX();
                    state.y = herb->GetPositionY();
                    state.z = herb->GetPositionZ();
                }
                if (state.target)
                {
                    state.kind = Errand::Herb;
                    botAI->TellMaster("Je cueille cette plante et je vous rejoins.");
                }
            }
        }
        if (state.kind == Errand::None)
            return false;
        state.closest = std::numeric_limits<float>::max();
        state.events.RescheduleEvent(Deadline, Milliseconds(state.kind == Errand::Herb ? 30000 : 180000));
        state.events.RescheduleEvent(Progress, Milliseconds(30000));
    }

    if (state.kind == Errand::Return)
    {
        state.x = master->GetPositionX();
        state.y = master->GetPositionY();
        state.z = master->GetPositionZ();
        if (bot->GetDistance(master) < 8.0f)
        {
            StopTrip(botAI, false);
            return false;
        }
    }
    if (state.kind == Errand::Herb)
    {
        GameObject* herb = botAI->GetGameObject(state.target);
        if (!herb || !herb->isSpawned() || !HasBagRoom(bot) || !SafeHerb(bot, herb))
        {
            StopTrip(botAI, true);
            return false;
        }
        if (bot->GetDistance(herb) < INTERACTION_DISTANCE - 1.0f)
        {
            if (!state.gathering)
            {
                LootObject loot(bot, state.target);
                if (!loot.IsLootPossible(bot))
                {
                    StopTrip(botAI, true);
                    return false;
                }
                bot->StopMoving();
                context->GetValue<LootObject>("loot target")->Set(loot);
                state.gathering = botAI->CastSpell(2366, bot);
                if (!state.gathering)
                    StopTrip(botAI, true);
            }
            return true;
        }
    }
    if (state.kind == Errand::Bags)
    {
        Creature* npc = ObjectAccessor::GetSpawnedCreatureByDBGUID(bot->GetMapId(), state.spawn);
        bool vendor = npc && bot->GetNPCIfCanInteractWith(npc->GetGUID(), UNIT_NPC_FLAG_VENDOR);
        bool repairer = npc && bot->GetNPCIfCanInteractWith(npc->GetGUID(), UNIT_NPC_FLAG_REPAIR);
        if (vendor || repairer)
        {
            bot->StopMoving();
            if (vendor)
                for (Item* item : botAI->GetInventoryItems())
                {
                    if (state.failedSales.count(item->GetGUID()) || !CanSellCompanionItem(botAI, item))
                        continue;
                    ObjectGuid guid = item->GetGUID();
                    uint32 before = item->GetCount();
                    uint32 entry = item->GetEntry();
                    WorldPacket packet(CMSG_SELL_ITEM);
                    packet << npc->GetGUID() << guid << before;
                    WorldPackets::Item::SellItem request(std::move(packet));
                    request.Read();
                    bot->GetSession()->HandleSellItemOpcode(request);
                    Item* remaining = bot->GetItemByGuid(guid);
                    uint32 after = remaining ? remaining->GetCount() : 0;
                    if (after >= before)
                        state.failedSales.insert(guid);
                    else
                    {
                        state.sold += before - after;
                        LOG_INFO("playerbots", "[CompanionBags] {} sold {} x {} at {}",
                            bot->GetName(), before - after, entry, npc->GetEntry());
                    }
                    return true;
                }
            state.repairs = RepairEquipment(bot, npc, state.reserve);
            state.rejectedBagVendors.insert(state.spawn);
            botAI->TellMaster("Entretien termine : " + std::to_string(state.sold) +
                " objets gris vendus, " + std::to_string(state.repairs) +
                " pieces de cuivre en reparations. Je vous rejoins.");
            if (NeedsBagSpace(bot) && !state.warnedBags)
            {
                botAI->TellMaster("Mes sacs restent presque pleins; les objets proteges sont conserves.");
                state.warnedBags = true;
            }
            state.events.RescheduleEvent(TownScan, Milliseconds(5000));
            Returning(botAI);
            return true;
        }
        if (npc)
        {
            state.x = npc->GetPositionX();
            state.y = npc->GetPositionY();
            state.z = npc->GetPositionZ();
        }
    }
    if (state.kind == Errand::PoisonVendor)
    {
        Creature* npc = ObjectAccessor::GetSpawnedCreatureByDBGUID(bot->GetMapId(), state.spawn);
        if (npc && bot->GetNPCIfCanInteractWith(npc->GetGUID(), UNIT_NPC_FLAG_VENDOR))
        {
            auto purchase = ChoosePoison(bot, npc->GetVendorItems(), state.reserve, PoisonStock(bot), npc);
            if (purchase.item)
            {
                bot->StopMoving();
                uint32 before = bot->GetItemCount(purchase.item, false);
                uint32 previousVendor = bot->GetSession()->GetCurrentVendor();
                bot->GetSession()->SetCurrentVendor(0);
                bot->BuyItemFromVendorSlot(npc->GetGUID(), purchase.slot, purchase.item,
                    uint8(purchase.batches), NULL_BAG, NULL_SLOT);
                bot->GetSession()->SetCurrentVendor(previousVendor);
                uint32 after = bot->GetItemCount(purchase.item, false);
                if (after > before)
                {
                    botAI->TellMaster("J'ai achete " +
                        ChatHelper::FormatItem(sObjectMgr->GetItemTemplate(purchase.item)));
                    LOG_INFO("playerbots", "[CompanionErrands] {} bought {} x {} at {}",
                        bot->GetName(), after - before, purchase.item, npc->GetEntry());
                    return true;
                }
                state.rejectedVendors.insert(state.spawn);
            }
            state.rejectedVendors.insert(state.spawn);
            botAI->TellMaster("Ravitaillement termine, je vous rejoins.");
            state.events.RescheduleEvent(TownScan, Milliseconds(5000));
            Returning(botAI);
            return true;
        }
        if (npc)
        {
            state.x = npc->GetPositionX();
            state.y = npc->GetPositionY();
            state.z = npc->GetPositionZ();
        }
    }
    if (state.kind == Errand::Trainer)
    {
        // Triggered learning spells may finish after TeachSpell returns. Verify before any new purchase.
        if (state.pendingLesson)
        {
            bool learned = HasLearnedLesson(bot, state.pendingLesson);
            if (!learned && !state.verificationDue)
                return true;
            LOG_INFO("playerbots.companion", "[Training] {} trainer={} spell={} known={} moneyBefore={} moneyAfter={}",
                bot->GetName(), state.pendingTrainer, state.pendingLesson, learned,
                state.moneyBeforeLesson, bot->GetMoney());
            if (learned)
            {
                ++state.learned;
                if (auto const* info = sSpellMgr->GetSpellInfo(state.pendingLesson))
                    botAI->TellMaster("J'ai appris " + ChatHelper::FormatSpell(info));
            }
            else
                state.rejectedLessons.insert(state.pendingLesson);
            RecordTraining(bot, learned ? "learned" : "learning_failed", state.pendingTrainer,
                state.pendingLesson, state.learned);
            state.pendingLesson = 0;
            state.verificationDue = false;
            state.events.CancelEvent(VerifyLesson);
            state.events.RescheduleEvent(Progress, Milliseconds(30000));
            return true;
        }
        Creature* npc = ObjectAccessor::GetSpawnedCreatureByDBGUID(bot->GetMapId(), state.spawn);
        if (npc && bot->GetNPCIfCanInteractWith(npc->GetGUID(), UNIT_NPC_FLAG_TRAINER))
        {
            auto* trainer = sObjectMgr->GetTrainer(npc->GetEntry());
            auto const* lesson = ChooseLesson(botAI, trainer, bot->GetReputationPriceDiscount(npc));
            if (lesson)
            {
                state.pendingLesson = lesson->SpellId;
                state.pendingTrainer = npc->GetEntry();
                state.moneyBeforeLesson = bot->GetMoney();
                state.verificationDue = false;
                state.events.RescheduleEvent(VerifyLesson, Milliseconds(3000));
                state.events.RescheduleEvent(Progress, Milliseconds(30000));
                bot->StopMoving();
                trainer->TeachSpell(npc, bot, state.pendingLesson);
                RecordTraining(bot, "verifying", npc->GetEntry(), state.pendingLesson, state.learned);
                return true;
            }
            bool budgetBlocked = ChooseLesson(botAI, trainer, bot->GetReputationPriceDiscount(npc), false);
            auto const lastTraining = GetCompanionTrainingSnapshot(bot->GetGUID());
            bool const failed = lastTraining.status == "learning_failed";
            RecordTraining(bot, budgetBlocked ? "budget_insufficient" : failed ? "learning_failed" : "visit_finished",
                npc->GetEntry(), 0, state.learned);
            LOG_INFO("playerbots.companion", "[Training] {} finished trainer={} learned={} money={} reserve={} budget={}",
                bot->GetName(), npc->GetEntry(), state.learned, bot->GetMoney(), state.reserve, budgetBlocked);
            botAI->TellMaster("Visite terminee, je vous rejoins.");
            state.events.RescheduleEvent(TownScan, Milliseconds(5000));
            Returning(botAI);
            return true;
        }
        if (npc)
        {
            state.x = npc->GetPositionX();
            state.y = npc->GetPositionY();
            state.z = npc->GetPositionZ();
        }
    }
    float distance = bot->GetDistance(state.x, state.y, state.z);
    if (distance + 2.0f < state.closest)
    {
        state.closest = distance;
        state.events.RescheduleEvent(Progress, Milliseconds(30000));
    }
    MoveTo(state.map, state.x, state.y, state.z, false, false, true, true);
    return true;
}
