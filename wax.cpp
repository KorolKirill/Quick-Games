#include <eosio/eosio.hpp>
#include <eosio/asset.hpp>
#include <eosio/system.hpp>
#include <math.h>
#include <eosio/singleton.hpp>
#include "util/atomicassets-interface.hpp"
#include "util/delphioracle-interface.hpp"
#include "util/randomness_provider.cpp"

using namespace std;
using namespace eosio;

static constexpr name DEFAULT_MARKETPLACE_CREATOR = name("fees.atomic");

CONTRACT wax : public eosio::contract
{
public:
    using contract::contract;

    wax(eosio::name receiver, eosio::name code, eosio::datastream<const char *> ds)
        : contract(receiver, code, ds)
    {
        // constructor, can't call function on delploying.
    }

    ACTION init(name fee_receiver, string game_name)
    {
        require_auth(get_self());
        config.get_or_create(get_self(), config_s{});
        config_s current_config = config.get();
        current_config.fee_receiver = fee_receiver;
        current_config.game_name = game_name;
        config.set(current_config, get_self());
    }

    ACTION setconfig(name fee_receiver, string game_name)
    {
        require_auth(get_self());
        config_s current_config = config.get();
        current_config.fee_receiver = fee_receiver;
        current_config.game_name = game_name;
        config.set(current_config, get_self());
    }

    ACTION setcyclecfg(uint16_t cycle,
                       uint32_t start_time,
                       asset cost_to_play)
    {
        require_auth(get_self());
        check(cycle > 0, "Cycle should be a positive number");
        check(start_time > now(), "Can't set the timer to past time");
        check(cost_to_play.amount > 0 && cost_to_play.symbol == symbol("WAX", 8), "Wrong asset");
        auto cycles_config_itr = cycles_config.find(cycle);
        if (cycles_config_itr == cycles_config.end())
        {
            cycles_config.emplace(get_self(), [&](auto &entry)
                                  {
            entry.cycle = cycle;
            entry.start_time = start_time; 
            entry.cost_to_play = cost_to_play; });
        }
        else
        {
            cycles_config.modify(cycles_config_itr, get_self(), [&](auto &entry)
                                 { 
                            entry.start_time = start_time; 
                            entry.cost_to_play = cost_to_play; });
        }
    }

    /**
     * Sets the version for the config table
     *
     * @required_auth The contract itself
     */
    ACTION setversion(string new_version)
    {
        require_auth(get_self());
        config_s current_config = config.get();
        current_config.version = new_version;
        config.set(current_config, get_self());
    }

    ACTION log(string text)
    {
    }

    // Join the cycle.
    // amount is limited to 10 per trx
    ACTION joingame(name player, uint16_t cycle, uint64_t amount, name refferal)
    {
        require_auth(player);
        uint8_t max_amount = 10;
        check(cycle > 0 && amount > 0 && amount <= max_amount, "Function params are wrong. ");
        auto cycles_config_itr = cycles_config.require_find(cycle, "Wrong cycle.");
        check(now() >= cycles_config_itr->start_time, "Cycle hasn't started yet.");
        auto balance_itr = balances.require_find(player.value, "You haven't deposited the tokens");
        check(balance_itr->quantity.amount >= cycles_config_itr->cost_to_play.amount * amount, "You don't have enough balance.");
        config_s current_config = config.get();

        // start distribution/joining
        cycles_t cycles_table = get_cycle_table(cycle);
        for (uint64_t i = 0; i < amount; i++)
        {
            internal_decrease_balance(player, cycles_config_itr->cost_to_play);

            auto availavle_index = cycles_table.available_primary_key();
            if (availavle_index == 1 || availavle_index == 0)
            {
                cycles_table.emplace(player, [&](auto &entry)
                                     {entry.index = 1;
                    entry.player = player; });
                log_a.send("First entry.");
            }
            else
            {
                auto availavle_index = cycles_table.available_primary_key();
                cycles_table.emplace(player, [&](auto &entry)
                                     {
                entry.index = availavle_index;
                entry.player = player; });

                // game fee - our 7-10 % commission for playing.
                auto service_fee = cycles_config_itr->cost_to_play;
                service_fee.amount = ceil((double)service_fee.amount / 100 * current_config.game_fee);
                transferWAX(current_config.fee_receiver, service_fee);
                // referal fee
                auto referal_fee = cycles_config_itr->cost_to_play;
                referal_fee.amount = floor((double)referal_fee.amount / 100 * current_config.referal_bonus);
                if (isRefferalPlaying(refferal, cycle) && refferal != player)
                {
                    transferWAX(refferal, referal_fee);
                    internal_add_statistic(refferal, referal_fee, asset(0, WAX_symbol));
                }
                else
                {
                    transferWAX(current_config.fee_receiver, referal_fee);
                    log_a.send("Refferal is not found or hasn't joined the cycle.");
                }
                // parent player payment

                auto parent_index = get_parent_index(availavle_index);
                auto cycles_table_parent_itr = cycles_table.require_find(parent_index, "Something went wrong");
                auto parent_payment = cycles_config_itr->cost_to_play;
                parent_payment.amount = floor((double)parent_payment.amount / 100 * (100 - current_config.game_fee - current_config.referal_bonus));
                transferWAX(cycles_table_parent_itr->player, parent_payment);

                internal_add_statistic(cycles_table_parent_itr->player, asset(0, WAX_symbol), parent_payment);
            }
        }
    }

    bool isRefferalPlaying(name refferal, uint16_t cycle)
    {
        cycles_t cycles_table = get_cycle_table(cycle);
        auto indexByName = cycles_table.get_index<"player"_n>();
        auto itrToolByName = indexByName.lower_bound(refferal.value);
        if (itrToolByName != indexByName.end())
        {
            return true;
        }

        return false;
    }

    void transferWAX(name to, asset quantity)
    {
        // todo add custom MEMO with link to the website.
        std::string memo = "";

        action{
            permission_level{get_self(), "active"_n},
            name("eosio.token"),
            "transfer"_n,
            std::make_tuple(get_self(), to, quantity, memo)}
            .send();
    }

    /**
     * This function is called when a transfer receipt from any token contract is sent to the atomicmarket contract
     * It handels deposits and adds the transferred tokens to the sender's balance table row
     */
    [[eosio::on_notify("eosio.token::transfer")]] void receive_token_transfer(name from, name to, asset quantity, string memo)
    {
        if (to != get_self())
        {
            return;
        }
        check(quantity.symbol == symbol("WAX", 8), "The transferred token is not supported");

        if (memo == "deposit")
        {
            internal_add_balance(from, quantity);
        }
        else
        {
        }
    }

private:
    symbol WAX_symbol =  symbol(symbol_code("WAX"), 8);

    uint64_t get_parent_index(uint64_t current_index)
    {
        if (current_index % 2 == 0)
        {
            return current_index / 2;
        }
        else
        {
            return (current_index - 1) / 2;
        }
    }

    // creates table if not already created.
    void internal_add_statistic(name user, asset invite_profit, asset game_profit)
    {
        auto statistic_itr = statistic.find(user.value);
        if (statistic_itr == statistic.end())
        {
            statistic.emplace(user, [&](auto &entry)
                                         { 
                                            entry.player = user; 
                                            entry.invite_profit = invite_profit; 
                                            entry.game_profit = game_profit; 
                                         });
        }
        else
        {
            statistic.modify(statistic_itr, user, [&](auto &entry)
                                        {
                         entry.invite_profit += invite_profit; 
                        entry.game_profit += game_profit;  });
        }
    }

    void internal_add_balance(
        name owner,
        asset quantity)
    {
        if (quantity.amount == 0)
        {
            return;
        }
        check(quantity.amount > 0, "Can't add negative balances");

        auto balance_itr = balances.find(owner.value);

        if (balance_itr == balances.end())
        {
            // No balance table row exists yet
            balances.emplace(get_self(), [&](auto &_balance)
                             {
            _balance.owner = owner;
            _balance.quantity = quantity; });
        }
        else
        {
            // A balance table row already exists for owner
            asset balance = balance_itr->quantity;
            balance.amount += quantity.amount;

            balances.modify(balance_itr, get_self(), [&](auto &_balance)
                            { _balance.quantity = balance; });
        }
    }

    void internal_decrease_balance(
        name owner,
        asset quantity)
    {
        auto balance_itr = balances.require_find(owner.value,
                                                 "The specified account does not have a balance table row");
        asset current_balance = balance_itr->quantity;
        check(current_balance.amount >= quantity.amount, "Not enough balance");
        current_balance.amount -= quantity.amount;
        if (current_balance.amount == 0)
        {
            balances.erase(balance_itr);
        }
        else
        {
            balances.modify(balance_itr, same_payer, [&](auto &_balance)
                            { _balance.quantity = current_balance; });
        }
    }

    uint32_t now()
    {
        return current_time_point().sec_since_epoch();
    }

    TABLE balances_s
    {
        name owner;
        asset quantity;

        uint64_t primary_key() const { return owner.value; };
    };

    typedef multi_index<name("balances"), balances_s> balances_t;

    TABLE cycles_s
    {
        uint64_t index; // available_primary_key()
        name player;
        uint64_t get_key2() const { return player.value; };
        uint64_t primary_key() const { return index; };
    };
    typedef multi_index<"cycles"_n, cycles_s,
                        indexed_by<"player"_n, const_mem_fun<cycles_s, uint64_t, &cycles_s::get_key2>>>
        cycles_t;

    TABLE cycles_config_s
    {
        uint16_t cycle;
        uint32_t start_time;
        asset cost_to_play;
        uint64_t primary_key() const { return cycle; };
    };
    typedef multi_index<name("cyclescfg"), cycles_config_s> cycles_config_t;

    TABLE statistic_s
    {
        name player;
        asset invite_profit;
        asset game_profit;
        uint64_t primary_key() const { return player.value; };
    };
    typedef multi_index<name("statistic"), statistic_s> statistic_t;

    TABLE config_s
    {
        string version = "0.0.1";
        name fee_receiver = name("");
        string game_name = "game_name";
        uint16_t referal_bonus = 15;
        uint16_t game_fee = 10;
        name atomicassets_account = atomicassets::ATOMICASSETS_ACCOUNT;
        name delphioracle_account = delphioracle::DELPHIORACLE_ACCOUNT;
    };

    typedef singleton<name("config"), config_s> config_t;
    // https://github.com/EOSIO/eosio.cdt/issues/280
    typedef multi_index<name("config"), config_s> config_t_for_abi;

    statistic_t statistic = statistic_t(get_self(), get_self().value);
    balances_t balances = balances_t(get_self(), get_self().value);
    config_t config = config_t(get_self(), get_self().value);
    cycles_config_t cycles_config = cycles_config_t(get_self(), get_self().value);

    cycles_t get_cycle_table(uint16_t cycle)
    {
        return cycles_t(get_self(), cycle);
    }

    using log_action_w = action_wrapper<"log"_n, &wax::log>;
    log_action_w log_a =  log_action_w(get_self(), {get_self(), "active"_n});
};
