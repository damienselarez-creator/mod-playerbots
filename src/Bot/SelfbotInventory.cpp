#include "SelfbotInventory.h"

#include "Bag.h"
#include "Chat.h"
#include "EventMap.h"
#include "InventoryPolicy.h"
#include "ItemPackets.h"
#include "ItemUsageValue.h"
#include "MotionMaster.h"
#include "NewRpgBaseAction.h"
#include "Playerbots.h"
#include "SelfbotProfessions.h"

#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace
{
    enum InventoryEvent : uint32 { CheckBags = 1, MoveVendor, TripDeadline };
    struct ProducedStack
    {
        uint32 item = 0;
        uint32 original = 0;
        uint32 produced = 0;
        uint32 expected = 0;
    };

    void Message(PlayerbotAI* ai, std::string const& message)
    {
        ai->selfbotCycle.objective = message;
        ChatHandler(ai->GetBot()->GetSession()).SendSysMessage(("[Sacs] " + message).c_str());
        LOG_INFO("playerbots", "[SelfbotInventory] {}: {}", ai->GetBot()->GetName(), message);
    }

    struct BagSpace { uint32 free = 0; uint32 total = 0; };

    BagSpace Space(Player* player)
    {
        BagSpace result;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        {
            ++result.total;
            if (!player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                ++result.free;
        }
        for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
        {
            Bag* bag = player->GetBagByPos(slot);
            // Specialized bags cannot guarantee space for the next quest reward or recipe.
            if (!bag || bag->GetTemplate()->BagFamily)
                continue;
            result.total += bag->GetBagSize();
            for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                if (!bag->GetItemByPos(i))
                    ++result.free;
        }
        return result;
    }

    bool NeedsRepair(Player* player)
    {
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            {
                uint32 maximum = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
                uint32 current = item->GetUInt32Value(ITEM_FIELD_DURABILITY);
                if (SelfbotInventoryPolicy::NeedsRepair(maximum, current))
                    return true;
            }
        return false;
    }
}

struct SelfbotInventoryState
{
    EventMap events;
    bool busy = false;
    bool pause = false;
    bool ready = true;
    bool selling = true;
    uint32 reserve = 0;
    uint32 vendor = 0;
    uint32 map = 0;
    WorldPosition destination;
    std::unordered_set<uint32> rejected;
    std::unordered_set<uint64> failedSales;
    std::unordered_map<uint64, uint32> beforeCraft;
    std::unordered_map<uint64, ProducedStack> produced;

    explicit SelfbotInventoryState(Player* player) : reserve(player->GetMoney() / 5), map(player->GetMapId())
    {
        events.ScheduleEvent(CheckBags, Milliseconds(0));
    }
};

namespace
{
    SelfbotInventoryState& State(PlayerbotAI* ai)
    {
        auto& state = ai->selfbotCycle.inventory;
        if (!state)
            state = std::make_shared<SelfbotInventoryState>(ai->GetBot());
        return *state;
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

    uint32 SaleQuantity(PlayerbotAI* ai, Item* item, SelfbotInventoryState& state)
    {
        auto const* proto = item->GetTemplate();
        if (proto->Quality > ITEM_QUALITY_NORMAL || state.failedSales.count(item->GetGUID().GetRawValue()))
            return 0;
        auto found = state.produced.find(item->GetGUID().GetRawValue());
        ProducedStack made;
        if (found != state.produced.end() && found->second.item == item->GetEntry() &&
            found->second.expected == item->GetCount())
            made = found->second;
        ItemUsage usage = ai->GetAiObjectContext()->GetValue<ItemUsage>("item usage", item->GetEntry())->Get();
        bool useful = usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE || usage == ITEM_USAGE_KEEP ||
                      usage == ITEM_USAGE_USE || usage == ITEM_USAGE_SKILL || usage == ITEM_USAGE_QUEST ||
                      usage == ITEM_USAGE_AMMO || usage == ITEM_USAGE_GUILD_TASK || usage == ITEM_USAGE_BROKEN_EQUIP;
        uint32 keep = 1;
        for (Item* other : ai->GetInventoryItems())
            if (other->GetEntry() == item->GetEntry() && other->GetGUID() < item->GetGUID())
                keep = 0;
        return SelfbotInventoryPolicy::SaleQuantity(Protected(ai, item) || useful, proto->Quality,
            item->GetCount(), made.produced, made.original,
            usage == ITEM_USAGE_VENDOR || usage == ITEM_USAGE_AH, keep);
    }

    class InventoryAction : public NewRpgBaseAction
    {
    public:
        explicit InventoryAction(PlayerbotAI* ai) : NewRpgBaseAction(ai, "selfbot inventory"), state(State(ai)) { }

        bool Execute(Event) override
        {
            BagSpace space = Space(bot);
            if (!state.busy)
            {
                state.selling = SelfbotInventoryPolicy::NeedsSpace(space.free, space.total);
                if (!state.selling && !NeedsRepair(bot))
                    return false;
                state.busy = true;
                state.rejected.clear();
                state.failedSales.clear();
                bot->GetMotionMaster()->Clear();
                bot->StopMoving();
                botAI->rpgInfo.ChangeToRest();
                if (state.selling && !HasSale())
                    return Pause("Sacs presque pleins, aucun objet vendable sans toucher aux objets proteges.");
                Message(botAI, state.selling ? "Sacs presque pleins : recherche d'un marchand." :
                    "Equipement a 20% de durabilite ou moins : recherche d'un reparateur.");
                if (!SelectVendor())
                    return Pause("Aucun marchand ou reparateur accessible a proximite.");
            }
            if (bot->GetMapId() != state.map)
                return Pause("Carte changee pendant la gestion des sacs; controle manuel retabli.");
            Creature* npc = ObjectAccessor::GetSpawnedCreatureByDBGUID(state.map, state.vendor);
            NPCFlags flag = state.selling ? UNIT_NPC_FLAG_VENDOR : UNIT_NPC_FLAG_REPAIR;
            if (!npc || !bot->GetNPCIfCanInteractWith(npc->GetGUID(), flag))
            {
                if (npc && npc->IsAlive() && npc->GetPhaseMask() & bot->GetPhaseMask())
                    MoveWorldObjectTo(npc->GetGUID(), INTERACTION_DISTANCE - 1);
                else
                    MoveFarTo(state.destination);
                return true;
            }
            bot->StopMoving();
            // Sell at most one stack per decision and re-evaluate its protections at the merchant.
            for (Item* item : botAI->GetInventoryItems())
            {
                if (!state.selling)
                    break;
                uint32 quantity = SaleQuantity(botAI, item, state);
                if (!quantity)
                    continue;
                ObjectGuid guid = item->GetGUID();
                uint32 before = item->GetCount();
                uint32 entry = item->GetEntry();
                WorldPacket packet(CMSG_SELL_ITEM);
                packet << npc->GetGUID() << guid << quantity;
                WorldPackets::Item::SellItem request(std::move(packet));
                request.Read();
                bot->GetSession()->HandleSellItemOpcode(request);
                Item* remaining = bot->GetItemByGuid(guid);
                uint32 after = remaining ? remaining->GetCount() : 0;
                if (after >= before)
                    state.failedSales.insert(guid.GetRawValue());
                else
                {
                    auto found = state.produced.find(guid.GetRawValue());
                    if (found != state.produced.end())
                    {
                        found->second.produced -= std::min(found->second.produced, before - after);
                        found->second.expected = after;
                    }
                    Message(botAI, "Vendu : objet " + std::to_string(entry) + " x" + std::to_string(before - after));
                }
                return true;
            }
            Repair(npc);
            space = Space(bot);
            if (SelfbotInventoryPolicy::NeedsSpace(space.free, space.total))
                return Pause("Vente terminee mais place insuffisante; objets proteges conserves. Controle manuel.");
            if (!state.selling && NeedsRepair(bot))
                return Pause("Reparation insuffisante : budget ou service indisponible. Controle manuel.");
            Finish();
            Message(botAI, "Sacs liberes : " + std::to_string(space.free) + " places. Reprise du cycle.");
            return true;
        }

        bool NextVendor()
        {
            state.rejected.insert(state.vendor);
            if (state.rejected.size() >= 3 || !SelectVendor())
                return Pause("Marchands inaccessibles; controle manuel pour liberer les sacs.");
            Message(botAI, "Marchand inaccessible; essai d'un autre marchand.");
            return true;
        }

    private:
        SelfbotInventoryState& state;

        bool HasSale()
        {
            for (Item* item : botAI->GetInventoryItems())
                if (SaleQuantity(botAI, item, state))
                    return true;
            return false;
        }

        bool Pause(std::string const& reason)
        {
            state.pause = true;
            bot->StopMoving();
            Message(botAI, reason + " Reprise : .playerbots bot self");
            SaveSelfbotCycle(botAI);
            return true;
        }

        void Finish()
        {
            state.busy = false;
            state.vendor = 0;
            state.events.CancelEvent(TripDeadline);
            bot->GetMotionMaster()->Clear();
            bot->StopMoving();
            botAI->selfbotCycle.professions.reset();
            botAI->selfbotCycle.configured = false;
            botAI->selfbotCycle.decisionElapsed = 15000;
        }

        bool SelectVendor()
        {
            float best = 6000;
            uint32 chosen = 0;
            for (auto const& pair : sObjectMgr->GetAllCreatureData())
            {
                auto const& data = pair.second;
                if (data.mapid != bot->GetMapId() || !(data.phaseMask & bot->GetPhaseMask()) ||
                    state.rejected.count(uint32(pair.first)))
                    continue;
                auto const* proto = sObjectMgr->GetCreatureTemplate(data.id);
                NPCFlags flag = state.selling ? UNIT_NPC_FLAG_VENDOR : UNIT_NPC_FLAG_REPAIR;
                if (!proto || !(proto->npcflag & flag))
                    continue;
                auto const* faction = sFactionTemplateStore.LookupEntry(proto->faction);
                if (!faction || bot->GetFactionTemplateEntry()->IsHostileTo(*faction))
                    continue;
                float distance = bot->GetDistance(data.posX, data.posY, data.posZ);
                if (distance < best)
                {
                    best = distance;
                    chosen = uint32(pair.first);
                    state.destination = WorldPosition(data.mapid, data.posX, data.posY, data.posZ);
                }
            }
            state.vendor = chosen;
            state.map = bot->GetMapId();
            state.events.RescheduleEvent(TripDeadline, Milliseconds(180000));
            bot->GetMotionMaster()->Clear();
            bot->StopMoving();
            return chosen != 0;
        }

        void Repair(Creature* vendor)
        {
            Creature* repairer = bot->GetNPCIfCanInteractWith(vendor->GetGUID(), UNIT_NPC_FLAG_REPAIR);
            if (!repairer)
                return;
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
                if (!SelfbotInventoryPolicy::CanRepair(bot->GetMoney(), state.reserve, price))
                    continue;
                uint32 before = bot->GetMoney();
                bot->DurabilityRepair(uint16(INVENTORY_SLOT_BAG_0 << 8) | slot, true, discount, false);
                spent += before - bot->GetMoney();
            }
            if (spent)
                Message(botAI, "Equipement repare : " + std::to_string(spent) + " pieces de cuivre.");
        }
    };
}

bool IsSelfbotInventoryManaged(PlayerbotAI* ai)
{
    return ai && IsSelfBot(ai->GetBot()) && ai->selfbotCycle.enabled;
}

bool SelfbotInventoryPauseRequested(PlayerbotAI* ai)
{
    return IsSelfbotInventoryManaged(ai) && ai->selfbotCycle.inventory && ai->selfbotCycle.inventory->pause;
}

bool UpdateSelfbotInventory(PlayerbotAI* ai, uint32 elapsed)
{
    if (!IsSelfbotInventoryManaged(ai))
        return false;
    Player* bot = ai->GetBot();
    auto& state = State(ai);
    if (state.pause)
        return true;
    // Combat, death recovery, casts and open loot retain priority over housekeeping.
    if (!bot->IsAlive() || bot->IsInCombat() || bot->IsNonMeleeSpellCast(false) ||
        bot->IsInFlight() || bot->GetLootGUID())
        return false;
    state.events.Update(elapsed);
    while (uint32 event = state.events.ExecuteEvent())
    {
        if (event == TripDeadline && state.busy)
            InventoryAction(ai).NextVendor();
        else if (event == CheckBags || event == MoveVendor)
            state.ready = true;
    }
    if (state.ready && !state.pause)
    {
        state.ready = false;
        InventoryAction(ai).Execute(Event());
        state.events.RescheduleEvent(CheckBags, Milliseconds(state.busy ? 1000 : 5000));
    }
    return state.busy || state.pause;
}

void BeginSelfbotCraftOutput(PlayerbotAI* ai, uint32 entry)
{
    auto& state = State(ai);
    state.beforeCraft.clear();
    for (Item* item : ai->GetInventoryItems())
        if (item->GetEntry() == entry)
            state.beforeCraft[item->GetGUID().GetRawValue()] = item->GetCount();
}

void RecordSelfbotCraftOutput(PlayerbotAI* ai, uint32 entry)
{
    auto& state = State(ai);
    for (Item* item : ai->GetInventoryItems())
    {
        if (item->GetEntry() != entry)
            continue;
        uint64 guid = item->GetGUID().GetRawValue();
        uint32 before = state.beforeCraft[guid];
        if (item->GetCount() <= before)
            continue;
        auto [found, added] = state.produced.try_emplace(guid, ProducedStack{entry, before, 0, before});
        if (found->second.expected != before)
            found->second = ProducedStack{entry, before, 0, before};
        found->second.produced += item->GetCount() - before;
        found->second.expected = item->GetCount();
    }
    state.beforeCraft.clear();
}
