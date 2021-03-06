#include <eosio/chain/exceptions.hpp>
#include <eosio/chain/resource_limits.hpp>
#include <eosio/chain/resource_limits_private.hpp>
#include <eosio/chain/transaction_metadata.hpp>
#include <eosio/chain/transaction.hpp>
#include <boost/tuple/tuple_io.hpp>
#include <eosio/chain/database_utils.hpp>
#include <algorithm>
#include <eosio/chain/stake.hpp>
#include <eosio/chain/account_object.hpp>
#include <eosio/chain/global_property_object.hpp>

#include <cyberway/chaindb/storage_payer_info.hpp>

namespace eosio { namespace chain { namespace resource_limits {
using namespace stake;

using cyberway::chaindb::cursor_kind;

using resource_index_set = table_set<
   resource_usage_table,
   resource_limits_state_table,
   resource_limits_config_table
>;

static_assert( config::rate_limiting_precision > 0, "config::rate_limiting_precision must be positive" );

static uint64_t update_elastic_limit(uint64_t current_limit, uint64_t average_usage, const elastic_limit_parameters& params) {
   uint64_t result = current_limit;
   if (average_usage > params.target) {
      result = safe_prop(result, params.decrease_rate.numerator, params.decrease_rate.denominator, false);
   } else {
      result = safe_prop(result, params.increase_rate.numerator, params.increase_rate.denominator, false);
   }

   return std::min(std::max(result, params.min), params.max);
}
void elastic_limit_parameters::set(uint64_t target_, uint64_t min_, uint64_t max_, uint32_t average_window_ms, uint16_t decrease_pct, uint16_t increase_pct) {
    EOS_ASSERT(decrease_pct < config::percent_100, resource_limit_exception, "incorrect elastic limit parameter 'decrease_pct'" );
    target = target_;
    min = min_;
    max = max_;
    periods = average_window_ms / config::block_interval_ms;
    EOS_ASSERT( periods > 0, resource_limit_exception, "elastic limit parameter 'periods' cannot be zero" );
    decrease_rate.numerator = config::percent_100 - decrease_pct;
    increase_rate.numerator = config::percent_100 + increase_pct;
}

void resource_limits_state_object::update_virtual_limit(const resource_limits_config_object& cfg, resource_id res) {
   virtual_limits[res] = update_elastic_limit(virtual_limits[res], block_usage_accumulators[res].average(), cfg.limit_parameters[res]);
}

void resource_limits_manager::add_indices() {
   resource_index_set::add_tables(_chaindb);
}

void resource_limits_manager::initialize_database() {
    auto& cfg = _chaindb.emplace<resource_limits_config_object>([](resource_limits_config_object& cfg){
        cfg.limit_parameters.resize(resources_num);
        cfg.account_usage_average_windows.resize(resources_num);
        for (size_t i = 0; i < resources_num; i++) {
            cfg.limit_parameters[i].set(
                config::default_target_virtual_limits[i],
                config::default_min_virtual_limits[i],
                config::default_max_virtual_limits[i],
                config::default_usage_windows[i],
                config::default_virtual_limit_decrease_pct[i],
                config::default_virtual_limit_increase_pct[i]
            );
            cfg.account_usage_average_windows[i] = config::default_account_usage_windows[i] / config::block_interval_ms;
        }
   });
   _chaindb.emplace<resource_limits_state_object>([&cfg](resource_limits_state_object& state) {
      state.block_usage_accumulators.resize(resources_num);
      state.pending_usage.resize(resources_num, 0);
      state.virtual_limits.resize(resources_num);

      for (size_t i = 0; i < resources_num; i++) {
         state.virtual_limits[i] = cfg.limit_parameters[i].max;
      }
   });

}

void resource_limits_manager::add_to_snapshot( const snapshot_writer_ptr& snapshot ) const {
   // TODO: Removed by CyberWay
}

void resource_limits_manager::read_from_snapshot( const snapshot_reader_ptr& snapshot ) {
   // TODO: Removed by CyberWay
}

storage_payer_info resource_limits_manager::get_storage_payer(uint32_t time_slot, account_name owner) {
    return {*this, owner, owner, time_slot};
}

void resource_limits_manager::initialize_account(const account_name& account, const storage_payer_info& payer) {
   _chaindb.emplace<resource_usage_object>(account.value, payer, [&]( resource_usage_object& bu ) {
      bu.accumulators.resize(resources_num);
   });
}

void resource_limits_manager::set_limit_params(const chain_config& chain_cfg) {
    const auto& config = _chaindb.get<resource_limits_config_object>();
    _chaindb.modify(config, [&](resource_limits_config_object& cfg) {

        for (size_t i = 0; i < resources_num; i++) {
            cfg.limit_parameters[i].set(
                chain_cfg.target_virtual_limits[i],
                chain_cfg.min_virtual_limits[i],
                chain_cfg.max_virtual_limits[i],
                chain_cfg.usage_windows[i],
                chain_cfg.virtual_limit_decrease_pct[i],
                chain_cfg.virtual_limit_increase_pct[i]
            );
            cfg.account_usage_average_windows[i] = chain_cfg.account_usage_windows[i] / config::block_interval_ms;
        }
   });
}

void resource_limits_manager::update_account_usage(const flat_set<account_name>& accounts, uint32_t time_slot) {
   const auto& config = _chaindb.get<resource_limits_config_object>();
   auto usage_table = _chaindb.get_table<resource_usage_object>();
   auto owner_idx = usage_table.get_index<by_id>();
   for( const auto& a : accounts ) {
      const auto& usage = owner_idx.get( a );
      usage_table.modify( usage, [&]( auto& bu ) {
          for (size_t i = 0; i < resources_num; i++) {
              bu.accumulators[i].add(0, time_slot, config.account_usage_average_windows[i]);
          }
      });
   }
}

void resource_limits_state_object::add_pending_delta(int64_t delta, const chain_config& chain_cfg, resource_id res) {
    static auto constexpr large_number_no_overflow = std::numeric_limits<int64_t>::max() / 2;
    delta = std::max(std::min(delta, large_number_no_overflow), -large_number_no_overflow);
    auto& pending = pending_usage[res];
    pending = std::max(std::min(pending + delta, large_number_no_overflow), -large_number_no_overflow);
    decltype(delta) max = chain_cfg.max_block_usage[res];
    EOS_ASSERT(pending <= max, block_resource_exhausted,
        "Block has insufficient resources(${res}): delta = ${delta}, new_pending = ${new_pending}, max = ${max}",
        ("res", static_cast<int>(res))("delta", delta)("new_pending", pending)("max", max));
}

void resource_limits_manager::add_transaction_usage(
    const flat_set<account_name>& accounts, const ratios& prices,
    uint64_t cpu_usage, uint64_t net_usage, uint64_t ram_usage,
    fc::time_point pending_block_time, bool validate
) {
   auto state_table = _chaindb.get_table<resource_limits_state_object>();
   const auto& state = state_table.get();
   const auto& config = _chaindb.get<resource_limits_config_object>();
   auto usage_table = _chaindb.get_table<resource_usage_object>();
   auto owner_idx = usage_table.get_index<by_id>();
   auto time_slot = block_timestamp_type(pending_block_time).slot;

   for( const auto& a : accounts ) {

      const auto& usage = owner_idx.get( a );

      usage_table.modify( usage, [&]( auto& bu ) {
          bu.accumulators[CPU].add(cpu_usage, time_slot, config.account_usage_average_windows[CPU]);
          bu.accumulators[NET].add(net_usage, time_slot, config.account_usage_average_windows[NET]);
          bu.accumulators[RAM].add(ram_usage, time_slot, config.account_usage_average_windows[RAM]);
      });
      if (validate) {
          // validate the resources available
          get_account_balance(pending_block_time, a, prices, true);
      }
   }
   // account for this transaction in the block and do not exceed those limits either
   const auto& chain_cfg = _chaindb.get<global_property_object>().configuration;
   state_table.modify(state, [&](resource_limits_state_object& rls) {
      rls.add_pending_delta(cpu_usage, chain_cfg, CPU);
      rls.add_pending_delta(net_usage, chain_cfg, NET);
      rls.add_pending_delta(ram_usage, chain_cfg, RAM);
   });
}

void resource_limits_manager::add_storage_usage(
   const flat_map<account_name, int64_t>& deltas, const ratios& prices, fc::time_point pending_block_time, bool validate
) {
   if( deltas.empty() ) {
       return;
   }

   symbol_code token_code { symbol(CORE_SYMBOL).to_symbol_code() };

   const stake_param_object* param = nullptr;
   const stake_stat_object*  stat  = nullptr;

   if (!(_chaindb.find<stake_param_object>(token_code.value, cursor_kind::OneRecord)) ||
       !(stat = _chaindb.find<stake_stat_object>(token_code.value, cursor_kind::OneRecord)) ||
       !stat->enabled || stat->total_staked == 0
   ) {
      return;
   }

   auto  time_slot = block_timestamp_type(pending_block_time).slot;
   auto& config = _chaindb.get<resource_limits_config_object>();

   int64_t delta = 0;
   for( const auto& a : deltas ) {
      if( 0 == a.second ) {
          continue;
      }
      delta += a.second;

      auto& usage = _chaindb.get<resource_usage_object>(a.first);
      _chaindb.modify( usage, [&]( auto& u ) {
          u.accumulators[STORAGE].add(a.second, time_slot, config.account_usage_average_windows[STORAGE]);
      });
      if( validate && (a.second > 0 || _validate_storage_price)) {
         get_account_balance(pending_block_time, a.first, prices, true);
      }
   }

   if ( 0 == delta ) {
       return;
   }

   auto  state_table = _chaindb.get_table<resource_limits_state_object>();
   auto& state = state_table.get();
   state_table.modify(state, [&](resource_limits_state_object& rls) {
      rls.add_pending_delta(delta, _chaindb.get<global_property_object>().configuration, STORAGE);
   });
}

void resource_limits_manager::add_storage_usage(const account_name& account, int64_t delta, uint32_t time_slot) {
   if (delta == 0) {
      return;
   }

   symbol_code token_code { symbol(CORE_SYMBOL).to_symbol_code() };

   const stake_param_object* param = nullptr;
   const stake_stat_object* stat = nullptr;

   if (_chaindb.get<account_object>(account).privileged || //assignments:
       !(_chaindb.find<stake_param_object>(token_code.value, cursor_kind::OneRecord)) ||
       !(stat  = _chaindb.find<stake_stat_object>(token_code.value, cursor_kind::OneRecord)) ||
       !stat->enabled || stat->total_staked == 0
   ) {

      return;
   }

   const auto& config = _chaindb.get<resource_limits_config_object>();
   auto state_table  = _chaindb.get_table<resource_limits_state_object>();
   const auto& state = state_table.get();
   state_table.modify(state, [&](resource_limits_state_object& rls) {
      rls.add_pending_delta(delta, _chaindb.get<global_property_object>().configuration, STORAGE);
   });
   auto& usage = _chaindb.get<resource_usage_object>(account);
   _chaindb.modify( usage, [&]( auto& u ) {
      u.accumulators[STORAGE].add(delta, time_slot, config.account_usage_average_windows[STORAGE]);
   });
}

std::vector<uint64_t> resource_limits_manager::get_account_usage(const account_name& account)const {
    const auto& config = _chaindb.get<resource_limits_config_object>();
    auto usage_index = _chaindb.get_index<resource_usage_object, by_id>();
    const auto& usage = usage_index.get(account);
    std::vector<uint64_t> ret(resources_num);

    for (size_t i = 0; i < resources_num; i++) {
        ret[i] = downgrade_cast<uint64_t>(integer_divide_ceil(
                            static_cast<uint128_t>(usage.accumulators[i].value_ex) * config.account_usage_average_windows[i],
                            static_cast<uint128_t>(config::rate_limiting_precision)));
    }
    return ret;
}

void resource_limits_manager::process_block_usage(uint32_t time_slot) {
   auto state_table = _chaindb.get_table<resource_limits_state_object>();
   const auto& s = state_table.get();
   const auto& config = _chaindb.get<resource_limits_config_object>();
   state_table.modify(s, [&](resource_limits_state_object& state){
      // apply pending usage, update virtual limits and reset the pending
      //block_usage_accumulators
      for (size_t i = 0; i < resources_num; i++) {
         state.block_usage_accumulators[i].add(state.pending_usage[i], time_slot, config.limit_parameters[i].periods);
         state.update_virtual_limit(config, static_cast<resource_id>(i));
         state.pending_usage[i] = 0;
      }
   });
}

uint64_t resource_limits_manager::get_virtual_block_limit(resource_id res) const {
   const auto& state = _chaindb.get<resource_limits_state_object>();
   return state.virtual_limits[res];
}

uint64_t resource_limits_manager::get_block_limit(resource_id res, const chain_config& chain_cfg) const {
   const auto& state = _chaindb.get<resource_limits_state_object>();
   const auto& config = _chaindb.get<resource_limits_config_object>();
   uint64_t usage = std::max(state.pending_usage[res], int64_t(0));
   uint64_t max = chain_cfg.max_block_usage[res];
   EOS_ASSERT(max >= usage, resource_limit_exception, "SYSTEM: incorrect usage");
   return max - usage;
}

ratios resource_limits_manager::get_pricelist() const {

    const auto& state  = _chaindb.get<resource_limits_state_object>();
    const auto& config = _chaindb.get<resource_limits_config_object>();

    std::vector<uint64_t> used_pct(resources_num);
    for (size_t i = 0; i < resources_num; i++) {
        used_pct[i] = std::max(safe_share_to_pct(state.block_usage_accumulators[i].average(), config.limit_parameters[i].target), config::min_resource_usage_pct);
    }
    uint64_t used_pct_sum = std::accumulate(used_pct.begin(), used_pct.end(), 0);

    ratios ret(resources_num, ratio{0ll, 1ll});

    symbol_code token_code { symbol(CORE_SYMBOL).to_symbol_code() };
    const stake_param_object* param = nullptr;
    const stake_stat_object* stat = nullptr;

    if ((param = _chaindb.find<stake_param_object>(token_code.value, cursor_kind::OneRecord)) &&
        (stat  = _chaindb.find<stake_stat_object>(token_code.value, cursor_kind::OneRecord)) &&
        stat->enabled && stat->total_staked != 0
    ) {
        EOS_ASSERT(stat->total_staked > 0, resource_limit_exception, "SYSTEM: incorrect total_staked");

        for (size_t i = 0; i < resources_num; i++) {
            auto virtual_capacity_in_window = static_cast<uint128_t>(state.virtual_limits[i]) * config.account_usage_average_windows[i];
            ret[i] = ratio {
                safe_prop_ceil<uint64_t>(stat->total_staked, used_pct[i], used_pct_sum),
                static_cast<uint64_t>(std::min(virtual_capacity_in_window, static_cast<uint128_t>(UINT64_MAX)))
            };
        }
    }

    return ret;
}

ratio resource_limits_manager::get_resource_usage_by_account_cost_ratio(account_name account, resource_id res) const {
    const auto resources_usage = get_account_usage(account);
    const auto price_list = get_pricelist();

    const auto requested_resource_price = price_list.at(res);
    const auto requested_resource_usage = resources_usage.at(res);

    uint64_t total_usage_cost = 0;
    for (size_t i = 0; i < resources_num; i++) {
        total_usage_cost += safe_prop_ceil(resources_usage[i], price_list[i].numerator, price_list[i].denominator);
    }

    return ratio{safe_prop_ceil(requested_resource_usage, requested_resource_price.numerator, requested_resource_price.denominator), total_usage_cost};
}

ratio resource_limits_manager::get_account_stake_ratio(fc::time_point pending_block_time, const account_name& account, bool update_state) {
    symbol_code token_code { symbol(CORE_SYMBOL).to_symbol_code() };

    const stake_param_object* param = nullptr;
    const stake_stat_object* stat = nullptr;

    if (_chaindb.get<account_object>(account).privileged || //assignments:
        !(param = _chaindb.find<stake_param_object>(token_code.value)) ||
        !(stat  = _chaindb.find<stake_stat_object>(token_code.value)) ||
        !stat->enabled || stat->total_staked == 0) {

        return {0,0};
    }

    EOS_ASSERT(stat->total_staked > 0, resource_limit_exception, "SYSTEM: incorrect total_staked");

    auto agents_table = _chaindb.get_table<stake_agent_object>();
    auto agents_idx = agents_table.get_index<stake_agent_object::by_key>();

    uint64_t staked = 0;
    auto agent = agents_idx.find(agent_key(token_code, account), cursor_kind::OneRecord);
    if (agent != agents_idx.end()) {
        if (update_state) {
            update_proxied(_chaindb, get_storage_payer(block_timestamp_type(pending_block_time).slot, account_name()),
                pending_block_time.sec_since_epoch(), token_code, account, false);
        }
        staked = agent->get_effective_stake();
    }

    return ratio{staked, static_cast<uint64_t>(stat->total_staked)};
}

uint64_t resource_limits_manager::get_used_resources_cost(account_name account, const std::vector<ratio>& prices, uint64_t max_cost) const {
    auto res_usage = get_account_usage(account);

    uint64_t cost = 0;
    for (size_t i = 0; i < resources_num; i++) {
        auto add = safe_prop_ceil(res_usage[i], prices[i].numerator, prices[i].denominator);
        cost = (UINT64_MAX - cost) > add ? cost + add : UINT64_MAX;
    }
    
    EOS_ASSERT(max_cost >= cost, account_resources_exceeded,
        "account ${a} has insufficient staked tokens (${s}).\n usage: cpu ${uc}, net ${un}, ram ${ur}, storage ${us}; \n prices: cpu ${pc}, net ${pn}, ram ${pr}, storage ${ps};\n cost ${c}",
        ("a", account)("s",max_cost)
        ("uc", res_usage[CPU])("un", res_usage[NET])("ur", res_usage[RAM])("us", res_usage[STORAGE])
        ("pc", prices   [CPU])("pn", prices   [NET])("pr", prices   [RAM])("ps", prices   [STORAGE])
        ("c", cost));

    return cost;
}

uint64_t resource_limits_manager::get_account_balance(fc::time_point pending_block_time, const account_name& account, const std::vector<ratio>& prices, bool update_state) {

    const auto account_stake_ratio = get_account_stake_ratio(pending_block_time, account, update_state);
    const auto staked = account_stake_ratio.numerator;
    const auto total_staked = account_stake_ratio.denominator;

    if (total_staked == 0) {
        return UINT64_MAX;
    }
    uint64_t cost = get_used_resources_cost(account, prices, update_state ? staked : UINT64_MAX);
    return (staked > cost) ? (staked - cost) : 0;
}

} } } /// eosio::chain::resource_limits
