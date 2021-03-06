#include <cyberway/chaindb/controller.hpp>
#include <cyberway/chaindb/object_value.hpp>
#include <cyberway/chaindb/exception.hpp>
#include <cyberway/chaindb/names.hpp>
#include <cyberway/chaindb/driver_interface.hpp>
#include <cyberway/chaindb/cache_map.hpp>
#include <cyberway/chaindb/undo_state.hpp>
#include <cyberway/chaindb/journal.hpp>
#include <cyberway/chaindb/mongo_driver.hpp>
#include <cyberway/chaindb/storage_calculator.hpp>
#include <cyberway/chaindb/storage_payer_info.hpp>
#include <cyberway/chaindb/index_order_validator.hpp>

#include <eosio/chain/snapshot.hpp>
#include <eosio/chain/name.hpp>
#include <eosio/chain/apply_context.hpp>
#include <eosio/chain/resource_limits.hpp>
#include <eosio/chain/transaction_context.hpp>

#include <boost/algorithm/string.hpp>

namespace cyberway { namespace chaindb {

    namespace config = eosio::chain::config;

    using eosio::chain::name;
    using eosio::chain::symbol;
    using eosio::chain::symbol_info;
    using eosio::chain::account_object;

    using fc::variants;
    using fc::variant_object;
    using fc::mutable_variant_object;

    using std::vector;

    using eosio::chain::abi_serialization_deadline_exception;

    service_state table_request::to_service(const primary_key_t pk) const {
        service_state service;

        service.code  = code;
        service.scope = scope;
        service.table = table;
        service.pk    = pk;

        return service;
    }

    service_state index_request::to_service(const primary_key_t pk) const {
        service_state service;

        service.code  = code;
        service.scope = scope;
        service.table = table;
        service.pk    = pk;

        return service;
    }

    std::ostream& operator<<(std::ostream& osm, const chaindb_type t) {
        switch (t) {
            case chaindb_type::MongoDB:
                osm << "MongoDB";
                break;

            default:
                osm << "_UNKNOWN_";
                break;
        }
        return osm;
    }

    std::istream& operator>>(std::istream& in, chaindb_type& type) {
        std::string s;
        in >> s;
        boost::algorithm::to_lower(s);
        if (s == "mongodb") {
            type = chaindb_type::MongoDB;
        } else {
            in.setstate(std::ios_base::failbit);
        }
        return in;
    }

    driver_interface::~driver_interface() = default;

    namespace { namespace _detail {
        std::unique_ptr<driver_interface> create_driver(
            chaindb_type type, journal& jrnl, string address, string sys_name
        ) {
            if (sys_name.empty()) {
                sys_name = names::system_code;
            }

            switch(type) {
                case chaindb_type::MongoDB:
                    return std::make_unique<mongodb_driver>(jrnl, std::move(address), std::move(sys_name));

                default:
                    break;
            }
            CYBERWAY_ASSERT(
                false, unknown_connection_type_exception,
                "Invalid type ${type} of ChainDB connection", ("type", type));
        }

    } } // namespace _detail

    //------------------------------------

    void storage_payer_info::add_usage() {
        if (BOOST_UNLIKELY(payer.empty() || !delta)) {
            // do nothing
        } else if (BOOST_LIKELY(!!apply_ctx)) {
            apply_ctx->add_storage_usage(*this);
        } else if (!!transaction_ctx) {
            transaction_ctx->add_storage_usage(*this);
        } else if (!!resource_mng) {
            CYBERWAY_ASSERT(0 > delta /* updating and removing */ || 0 == time_slot /* genesis */,
                eosio::chain::resource_limit_exception, "SYSTEM: attempt to use STORAGE without authorization");
            resource_mng->add_storage_usage(payer, delta, time_slot);
        }
    }

    void storage_payer_info::get_payer_from(const object_value& obj) {
        if (owner.empty()) {
            owner = obj.service.payer;
        }

        if (payer.empty()) {
            payer = obj.service.payer;
        }
    }

    void storage_payer_info::set_payer_in(object_value& obj) const {
        obj.service.payer  = payer;
        obj.service.size   = size;
        obj.service.in_ram = in_ram;
    }

    //------------------------------------

    struct chaindb_controller_impl final {
        chaindb_controller& controller_;
        journal journal_;
        std::unique_ptr<driver_interface> driver_ptr_;
        driver_interface& driver_;
        system_abi_info system_abi_info_;
        account_abi_info history_abi_info_;
        cache_map cache_;
        undo_stack undo_;

        chaindb_controller_impl(chaindb_controller& controller, const chaindb_type t, string address, string sys_name)
        : controller_(controller),
          driver_ptr_(_detail::create_driver(t, journal_, std::move(address), std::move(sys_name))),
          driver_(*driver_ptr_.get()),
          system_abi_info_(driver_),
          history_abi_info_(config::history_account_name, eosio::chain::history_contract_abi()) {
        }

        ~chaindb_controller_impl() = default;

        void restore_db() {
            system_abi_info_.init_abi();
            undo_.restore();
        }

        void drop_db() {
            cache_.clear(); // reset all cached values
            undo_.clear(); // remove all undo states
            journal_.clear(); // remove all pending changes
            driver_.drop_db(); // drop database
        }

        void initialize_db() {
            drop_db(); // force to drop old database
            system_abi_info_.abi().verify_tables_structure(driver_);
            history_abi_info_.abi().verify_tables_structure(driver_);
        }

        const cursor_info& current(const cursor_info& cursor) const {
            if (primary_key::Unset == cursor.pk) {
                driver_.current(cursor);
            }
            return cursor;
        }

        const cursor_info& current(const cursor_request& request) const {
            return current(driver_.cursor(request));
        }

        table_info table_by_request(const table_request& request) {
            return get_table(request);
        }

        find_info lower_bound(
            const index_request& request, const cursor_kind kind, const char* value, const size_t size
        ) {
            auto key    = request.to_service();
            auto index  = get_index(request);

            cache_object_ptr cache_ptr;

            if (value && size) {
                if (index.index->unique) {
                    cache_ptr = cache_.find(key, request.index, value, size);
                }

                if (!cache_ptr) {
                    cache_ptr = cache_.find_unsuccess(key, request.index, value, size);
                }
            }

            switch (kind) {
                case cursor_kind::ManyRecords:
                    break;

                case cursor_kind::InRAM:
                    if (!cache_ptr) {
                        return {end_cursor, primary_key::End, nullptr, controller_, request.code};
                    }

                case cursor_kind::OneRecord:
                    if (cache_ptr) {
                        return {ram_cursor, cache_ptr->pk(), std::move(cache_ptr), controller_, request.code};
                    }
                    break;

                default:
                    assert(false);
            }

            auto  object = index.abi().to_object(index, value, size);
            auto& cursor = driver_.lower_bound(std::move(index), object);
            if (cache_ptr) {
                cursor.pk     = cache_ptr->pk();
                cursor.object = cache_ptr->object();
                return {cursor.id, cursor.pk, std::move(cache_ptr), controller_, request.code};
            }

            current(cursor);
            if (value && size) {
                if (primary_key::is_good(cursor.pk)) {
                    cache_ptr = cache_.find(request.to_service(cursor.pk));
                    if (cache_ptr && !cache_ptr->object().value.has_value(object)) {
                        cache_.emplace_unsuccess(cursor.index, value, size, cursor.pk);
                    }
                } else {
                    cache_.emplace_unsuccess(cursor.index, value, size, cursor.pk);
                }
            }

            return {cursor.id, cursor.pk, std::move(cache_ptr), controller_, request.code};
        }

        find_info lower_bound(const table_request& request, const cursor_kind kind, const primary_key_t pk) {
            auto key   = request.to_service(pk);
            auto index = get_pk_index(request);
            auto value = primary_key::to_variant(index, pk);

            auto cache_ptr = cache_.find(key);
            if (!cache_ptr) {
                cache_ptr = cache_.find_unsuccess(key);
            }

            switch (kind) {
                case cursor_kind::ManyRecords:
                    break;

                case cursor_kind::InRAM:
                    if (!cache_ptr) {
                        return {end_cursor, primary_key::End, nullptr, controller_, request.code};
                    }

                case cursor_kind::OneRecord:
                    if (cache_ptr) {
                        return {ram_cursor, cache_ptr->pk(), std::move(cache_ptr), controller_, request.code};
                    }
                    break;

                default:
                    assert(false);
            }

            auto& cursor = driver_.lower_bound(std::move(index), std::move(value));
            if (cache_ptr) {
                cursor.pk     = cache_ptr->pk();
                cursor.object = cache_ptr->object();
                return {cursor.id, cursor.pk, std::move(cache_ptr), controller_, request.code};
            }

            current(cursor);
            if (pk != cursor.pk) {
                cache_.emplace_unsuccess(cursor.index, pk, cursor.pk);
            }

            return {cursor.id, cursor.pk, nullptr, controller_, request.code};
        }

        // API request, it can't use cache
        find_info lower_bound(const index_request& request, const variant& key) {
            const auto index = get_index(request);
            index_order_validator(index).verify(key);
            const auto& cursor = current(driver_.lower_bound(get_index(request), key));
            return {cursor.id, cursor.pk, nullptr, controller_, request.code};
        }

        const cursor_info& upper_bound(const index_request& request, const char* key, const size_t size) {
            auto index = get_index(request);
            auto value = index.abi().to_object(index, key, size);
            return current(driver_.upper_bound(std::move(index), std::move(value)));
        }

        const cursor_info& upper_bound(const table_request& request, const primary_key_t pk) {
            auto index = get_pk_index(request);
            auto value = primary_key::to_variant(index, pk);
            return current(driver_.upper_bound(std::move(index), std::move(value)));
        }

        const cursor_info& upper_bound(const index_request& request, const variant& key) {
            auto index = get_index(request);
            index_order_validator(index).verify(key);
            return current(driver_.upper_bound(get_index(request), key));
        }

        const cursor_info& locate_to(const index_request& request, const char* key, size_t size, primary_key_t pk) {
            auto index = get_index(request);
            auto value = index.abi().to_object(index, key, size);
            return driver_.locate_to(std::move(index), std::move(value), pk);
        }

        const cursor_info& begin(const index_request& request) {
            return current(driver_.begin(get_index(request)));
        }

        const cursor_info& end(const index_request& request) {
            return driver_.end(get_index(request));
        }

        primary_key_t available_pk(const table_request& request) {
            return driver_.available_pk(get_table(request));
        }

        void set_cache_converter(const table_request& request, const cache_converter_interface& converter) {
            auto table = get_table(request);
            cache_.set_cache_converter(table, converter);
        }

        bool has_cache_converter(const table_request& request) {
            auto table = get_table(request);
            return cache_.has_cache_converter(table);
        }

        cache_object_ptr create_cache_object(const table_request& req, const storage_payer_info& storage) {
            auto table = get_table(req);
            auto item = cache_.create(table, storage);
            if (BOOST_UNLIKELY(!item)) {
                auto pk = driver_.available_pk(table);
                cache_.set_next_pk(table, pk);
                item = cache_.create(table, storage);
            }
            return item;
        }

        cache_object_ptr create_cache_object(
            const table_request& req, const primary_key_t pk, const storage_payer_info& storage
        ) {
            auto table = get_table(req);
            return cache_.create(table, pk, storage);
        }

        void destroy_cache_object(cache_object& obj) {
            cache_.destroy(obj);
        }

        cache_object_ptr get_cache_object(const cursor_info& cursor, const bool with_blob) {
            auto cache_ptr = cache_.find(cursor.index.to_service(cursor.pk));

            if (BOOST_UNLIKELY(!cache_ptr)) {
                auto obj = object_at_cursor(cursor, false);

                CYBERWAY_ASSERT(!obj.is_null(), driver_absent_object_exception,
                    "Requesting not-exist object from the table ${table}",
                    ("table", get_full_table_name(cursor.index)));

                cache_ptr = cache_.emplace(cursor.index, std::move(obj));
            }

            init_blob(cache_ptr, cursor.index, with_blob);

            return cache_ptr;
        }

        cache_object_ptr get_cache_object(const cursor_request& req, const bool with_blob) {
            return get_cache_object(current(req), with_blob);
        }

        void init_blob(cache_object_ptr& cache_ptr, const table_info& info, const bool with_blob) {
            if (cache_ptr && with_blob && !cache_ptr->has_blob()) {
                cache_ptr->set_blob(info.abi().to_bytes(info, cache_ptr->object().value));
            }
        }

        account_abi_info get_account_abi_info(const account_name_t code) {
            if (is_system_code(code)) {
                return system_abi_info_.info();
            }

            if (config::history_account_name == code) {
                return history_abi_info_;
            }

            auto cache_ptr = cache_.find(system_abi_info_.to_service(code));
            if (cache_ptr) {
                return account_abi_info(std::move(cache_ptr));
            }

            auto obj = driver_.object_by_pk(system_abi_info_.account_index(), code);
            if (!obj.is_null()) {
                auto cache_ptr = cache_.emplace(system_abi_info_.account_index(), std::move(obj));
                return account_abi_info(std::move(cache_ptr));
            }
            return account_abi_info();
        }

        // From contracts
        int insert(
            const table_request& request, const storage_payer_info& storage,
            const primary_key_t pk, const char* data, const size_t size
        ) {
            auto table = get_table(request);
            auto value = table.abi().to_object(table, data, size);
            auto obj   = to_object_value(table, pk, std::move(value));

            return insert(table, storage, obj);
        }

        void insert(const table_name_t& table, const account_name& code, object_value object, storage_payer_info payer) {
            const auto info = find_table<table_info>(table_request{object.service.code, object.service.scope, object.service.table});

            if (table == table_name(names::undo_table).value && code == config::system_account_name) {
                undo_.force_undo(info, object);
            } else {
                insert(info, payer, object);
            }
        }

        // From internal
        int insert(cache_object& cache_obj, variant value, const storage_payer_info& storage) {
            auto table = get_table(cache_obj);
            auto obj   = to_object_value(table, cache_obj.pk(), std::move(value));

            auto delta = insert(table, storage, obj);
            cache_.set_object(table, cache_obj, std::move(obj));

            return delta;
        }

        // From contracts
        int update(
            const table_request& request, storage_payer_info storage,
            const primary_key_t pk, const char* data, const size_t size
        ) {
            auto table = get_table(request);
            auto value = table.abi().to_object(table, data, size);
            auto obj   = to_object_value(table, pk, std::move(value));
            auto orig_cache_ptr = get_cache_object(table, obj.pk(), false);

            storage.in_ram = orig_cache_ptr->object().service.in_ram;
            auto delta = update(table, std::move(storage), obj, orig_cache_ptr->object());
            cache_.emplace(table, std::move(obj));

            return delta;
        }

        // From internal
        int update(cache_object& cache_obj, variant value, storage_payer_info storage) {
            auto table = get_table(cache_obj);
            auto obj   = object_value{table.to_service(cache_obj.pk()), std::move(value)};

            storage.in_ram = cache_obj.service().in_ram;
            try {
                auto delta = update(table, std::move(storage), obj, cache_obj.object());
                cache_.set_object(table, cache_obj, std::move(obj));
                return delta;
            } catch (...) {
                // case of throwing in storage_payer_info::add_usage(), value_verifier::verify()
                cache_.set_value(table, cache_obj, cache_obj.object());
                throw;
            }
        }

        void change_ram_state(cache_object& cache_obj, storage_payer_info storage) {
            auto table    = get_table(cache_obj);
            auto obj      = cache_obj.object();
            auto orig_obj = cache_obj.object();

            obj.service.in_ram = storage.in_ram;
            storage.size  = obj.service.size;
            storage.delta = 0;
            update(table, storage, obj, std::move(orig_obj));
            cache_.set_service(table, cache_obj, std::move(obj.service));
        }

        // From contracts
        int remove(const table_request& request, const storage_payer_info& storage, const primary_key_t pk) {
            auto table = get_table(request);
            auto cache_ptr = get_cache_object(table, pk, false);

            return remove(table, storage, cache_ptr->object());
        }

        // From internal
        int remove(cache_object& itm, const storage_payer_info& storage) {
            auto table = get_table(itm);
            auto orig_obj = itm.object();

            return remove(table, storage, std::move(orig_obj));
        }

        cache_object_ptr get_cache_object(const table_request& request, const primary_key_t pk, const bool with_blob) {
            return get_cache_object(get_table(request), pk, with_blob);
        }

        object_value object_at_cursor(const cursor_request& request, const bool with_decors) {
            return object_at_cursor(current(request), with_decors);
        }

        void set_revision(revision_t revision) {
            undo_.set_revision(revision);
            cache_.set_revision(revision);
        }
        
        void set_subjective_ram(uint64_t size, uint64_t reserved_size, uint32_t rlm) const {
            cache_.set_subjective_ram(size, reserved_size, rlm);
        }

        chaindb_session start_undo_session(bool enabled) {
            auto revision = undo_.start_undo_session(enabled);
            if (enabled) {
                cache_.start_session(revision);
            }
            return chaindb_session(controller_, revision);
        }

        void push_revision(const revision_t revision) {
            cache_.push_session(revision);
        }

        void squash_revision(const revision_t revision) {
            undo_.squash(revision);
            cache_.squash_session(revision);
        }

        void undo_revision(const revision_t revision) {
            auto reset_undo_restorer = fc::make_scoped_exit([this](){
                driver_.disable_undo_restore();
            });

            driver_.enable_undo_restore();
            undo_.undo(revision);
            cache_.undo_session(revision);
            driver_.apply_all_changes();
        }

        void commit_revision(const revision_t revision) {
            undo_.commit(revision);
        }

        object_value object_by_pk(const table_request& request, const primary_key_t pk) {
            auto cache_ptr = cache_.find(request.to_service(pk));
            if (cache_ptr) {
                return cache_ptr->object();
            }

            auto table = get_table(request);
            auto obj   = driver_.object_by_pk(table, pk);
            validate_object(table, obj, pk);

            if (!obj.is_null()) {
                return cache_.emplace(table, std::move(obj))->object();
            }

            return obj;
        }

        eosio::chain::bytes serialize(const abi_info& abi, const object_value& object) {
            return abi.to_bytes(find_table<table_info>(table_request{object.service.code, object.service.scope, object.service.table}), object.value);
        }

        fc::variant deserialize(const table_request& request, const abi_info& abi, const bytes& serialized) {
            return abi.to_object(find_table<table_info>(request), serialized.data(), serialized.size());
        }

    private:
        object_value to_object_value(const table_info& table, const primary_key_t pk, variant value) const {
            return {table.to_service(pk), std::move(value)};
        }

        object_value object_at_cursor(const cursor_info& cursor, const bool with_decors) {
            auto obj = driver_.object_at_cursor(cursor, with_decors);
            validate_object(cursor.index, obj, cursor.pk);
            return obj;
        }

        table_info get_table(const cache_object& itm) {
            auto& service = itm.object().service;
            auto info = find_table<table_info>(service);
            CYBERWAY_ASSERT(info.is_valid(), unknown_table_exception,
                "ABI table ${table} doesn't exists", ("table", get_full_table_name(service)));

            return info;
        }

        template <typename Request>
        table_info get_table(const Request& request) {
            auto info = find_table<table_info>(request);
            CYBERWAY_ASSERT(info.is_valid(), unknown_table_exception,
                "ABI table ${code}.${table} doesn't exists",
                ("code", get_code_name(request))("table", get_table_name(request.table)));

            return info;
        }

        index_info get_index(const index_request& request) {
            auto info = find_index(request);
            CYBERWAY_ASSERT(info.is_valid(), unknown_index_exception,
                "ABI index ${code}.${table}.${index} doesn't exists",
                ("code", get_code_name(request))("table", get_table_name(request.table))
                ("index", get_index_name(request.index)));

            return info;
        }

        template <typename Request>
        index_info get_pk_index(const Request& request) {
            auto table = get_table(request);
            auto index = index_info(table);
            index.index = index.abi().find_pk_index(*table.table);
            return index;
        }

        template <typename Info, typename Request>
        Info find_table(const Request& request) {
            Info info(request.code, request.scope);
            auto account_abi = get_account_abi_info(request.code);
            if (!account_abi.has_abi_info()) {
                return info;
            }

            info.table = account_abi.abi().find_table(request.table);
            if (!info.table) {
                return info;
            }

            info.account_abi = std::move(account_abi);
            info.pk_order    = info.abi().find_pk_order(*info.table);
            return info;
        }

        index_info find_index(const index_request& request) {
            auto info = find_table<index_info>(request);
            if (!info.table) {
                return info;
            }

            info.index = info.abi().find_index(*info.table, request.index);
            return info;
        }

        cache_object_ptr get_cache_object(const table_info& table, const primary_key_t pk, const bool with_blob) {
            auto cache_ptr = cache_.find(table.to_service(pk));

            if (BOOST_UNLIKELY(!cache_ptr)) {
                auto obj = driver_.object_by_pk(table, pk);
                validate_object(table, obj, pk);

                CYBERWAY_ASSERT(!obj.is_null(), driver_absent_object_exception,
                    "Requesting not-exist object from the table ${table}",
                    ("table", get_full_table_name(table)));

                cache_ptr = cache_.emplace(table, std::move(obj));
            }

            init_blob(cache_ptr, table, with_blob);

            return cache_ptr;
        }

        void validate_object(const table_info& table, const object_value& obj, const primary_key_t pk) const {
            if (!primary_key::is_good(obj.pk())) {
                CYBERWAY_ASSERT(obj.is_null(), driver_wrong_object_exception,
                    "Driver returns the row '${obj}' from the table ${table} instead of null for end iterator",
                    ("obj", obj.value)("table", get_full_table_name(table)));
                return;
            }

            CYBERWAY_ASSERT(obj.value.is_object(), invalid_abi_store_type_exception,
                "Receives ${obj} instead of object from the table ${table}",
                ("obj", obj.value)("table", get_full_table_name(table)));

            auto& value = obj.value.get_object();
            CYBERWAY_ASSERT(value.end() == value.find(names::service_field), reserved_field_exception,
                "Object has the reserved field ${field} for the table ${table}",
                ("field", names::service_field)("table", get_full_table_name(table)));
        }

        void validate_pk_value(const table_info& table, const object_value& obj) const {
            CYBERWAY_ASSERT(primary_key::from_variant(table, obj.value).value() == obj.pk(), primary_key_exception,
                "Object '${obj}' from the table ${table} has wrong value '${pk}' in the primary key",
                ("obj", obj.value)
                ("pk", primary_key::from_raw(table, obj.pk()).to_string())
                ("table", get_full_table_name(table)));
        }

        int insert(const table_info& table, storage_payer_info charge, object_value& obj) {
            validate_object(table, obj, obj.pk());
            validate_pk_value(table, obj);

            charge.size   = calc_storage_usage(table, obj.value);
            charge.in_ram = true;
            charge.delta += charge.size;

            // insert object to storage
            charge.set_payer_in(obj);
            obj.service.revision = undo_.revision();

            undo_.insert(table, obj);

            // don't charge on genesis
            if (undo_.revision() > start_revision) {
                charge.add_usage();
            }

            return charge.delta;
        }

        int update(const table_info& table, storage_payer_info charge, object_value& obj, object_value orig_obj) {
            validate_object(table, obj, obj.pk());
            validate_pk_value(table, obj);

            charge.size   = calc_storage_usage(table, obj.value);
            charge.delta += charge.size - orig_obj.service.size;

            if (charge.delta <= 0) {
                charge.payer = charge.owner;
            }
            charge.get_payer_from(orig_obj);

            // don't charge on genesis
            if (undo_.revision() > start_revision) {
                charge.add_usage();
            }

            // update object in storage
            charge.set_payer_in(obj);
            obj.service.revision = undo_.revision();

            undo_.update(table, std::move(orig_obj), obj);

            return charge.delta;
        }

        int remove(const table_info& table, storage_payer_info refund, object_value orig_obj) {
            auto pk = orig_obj.pk();

            refund.get_payer_from(orig_obj);
            refund.size  =  orig_obj.service.size;
            refund.delta = -orig_obj.service.size;

            // refund the payer
             if (undo_.revision() > start_revision) {
                refund.add_usage();
             }

            undo_.remove(table, std::move(orig_obj));
            cache_.remove(table, pk);

            return refund.delta;
        }
    }; // class chaindb_controller::controller_impl_

    chaindb_controller::chaindb_controller(const chaindb_type t, string address, string sys_name)
    : impl_(std::make_unique<chaindb_controller_impl>(*this, t, std::move(address), std::move(sys_name))) {
        impl_->undo_.init(*this, impl_->journal_);
    }

    chaindb_controller::~chaindb_controller() = default;

    const system_abi_info& chaindb_controller::get_system_abi_info() const {
        return impl_->system_abi_info_;
    }

    const driver_interface& chaindb_controller::get_driver() const {
        return impl_->driver_;
    }

    const cache_map& chaindb_controller::get_cache_map() const {
        return impl_->cache_;
    }

    const undo_stack& chaindb_controller::get_undo_stack() const {
        return impl_->undo_;
    }

    void chaindb_controller::restore_db() const {
        impl_->restore_db();
    }

    void chaindb_controller::drop_db() const {
        impl_->drop_db();
    }

    void chaindb_controller::initialize_db() const {
        impl_->initialize_db();
    }

    void chaindb_controller::push_cache() const {
        impl_->cache_.push(revision());
    }

    void chaindb_controller::enable_rev_bad_update() const {
        // https://github.com/cyberway/cyberway/issues/1094
        impl_->driver_.enable_rev_bad_update();
    }

    void chaindb_controller::disable_rev_bad_update() const {
        // https://github.com/cyberway/cyberway/issues/1094
        impl_->driver_.disable_rev_bad_update();
    }

    void chaindb_controller::close(const cursor_request& request) const {
        impl_->driver_.close(request);
    }

    void chaindb_controller::close_code_cursors(const account_name& code) const {
        impl_->driver_.close_code_cursors(code);
    }

    void chaindb_controller::apply_all_changes() const {
        impl_->driver_.apply_all_changes();
    }

    void chaindb_controller::apply_code_changes(const account_name& code) const {
        impl_->driver_.apply_code_changes(code);
    }

    find_info chaindb_controller::lower_bound(
        const index_request& request, const cursor_kind kind, const char* key, size_t size
    ) const {
        return impl_->lower_bound(request, kind, key, size);
    }

    find_info chaindb_controller::lower_bound(
        const table_request& request, const cursor_kind kind, const primary_key_t pk
    ) const {
        return impl_->lower_bound(request, kind, pk);
    }

    find_info chaindb_controller::lower_bound(const index_request& request, const variant& orders) const {
        return impl_->lower_bound(request, orders);
    }

    find_info chaindb_controller::upper_bound(const index_request& request, const char* key, size_t size) const {
        const auto& info = impl_->upper_bound(request, key, size);
        return {info.id, info.pk, nullptr, *this, request.code};
    }

    find_info chaindb_controller::upper_bound(const table_request& request, const primary_key_t pk) const {
        const auto& info = impl_->upper_bound(request, pk);
        return {info.id, info.pk, nullptr, *this, request.code};
    }

    find_info chaindb_controller::upper_bound(const index_request& request, const variant& orders) const {
        auto info = impl_->upper_bound(request, orders);
        return {info.id, info.pk, nullptr, *this, request.code};
    }

    find_info chaindb_controller::locate_to(
        const index_request& request, const char* key, size_t size, primary_key_t pk
    ) const {
        const auto& info = impl_->locate_to(request, key, size, pk);
        return {info.id, info.pk, nullptr, *this, request.code};
    }

    find_info chaindb_controller::begin(const index_request& request) const {
        const auto& info = impl_->begin(request);
        return {info.id, info.pk, nullptr, *this, request.code};
    }

    find_info chaindb_controller::end(const index_request& request) const {
        const auto& info = impl_->end(request);
        return {info.id, info.pk, nullptr, *this, request.code};
    }

    cursor_t chaindb_controller::clone(const cursor_request& request) const {
        return impl_->driver_.clone(request).id;
    }

    primary_key_t chaindb_controller::current(const cursor_request& request) const {
        return impl_->current(request).pk;
    }

    primary_key_t chaindb_controller::next(const cursor_request& request) const {
        auto& driver = impl_->driver_;
        return driver.next(driver.cursor(request)).pk;
    }

    primary_key_t chaindb_controller::prev(const cursor_request& request) const{
        auto& driver = impl_->driver_;
        return driver.prev(driver.cursor(request)).pk;
    }

    void chaindb_controller::set_cache_converter(
        const table_request& table, const cache_converter_interface& conv
    ) const {
        impl_->set_cache_converter(table, conv);
    }

    bool chaindb_controller::has_cache_converter(const table_request& table) const {
        return impl_->has_cache_converter(table);
    }

    cache_object_ptr chaindb_controller::create_cache_object(
        const table_request& table, const storage_payer_info& storage
    ) const {
        return impl_->create_cache_object(table, storage);
    }

    cache_object_ptr chaindb_controller::create_cache_object(
        const table_request& table, const primary_key_t pk, const storage_payer_info& storage
    ) const {
        return impl_->create_cache_object(table, pk, storage);
    }

    cache_object_ptr chaindb_controller::get_cache_object(const cursor_request& cursor, const bool with_blob) const {
        return impl_->get_cache_object(cursor, with_blob);
    }

    cache_object_ptr chaindb_controller::get_cache_object(
        const table_request& request, const primary_key_t pk, const bool with_blob
    ) const {
        return impl_->get_cache_object(request, pk, with_blob);
    }

    account_abi_info chaindb_controller::get_account_abi_info(const account_name_t code) const {
        return impl_->get_account_abi_info(code);
    }

    void chaindb_controller::destroy_cache_object(cache_object& obj) const {
        return impl_->destroy_cache_object(obj);
    }

    primary_key_t chaindb_controller::available_pk(const table_request& request) const {
        return impl_->available_pk(request);
    }

    int chaindb_controller::insert(
        const table_request& request, const storage_payer_info& storage,
        primary_key_t pk, const char* data, size_t size
    ) const {
         return impl_->insert(request, storage, pk, data, size);
    }

    int chaindb_controller::update(
        const table_request& request, const storage_payer_info& storage,
        primary_key_t pk, const char* data, size_t size
    ) const {
        return impl_->update(request, storage, pk, data, size);
    }

    int chaindb_controller::remove(
        const table_request& request, const storage_payer_info& storage, primary_key_t pk
    ) const{
        return impl_->remove(request, storage, pk);
    }

    int chaindb_controller::insert(cache_object& itm, variant data, const storage_payer_info& storage) const {
        return impl_->insert(itm, std::move(data), storage);
    }

    int chaindb_controller::update(cache_object& itm, variant data, const storage_payer_info& storage) const {
        return impl_->update(itm, std::move(data), storage);
    }

    int chaindb_controller::remove(cache_object& itm, const storage_payer_info& storage) const {
        return impl_->remove(itm, storage);
    }

    void chaindb_controller::insert(const table_name_t& table, const account_name& code, object_value obj, storage_payer_info payer) const {
        impl_->insert(table, code, std::move(obj), std::move(payer));
    }

    void chaindb_controller::change_ram_state(cache_object& cache_obj, const storage_payer_info& storage) const {
        impl_->change_ram_state(cache_obj, storage);
    }

    table_info chaindb_controller::table_by_request(const table_request& request) const {
        return impl_->table_by_request(request);
    }

    index_info chaindb_controller::index_at_cursor(const cursor_request& request) const {
        return impl_->current(request).index;
    }

    object_value chaindb_controller::object_at_cursor(const cursor_request& request) const {
        return impl_->object_at_cursor(request, true);
    }

    object_value chaindb_controller::object_by_pk(const table_request& request, primary_key_t pk) const {
        return impl_->object_by_pk(request, pk);
    }

    revision_t chaindb_controller::revision() const {
        return impl_->undo_.revision();
    }

    void chaindb_controller::set_revision(revision_t revision) const {
        return impl_->set_revision(revision);
    }
    
    void chaindb_controller::set_subjective_ram(uint64_t size, uint64_t reserved_size, uint32_t rlm) const {
        impl_->set_subjective_ram(size, reserved_size, rlm);
    }

    chaindb_session chaindb_controller::start_undo_session(bool enabled) const {
        return impl_->start_undo_session(enabled);
    }

    void chaindb_controller::undo_last_revision() const {
        return impl_->undo_revision(revision());
    }

    void chaindb_controller::commit_revision(const revision_t revision) const {
        return impl_->commit_revision(revision);
    }

    eosio::chain::bytes chaindb_controller::serialize(const abi_info& abi, const object_value& object) const {
        return impl_->serialize(abi, object);
    }

    fc::variant chaindb_controller::deserialize(const table_request& request, const abi_info& abi, const bytes& bytes) const {
        return impl_->deserialize(request, abi, bytes);
    }

    //-------------------------------------

    chaindb_session::chaindb_session(chaindb_controller& controller, const revision_t rev)
    : controller_(controller),
      apply_(true),
      revision_(rev) {
        if (impossible_revision == rev) {
            apply_ = false;
        }
    }

    chaindb_session::chaindb_session(chaindb_session&& mv)
    : controller_(mv.controller_),
      apply_(mv.apply_),
      revision_(mv.revision_) {
        mv.apply_ = false;
    }

    chaindb_session::~chaindb_session() {
        undo();
    }

    void chaindb_session::push() {
        if (apply_) {
            CYBERWAY_ASSERT(revision_ == controller_.revision(), session_exception,
                "Wrong apply revision ${apply_revision} != ${revision}",
                ("revision", controller_.revision())("apply_revision", revision_));
            controller_.impl_->push_revision(revision_);
        }
        apply_ = false;
    }

    void chaindb_session::apply_changes() {
        if (apply_) {
            CYBERWAY_ASSERT(revision_ == controller_.revision(), session_exception,
                "Wrong apply revision ${apply_revision} != ${revision}",
                ("revision", controller_.revision())("apply_revision", revision_));
            controller_.apply_all_changes();
        }
    }

    void chaindb_session::squash() {
        if (apply_) {
            controller_.impl_->squash_revision(revision_);
        }
        apply_ = false;
    }

    void chaindb_session::undo() {
        if (apply_) {
            controller_.impl_->undo_revision(revision_);
        }
        apply_ = false;
    }

    uint64_t chaindb_session::calc_ram_bytes() const {
        return controller_.impl_->cache_.calc_ram_bytes(revision_);
    }

    find_info::find_info(const chaindb_controller& controller, account_name_t code) :
        cursor(cyberway::chaindb::invalid_cursor),
        pk(cyberway::chaindb::primary_key::End),
        controller(controller),
        code(code) {
    }

    find_info::find_info(cursor_t cursor, primary_key_t pk, cache_object_ptr object_ptr, const chaindb_controller& controller, account_name_t code) :
        cursor(cursor),
        pk(pk),
        object_ptr(std::move(object_ptr)),
        controller(controller),
        code(code) {
    }

    find_info::find_info(find_info&& other) noexcept :
        object_ptr(std::move(other.object_ptr)),
        controller(other.controller),
        code(other.code) {
        this->operator =(std::move(other));
    }

    find_info::~find_info() {
        if (is_cursor_initialized()) {
            controller.close({code, cursor});
        }
    }

    find_info&find_info::operator = (find_info&& other) noexcept {
        if (is_cursor_initialized()) {
            controller.close({code, cursor});
        }

        object_ptr = std::move(other.object_ptr);
        this->cursor = other.cursor;
        this->pk = other.pk;

        other.cursor = cyberway::chaindb::invalid_cursor;
        other.pk = cyberway::chaindb::primary_key::End;
        return *this;
    }

    find_info& find_info::operator ++() {
        pk = controller.next({code, cursor});
        object_ptr.reset();
        return *this;
    }

    find_info& find_info::operator --() {
        pk = controller.prev({code, cursor});
        object_ptr.reset();
        return *this;
    }

    bool find_info::operator == (cursor_t cursor) const {
        return this->cursor == cursor;
    }

    bool find_info::is_cursor_initialized() const {
        return (cursor > cyberway::chaindb::invalid_cursor);
    }

    find_info find_info::clone() const {
        return {is_cursor_initialized() ? controller.clone({code, cursor}) : cursor, pk,  object_ptr, controller, code};
    }

} } // namespace cyberway::chaindb
