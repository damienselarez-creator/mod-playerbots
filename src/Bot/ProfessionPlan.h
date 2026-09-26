#ifndef PLAYERBOT_PROFESSION_PLAN_H
#define PLAYERBOT_PROFESSION_PLAN_H

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <vector>

namespace SelfbotCraft
{
    struct Recipe
    {
        uint32_t spell = 0;
        uint32_t item = 0;
        uint32_t output = 1;
        std::map<uint32_t, uint32_t> reagents;
        std::map<uint32_t, uint32_t> tools;
    };

    struct Step
    {
        // spell == 0 means acquire quantity items; otherwise cast quantity times.
        uint32_t spell = 0;
        uint32_t item = 0;
        uint32_t quantity = 0;
    };

    struct Plan
    {
        bool valid = false;
        double cost = 0;
        std::vector<Step> steps;
        std::map<uint32_t, uint32_t> inventory;
    };

    // Builds a bounded, ordered dependency plan with a shared inventory ledger.
    // A negative source price means no usable acquisition source is known.
    class Planner
    {
    public:
        Planner(std::vector<Recipe> const& recipes, std::function<double(uint32_t)> sourceCost)
            : _recipes(recipes), _sourceCost(std::move(sourceCost)) { }

        Plan Build(Recipe const& root, std::map<uint32_t, uint32_t> inventory) const
        {
            Plan result;
            result.inventory = std::move(inventory);
            std::set<uint32_t> ancestors;
            uint32_t work = 0;
            result.valid = Make(root, 1, result, ancestors, work);
            if (!result.valid)
                result.steps.clear();
            return result;
        }

    private:
        bool Need(uint32_t item, uint32_t quantity, Plan& plan,
                  std::set<uint32_t>& ancestors, uint32_t& work) const
        {
            if (++work > 2048 || quantity > 1000 || ancestors.size() > 8)
                return false;
            uint32_t have = std::min(quantity, plan.inventory[item]);
            plan.inventory[item] -= have;
            quantity -= have;
            if (!quantity)
                return true;

            Plan best;
            double price = _sourceCost(item);
            if (price >= 0)
            {
                best = plan;
                best.valid = true;
                best.steps.push_back({0, item, quantity});
                best.cost += price * quantity;
            }
            for (auto const& recipe : _recipes)
            {
                if (recipe.item != item || !recipe.output || ancestors.count(recipe.spell))
                    continue;
                Plan candidate = plan;
                uint32_t casts = (quantity + recipe.output - 1) / recipe.output;
                if (!Make(recipe, casts, candidate, ancestors, work))
                    continue;
                candidate.inventory[item] -= quantity;
                if (!best.valid || candidate.cost < best.cost)
                {
                    best = std::move(candidate);
                    best.valid = true;
                }
            }
            if (!best.valid || best.steps.size() > 128)
                return false;
            plan = std::move(best);
            return true;
        }

        bool Make(Recipe const& recipe, uint32_t casts, Plan& plan,
                  std::set<uint32_t>& ancestors, uint32_t& work) const
        {
            if (!recipe.spell || !recipe.item || !recipe.output || !casts || casts > 200 ||
                recipe.reagents.empty() || ancestors.size() >= 8 || ancestors.count(recipe.spell))
                return false;
            ancestors.insert(recipe.spell);
            bool valid = true;
            for (auto const& tool : recipe.tools)
            {
                if (!Need(tool.first, 1, plan, ancestors, work))
                {
                    valid = false;
                    break;
                }
                ++plan.inventory[tool.first]; // Tools are retained by the real spell cast.
            }
            for (auto const& reagent : recipe.reagents)
            {
                if (!valid)
                    break;
                uint64_t total = uint64_t(reagent.second) * casts;
                if (!reagent.first || !reagent.second || total > 1000 ||
                    !Need(reagent.first, uint32_t(total), plan, ancestors, work))
                {
                    valid = false;
                    break;
                }
            }
            ancestors.erase(recipe.spell);
            if (!valid || plan.steps.size() >= 128 || uint64_t(recipe.output) * casts > 1000)
                return false;
            plan.steps.push_back({recipe.spell, recipe.item, casts});
            plan.inventory[recipe.item] += recipe.output * casts;
            plan.cost += casts;
            return true;
        }

        std::vector<Recipe> const& _recipes;
        std::function<double(uint32_t)> _sourceCost;
    };
}

#endif
