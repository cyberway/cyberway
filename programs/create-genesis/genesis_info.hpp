#pragma once
#include "genesis_exception.hpp"
#include "posting_rules.hpp"
#include <eosio/chain/types.hpp>
#include <eosio/chain/asset.hpp>
#include <eosio/chain/authority.hpp>
#include <fc/reflect/reflect.hpp>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>

namespace cyberway { namespace genesis {

using namespace eosio::chain;
namespace bfs = boost::filesystem;

struct genesis_info {

    struct file_hash {
        bfs::path path;
        fc::sha256 hash;
    };

    struct permission {
        permission_name name;
        fc::optional<unsigned> threshold;
        fc::optional<permission_name> parent;   // defaults: "" for "owner" permission; "owner" for "active"; "active" for others; numeric id if adding permission to existing account
        std::string key;                // can use "INITIAL" and "key/weight"; only empty "key" can co-exist with non-empty "keys" and vice-versa
        std::vector<string> keys;       // can use "INITIAL" and "key/weight"
        std::vector<string> accounts;   // can use "name@permission" and "name@permission/weight"
        std::vector<string> waits;      // can use "wait/weight"

        void init() {
            if (key.length() > 0) {
                keys.emplace_back(key);
                key = "";
            }
        }

        permission_name get_parent() const {
            if (parent) {
                return *parent;
            }
            return name == N(owner) ? N() : name == N(active) ? N(owner) : N(active);
        }

        std::vector<key_weight> key_weights(const public_key_type& initial_key) const {
            std::vector<key_weight> r;
            for (const auto& key: keys) {
                auto k_weight = split_weight(key);
                r.emplace_back(key_weight{k_weight.first == "INITIAL" ? initial_key : public_key_type(k_weight.first), k_weight.second});
            }
            return r;
        }

        std::pair<string,weight_type> split_weight(const string& str) const {
            std::vector<string> parts;
            split(parts, str, boost::algorithm::is_any_of("/"));
            unsigned weight = parts.size() == 1 ? 1 : boost::lexical_cast<weight_type>(parts[1]);
            return std::make_pair(parts[0], weight);
        }

        std::vector<permission_level_weight> perm_levels(account_name code, std::function<account_name(const std::string&)> name_resolver) const {
            std::vector<permission_level_weight> r;
            for (const auto& a: accounts) {
                std::vector<string> parts;
                auto perm_weight = split_weight(a);
                split(parts, perm_weight.first, boost::algorithm::is_any_of("@"));
                auto acc = parts[0].size() == 0 ? code : name_resolver(parts[0]);
                auto perm = account_name(parts.size() == 1 ? "" : parts[1].c_str());
                r.emplace_back(permission_level_weight{permission_level{acc, perm}, perm_weight.second});
            }
            return r;
        }

        std::vector<wait_weight> wait_weights() const {
            std::vector<wait_weight> r;
            for (const auto& w: waits) {
                auto a = split_weight(w);
                r.emplace_back(wait_weight{boost::lexical_cast<uint32_t>(a.first), a.second});
            }
            return r;
        }

        authority make_authority(const public_key_type& initial_key, account_name code, std::function<account_name(const std::string&)> name_resolver) const {
            return authority(threshold ? *threshold : 1, key_weights(initial_key), perm_levels(code, name_resolver), wait_weights());
        }
    };

    struct account {
        account_name name;
        fc::optional<bool> update;
        fc::optional<bool> privileged;
        std::vector<permission> permissions;
        fc::optional<file_hash> abi;
        fc::optional<file_hash> code;
        fc::optional<asset> sys_balance;
        fc::optional<asset> sys_staked;
        fc::optional<string> prod_key;
    };

    bfs::path state_file;   // ? file_hash
    bfs::path genesis_json;
    std::vector<account> accounts;

    struct auth_link {
        using names_pair = std::pair<name,name>;
        string permission;              // account@permission
        std::vector<string> links;      // each link is "contract:action" or "contract:*" (simple "contract" also works)

        names_pair get_permission() const {
            std::vector<string> parts;
            split(parts, permission, boost::algorithm::is_any_of("@"));
            return names_pair{name(parts[0]), name(parts[1])};
        }

        std::vector<names_pair> get_links() const {
            std::vector<names_pair> r;
            for (const auto& l: links) {
                std::vector<string> parts;
                split(parts, l, boost::algorithm::is_any_of(":"));
                name action{parts.size() > 1 ? string_to_name(parts[1].c_str()) : 0};
                r.emplace_back(names_pair{name(parts[0]), action});
            }
            return r;
        }
    };
    std::vector<auth_link> auth_links;

    struct transit_account_authority {
        name name;
        std::string username;
        std::vector<permission> permissions;
    };
    std::vector<transit_account_authority> transit_account_authorities;

    struct delegateuse_item {
        std::string from;
        std::string to;
        std::string quantity;
    };
    std::vector<delegateuse_item> delegateuse;

    struct table {
        struct row {
            string scope;   // can be name/symbol/symbol_code
            name payer;
            uint64_t pk;
            variant data;

            uint64_t get_scope() const {
                name maybe_name{string_to_name(scope.c_str())};
                if (maybe_name.to_string() == scope) {
                    return name{scope}.value;
                } else if (scope.find(',') != string::npos) {
                    return symbol::from_string(scope).value();
                } else {
                    return symbol::from_string("0,"+scope).to_symbol_code().value;
                }
            }
        };
        account_name code;
        name table;
        string abi_type;
        std::vector<row> rows;
    };
    std::vector<table> tables;

    // parameters
    struct golos_config {
        std::string domain;

        struct golos_names {
            account_name issuer;
            account_name control;
            account_name emission;
            account_name vesting;
            account_name posting;
            account_name social;
            account_name charge;
            account_name memo;
        } names;

        struct recovery_params {
            uint32_t wait_days;
        } recovery;

        int64_t max_supply = 0;
        int64_t sys_max_supply = 0;

        struct start_trx_params {
            uint16_t delay_minutes = 60;
            uint16_t expiration_hours = 3;
        } start_trx;
    } golos;

    struct stake_params {
        bool enabled = false;
        std::vector<uint8_t> max_proxies;
        int64_t depriving_window;
        int64_t min_own_staked_for_election = 0;
    };
    struct funds_share {
        account_name name;
        // value multiplied to base CYBER supply. use num/denom to avoid floating point
        int64_t numerator;
        int64_t denominator;
    };

    struct parameters {
        uint8_t initial_prod_count = 0;
        stake_params stake;
        posting_rules posting_rules;
        std::vector<funds_share> funds;
    } params;

    struct ee_parameters {
        struct ee_history_days {
            uint16_t transfers = 30;
            uint16_t withdraws = 30;
            uint16_t rewards = 30;
        } history_days;
    } ee_params;

    void init() {
        for (auto& a: accounts) {
            for (auto& p: a.permissions) {
                EOS_ASSERT(p.key.length() == 0 || p.keys.size() == 0, genesis_exception,
                    "Account ${a} permission can't contain both `key` and `keys` fields at the same time", ("a",a.name));
                p.init();
            }
        }
        for (auto& a: transit_account_authorities) {
            for (auto& p: a.permissions) {
                EOS_ASSERT(p.key.length() == 0 || p.keys.size() == 0, genesis_exception,
                    "Transit account ${a} permission can't contain both `key` and `keys` fields at the same time", ("a",a.name));
                p.init();
            }
        }
    }
};

}} // cyberway::genesis

FC_REFLECT(cyberway::genesis::genesis_info::file_hash, (path)(hash))
FC_REFLECT(cyberway::genesis::genesis_info::permission, (name)(threshold)(parent)(key)(keys)(accounts)(waits))
FC_REFLECT(cyberway::genesis::genesis_info::account, (name)(update)(privileged)(permissions)(abi)(code)(sys_balance)(sys_staked)(prod_key))
FC_REFLECT(cyberway::genesis::genesis_info::auth_link, (permission)(links))
FC_REFLECT(cyberway::genesis::genesis_info::transit_account_authority, (username)(permissions))
FC_REFLECT(cyberway::genesis::genesis_info::delegateuse_item, (from)(to)(quantity))
FC_REFLECT(cyberway::genesis::genesis_info::table::row, (scope)(payer)(pk)(data))
FC_REFLECT(cyberway::genesis::genesis_info::table, (code)(table)(abi_type)(rows))
FC_REFLECT(cyberway::genesis::genesis_info::golos_config::golos_names,
    (issuer)(control)(emission)(vesting)(posting)(social)(charge)(memo))
FC_REFLECT(cyberway::genesis::genesis_info::golos_config::recovery_params, (wait_days))
FC_REFLECT(cyberway::genesis::genesis_info::golos_config::start_trx_params, (delay_minutes)(expiration_hours))
FC_REFLECT(cyberway::genesis::genesis_info::golos_config,
    (domain)(names)(recovery)(max_supply)(sys_max_supply)(start_trx))
FC_REFLECT(cyberway::genesis::genesis_info::stake_params,
    (enabled)(max_proxies)(depriving_window)(min_own_staked_for_election))
FC_REFLECT(cyberway::genesis::genesis_info::funds_share, (name)(numerator)(denominator))
FC_REFLECT(cyberway::genesis::genesis_info::parameters, (initial_prod_count)(stake)(posting_rules)(funds))
FC_REFLECT(cyberway::genesis::genesis_info::ee_parameters::ee_history_days, (transfers)(withdraws)(rewards))
FC_REFLECT(cyberway::genesis::genesis_info::ee_parameters, (history_days))
FC_REFLECT(cyberway::genesis::genesis_info, (state_file)(genesis_json)(accounts)(auth_links)(transit_account_authorities)(delegateuse)(tables)(golos)(params)(ee_params))
