#include "genesis_ee_builder.hpp"
#include "diff_match_patch.h"
#include <boost/locale/encoding_utf.hpp>
#include <fc/utf8.hpp>
#include "../config.hpp"
#include "../genesis_generate_name.hpp"

using boost::locale::conv::utf_to_utf;

#define MEGABYTE 1024*1024

// Comments:
// 8000000 * 480 = 4.0 GB
// +
// 8000000 * 144 * 5 (votes on comment) = 6.0 GB
// +
// 1000000 * 128 (reblogs on comment) = 0.2 GB
// Follows:
// 2300000 * 160 = 0.4 GB
#define MAP_FILE_SIZE uint64_t(25*1024)*MEGABYTE

namespace cyberway { namespace genesis { namespace ee {

genesis_ee_builder::genesis_ee_builder(
    const genesis_create& genesis, const std::string& shared_file, uint32_t last_block)
    :   genesis_(genesis), info_(genesis.get_info()), exp_info_(genesis.get_exp_info()),
        last_block_(last_block), maps_(shared_file, chainbase::database::read_write, MAP_FILE_SIZE) {
    maps_.add_index<comment_header_index>();
    maps_.add_index<vote_header_index>();
    maps_.add_index<reblog_header_index>();
    maps_.add_index<follow_header_index>();
    maps_.add_index<account_metadata_index>();
}

genesis_ee_builder::~genesis_ee_builder() {
}

golos_dump_header genesis_ee_builder::read_header(bfs::ifstream& in, const bfs::path& file) {
    EOS_ASSERT(!in_dump_dir_.empty() && bfs::exists(file), file_not_found_exception,
        "Operation dump file not found.");

    in.open(file);

    golos_dump_header h;
    in.read((char*)&h, sizeof(h));
    EOS_ASSERT(std::string(h.magic) == golos_dump_header::expected_magic, ee_genesis_exception,
        "Unknown format of the operation dump file.");
    EOS_ASSERT(h.version == golos_dump_header::expected_version, ee_genesis_exception,
        "Wrong version of the operation dump file.");
    return h;
}

template<typename Operation>
bool genesis_ee_builder::read_operation(bfs::ifstream& in, Operation& op) {
    auto op_offset = in.tellg();
    op = Operation(); // Some types unpacking without clearing
    fc::raw::unpack(in, op);
    op.offset = op_offset;

    if (!in) {
        in.clear(); // Clear EOF flag to allow reuse stream
        return false;
    }

    if (op.num.first > last_block_) {
        return false;
    }
    return true;
}

std::wstring utf8_to_wstring(const std::string& str) {
    return utf_to_utf<wchar_t>(str.c_str(), str.c_str() + str.size());
}

std::string wstring_to_utf8(const std::wstring& str) {
    return utf_to_utf<char>(str.c_str(), str.c_str() + str.size());
}

void genesis_ee_builder::process_delete_comments() {
    std::cout << "-> Reading comment deletions..." << std::endl;

    try {
        read_header(dump_delete_comments, in_dump_dir_ / "delete_comments");
    } catch (file_not_found_exception& ex) {
        wlog("No comment deletions file");
        return;
    }

    const auto& comments = maps_.get_index<comment_header_index, by_hash>();

    delete_comment_operation op;
    while (read_operation(dump_delete_comments, op)) {
        auto comment = comments.find(op.hash);
        if (comment != comments.end()) {
            maps_.modify(*comment, [&](auto& c) {
                c.last_delete_op = op.num;
            });
            continue;
        }

        maps_.create<comment_header>([&](auto& c) {
            c.hash = op.hash;
            c.last_delete_op = op.num;
            c.parent_hash = 1; // mark as deleted
        });
    }
}

void genesis_ee_builder::process_comments() {
    std::cout << "-> Reading comments..." << std::endl;

    try {
        read_header(dump_comments, in_dump_dir_ / "comments");
    } catch (file_not_found_exception& ex) {
        wlog("No comments file");
        return;
    }

    const auto& comments = maps_.get_index<comment_header_index, by_hash>();

    comment_operation op;
    while (read_operation(dump_comments, op)) {
        uint64_t parent_hash = 0;
        if (op.parent_author.size() != 0) {
            auto parent = std::string(op.parent_author) + "/" + op.parent_permlink;
            parent_hash = fc::hash64(parent.c_str(), parent.length());
        }

        auto fill_optionals = [&](auto& c) {
            if (op.title.size()) {
                c.title_offset = op.offset;
            }
            if (op.valid_meta) {
                c.meta_offset = op.offset;
            }
        };

        auto comment = comments.find(op.hash);
        if (comment != comments.end()) {
            if (comment->last_delete_op > op.num) {
                continue;
            }

            bool patch = false;
            try {
                if (diff_match_patch<std::wstring>().patch_fromText(utf8_to_wstring(op.body)).size()) {
                    patch = true;
                }
            } catch (...) {
            }

            maps_.modify(*comment, [&](auto& c) {
                c.parent_hash = parent_hash;
                if (!patch) {
                    c.offsets.clear();
                }
                c.offsets.emplace_back(op.offset);
                fill_optionals(c);
                c.create_op = op.num;
                if (c.created == fc::time_point_sec::min()) {
                    c.created = op.timestamp;
                }
            });
            continue;
        }

        maps_.create<comment_header>([&](auto& c) {
            c.hash = op.hash;
            c.parent_hash = parent_hash;
            c.offsets.emplace_back(op.offset);
            fill_optionals(c);
            c.create_op = op.num;
            c.created = op.timestamp;
        });
    }
}

void genesis_ee_builder::process_rewards() {
    std::cout << "-> Reading rewards..." << std::endl;

    try {
        read_header(dump_rewards, in_dump_dir_ / "total_comment_rewards");
    } catch (file_not_found_exception& ex) {
        wlog("No rewards file");
        return;
    }

    const auto& comments = maps_.get_index<comment_header_index, by_hash>();

    total_comment_reward_operation op;
    while (read_operation(dump_rewards, op)) {
        auto comment = comments.find(op.hash);
        if (comment != comments.end() && op.num > comment->last_delete_op) {
            maps_.modify(*comment, [&](auto& c) {
                c.author_reward += op.author_reward.get_amount();
                c.benefactor_reward += op.benefactor_reward.get_amount();
                c.curator_reward += op.curator_reward.get_amount();
                c.net_rshares = op.net_rshares;
            });
        }
    }
}

void genesis_ee_builder::process_votes() {
    std::cout << "-> Reading votes..." << std::endl;

    try {
        read_header(dump_votes, in_dump_dir_ / "votes");
    } catch (file_not_found_exception& ex) {
        wlog("No votes file");
        return;
    }

    const auto& votes = maps_.get_index<vote_header_index, by_hash_voter>();

    vote_operation op;
    while (read_operation(dump_votes, op)) {
        auto vote = votes.find(std::make_tuple(op.hash, op.voter));
        if (vote != votes.end()) {
            maps_.modify(*vote, [&](auto& v) {
                v.op_num = op.num;
                v.weight = op.weight;
                v.rshares = op.rshares;
                v.timestamp = op.timestamp;
            });
            continue;
        }

        maps_.create<vote_header>([&](auto& v) {
            v.hash = op.hash;
            v.voter = op.voter;
            v.op_num = op.num;
            v.weight = op.weight;
            v.rshares = op.rshares;
            v.timestamp = op.timestamp;
        });
    }
}

void genesis_ee_builder::process_reblogs() {
    std::cout << "-> Reading reblogs..." << std::endl;

    try {
        read_header(dump_reblogs, in_dump_dir_ / "reblogs");
    } catch (file_not_found_exception& ex) {
        wlog("No reblogs file");
        return;
    }

    const auto& reblogs = maps_.get_index<reblog_header_index, by_hash_account>();

    reblog_operation op;
    while (read_operation(dump_reblogs, op)) {
        auto reblog = reblogs.find(std::make_tuple(op.hash, op.account));
        if (reblog != reblogs.end()) {
            maps_.modify(*reblog, [&](auto& r) {
                r.op_num = op.num;
                r.offset = op.offset;
            });
            continue;
        }

        maps_.create<reblog_header>([&](auto& r) {
            r.hash = op.hash;
            r.account = op.account;
            r.op_num = op.num;
            r.offset = op.offset;
        });
    }
}

void genesis_ee_builder::process_delete_reblogs() {
    std::cout << "-> Reading reblog deletions..." << std::endl;

    try {
        read_header(dump_delete_reblogs, in_dump_dir_ / "delete_reblogs");
    } catch (file_not_found_exception& ex) {
        wlog("No reblog deletions file");
        return;
    }

    const auto& reblogs = maps_.get_index<reblog_header_index, by_hash_account>();

    delete_reblog_operation op;
    while (read_operation(dump_delete_reblogs, op)) {
        auto reblog = reblogs.find(std::make_tuple(op.hash, op.account));
        if (op.num > reblog->op_num) {
            maps_.remove(*reblog);
        }
    }
}

void genesis_ee_builder::process_transfers() {
    std::cout << "-> Reading transfers..." << std::endl;

    try {
        read_header(dump_transfers, in_dump_dir_ / "transfers");
    } catch (file_not_found_exception& ex) {
        wlog("No transfers file");
        return;
    }
}

void genesis_ee_builder::process_withdraws() {
    std::cout << "-> Reading withdraws..." << std::endl;

    try {
        read_header(dump_vesting_withdraws, in_dump_dir_ / "vesting_withdraws");
    } catch (file_not_found_exception& ex) {
        wlog("No vesting withdraws file");
        return;
    }
}

void genesis_ee_builder::process_rewards_history() {
    std::cout << "-> Reading rewards history..." << std::endl;

    try {
        read_header(dump_author_rewards, in_dump_dir_ / "author_rewards");
    } catch (file_not_found_exception& ex) {
        wlog("No author rewards file");
    }

    try {
        read_header(dump_benefactor_rewards, in_dump_dir_ / "benefactor_rewards");
    } catch (file_not_found_exception& ex) {
        wlog("No benefactor rewards file");
    }

    try {
        read_header(dump_curation_rewards, in_dump_dir_ / "curation_rewards");
    } catch (file_not_found_exception& ex) {
        wlog("No curation rewards file");
    }

    try {
        read_header(dump_delegation_rewards, in_dump_dir_ / "delegation_rewards");
    } catch (file_not_found_exception& ex) {
        wlog("No delegation rewards file");
    }
}

void genesis_ee_builder::process_follows() {
    std::cout << "-> Reading follows..." << std::endl;

    try {
        read_header(dump_follows, in_dump_dir_ / "follows");
    } catch (file_not_found_exception& ex) {
        wlog("No follows file");
        return;
    }

    const auto& follows = maps_.get_index<follow_header_index, by_pair>();

    follow_operation op;
    while (read_operation(dump_follows, op)) {
        bool ignores = false;
        if (op.what & (1 << ignore)) {
            ignores = true;
        }

        auto follow = follows.find(std::make_tuple(op.follower, op.following));
        if (follow != follows.end()) {
            if (op.what == 0) {
                maps_.remove(*follow);
                continue;
            }

            maps_.modify(*follow, [&](auto& f) {
                f.ignores = ignores;
            });
            continue;
        }

        if (op.what == 0) {
            continue;
        }

        maps_.create<follow_header>([&](auto& f) {
            f.follower = op.follower;
            f.following = op.following;
            f.ignores = ignores;
        });
    }
}

void genesis_ee_builder::process_account_metas() {
    std::cout << "-> Reading account metas..." << std::endl;

    try {
        read_header(dump_metas, in_dump_dir_ / "account_metas");
    } catch (file_not_found_exception& ex) {
        wlog("No account metas file");
        return;
    }

    const auto& meta_index = maps_.get_index<account_metadata_index, by_account>();

    account_metadata_operation op;
    while (read_operation(dump_metas, op)) {
        auto meta_itr = meta_index.find(op.account);
        if (meta_itr != meta_index.end()) {
            maps_.modify(*meta_itr, [&](auto& meta) {
                meta.offset = op.offset;
            });
            continue;
        }

        maps_.create<account_metadata>([&](auto& meta) {
            meta.account = op.account;
            meta.offset = op.offset;
        });
    }
}

void genesis_ee_builder::read_operation_dump(const bfs::path& in_dump_dir) {
    in_dump_dir_ = in_dump_dir;

    std::cout << "Reading operation dump from " << in_dump_dir_ << "..." << std::endl;

    process_delete_comments();
    process_comments();
    process_rewards();
    process_votes();
    process_reblogs();
    process_delete_reblogs();
    process_transfers();
    process_withdraws();
    process_rewards_history();
    process_follows();
    process_account_metas();
}

void genesis_ee_builder::write_contracts_abis() {
    std::cout << "-> Writing ABIs..." << std::endl;
    auto& out = out_.get_serializer(event_engine_genesis::contracts);
    out.start_section(config::system_account_name, N(setabi), "setabi");
    for (const auto& acc: genesis_.get_contracts()) if (acc.second.abi.size()) {
        out.emplace<setabi_info>([&](auto& r) {
            r.account = acc.first;
            r.abi = acc.second.abi;
        });
    }
}

void genesis_ee_builder::build_votes(std::vector<vote_info>& votes, uint64_t msg_hash, operation_number msg_created) {
    const auto& vote_idx = maps_.get_index<vote_header_index, by_hash_voter>();

    auto vote_itr = vote_idx.lower_bound(msg_hash);
    for (; vote_itr != vote_idx.end() && vote_itr->hash == msg_hash; ++vote_itr) {
        auto& vote = *vote_itr;

        if (vote.op_num < msg_created) {
            continue;
        }

        votes.emplace_back([&](auto& v) {
            v.voter = generate_name(vote.voter);
            v.weight = vote.weight;
            v.time = vote.timestamp;
            v.rshares = vote.rshares;
        }, 0);

        std::sort(votes.begin(), votes.end(), [&](const auto& a, const auto& b) {
            return a.rshares > b.rshares;
        });
    }
}

void genesis_ee_builder::build_reblogs(std::vector<reblog_info>& reblogs, uint64_t msg_hash, operation_number msg_created, bfs::ifstream& dump_reblogs) {
    if (!dump_reblogs.is_open()) {
        return;
    }

    const auto& reblog_idx = maps_.get_index<reblog_header_index, by_hash_account>();

    auto reblog_itr = reblog_idx.lower_bound(msg_hash);
    for (; reblog_itr != reblog_idx.end() && reblog_itr->hash == msg_hash; ++reblog_itr) {
        auto& reblog = *reblog_itr;

        if (reblog.op_num < msg_created) {
            continue;
        }

        dump_reblogs.seekg(reblog.offset);
        reblog_operation op;
        read_operation(dump_reblogs, op);

        reblogs.emplace_back([&](auto& r) {
            r.account = generate_name(reblog.account);
            r.title = op.title;
            r.body = op.body;
            r.time = op.timestamp;
        }, 0);
    }
}

comment_operation genesis_ee_builder::get_comment(const comment_header& comment) {
    comment_operation op;
    std::string body;

    dump_comments.seekg(comment.offsets[0]);
    read_operation(dump_comments, op);
    body = op.body;

    for (auto i = 1; i < comment.offsets.size(); ++i) {
        dump_comments.seekg(comment.offsets[i]);
        read_operation(dump_comments, op);

        diff_match_patch<std::wstring> dmp;
        auto patch = dmp.patch_fromText(utf8_to_wstring(op.body));
        auto patched_body = wstring_to_utf8(dmp.patch_apply(patch, utf8_to_wstring(body)).first);
        if (!fc::is_utf8(patched_body)) {
            body = fc::prune_invalid_utf8(patched_body);
        } else {
            body = patched_body;
        }
    }

    op.body = body;

    if (comment.title_offset) {
        comment_operation opt;
        dump_comments.seekg(comment.title_offset);
        read_operation(dump_comments, opt);
        op.title = opt.title;
    }

    if (comment.meta_offset) {
        comment_operation opt;
        dump_comments.seekg(comment.meta_offset);
        read_operation(dump_comments, opt);
        op.tags = opt.tags;
        op.language = opt.language;
    }

    return op;
}

void genesis_ee_builder::write_messages() {
    if (!dump_comments.is_open()) {
        return;
    }

    std::cout << "-> Writing messages..." << std::endl;
    auto& out = out_.get_serializer(event_engine_genesis::messages);
    out.start_section(info_.golos.names.posting, N(message), "message_info");

    auto to_gls = genesis_.get_gbg_to_golos_converter();

    const auto& comment_idx = maps_.get_index<comment_header_index, by_parent_hash>();

    std::function<void(uint64_t)> build_children = [&](uint64_t parent_hash) {
        auto comment_itr = comment_idx.lower_bound(parent_hash);
        for (; comment_itr != comment_idx.end() && comment_itr->parent_hash == parent_hash; ++comment_itr) {
            auto& comment = *comment_itr;
            comment_operation op = get_comment(comment);

            out.emplace<comment_info>([&](auto& c) {
                c.parent_author = generate_name(op.parent_author);
                c.parent_permlink = op.parent_permlink;
                c.author = generate_name(op.author);
                c.permlink = op.permlink;
                c.title = op.title;
                c.body = op.body;
                c.tags = op.tags;
                c.language = op.language;
                c.created = comment.created;
                c.last_update = op.timestamp;
                c.net_rshares = comment.net_rshares;
                c.author_reward = asset(comment.author_reward, symbol(GLS));
                c.benefactor_reward = asset(comment.benefactor_reward, symbol(GLS));
                c.curator_reward = asset(comment.curator_reward, symbol(GLS));
                build_votes(c.votes, comment.hash, comment.last_delete_op);
                build_reblogs(c.reblogs, comment.hash, comment.last_delete_op, dump_reblogs);
                const auto active_itr = exp_info_.active_comments.find(comment.hash);
                if (active_itr != exp_info_.active_comments.end()) {
                    auto& active = active_itr->second;
                    c.archived = false;
                    for (const auto& b: active.beneficiaries) {
                        c.benefics_prcnt += b.weight;
                    }
                    c.rewardweight = active.reward_weight;
                    c.max_payout = to_gls.convert(active.max_accepted_payout);
                    c.curators_prcnt = active.curation_rewards_percent;
                    c.tokenprop = active.percent_steem_dollars / 2;
                    c.net_rshares = active.net_rshares;
                }
            });

            build_children(comment.hash);
        }
    };

    build_children(0);
}

void genesis_ee_builder::write_transfers() {
    if (!dump_transfers.is_open()) {
        return;
    }

    std::cout << "-> Writing transfers..." << std::endl;
    auto& out = out_.get_serializer(event_engine_genesis::transfers);
    out.start_section(config::token_account_name, N(transfer), "transfer");

    auto start_time = genesis_.get_conf().initial_timestamp;
    start_time -= fc::days(info_.ee_params.history_days.transfers);

    transfer_operation op;
    while (read_operation(dump_transfers, op)) {
        if (op.timestamp < start_time) {
            continue;
        }

        out.emplace<transfer_info>([&](auto& t) {
            t.from = generate_name(op.from);
            t.to = generate_name(op.to);
            t.quantity = op.amount;
            t.memo = op.memo;
            t.to_vesting = op.to_vesting;
            t.time = op.timestamp;
        });
    }
}

void genesis_ee_builder::write_withdraws() {
    if (!dump_vesting_withdraws.is_open()) {
        return;
    }

    std::cout << "-> Writing withdraws..." << std::endl;
    auto& out = out_.get_serializer(event_engine_genesis::withdraws);
    out.start_section(info_.golos.names.vesting, N(withdraw), "withdraw");

    auto start_time = genesis_.get_conf().initial_timestamp;
    start_time -= fc::days(info_.ee_params.history_days.withdraws);

    fill_vesting_withdraw_operation op;
    while (read_operation(dump_vesting_withdraws, op)) {
        if (op.timestamp < start_time) {
            continue;
        }

        out.emplace<withdraw_info>([&](auto& t) {
            t.from = generate_name(op.from_account);
            t.to = generate_name(op.to_account);
            t.quantity = op.deposited;
            t.time = op.timestamp;
        });
    }
}

void genesis_ee_builder::write_delegations() {
    std::cout << "-> Writing delegations..." << std::endl;
    auto& out = out_.get_serializer(event_engine_genesis::delegations);
    out.start_section(info_.golos.names.vesting, N(delegate), "delegate");

    for (auto& d : exp_info_.delegations) {
        out.insert(d);
    }
}

void genesis_ee_builder::write_rewards_history() {
    auto& out = out_.get_serializer(event_engine_genesis::rewards);

    auto start_time = genesis_.get_conf().initial_timestamp;
    start_time -= fc::days(info_.ee_params.history_days.rewards);

    const auto& comments = maps_.get_index<comment_header_index, by_hash>();
    auto comment_by_op = [&](auto& op) {
        comment_operation cop;
        auto cmt = comments.find(op.hash);
        if (cmt == comments.end()) {
            return cop;
        }
        dump_comments.seekg(cmt->offsets.back());
        read_operation(dump_comments, cop); // has incorrect body without patches but runs faster
        return cop;
    };

    if (dump_author_rewards.is_open()) {
        std::cout << "-> Writing author rewards..." << std::endl;
        out.start_section(info_.golos.names.posting, N(authreward), "author_reward");

        author_reward_operation op;
        while (read_operation(dump_author_rewards, op)) {
            if (op.timestamp < start_time) {
                continue;
            }

            auto cop = comment_by_op(op);

            out.emplace<author_reward>([&](auto& r) {
                r.author = generate_name(op.author);
                r.permlink = op.permlink;
                r.parent_author = generate_name(cop.parent_author);
                r.parent_permlink = cop.parent_permlink;
                r.sbd_and_steem_payout = op.sbd_and_steem_in_golos;
                r.vesting_payout = op.vesting_payout_in_golos;
                r.time = op.timestamp;
            });
        }
    }

    if (dump_curation_rewards.is_open()) {
        std::cout << "-> Writing curation rewards..." << std::endl;
        out.start_section(info_.golos.names.posting, N(curreward), "curation_reward");

        curation_reward_operation op;
        while (read_operation(dump_curation_rewards, op)) {
            if (op.timestamp < start_time) {
                continue;
            }

            auto cop = comment_by_op(op);

            out.emplace<curation_reward>([&](auto& r) {
                r.curator = generate_name(op.curator);
                r.reward = op.reward_in_golos;
                r.author = generate_name(op.comment_author);
                r.permlink = op.comment_permlink;
                r.parent_author = generate_name(cop.parent_author);
                r.parent_permlink = cop.parent_permlink;
                r.time = op.timestamp;
            });
        }
    }

    if (dump_benefactor_rewards.is_open()) {
        std::cout << "-> Writing benefactor rewards..." << std::endl;
        out.start_section(info_.golos.names.posting, N(benreward), "benefactor_reward");

        comment_benefactor_reward_operation op;
        while (read_operation(dump_benefactor_rewards, op)) {
            if (op.timestamp < start_time) {
                continue;
            }

            auto cop = comment_by_op(op);

            out.emplace<benefactor_reward>([&](auto& r) {
                r.benefactor = generate_name(op.benefactor);
                r.author = generate_name(op.author);
                r.permlink = op.permlink;
                r.parent_author = generate_name(cop.parent_author);
                r.parent_permlink = cop.parent_permlink;
                r.reward = op.reward_in_golos;
                r.time = op.timestamp;
            });
        }
    }

    if (dump_delegation_rewards.is_open()) {
        std::cout << "-> Writing delegation rewards..." << std::endl;
        out.start_section(info_.golos.names.posting, N(delreward), "delegation_reward");

        delegation_reward_operation op;
        while (read_operation(dump_delegation_rewards, op)) {
            if (op.timestamp < start_time) {
                continue;
            }

            out.emplace<delegation_reward>([&](auto& r) {
                r.delegator = generate_name(op.delegator);
                r.delegatee = generate_name(op.delegatee);
                r.reward = op.vesting_shares_in_golos;
                r.time = op.timestamp;
            });
        }
    }
}

void genesis_ee_builder::write_pinblocks() {
    if (!dump_follows.is_open()) {
        return;
    }
    std::cout << "-> Writing pinblocks..." << std::endl;
    const auto& follow_index = maps_.get_index<follow_header_index, by_id>();

    auto& out = out_.get_serializer(event_engine_genesis::pinblocks);
    out.start_section(info_.golos.names.social, N(pin), "pin");

    for (const auto& follow : follow_index) {
        if (follow.ignores) {
            continue;
        }
        out.emplace<pin_info>([&](auto& p) {
            p.pinner = generate_name(follow.follower);
            p.pinning = generate_name(follow.following);
        });
    }

    out.start_section(info_.golos.names.social, N(block), "block");

    for (const auto& follow : follow_index) {
        if (!follow.ignores) {
            continue;
        }
        out.emplace<block_info>([&](auto& b) {
            b.blocker = generate_name(follow.follower);
            b.blocking = generate_name(follow.following);
        });
    }
}

void genesis_ee_builder::write_accounts() {
    std::cout << "-> Writing accounts..." << std::endl;
    auto& out = out_.get_serializer(event_engine_genesis::accounts);
    out.start_section(config::system_account_name, N(domain), "domain_info");

    const auto app = info_.golos.names.issuer;
    out.insert(mvo
        ("owner", app)
        ("linked_to", app)
        ("name", info_.golos.domain)
    );

    const auto& meta_index = maps_.get_index<account_metadata_index, by_account>();

    out.start_section(config::system_account_name, N(account), "account_info");

    for (auto& a : exp_info_.account_infos) {
        auto acc = a.second;
        acc["json_metadata"] = "";
        if (dump_metas) {
            auto meta = meta_index.find(account_name_type(acc["name"].as_string()));
            if (meta != meta_index.end()) {
                dump_metas.seekg(meta->offset);
                account_metadata_operation op;
                read_operation(dump_metas, op);

                acc["json_metadata"] = op.json_metadata;
            } else {
                acc["json_metadata"] = "{created_at: 'GENESIS'}";
            }
        }
        out.insert(acc);
    }
}

void genesis_ee_builder::write_witnesses() {
    std::cout << "-> Writing witnesses..." << std::endl;
    auto& out = out_.get_serializer(event_engine_genesis::witnesses);
    out.start_section(info_.golos.names.control, N(witnessstate), "witnessstate");
    for (auto& w : exp_info_.witnesses) {
        auto wtn = w.second;
        fc::flat_set<name> votes;
        auto v = exp_info_.witness_votes.find(w.first);
        if (v != exp_info_.witness_votes.end()) {
            votes = v->second;
        }
        out.insert(wtn("votes", votes));
    }
}

void genesis_ee_builder::write_funds() {
    std::cout << "-> Writing funds..." << std::endl;

    auto& out = out_.get_serializer(event_engine_genesis::funds);
    out.start_section(config::token_account_name, N(currency), "currency_stats");

    for (auto& cs : exp_info_.currency_stats) {
        out.insert(cs);
    }

    out.start_section(config::token_account_name, N(balance), "balance_event");

    for (auto& be : exp_info_.balance_events) {
        out.insert(be);
    }

    out.start_section(info_.golos.names.vesting, N(stat), "vesting_supply");

    out.insert(exp_info_.vesting_supply);
}

void genesis_ee_builder::write_balance_converts() {
    std::cout << "-> Writing genesis balance conversions..." << std::endl;
    auto& out = out_.get_serializer(event_engine_genesis::balance_conversions);
    out.start_section(config::token_account_name, N(genesis.conv), "balance_convert");
    auto store_convs = [&](auto* conv) {
        for (const auto& cg: *conv) {
            auto& c = cg.second;
            if (c.value.get_amount() > 0) {
                out.emplace<balance_convert_info>([&](auto& t) {
                    t.owner = genesis_.name_by_idx(cg.first);
                    t.amount = c.value;
                    t.memo = c.memo;
                });
            }
        }
    };
    store_convs(exp_info_.conv_gls);
    store_convs(exp_info_.conv_gbg);
}

void genesis_ee_builder::build(const bfs::path& out_dir) {
    std::cout << "Writing genesis to " << out_dir << "..." << std::endl;

    out_.start(out_dir, fc::sha256());

    write_contracts_abis();
    write_messages();
    write_transfers();
    write_withdraws();
    write_delegations();
    write_rewards_history();
    write_pinblocks();
    write_accounts();
    write_witnesses();
    write_funds();
    write_balance_converts();

    out_.finalize();
}

} } } // cyberway::genesis::ee
