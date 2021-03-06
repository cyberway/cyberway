#pragma once
#include "genesis_exception.hpp"
#include "genesis_info.hpp"
#include "export_info.hpp"
#include "supply_distributor.hpp"
#include <eosio/chain/genesis_state.hpp>
#include <fc/crypto/sha256.hpp>
#include <boost/filesystem.hpp>


namespace cyberway { namespace genesis {

using namespace eosio::chain;
namespace bfs = boost::filesystem;
using mvo = fc::mutable_variant_object;

struct contract_abicode {
    bool update;
    bool privileged;
    bytes abi;
    bytes code;
    fc::sha256 code_hash;
    fc::sha256 abi_hash;
};
using contracts_map = std::map<name, contract_abicode>;

class genesis_create final {
public:
    genesis_create(const genesis_create&) = delete;
    genesis_create();
    ~genesis_create();

    void read_state(const bfs::path& state_file, bool dump_closed_permlinks);
    void write_genesis(const bfs::path& out_file, const genesis_info&, const genesis_state&, const contracts_map&);
    void dump_closed_permlinks(const bfs::path& out_file);

    // ee interface
    const genesis_info& get_info() const;
    const genesis_state& get_conf() const;
    const contracts_map& get_contracts() const;
    const export_info& get_exp_info() const;
    name name_by_idx(acc_idx idx) const;
    supply_distributor get_gbg_to_golos_converter() const;

private:
    struct genesis_create_impl;
    std::unique_ptr<genesis_create_impl> _impl;
};


}} // cyberway::genesis
