#pragma once

#include <cyberway/chaindb/common.hpp>
#include <cyberway/chaindb/cache_item.hpp>

namespace cyberway { namespace chaindb {

    struct table_info {
        const account_name_t code     = 0;
        const scope_name_t   scope    = 0;
        const table_def*     table    = nullptr;
        const order_def*     pk_order = nullptr;
        const abi_info*      abi      = nullptr;

        cache_object_ptr     account_ptr; // ptr to account with abi

        table_info(account_name_t c, scope_name_t s)
        : code(c), scope(s) {
        }

        bool is_valid() const {
            assert((nullptr == table) == (!account_ptr) == (nullptr == pk_order) == (nullptr == abi));
            return nullptr != table;
        }

        table_name_t table_name() const {
            assert(is_valid());
            return table->name.value;
        }

        service_state to_service(const primary_key_t pk = primary_key::Unset) const {
            service_state service;

            service.code  = code;
            service.scope = scope;
            service.table = table_name();
            service.pk    = pk;

            return service;
        }
    }; // struct table_info

    struct index_info: public table_info {
        const index_def* index = nullptr;

        using table_info::table_info;

        index_info(const table_info& src)
        : table_info(src) {
        }

        bool is_valid() const {
            return table_info::is_valid() && nullptr != index;
        }

        index_name_t index_name() const {
            assert(is_valid());
            return index->name.value;
        }
    }; // struct index_info

} } // namespace cyberway::chaindb