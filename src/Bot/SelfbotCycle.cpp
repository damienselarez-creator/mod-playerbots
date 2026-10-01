#include "SelfbotCycle.h"
#include "SelfbotProfessions.h"
#include "SelfbotQuestSurvey.h"

#include "Chat.h"
#include "MotionMaster.h"
#include "NewRpgBaseAction.h"
#include "Playerbots.h"
#include "PlayerbotSpellRepository.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
    char const* PhaseName(uint32 phase)
    {
        static char const* names[] = {"quetes", "metiers", "reputations (quetes)"};
        return names[phase % 3];
    }

    std::filesystem::path StatePath(Player* player)
    {
        return std::filesystem::path("selfbot-cycle") / (std::to_string(player->GetGUID().GetRawValue()) + ".txt");
    }

    void Notify(PlayerbotAI* ai, std::string const& message)
    {
        ChatHandler(ai->GetBot()->GetSession()).SendSysMessage(("[Cycle] " + message).c_str());
        LOG_INFO("playerbots", "[SelfbotCycle] {}: {}", ai->GetBot()->GetName(), message);
    }

    bool Load(Player* player, SelfbotCycle& state)
    {
        std::ifstream input(StatePath(player));
        if (!input)
            return false;
        return state.Read(input);
    }

    void Configure(PlayerbotAI* ai)
    {
        ai->ChangeStrategy("-follow,-stay,-passive,-rpg,-travel,-maintenance,-move random,+new rpg,+grind,+loot,+gather",
                           BOT_STATE_NON_COMBAT);
        ai->ChangeStrategy("-passive", BOT_STATE_COMBAT);
        if (ai->selfbotCycle.phase == 1)
            ai->ChangeStrategy("-grind", BOT_STATE_NON_COMBAT);
        ai->rpgInfo.ChangeToIdle();
        if (ai->selfbotCycle.phase == 1)
            ai->rpgInfo.ChangeToRest();
        ai->selfbotCycle.configured = true;
    }

    bool HasReputationReward(Quest const* quest)
    {
        for (uint32 i = 0; i < QUEST_REPUTATIONS_COUNT; ++i)
            if (quest->RewardFactionId[i] &&
                (quest->RewardFactionValueId[i] > 0 || quest->RewardFactionValueIdOverride[i] > 0))
                return true;
        return false;
    }

    class CycleAction : public NewRpgBaseAction
    {
    public:
        explicit CycleAction(PlayerbotAI* ai) : NewRpgBaseAction(ai, "selfbot cycle") { }

        bool Execute(Event) override
        {
            SelfbotCycle& state = botAI->selfbotCycle;
            if (!bot->IsAlive() || bot->IsInCombat() || bot->IsNonMeleeSpellCast(false) || bot->IsInFlight())
                return false;

            if (state.phase == 1)
                return ExecuteSelfbotProfessions(botAI);

            if (botAI->rpgInfo.GetStatus() == RPG_DO_QUEST)
            {
                auto* questData = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data);
                if (!questData)
                    return false;
                uint32 id = questData->questId;
                auto itr = bot->getQuestStatusMap().find(id);
                uint64 fingerprint = id;
                if (itr != bot->getQuestStatusMap().end())
                {
                    fingerprint = fingerprint * 31 + itr->second.Status;
                    for (uint32 count : itr->second.CreatureOrGOCount)
                        fingerprint = fingerprint * 31 + count;
                    for (uint32 count : itr->second.ItemCount)
                        fingerprint = fingerprint * 31 + count;
                }
                if (fingerprint != state.progress)
                {
                    state.progress = fingerprint;
                    state.stalled = 0;
                }
                if (state.stalled < 600000)
                    return false;
                botAI->lowPriorityQuest.insert(id);
                Notify(botAI, "Quete sans progression depuis 10 minutes : autre objectif, sans abandon.");
                botAI->rpgInfo.ChangeToIdle();
            }

            if (ContinueSelfbotQuestSurvey(botAI))
            {
                state.stalled = 0;
                return true;
            }

            if (state.decisionElapsed < 15000)
                return false;
            state.decisionElapsed = 0;
            uint32 selected = 0;
            int32 bestScore = -100000;
            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 id = bot->GetQuestSlotQuestId(slot);
                Quest const* quest = sObjectMgr->GetQuestTemplate(id);
                if (!quest || !IsQuestCapableDoing(quest) || botAI->lowPriorityQuest.count(id))
                    continue;
                if (state.phase == 2 && !HasReputationReward(quest))
                    continue;
                std::vector<POIInfo> points;
                if (!GetQuestPOIPosAndObjectiveIdx(id, points, true))
                    continue;
                int32 score = (bot->GetQuestStatus(id) == QUEST_STATUS_COMPLETE ? 10000 : 0) -
                              std::abs(int32(bot->GetLevel()) - bot->GetQuestLevel(quest));
                if (score > bestScore)
                {
                    bestScore = score;
                    selected = id;
                }
            }
            if (selected)
            {
                botAI->rpgInfo.ChangeToDoQuest(selected, sObjectMgr->GetQuestTemplate(selected));
                state.stalled = 0;
                state.progress = 0;
                state.objective = "Quete " + std::to_string(selected);
                Notify(botAI, state.objective + " / " + PhaseName(state.phase));
                return true;
            }
            if (ExecuteSelfbotQuestSurvey(botAI))
            {
                state.stalled = 0;
                return true;
            }
            state.objective = SelfbotQuestSurveyStatus(botAI);
            if (state.stalled >= 300000)
            {
                Notify(botAI, "Parcours sans nouvel objectif accessible; plage suivante. " + state.objective);
                state.remaining = 0;
                return true;
            }
            return false;
        }

    };
}

void SaveSelfbotCycle(PlayerbotAI* ai)
{
    auto& state = ai->selfbotCycle;
    if (!state.loaded)
        return;
    try
    {
        auto path = StatePath(ai->GetBot());
        std::filesystem::create_directories(path.parent_path());
        auto temporary = path;
        temporary += ".tmp";
        std::ofstream output(temporary, std::ios::trunc);
        state.Write(output);
        output.flush();
        if (!output)
            throw std::runtime_error("ecriture impossible");
        output.close();
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("remplacement impossible");
#else
        std::filesystem::rename(temporary, path);
#endif
    }
    catch (std::exception const& error)
    {
        Notify(ai, std::string("Sauvegarde du cycle impossible : ") + error.what());
    }
}

void ResumeSelfbotCycle(PlayerbotAI* ai)
{
    if (IsSelfBot(ai->GetBot()) && Load(ai->GetBot(), ai->selfbotCycle) && ai->selfbotCycle.enabled)
    {
        Configure(ai);
        Notify(ai, std::string("Reprise : ") + PhaseName(ai->selfbotCycle.phase));
    }
}

void UpdateSelfbotCycle(PlayerbotAI* ai, uint32 elapsed)
{
    auto& state = ai->selfbotCycle;
    if (!state.enabled || !IsSelfBot(ai->GetBot()))
        return;
    if (!state.configured)
        Configure(ai);
    Player* bot = ai->GetBot();
    bool safe = bot->IsAlive() && !bot->IsInCombat() && !bot->IsNonMeleeSpellCast(false) && !bot->IsInFlight();
    state.decisionElapsed = std::min<uint32>(15000, state.decisionElapsed + elapsed);
    state.stalled = std::min<uint32>(600000, state.stalled + elapsed);
    if (state.Tick(elapsed, safe))
    {
        bot->StopMoving();
        Configure(ai);
        ai->lowPriorityQuest.clear();
        state.objective = "Selection du prochain objectif";
        state.decisionElapsed = 15000;
        Notify(ai, std::string("Nouvelle plage : ") + PhaseName(state.phase));
        SaveSelfbotCycle(ai);
    }
    UpdateSelfbotProfessions(ai, elapsed);
    UpdateSelfbotQuestSurvey(ai, elapsed);
    state.saveElapsed += elapsed;
    if (state.saveElapsed >= 60000)
    {
        state.saveElapsed = 0;
        SaveSelfbotCycle(ai);
    }
}

bool ExecuteSelfbotCycle(PlayerbotAI* ai)
{
    return CycleAction(ai).Execute(Event());
}

bool HandleSelfbotCycleCommand(ChatHandler* handler, char const* args)
{
    Player* player = handler->GetPlayer();
    if (!player)
        return false;
    auto* session = player->GetSession();
    if (!SelfbotManualSessionAllowed(session->IsHeadless(), !session->IsSocketClosed(), session->IsLoggingOut()))
    {
        handler->SendSysMessage("[Cycle] Une connexion manuelle active est obligatoire.");
        return true;
    }
    std::istringstream input(args ? args : "");
    std::string command;
    std::string hours;
    input >> command >> hours;
    PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
    if (command == "status" || command.empty())
    {
        SelfbotCycle state;
        bool found = ai ? ai->selfbotCycle.loaded : Load(player, state);
        if (ai)
            state = ai->selfbotCycle;
        if (!found || !state.enabled)
            handler->SendSysMessage("[Cycle] Inactif. .playerbots cycle start 2 (ou 4)");
        else
        {
            std::string message = std::string("[Cycle] ") + (ai ? "Actif : " : "Pause : ") +
                PhaseName(state.phase) + ", reste " + std::to_string((state.remaining + 59999) / 60000) +
                " min. " + state.objective;
            handler->SendSysMessage(message.c_str());
        }
        return true;
    }
    if (ai && !IsSelfBot(ai->GetBot()))
    {
        handler->SendSysMessage("[Cycle] Commande reservee au personnage que vous jouez.");
        return true;
    }
    if (command == "survey")
    {
        if (!ai || !ai->selfbotCycle.enabled)
        {
            handler->SendSysMessage("[Exploration] Activez votre cycle pour consulter le parcours de la zone.");
            return true;
        }
        if (hours == "reset")
        {
            if (player->IsInCombat() || player->IsInFlight())
            {
                handler->SendSysMessage("[Exploration] Reinitialisez le parcours hors combat et hors vol.");
                return true;
            }
            ResetSelfbotQuestSurvey(ai);
        }
        handler->SendSysMessage(("[Exploration] " + SelfbotQuestSurveyStatus(ai)).c_str());
        return true;
    }
    if (command == "pause" || command == "stop")
    {
        if (!ai)
        {
            if (command == "stop")
            {
                std::error_code error;
                std::filesystem::remove(StatePath(player), error);
                if (error)
                {
                    handler->SendSysMessage("[Cycle] Impossible de supprimer le programme sauvegarde.");
                    return true;
                }
            }
        }
        else
        {
            if (command == "stop")
                ai->selfbotCycle.enabled = false;
            delete ai;
        }
        player->InterruptNonMeleeSpells(false);
        player->AttackStop();
        player->GetMotionMaster()->Clear();
        player->StopMoving();
        handler->SendSysMessage("[Cycle] Controle manuel retabli.");
        return true;
    }
    if (command == "professions" && ai && ai->selfbotCycle.enabled)
    {
        uint32 first = 0, second = 0;
        std::istringstream selection(args);
        selection >> command >> first >> second;
        if (!IsSelfbotPrimaryProfession(first) ||
            (second && (!IsSelfbotPrimaryProfession(second) || first == second)))
        {
            handler->SendSysMessage("[Cycle] professions <id metier> [id second metier]. Aucun metier oublie.");
            return true;
        }
        ai->selfbotCycle.profession1 = first;
        ai->selfbotCycle.profession2 = second;
        ai->selfbotCycle.professions.reset();
        SaveSelfbotCycle(ai);
        handler->SendSysMessage("[Cycle] Metiers a apprendre enregistres; metiers existants conserves.");
        return true;
    }
    if (command == "next" && ai && ai->selfbotCycle.enabled)
    {
        ai->selfbotCycle.remaining = 0;
        handler->SendSysMessage("[Cycle] Changement demande a la fin du combat/sort/trajet en vol.");
        return true;
    }
    if (command != "start" || (hours != "2" && hours != "4"))
    {
        handler->SendSysMessage("[Cycle] start 2|4, status, next, pause, stop. Reprise : .playerbots bot self");
        return true;
    }
    if (!sPlayerbotAIConfig.selfBotLevel ||
        (sPlayerbotAIConfig.selfBotLevel == 1 && player->GetSession()->GetSecurity() < SEC_GAMEMASTER))
    {
        handler->SendSysMessage("[Cycle] Le reglage SelfBotLevel n'autorise pas votre compte.");
        return true;
    }
    if (player->GetMap()->IsDungeon() || player->InBattleground() || player->IsInCombat())
    {
        handler->SendSysMessage("[Cycle] Demarrez le test en exterieur, hors combat.");
        return true;
    }
    if (!ai)
    {
        PlayerbotsMgr::instance().AddPlayerbotData(player, true);
        ai = GET_PLAYERBOT_AI(player);
        ai->SetMaster(player);
    }
    SelfbotCycle previous;
    if (ai->selfbotCycle.loaded)
        previous = ai->selfbotCycle;
    else
        Load(player, previous);
    ai->selfbotCycle = SelfbotCycle();
    ai->selfbotCycle.surveyProgress = previous.surveyProgress;
    ai->selfbotCycle.profession1 = previous.profession1;
    ai->selfbotCycle.profession2 = previous.profession2;
    ai->selfbotCycle.loaded = true;
    ai->selfbotCycle.enabled = true;
    ai->selfbotCycle.duration = hours == "2" ? 7200000 : 14400000;
    ai->selfbotCycle.remaining = ai->selfbotCycle.duration;
    Configure(ai);
    SaveSelfbotCycle(ai);
    Notify(ai, "Test demarre : quetes, metiers, reputations. Plages de " + hours + " h actives.");
    return true;
}
