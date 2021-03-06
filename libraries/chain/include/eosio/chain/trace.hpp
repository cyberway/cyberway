/**
 *  @file
 *  @copyright defined in eos/LICENSE
 */
#pragma once

#include <eosio/chain/action.hpp>
#include <eosio/chain/action_receipt.hpp>
#include <eosio/chain/block.hpp>
#include <eosio/chain/event.hpp>

namespace eosio { namespace chain {

   struct base_action_trace {
      base_action_trace( const action_receipt& r ):receipt(r){}
      base_action_trace(){}

      action_receipt       receipt;
      action               act;
      bool                 context_free = false;
      fc::microseconds     elapsed;
      string               console;

      transaction_id_type  trx_id; ///< the transaction that generated this action
      uint32_t             block_num = 0;
      block_timestamp_type block_time;
      fc::optional<block_id_type>     producer_block_id;
      fc::optional<fc::exception>     except;
      std::vector<event>              events;
   };

   struct action_trace : public base_action_trace {
      using base_action_trace::base_action_trace;

      vector<action_trace> inline_traces;
   };

   struct transaction_trace;
   using transaction_trace_ptr = std::shared_ptr<transaction_trace>;

   struct transaction_trace {
      transaction_id_type                        id;
      uint32_t                                   block_num = 0;
      block_timestamp_type                       block_time;
      fc::optional<block_id_type>                producer_block_id;
      fc::optional<transaction_receipt_header>   receipt;
      fc::microseconds                           elapsed;
      uint64_t                                   ram_bytes = 0;
      uint64_t                                   net_usage = 0;
      int64_t                                    storage_bytes = 0;
      bool                                       scheduled = false;
      vector<action_trace>                       action_traces; ///< disposable

      transaction_trace_ptr                      failed_dtrx_trace;
      fc::optional<fc::exception>                except;
      std::exception_ptr                         except_ptr;

      bool nested = false;
      bool sent_nested = false;
   };

} }  /// namespace eosio::chain

FC_REFLECT( eosio::chain::base_action_trace,
                    (receipt)(act)(context_free)(elapsed)(console)(trx_id)
                    (block_num)(block_time)(producer_block_id)(except) )

FC_REFLECT_DERIVED( eosio::chain::action_trace,
                    (eosio::chain::base_action_trace), (inline_traces) )

FC_REFLECT( eosio::chain::transaction_trace, (id)(block_num)(block_time)(producer_block_id)
                                             (receipt)(elapsed)(ram_bytes)(net_usage)(storage_bytes)(scheduled)
                                             (action_traces)(failed_dtrx_trace)(except)(nested)(sent_nested) )
