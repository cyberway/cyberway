#pragma once
#include "golos_dump_container.hpp"
#include "golos_operations.hpp"
#include "event_engine_genesis.hpp"
#include "map_objects.hpp"
#include "../genesis_create.hpp"

#include <chainbase/chainbase.hpp>
#include <fc/exception/exception.hpp>
#include <boost/filesystem.hpp>

namespace cyberway { namespace genesis { namespace ee {

namespace bfs = boost::filesystem;
using mvo = fc::mutable_variant_object;
using cyberway::genesis::genesis_info;
using namespace cyberway::golos::ee;

class genesis_ee_builder final {
public:
    genesis_ee_builder(const genesis_ee_builder&) = delete;
    genesis_ee_builder(const genesis_create&, const std::string& shared_file, uint32_t last_block);
    ~genesis_ee_builder();

    void read_operation_dump(const bfs::path& in_dump_dir);
    void build(const bfs::path& out_dir);
private:
    golos_dump_header read_header(bfs::ifstream& in, const bfs::path& file);
    template<typename Operation>
    bool read_operation(bfs::ifstream& in, Operation& op);

    void process_delete_comments();
    void process_comments();
    void process_rewards();
    void process_votes();
    void process_reblogs();
    void process_delete_reblogs();
    void process_transfers();
    void process_withdraws();
    void process_rewards_history();
    void process_follows();
    void process_account_metas();

    void write_contracts_abis();
    void build_votes(std::vector<vote_info>& votes, uint64_t msg_hash, operation_number msg_created);
    void build_reblogs(std::vector<reblog_info>& reblogs, uint64_t msg_hash, operation_number msg_created, bfs::ifstream& dump_reblogs);
    comment_operation get_comment(const comment_header& comment);
    void write_messages();
    void write_transfers();
    void write_withdraws();
    void write_delegations();
    void write_rewards_history();
    void write_pinblocks();
    void write_accounts();
    void write_witnesses();
    void write_funds();
    void write_balance_converts();

    bfs::ifstream dump_delete_comments;
    bfs::ifstream dump_comments;
    bfs::ifstream dump_rewards;
    bfs::ifstream dump_votes;
    bfs::ifstream dump_reblogs;
    bfs::ifstream dump_delete_reblogs;
    bfs::ifstream dump_transfers;
    bfs::ifstream dump_vesting_withdraws;
    bfs::ifstream dump_author_rewards;
    bfs::ifstream dump_benefactor_rewards;
    bfs::ifstream dump_curation_rewards;
    bfs::ifstream dump_delegation_rewards;
    bfs::ifstream dump_follows;
    bfs::ifstream dump_metas;

    bfs::path in_dump_dir_;

    const genesis_create& genesis_;
    const genesis_info& info_;
    const export_info& exp_info_;

    event_engine_genesis out_;
    uint32_t last_block_;
    chainbase::database maps_;
};

} } } // cyberway::genesis::ee
