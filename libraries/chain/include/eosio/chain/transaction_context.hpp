#pragma once
#include <eosio/chain/controller.hpp>
#include <eosio/chain/trace.hpp>
#include "resource_limits.hpp"
#include <signal.h>

namespace cyberway { namespace chaindb {
    struct storage_payer_info;
} } // namespace cyberway::chaindb

namespace cyberway { namespace chain {
    struct providebw;
} } // namespace cyberway::chaindb

namespace eosio { namespace chain {

   using cyberway::chaindb::storage_payer_info;

// TODO: request bw, why provided?
//   struct provided_bandwith {
//       provided_bandwith() = default;
//
//       void confirm(account_name provider);
//
//       bool is_confirmed() const {return confirmed_;}
//
//       int64_t get_net_limit() const {return net_limit_;}
//       int64_t get_cpu_limit() const {return cpu_limit_;}
//
//       void set_net_limit(int64_t net_limit);
//       void set_cpu_limit(int64_t cpu_limit);
//
//       account_name get_provider() const {return provider_;}
//
//   private:
//
//       void verify_limits_not_confirmed();
//
//       int64_t net_limit_ = 0;
//       int64_t cpu_limit_ = 0;
//       bool confirmed_ = false;
//       account_name provider_;
//   };
//
//   struct bandwith_request_result {
//       std::map<account_name, provided_bandwith> bandwith;
//       uint64_t used_net;
//       uint64_t used_cpu;
//   };

   struct deadline_timer {
         deadline_timer();
         ~deadline_timer();

         void start(fc::time_point tp);
         void stop();

         static volatile sig_atomic_t expired;
      private:
         static void timer_expired(int);
         static bool initialized;
   };

   class transaction_context {
      private:
         void init( uint64_t initial_net_usage);

      public:

         transaction_context( controller& c,
                              const signed_transaction& t,
                              const transaction_id_type& trx_id,
                              fc::time_point start = fc::time_point::now() );

         void init_for_implicit_trx( uint64_t initial_net_usage = 0 );

         void init_for_input_trx( uint64_t packed_trx_unprunable_size,
                                  uint64_t packed_trx_prunable_size,
                                  bool skip_recording);

         void init_for_deferred_trx( fc::time_point published );

         void exec();
         void finalize();
         void squash();
         void undo();
         void validate_bw_usage();

         inline void add_net_usage( uint64_t u ) {
            net_usage += u;
            hard_limits[resource_limits::NET].check(net_usage);
            available_resources.add_net_usage(u);
            reset_billing_timer();
         }

         void checktime()const;

         void pause_billing_timer();
         void resume_billing_timer();
         void reset_billing_timer();
         
         int64_t get_used_cpu_time(fc::time_point now) const;
         uint32_t update_billed_cpu_time( fc::time_point now);
         uint64_t update_billed_ram_bytes();

         void add_storage_usage( const storage_payer_info& );

// TODO: request bw, why provided?
//         uint64_t get_provided_net_limit(account_name account) const;
//
//         uint64_t get_provided_cpu_limit(account_name account) const;
//
//         std::map<account_name, provided_bandwith> get_provided_bandwith() const {return provided_bandwith_;}
//
//         bool is_provided_bandwith_confirmed(account_name account) const;
//
//         void set_provided_bandwith(std::map<account_name, provided_bandwith>&& bandwith);
//
//         void set_provided_bandwith_limits(account_name account, uint64_t net_limit, uint64_t cpu_limit);
//
//         void confirm_provided_bandwith_limits(account_name account, account_name provider);

         void validate_referenced_accounts(const transaction& trx) const;

         const account_name& get_storage_provider(const account_name& owner) const;
         storage_payer_info get_storage_payer(const account_name& owner);

      private:

         friend struct controller_impl;
         friend class apply_context;

         void add_storage_provider(const cyberway::chain::providebw& bw);

         void dispatch_action( action_trace& trace, const action& a, account_name receiver, bool context_free = false, uint32_t recurse_depth = 0 );
         inline void dispatch_action( action_trace& trace, const action& a, bool context_free = false ) {
            dispatch_action(trace, a, a.account, context_free);
         };
         void schedule_transaction();
         void record_transaction( const transaction_id_type& id, fc::time_point_sec expire );

      /// Fields:
      public:
      
         struct hard_limit {
            const controller& control;
            const resource_limits::resource_id res_id; 
            const bool subjective; 
            uint64_t max = 0; 
            bool due_to_block = true; 
            hard_limit(const controller& control_, const resource_limits::resource_id res_id_, bool subjective_); 
            void init(uint64_t block_limit);
            void update(uint64_t limit); 
            void check(int64_t arg) const;
         }; 
          
         std::array<hard_limit, resource_limits::resources_num> hard_limits; 

         controller&                   control;
         const signed_transaction&     trx;
         transaction_id_type           id;
         // TODO: removed by CyberWay
         // optional<chainbase::database::session>  undo_session;
         optional<chaindb_session>     chaindb_undo_session;
         transaction_trace_ptr         trace;
         fc::time_point                start;

         fc::time_point                published;

         vector<action_receipt>        executed;
         flat_set<account_name>        bill_to_accounts;
         flat_map<account_name, int64_t> accounts_storage_deltas;

         resource_limits::ratios       pricelist;

         fc::microseconds              delay;
         bool                          is_input           = false;
         bool                          apply_context_free = true;

         fc::time_point                caller_set_deadline = fc::time_point::maximum();
         fc::microseconds              leeway = fc::microseconds(0);
         int64_t                       billed_cpu_time_us = 0;
         bool                          explicit_billed_cpu_time = false;

         uint64_t&                     billed_ram_bytes;
         bool                          explicit_billed_ram_bytes = false;

         fc::flat_map<account_name, account_name> storage_providers;

         bool                          is_nested = false;
         optional<transaction>         nested_trx;

      private:
         bool                          is_initialized = false;

         uint64_t&                     net_usage; /// reference to trace->net_usage

         int64_t&                      storage_bytes; /// reference to trace->storage_bytes

         fc::time_point                _deadline = fc::time_point::maximum();
         bool                          timer_off = false;    
         fc::time_point                pseudo_start;
         fc::microseconds              billed_time;
         fc::microseconds get_billing_timer_duration_limit()const {
             return fc::microseconds(std::min(available_resources.get_min_cpu_limit(), hard_limits[resource_limits::CPU].max));
         };

         deadline_timer                _deadline_timer;

// TODO: request bw, why provided?
//         std::map<account_name, provided_bandwith> provided_bandwith_;

        class available_resources_t {
            transaction_context& trx_ctx;
            fc::flat_map<account_name, uint64_t> cpu_limits;
            uint64_t min_cpu_limit = UINT64_MAX;
        public:
            available_resources_t(transaction_context& ctx): trx_ctx(ctx) { }

            void init();
            void update_storage_usage(const storage_payer_info&);
            void add_net_usage(int64_t delta);
            void check_cpu_usage(int64_t usage) const;
            uint64_t get_min_cpu_limit()const { return min_cpu_limit; };
        };
         
        available_resources_t available_resources;
    };

} }
