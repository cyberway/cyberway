#include <cyberway/chaindb/undo_state.hpp>
#include <cyberway/chaindb/controller.hpp>
#include <cyberway/chaindb/driver_interface.hpp>
#include <cyberway/chaindb/exception.hpp>
#include <cyberway/chaindb/cache_map.hpp>
#include <cyberway/chaindb/table_object.hpp>
#include <cyberway/chaindb/journal.hpp>
#include <cyberway/chaindb/value_verifier.hpp>

#include <eosio/chain/config.hpp>

/** Session exception is a critical errors and they doesn't handle by chain */
#define CYBERWAY_SESSION_ASSERT(expr, FORMAT, ...)                      \
    FC_MULTILINE_MACRO_BEGIN                                            \
        if (BOOST_UNLIKELY( !(expr) )) {                                \
            elog(FORMAT, __VA_ARGS__);                                  \
            FC_THROW_EXCEPTION(session_exception, FORMAT, __VA_ARGS__); \
        }                                                               \
    FC_MULTILINE_MACRO_END

#define CYBERWAY_SESSION_THROW(FORMAT, ...)                             \
    FC_MULTILINE_MACRO_BEGIN                                            \
        elog(FORMAT, __VA_ARGS__);                                      \
        FC_THROW_EXCEPTION(session_exception, FORMAT, __VA_ARGS__);     \
    FC_MULTILINE_MACRO_END

namespace cyberway { namespace chaindb {

    using fc::mutable_variant_object;

    enum class undo_stage {
        Unknown,
        New,
        Stack,
    }; // enum undo_stage

    class table_undo_stack;

    struct undo_state final {
        undo_state(table_undo_stack& table, revision_t rev): table_(table), revision_(rev) {
        }

        void set_next_pk(primary_key_t, primary_key_t);
        void move_next_pk(undo_state& src);
        void reset_next_pk();
        object_value next_pk_object(variant val = variant()) const;

        bool has_next_pk() const {
            return primary_key::Unset != next_pk_;
        }

        primary_key_t next_pk() const {
            return next_pk_;
        }

        revision_t revision() const {
            return revision_;
        }

        void down_revision() {
            --revision_;
        }

        using pk_value_map_t_ = std::map<primary_key_t, object_value>;

        table_undo_stack& table_;
        pk_value_map_t_   new_values_;
        pk_value_map_t_   old_values_;
        pk_value_map_t_   removed_values_;

    private:
        primary_key_t     next_pk_      = primary_key::Unset;
        primary_key_t     undo_next_pk_ = primary_key::Unset;
        revision_t        revision_     = impossible_revision;
    }; // struct undo_state

    class table_undo_stack final: public table_object::object {
        undo_stage stage_    = undo_stage::Unknown; // <- state machine - changes via actions
        revision_t revision_ = impossible_revision; // <- part of state machine - changes via actions
        std::deque<undo_state> stack_;              // <- access depends on state machine

        std::map<revision_t, primary_key_t> undo_next_pk_map_; // <- map of undo_next_pk_ by revision

    public:
        table_undo_stack() = delete;

        table_undo_stack(const table_info& src, const revision_t rev)
        : object(src),
          stage_(undo_stage::New),
          revision_(rev) {
        }

        revision_t head_revision() const {
            if (stack_.empty()) {
                return 0;
            }
            return stack_.back().revision();
        }

        revision_t revision() const {
            return revision_;
        }

        void start_session(const revision_t rev) {
            CYBERWAY_SESSION_ASSERT(revision_ < rev,
                "Bad revision ${table_revision} (new ${revision}) for the table ${table}.",
                ("table", get_full_table_name())("table_revision", revision_)
                ("revision", rev));

            revision_ = rev;
            stage_ = undo_stage::New;
        }

        undo_state& head() {
            switch (stage_) {
                case undo_stage::New: {
                    stage_ = undo_stage::Stack;
                    stack_.emplace_back(*this, revision_);
                }

                case undo_stage::Stack:
                    return stack_.back();

                case undo_stage::Unknown:
                    break;
            }

            CYBERWAY_SESSION_THROW("Wrong stage ${stage} of the table ${table} on getting of a head.",
                ("table", get_full_table_name())("stage", stage_));
        }

        undo_state& tail() {
            if (!stack_.empty()) {
                return stack_.front();
            }

            CYBERWAY_SESSION_THROW("Wrong stage ${stage} of the table ${table} on getting of a tail.",
                ("table", get_full_table_name())("stage", stage_));
        }

        undo_state& prev_state() {
            switch (stage_) {
                case undo_stage::Unknown:
                    break;

                case undo_stage::Stack:
                    CYBERWAY_SESSION_ASSERT(size() >= 2,
                        "The table ${table} doesn't have 2 states.", ("table", get_full_table_name()));
                    return stack_[stack_.size() - 2];

                case undo_stage::New:
                    CYBERWAY_SESSION_ASSERT(!empty(),
                        "The table ${table} doesn't have any state.", ("table", get_full_table_name()));
                    return stack_.back();
            }

            CYBERWAY_SESSION_THROW("Wrong stage ${stage} of the table ${table} on getting of a previous state.",
                ("table", get_full_table_name())("stage", stage_));
        }

        void squash() {
            switch (stage_) {
                case undo_stage::Unknown:
                    break;

                case undo_stage::Stack:
                    stack_.back().down_revision();

                case undo_stage::New: {
                    --revision_;
                    update_stage();
                    return;
                }
            }

            CYBERWAY_SESSION_THROW("Wrong stage ${stage} of the table ${table} on squashing of changes.",
                ("table", get_full_table_name())("stage", stage_));
        }

        void undo() {
            switch (stage_) {
                case undo_stage::Unknown:
                    break;

                case undo_stage::Stack:
                    stack_.pop_back();

                case undo_stage::New: {
                    --revision_;
                    update_stage();
                    return;
                }
            }

            CYBERWAY_SESSION_THROW("Wrong stage ${stage} of the table ${table} on undoing of changes.",
                ("table", get_full_table_name())("stage", stage_));
        }

        void commit() {
            if (!stack_.empty()) {
                stack_.pop_front();
                if (stack_.empty()) {
                    revision_ = impossible_revision;
                    stage_ = undo_stage::Unknown;
                }
            } else {
                CYBERWAY_SESSION_THROW("Wrong stage ${stage} of the table ${table} on committing of changes.",
                    ("table", get_full_table_name())("stage", stage_));
            }
        }

        primary_key_t set_undo_next_pk(const revision_t rev, const primary_key_t undo_pk) {
            return undo_next_pk_map_.emplace(rev, undo_pk).first->second;
        }

        void move_undo_next_pk(const revision_t dst, const revision_t src) {
            auto itr = undo_next_pk_map_.find(src);
            undo_next_pk_map_.emplace(dst, itr->second);
            undo_next_pk_map_.erase(src);
        }

        void remove_undo_next_pk(const revision_t rev) {
            for (auto itr = undo_next_pk_map_.begin(), etr = undo_next_pk_map_.end(); etr != itr;) {
                if (itr->first < rev) {
                    undo_next_pk_map_.erase(itr++);
                } else {
                    break;
                }
            }
        }

        size_t size() const {
            return stack_.size();
        }

        bool stack_empty() const {
            return stack_.empty();
        }

        bool empty() const {
            return stack_.empty();
        }

    private:
        void update_stage() {
            if (!empty() && revision_ == stack_.back().revision()) {
                stage_ = undo_stage::Stack;
            } else if (revision_ > 0) {
                stage_ = undo_stage::New;
            } else {
                revision_ = impossible_revision;
                stage_ = undo_stage::Unknown;
            }
        }

    }; // struct table_undo_stack

    void undo_state::set_next_pk(const primary_key_t next_pk, primary_key_t undo_pk) {
        next_pk_      = next_pk;
        undo_next_pk_ = table_.set_undo_next_pk(revision_, undo_pk);
    }

    void undo_state::move_next_pk(undo_state& src) {
        next_pk_      = src.next_pk_;
        undo_next_pk_ = src.undo_next_pk_;

        src.reset_next_pk();
        table_.move_undo_next_pk(revision_, src.revision_);
    }

    void undo_state::reset_next_pk() {
        next_pk_      = primary_key::Unset;
        undo_next_pk_ = primary_key::Unset;
    }

    object_value undo_state::next_pk_object(variant value) const {
        auto& info = table_.info();
        auto  obj  = object_value{info.to_service(), std::move(value)};

        obj.service.revision = revision_;
        obj.service.undo_pk  = undo_next_pk_;
        obj.service.undo_rec = undo_record::NextPk;

        return obj;
    }

    struct undo_stack_impl final {
        undo_stack_impl(revision_t& revision, chaindb_controller& controller, journal& jrnl)
        : revision_(revision),
          controller_(controller),
          driver_(controller.get_driver()),
          cache_(controller.get_cache_map()),
          journal_(jrnl),
          verifier_(controller) {
        }

        void clear() {
            tables_.clear();
            revision_ = 0;
            tail_revision_ = 0;
        }

        revision_t revision() const {
            return revision_;
        }

        void set_revision(const revision_t rev) {
            CYBERWAY_SESSION_ASSERT(tables_.empty(), "Cannot set revision while there is an existing undo stack.");
            revision_ = rev;
            tail_revision_ = rev;
            stage_ = undo_stage::Unknown;
        }

        revision_t start_undo_session(bool enabled) {
            if (enabled) {
                ++revision_;
                for (auto& const_table: tables_) {
                    auto& table = const_cast<table_undo_stack&>(const_table); // not critical
                    table.start_session(revision_);
                }
                stage_ = undo_stage::Stack;
                return revision_;
            } else {
                return impossible_revision;
            }
        }

        bool enabled() const {
            switch (stage_) {
                case undo_stage::Stack:
                case undo_stage::New:
                    return true;

                case undo_stage::Unknown:
                    break;
            }
            return false;
        }

        void undo(const revision_t undo_revision) {
            CYBERWAY_SESSION_ASSERT(revision_ == undo_revision,
                "Wrong undo revision ${revision} != ${undo_revision}",
                ("revision", revision_)("undo_revision", undo_revision));
            for_tables([&](auto& table){
                if (!table.stack_empty()) {
                    undo(table, undo_revision);
                }
            });
            --revision_;
            if (revision_ == tail_revision_) {
                stage_ = undo_stage::Unknown;
            }
        }

        void squash(const revision_t squash_revision) {
            CYBERWAY_SESSION_ASSERT(revision_ == squash_revision,
                "Wrong squash revision ${revision} != ${squash_revision}",
                ("revision", revision_)("squash_revision", squash_revision));
            for_tables([&](auto& table){
                if (!table.stack_empty()) {
                    squash(table, squash_revision);
                }
            });
            --revision_;
            if (revision_ == tail_revision_) {
                stage_ = undo_stage::Unknown;
            }
        }

        void commit(const revision_t commit_revision) {
            if (commit_revision <= tail_revision_) {
                // happens on replaying ...
                return;
            }

            for_tables([&](auto& table){
                commit(table, commit_revision);
            });
            tail_revision_ = commit_revision;
            if (revision_ == tail_revision_) {
                stage_ = undo_stage::Unknown;
            }
        }

        void force_undo(const table_info& table, object_value obj) {
            auto  ctx = journal_.create_ctx(table);
            undo_pk_ = std::max(undo_pk_, obj.service.undo_pk) + 1;
            journal_.write_undo(ctx, write_operation::insert(std::move(obj)));
        }

        void insert(const table_info& table, object_value obj) {
            verifier_.verify(table, obj);
            cache_.clear_unsuccess(table);
            if (enabled()) {
                insert(get_table(table), std::move(obj));
            } else {
                journal_.write_data(table, write_operation::insert(std::move(obj)));
            }
        }

        void update(const table_info& table, object_value orig_obj, object_value obj) {
            verifier_.verify(table, obj);
            cache_.clear_unsuccess(table);
            if (enabled()) {
                update(get_table(table), std::move(orig_obj), std::move(obj));
            } else {
                journal_.write_data(table, write_operation::update(std::move(obj)));
            }
        }

        void remove(const table_info& table, object_value orig_obj) {
            cache_.clear_unsuccess(table);
            driver_.skip_pk(table, orig_obj.pk());
            if (enabled()) {
                remove(get_table(table), std::move(orig_obj));
            } else {
                journal_.write_data(table, write_operation::remove(std::move(orig_obj)));
            }
        }

        index_info get_revision_index() {
            static index_def rev_index = [&]() {
                index_def index(N(revision), true, {});

                order_def rev_order("_SERVICE_.rev", "asc");
                rev_order.path = {"_SERVICE_", "rev"};
                rev_order.type = "int64";

                order_def upk_order("_SERVICE_.upk", "asc");
                upk_order.path = {"_SERVICE_", "upk"};
                upk_order.type = "uint64";

                index.orders.push_back(rev_order);
                index.orders.push_back(upk_order);

                return index;
            }();

            index_info index;

            index.account_abi = controller_.get_account_abi_info(config::system_account_name);
            index.table    = index.abi().find_table(N(undo));
            index.pk_order = index.abi().find_pk_order(*index.table);
            index.index    = &rev_index;

            return index;
        }

        struct abi_history_t_ final {
            revision_t revision;
            account_abi_info info;
        }; // struct abi_history

        using abi_history_map_t_ = fc::flat_map<account_name_t, std::deque<abi_history_t_>>;

        abi_history_map_t_ load_abi_history(const index_info& index) {
            abi_history_map_t_ map;

            map.reserve(32);
            auto  account_table = tag<account_object>::get_code();
            auto& cursor = driver_.lower_bound(index, {});
            for (; cursor.pk != primary_key::End; driver_.next(cursor)) {
                auto obj = driver_.object_at_cursor(cursor, false);
                if (!is_system_code(obj.service.code) || obj.service.table != account_table) {
                    continue;
                }

                switch (obj.service.undo_rec) {
                    case undo_record::NextPk:
                    case undo_record::NewValue:
                        continue;

                    case undo_record::OldValue:
                    case undo_record::RemovedValue:
                        break;

                    case undo_record::Unknown:
                    default:
                        CYBERWAY_SESSION_THROW("Unknown undo state on loading from DB");
                }

                auto& abi = obj.value["abi"];
                abi_def def;
                if (abi.is_blob() && abi_serializer::to_abi(abi.get_blob().data, def)) {
                    map[cursor.pk].push_back({obj.service.revision, account_abi_info(cursor.pk, std::move(def))});
                }
            }
            driver_.close({cursor.index.code, cursor.id});
            return map;
        }

        void restore() try {
            if (start_revision <= revision_ || start_revision <= tail_revision_) {
                ilog( "Skip restore undo state, tail revision ${tail}, head revision = ${head}",
                    ("head", revision_)("tail", tail_revision_));
                return;
            }

            auto index = get_revision_index();
            driver_.create_index(index);

            auto abi_map = load_abi_history(index);

            auto get_account_abi_info = [&](const auto code, const auto rev) -> account_abi_info {
                auto mtr = abi_map.find(code);
                if (abi_map.end() != mtr) for (auto& itm: mtr->second) if (itm.revision > rev) {
                    return itm.info;
                }
                return controller_.get_account_abi_info(code);
            };

            auto get_state = [&](const auto& service) -> undo_state& {
                auto table = table_info(service.code, service.scope);

                table.account_abi = get_account_abi_info(service.code, service.revision);
                table.table       = table.abi().find_table(service.table);
                table.pk_order    = table.abi().find_pk_order(*table.table);

                auto& stack = get_table(table);
                if (stack.revision() != service.revision) {
                    stack.start_session(service.revision);
                }
                return stack.head();
            };

            auto& cursor = driver_.lower_bound(index, {});
            for (; cursor.pk != primary_key::End; driver_.next(cursor)) {
                auto  obj   = driver_.object_at_cursor(cursor, false);
                auto  pk    = obj.pk();
                auto& state = get_state(obj.service);

                if (obj.service.undo_pk  >= undo_pk_ ) undo_pk_       = obj.service.undo_pk + 1;
                if (obj.service.revision >  revision_) revision_      = obj.service.revision;
                if (start_revision >= tail_revision_)  tail_revision_ = obj.service.revision - 1;

                switch (obj.service.undo_rec) {
                    case undo_record::NewValue:
                        state.new_values_.emplace(pk, std::move(obj));
                        break;

                    case undo_record::OldValue:
                        state.old_values_.emplace(pk, std::move(obj));
                        break;

                    case undo_record::RemovedValue:
                        state.removed_values_.emplace(pk, std::move(obj));
                        break;

                    case undo_record::NextPk: {
                        auto next_pk = obj.value.get_object()[names::next_pk_field].as_uint64();
                        auto undo_pk = obj.service.undo_pk;
                        state.set_next_pk(next_pk, undo_pk);
                        break;
                    }

                    case undo_record::Unknown:
                    default:
                        CYBERWAY_SESSION_THROW("Unknown undo state on loading from DB");
                }
            }
            driver_.close({cursor.index.code, cursor.id});

            driver_.drop_index(index);

            if (revision_ != tail_revision_) {
                stage_ = undo_stage::Stack;

                for (auto& const_table: tables_) if (const_table.revision() != revision_) {
                    auto& table = const_cast<table_undo_stack&>(const_table); // not critical
                    table.start_session(revision_);
                }
            }
        } catch (const session_exception&) {
            throw;
        } catch (const std::exception& e) {
            CYBERWAY_SESSION_THROW(e.what());
        }

    private:
        void restore_undo_state(object_value& obj) {
            obj.service.revision = obj.service.undo_revision;
            obj.service.payer    = obj.service.undo_payer;
            obj.service.size     = obj.service.undo_size;
            obj.service.in_ram   = obj.service.undo_in_ram;
        }

        void undo(table_undo_stack& table, const revision_t undo_rev) {
            if (undo_rev > table.head_revision()) {
                table.undo();
                return;
            }

            auto& head = table.head();

            CYBERWAY_SESSION_ASSERT(head.revision() == undo_rev,
                "Wrong undo revision ${undo_revision} != ${revision} for the table ${table}:${scope}",
                ("revision", head.revision())("undo_revision", undo_rev)
                ("table", get_full_table_name(table.info()))("scope", table.scope()));

            auto ctx = journal_.create_ctx(table.info());
            cache_.clear_unsuccess(table.info());

            for (auto& obj: head.old_values_) {
                auto undo_pk = obj.second.clone_service();

                restore_undo_state(obj.second);
                verifier_.verify(table.info(), obj.second);
                cache_.emplace(table.info(), obj.second);

                journal_.write(ctx,
                    write_operation::update(undo_rev, std::move(obj.second)),
                    write_operation::remove(undo_rev, std::move(undo_pk)));
            }

            for (auto& obj: head.new_values_) {
                cache_.remove(table.info(), obj.first);
                driver_.skip_pk(table.info(), obj.first);
                journal_.write(ctx,
                    write_operation::remove(undo_rev, obj.second.clone_service()),
                    write_operation::remove(undo_rev, obj.second.clone_service()));
            }

            for (auto& obj: head.removed_values_) {
                auto undo_pk = obj.second.clone_service();

                restore_undo_state(obj.second);
                verifier_.verify(table.info(), obj.second);
                cache_.emplace(table.info(), obj.second);

                journal_.write(ctx,
                    write_operation::insert(std::move(obj.second)),
                    write_operation::remove(undo_rev, std::move(undo_pk)));
            }

            if (head.has_next_pk()) {
                cache_.set_next_pk(table.info(), head.next_pk());
            }

            remove_next_pk(ctx, table, head);

            table.undo();
        }

        template <typename Write>
        void process_state(undo_state& state, Write&& write) const {
            const auto rev = state.revision();
            for (auto& obj: state.old_values_)     write(true,  obj.second, rev);
            for (auto& obj: state.new_values_)     write(true,  obj.second, rev);
            for (auto& obj: state.removed_values_) write(false, obj.second, rev);
        }

        template <typename Ctx>
        void remove_next_pk(Ctx& ctx, const table_undo_stack& table, undo_state& state) const {
            if (!state.has_next_pk()) return;

            journal_.write_undo(ctx, write_operation::remove(state.revision(), state.next_pk_object()));
            state.reset_next_pk();
        }

        void squash_state(table_undo_stack& table, undo_state& state) const {
            auto ctx = journal_.create_ctx(table.info());

            process_state(state, [&](bool has_data, auto& obj, auto& rev) {
                if (has_data) {
                    cache_.set_revision(obj, rev - 1);
                    journal_.write_data(ctx, write_operation::revision(rev, obj.clone_service()));
                }
                journal_.write_undo(ctx, write_operation::revision(rev, obj.clone_service()));
                obj.service.revision = rev - 1;
            });

            if (state.has_next_pk()) {
                journal_.write_undo(ctx, write_operation::revision(state.revision(), state.next_pk_object()));
                table.move_undo_next_pk(state.revision() - 1, state.revision());
            }

            table.squash();
        }

        void remove_state(table_undo_stack& table, undo_state& state) const {
            auto ctx = journal_.create_ctx(table.info());

            process_state(state, [&](bool has_data, auto& obj, auto& rev) {
                if (has_data) {
                    cache_.set_revision(obj, rev - 1);
                    journal_.write_data(ctx, write_operation::revision(rev, obj.clone_service()));
                }
                journal_.write_undo(ctx, write_operation::remove(rev, obj.clone_service()));
            });

            remove_next_pk(ctx, table, state);

            table.undo();
        }

        void squash(table_undo_stack& table, const revision_t squash_rev) {
            if (squash_rev > table.head_revision()) {
                table.squash();
                return;
            }

            auto& state = table.head();
            CYBERWAY_SESSION_ASSERT(state.revision() == squash_rev,
                "Wrong squash revision ${squash_revision} != ${revision} for the table ${table}:${scope}",
                ("revision", state.revision())("squash_revision", squash_rev)
                ("table", get_full_table_name(table.info()))("scope", table.scope()));

            // Only one stack item
            if (table.size() == 1) {
                if (state.revision() - 1 > tail_revision_) {
                    squash_state(table, state);
                } else {
                    remove_state(table, state);
                }
                return;
            }

            auto& prev_state = table.prev_state();

            // Two stack items but they are not neighbours
            if (prev_state.revision() != state.revision() - 1) {
                squash_state(table, state);
                return;
            }

            // An object's relationship to a state can be:
            // in new_ids            : new
            // in old_values (was=X) : upd(was=X)
            // in removed (was=X)    : del(was=X)
            // not in any of above   : nop
            //
            // When merging A=prev_state and B=state we have a 4x4 matrix of all possibilities:
            //
            //                   |--------------------- B ----------------------|
            //
            //                +------------+------------+------------+------------+
            //                | new        | upd(was=Y) | del(was=Y) | nop        |
            //   +------------+------------+------------+------------+------------+
            // / | new        | N/A        | new       A| nop       C| new       A|
            // | +------------+------------+------------+------------+------------+
            // | | upd(was=X) | N/A        | upd(was=X)A| del(was=X)C| upd(was=X)A|
            // A +------------+------------+------------+------------+------------+
            // | | del(was=X) | upd(was=X) | N/A        | N/A        | del(was=X)A|
            // | +------------+------------+------------+------------+------------+
            // \ | nop        | new       B| upd(was=Y)B| del(was=Y)B| nop      AB|
            //   +------------+------------+------------+------------+------------+
            //
            // Each entry was composed by labelling what should occur in the given case.
            //
            // Type A means the composition of states contains the same entry as the first of the two merged states for that object.
            // Type B means the composition of states contains the same entry as the second of the two merged states for that object.
            // Type C means the composition of states contains an entry different from either of the merged states for that object.
            // Type N/A means the composition of states violates causal timing.
            // Type AB means both type A and type B simultaneously.
            //
            // The merge() operation is defined as modifying prev_state in-place to be the state object which represents the composition of
            // state A and B.
            //
            // Type A (and AB) can be implemented as a no-op; prev_state already contains the correct value for the merged state.
            // Type B (and AB) can be implemented by copying from state to prev_state.
            // Type C needs special case-by-case logic.
            // Type N/A can be ignored or assert(false) as it can only occur if prev_state and state have illegal values
            // (a serious logic error which should never happen).
            //

            // We can only be outside type A/AB (the nop path) if B is not nop, so it suffices to iterate through B's three containers.

            auto ctx = journal_.create_ctx(table.info());

            for (auto& obj: state.old_values_) {
                const auto pk = obj.second.pk();
                bool  exists = false;

                // 1. new+upd -> new, type A
                auto nitr = prev_state.new_values_.find(pk);
                if (prev_state.new_values_.end() != nitr) {
                    exists = true;
                    copy_undo_object(nitr->second, obj.second);
                } else {
                    // 2. upd(was=X) + upd(was=Y) -> upd(was=X), type A
                    auto oitr = prev_state.old_values_.find(pk);
                    if (prev_state.old_values_.end() != oitr) {
                        exists = true;
                        copy_undo_object(oitr->second, obj.second);
                    }
                }

                if (exists) {
                    cache_.set_revision(obj.second, prev_state.revision());
                    journal_.write(ctx,
                        write_operation::revision(state.revision(), obj.second.clone_service()),
                        write_operation::remove(  state.revision(), obj.second.clone_service()));
                    continue;
                }

                // del+upd -> N/A
                CYBERWAY_SESSION_ASSERT(!prev_state.removed_values_.count(pk),
                    "UB for the table ${table}: Delete + Update", ("table", get_full_table_name(table.info())));

                // nop+upd(was=Y) -> upd(was=Y), type B

                cache_.set_revision(obj.second, prev_state.revision());
                journal_.write(ctx,
                    write_operation::revision(state.revision(), obj.second.clone_service()),
                    write_operation::revision(state.revision(), obj.second.clone_service()));

                obj.second.service.revision = prev_state.revision();
                
                prev_state.old_values_.emplace(pk, std::move(obj.second));
            }

            for (auto& obj: state.new_values_) {
                const auto pk = obj.second.pk();

                cache_.set_revision(obj.second, prev_state.revision());

                auto ritr = prev_state.removed_values_.find(pk);
                if (ritr != prev_state.removed_values_.end()) {
                    // del(was=X) + ins(was=Y) -> up(was=X)

                    journal_.write_undo(ctx, write_operation::remove(state.revision(), obj.second.clone_service()));

                    ritr->second.service.undo_rec = undo_record::OldValue;
                    journal_.write_undo(ctx, write_operation::update(ritr->second));

                    prev_state.old_values_.emplace(std::move(*ritr));
                    prev_state.removed_values_.erase(ritr);
                } else {
                    // *+new, but we assume the N/A cases don't happen, leaving type B nop+new -> new

                    journal_.write(ctx,
                        write_operation::revision(state.revision(), obj.second.clone_service()),
                        write_operation::revision(state.revision(), obj.second.clone_service()));

                    obj.second.service.revision = prev_state.revision();

                    prev_state.new_values_.emplace(pk, std::move(obj.second));
                }
            }

            // *+del
            for (auto& obj: state.removed_values_) {
                const auto pk = obj.second.pk();

                // new + del -> nop (type C)
                auto nitr = prev_state.new_values_.find(pk);
                if (nitr != prev_state.new_values_.end()) {
                    prev_state.new_values_.erase(nitr);

                    journal_.write_undo(ctx, write_operation::remove(state.revision(), obj.second.clone_service()));
                    continue;
                }

                // upd(was=X) + del(was=Y) -> del(was=X)
                auto oitr = prev_state.old_values_.find(pk);
                if (oitr != prev_state.old_values_.end()) {
                    prev_state.removed_values_.emplace(std::move(*oitr));
                    prev_state.old_values_.erase(oitr);

                    journal_.write_undo(ctx, write_operation::remove(state.revision(), obj.second.clone_service()));
                    continue;
                }

                // del + del -> N/A
                CYBERWAY_SESSION_ASSERT(!prev_state.removed_values_.count(pk),
                    "UB for the table ${table}: Delete + Delete", ("table", get_full_table_name(table.info())));

                // nop + del(was=Y) -> del(was=Y)

                journal_.write_undo(ctx, write_operation::revision(state.revision(), obj.second.clone_service()));
                
                obj.second.service.revision = prev_state.revision();
                
                prev_state.removed_values_.emplace(std::move(obj));
            }

            if (state.has_next_pk()) {
                if (!prev_state.has_next_pk()) {
                    journal_.write_undo(ctx, write_operation::revision(state.revision(), state.next_pk_object()));
                    prev_state.move_next_pk(state);
                } else {
                    remove_next_pk(ctx, table, state);
                }
            }

            table.undo();
        }

        void commit(table_undo_stack& table, const revision_t commit_rev) {
            table.remove_undo_next_pk(commit_rev);
            if (table.empty()) return;

            auto ctx = journal_.create_ctx(table.info());

            while (!table.empty()) {
                auto& state = table.tail();

                if (state.revision() > commit_rev) return;

                process_state(state, [&](bool, auto& obj, auto& rev){
                    journal_.write_undo(ctx, write_operation::remove(rev, obj.clone_service()));
                });
                remove_next_pk(ctx, table, state);

                table.commit();
            }
        }

        void copy_undo_object(object_value& dst, const object_value& src) {
            dst.service.payer  = src.service.payer;
            dst.service.size   = src.service.size;
            dst.service.in_ram = src.service.in_ram;
        }

        void copy_undo_object(object_value& dst, const object_value& src, const undo_record rec) {
            copy_undo_object(dst, src);
            dst.service.undo_rec = rec;
        }

        void init_undo_object(object_value& dst, undo_record rec) {
            dst.service.undo_revision = dst.service.revision;
            dst.service.undo_payer    = dst.service.payer;
            dst.service.undo_size     = dst.service.size;
            dst.service.undo_in_ram   = dst.service.in_ram;

            dst.service.revision      = revision_;
            dst.service.undo_pk       = generate_undo_pk();
            dst.service.undo_rec      = rec;
        }

        void insert(table_undo_stack& table, object_value obj) {
            const auto pk = obj.pk();
            auto& head = table.head();
            auto  ctx = journal_.create_ctx(table.info());

            journal_.write_data(ctx, write_operation::insert(obj));

            auto ritr = head.removed_values_.find(pk);
            if (head.removed_values_.end() != ritr) {
                copy_undo_object(ritr->second, obj, undo_record::OldValue);
                journal_.write_undo(ctx, write_operation::update(ritr->second.clone_service()));

                head.old_values_.emplace(std::move(*ritr));
                head.removed_values_.erase(ritr);
                return;
            }

            init_undo_object(obj, undo_record::NewValue);
            journal_.write_undo(ctx, write_operation::insert(obj.clone_service()));
            head.new_values_.emplace(pk, std::move(obj));

            if (!head.has_next_pk()) {
                head.set_next_pk(pk, generate_undo_pk());

                auto val = mutable_variant_object{names::next_pk_field, pk};
                journal_.write_undo(ctx, write_operation::insert(head.next_pk_object(std::move(val))));
            }
        }

        void update(table_undo_stack& table, object_value orig_obj, object_value obj) {
            const auto pk = orig_obj.pk();
            auto& head = table.head();
            auto  ctx = journal_.create_ctx(table.info());

            journal_.write_data(ctx, write_operation::update(obj));

            auto nitr = head.new_values_.find(pk);
            if (head.new_values_.end() != nitr) {
                copy_undo_object(nitr->second, obj);
                journal_.write_undo(ctx, write_operation::update(nitr->second.clone_service()));
                return;
            }

            auto oitr = head.old_values_.find(pk);
            if (head.old_values_.end() != oitr) {
                copy_undo_object(oitr->second, obj);
                journal_.write_data(ctx, write_operation::update(std::move(obj)));
                return;
            }

            init_undo_object(orig_obj, undo_record::OldValue);
            copy_undo_object(orig_obj, obj);
            journal_.write_undo(ctx, write_operation::insert(orig_obj));
            head.old_values_.emplace(pk, std::move(orig_obj));
        }

        void remove(table_undo_stack& table, object_value orig_obj) {
            const auto pk = orig_obj.pk();
            auto& head = table.head();
            auto  ctx = journal_.create_ctx(table.info());

            journal_.write_data(ctx, write_operation::remove(orig_obj.clone_service()));

            auto nitr = head.new_values_.find(pk);
            if (head.new_values_.end() != nitr) {
                journal_.write_undo(ctx, write_operation::remove(std::move(nitr->second)));
                head.new_values_.erase(nitr);
                return;
            }

            auto oitr = head.old_values_.find(pk);
            if (oitr != head.old_values_.end()) {
                oitr->second.service.undo_rec = undo_record::RemovedValue;
                journal_.write_undo(ctx, write_operation::update(oitr->second));

                head.removed_values_.emplace(std::move(*oitr));
                head.old_values_.erase(oitr);
                return;
            }

            init_undo_object(orig_obj, undo_record::RemovedValue);
            journal_.write_undo(ctx, write_operation::insert(orig_obj));
            head.removed_values_.emplace(pk, std::move(orig_obj));
        }

        table_undo_stack& get_table(const table_info& table) {
            auto itr = table_object::find(tables_, table);
            if (tables_.end() != itr) {
                return const_cast<table_undo_stack&>(*itr); // not critical
            }

            return table_object::emplace(tables_, table, revision_);
        }

        template <typename Lambda>
        void for_tables(Lambda&& lambda) try {
            for (auto itr = tables_.begin(), etr = tables_.end(); etr != itr; ) {
                auto& table = const_cast<table_undo_stack&>(*itr);
                lambda(table);

                if (table.empty()) {
                    tables_.erase(itr++);
                } else {
                    ++itr;
                }
            }
        } FC_LOG_AND_RETHROW()

        primary_key_t generate_undo_pk() {
            if (!primary_key::is_good(undo_pk_)) {
                undo_pk_ = 1;
            }
            return undo_pk_++;
        }

        using index_t_ = table_object::index<table_undo_stack>;

        undo_stage    stage_ = undo_stage::Unknown;
        revision_t&   revision_;
        revision_t    tail_revision_ = 0;
        primary_key_t undo_pk_ = 1;
        index_t_      tables_;

        const chaindb_controller& controller_;
        const driver_interface&   driver_;
        const cache_map&          cache_;
        journal&                  journal_;

        value_verifier verifier_;
    }; // struct undo_stack_impl

    undo_stack::undo_stack()
    : revision_(0) {
    }

    undo_stack::~undo_stack() = default;

    void undo_stack::init(chaindb_controller& controller, journal& jrnl) {
        assert(!impl_);
        impl_ = std::make_unique<undo_stack_impl>(revision_, controller, jrnl);
    }

    void undo_stack::restore() const {
        impl_->restore();
    }

    void undo_stack::clear() const {
        impl_->clear();
    }

    revision_t undo_stack::start_undo_session(bool enabled) const {
        return impl_->start_undo_session(enabled);
    }

    void undo_stack::set_revision(const revision_t rev) const {
        impl_->set_revision(rev);
    }

    bool undo_stack::enabled() const {
        return impl_->enabled();
    }

    void undo_stack::undo(revision_t undo_rev) const {
        impl_->undo(undo_rev);
    }

    void undo_stack::squash(const revision_t squash_rev) const {
        impl_->squash(squash_rev);
    }

    void undo_stack::commit(const revision_t commit_rev) const {
        impl_->commit(commit_rev);
    }

    void undo_stack::force_undo(const table_info& table, object_value obj) const {
        impl_->force_undo(table, std::move(obj));
    }

    void undo_stack::insert(const table_info& table, object_value obj) const {
        impl_->insert(table, std::move(obj));
    }

    void undo_stack::update(const table_info& table, object_value orig_obj, object_value obj) const {
        impl_->update(table,  std::move(orig_obj), std::move(obj));
    }

    void undo_stack::remove(const table_info& table, object_value orig_obj) const {
        impl_->remove(table,  std::move(orig_obj));
    }

} } // namespace cyberway::chaindb

FC_REFLECT_ENUM(cyberway::chaindb::undo_stage, (Unknown)(New)(Stack))
