#include <eosio/chain/apply_context.hpp>
#include <eosio/chain/transaction_context.hpp>
#include <eosio/chain/authorization_manager.hpp>
#include <eosio/chain/exceptions.hpp>
#include <eosio/chain/resource_limits.hpp>
#include <eosio/chain/generated_transaction_object.hpp>
#include <eosio/chain/transaction_object.hpp>
#include <eosio/chain/global_property_object.hpp>

#include <cyberway/chaindb/controller.hpp>
#include <cyberway/chain/cyberway_contract_types.hpp>

#pragma push_macro("N")
#undef N
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/min.hpp>
#include <boost/accumulators/statistics/max.hpp>
#include <boost/accumulators/statistics/weighted_mean.hpp>
#include <boost/accumulators/statistics/weighted_variance.hpp>
#pragma pop_macro("N")

#include <chrono>

namespace eosio { namespace chain {
using namespace int_arithmetic;

namespace bacc = boost::accumulators;

   struct deadline_timer_verify {
      deadline_timer_verify() {
         //keep longest first in list. You're effectively going to take test_intervals[0]*sizeof(test_intervals[0])
         //time to do the the "calibration"
         int test_intervals[] = {50000, 10000, 5000, 1000, 500, 100, 50, 10};

         struct sigaction act;
         sigemptyset(&act.sa_mask);
         act.sa_handler = timer_hit;
         act.sa_flags = 0;
         if(sigaction(SIGALRM, &act, NULL))
            return;

         sigset_t alrm;
         sigemptyset(&alrm);
         sigaddset(&alrm, SIGALRM);
         int dummy;

         for(int& interval : test_intervals) {
            unsigned int loops = test_intervals[0]/interval;

            for(unsigned int i = 0; i < loops; ++i) {
               struct itimerval enable = {{0, 0}, {0, interval}};
               hit = 0;
               auto start = std::chrono::high_resolution_clock::now();
               if(setitimer(ITIMER_REAL, &enable, NULL))
                  return;
               while(!hit) {}
               auto end = std::chrono::high_resolution_clock::now();
               int timer_slop = std::chrono::duration_cast<std::chrono::microseconds>(end-start).count() - interval;

               //since more samples are run for the shorter expirations, weigh the longer expirations accordingly. This
               //helps to make a few results more fair. Two such examples: AWS c4&i5 xen instances being rather stable
               //down to 100us but then struggling with 50us and 10us. MacOS having performance that seems to correlate
               //with expiry length; that is, long expirations have high error, short expirations have low error.
               //That said, for these platforms, a tighter tolerance may possibly be achieved by taking performance
               //metrics in mulitple bins and appliying the slop based on which bin a deadline resides in. Not clear
               //if that's worth the extra complexity at this point.
               samples(timer_slop, bacc::weight = interval/(float)test_intervals[0]);
            }
         }
         timer_overhead = bacc::mean(samples) + sqrt(bacc::variance(samples))*2; //target 95% of expirations before deadline
         use_deadline_timer = timer_overhead < 1000;

         act.sa_handler = SIG_DFL;
         sigaction(SIGALRM, &act, NULL);
      }

      static void timer_hit(int) {
         hit = 1;
      }
      static volatile sig_atomic_t hit;

      bacc::accumulator_set<int, bacc::stats<bacc::tag::mean, bacc::tag::min, bacc::tag::max, bacc::tag::variance>, float> samples;
      bool use_deadline_timer = false;
      int timer_overhead;
   };
   volatile sig_atomic_t deadline_timer_verify::hit;
   static deadline_timer_verify deadline_timer_verification;

// TODO: request bw, why provided?
//   void provided_bandwith::confirm(account_name provider) {
//       verify_limits_not_confirmed();
//       confirmed_ = true;
//       provider_ = provider;
//   }
//
//   void provided_bandwith::set_net_limit(int64_t net_limit) {
//       verify_limits_not_confirmed();
//       this->net_limit_ = net_limit;
//   }
//
//   void provided_bandwith::set_cpu_limit(int64_t cpu_limit) {
//       verify_limits_not_confirmed();
//       this->cpu_limit_ = cpu_limit;
//   }
//
//   void provided_bandwith::verify_limits_not_confirmed() {
//        EOS_ASSERT(!confirmed_,  bandwith_already_confirmed, "Bandwith has been already confirmed. No changes could be done");
//   }

   deadline_timer::deadline_timer() {
      if(initialized)
         return;
      initialized = true;

      #define TIMER_STATS_FORMAT "min:${min}us max:${max}us mean:${mean}us stddev:${stddev}us"
      #define TIMER_STATS \
         ("min", bacc::min(deadline_timer_verification.samples))("max", bacc::max(deadline_timer_verification.samples)) \
         ("mean", (int)bacc::mean(deadline_timer_verification.samples))("stddev", (int)sqrt(bacc::variance(deadline_timer_verification.samples))) \
         ("t", deadline_timer_verification.timer_overhead)

      if(deadline_timer_verification.use_deadline_timer) {
         struct sigaction act;
         act.sa_handler = timer_expired;
         sigemptyset(&act.sa_mask);
         act.sa_flags = 0;
         if(sigaction(SIGALRM, &act, NULL) == 0) {
            ilog("Using ${t}us deadline timer for checktime: " TIMER_STATS_FORMAT, TIMER_STATS);
            return;
         }
      }

      wlog("Using polled checktime; deadline timer too inaccurate: " TIMER_STATS_FORMAT, TIMER_STATS);
      deadline_timer_verification.use_deadline_timer = false; //set in case sigaction() fails above
   }

   void deadline_timer::start(fc::time_point tp) {
      if(tp == fc::time_point::maximum()) {
         expired = 0;
         return;
      }
      if(!deadline_timer_verification.use_deadline_timer) {
         expired = 1;
         return;
      }
      microseconds x = tp.time_since_epoch() - fc::time_point::now().time_since_epoch();
      if(x.count() <= deadline_timer_verification.timer_overhead)
         expired = 1;
      else {
         struct itimerval enable = {{0, 0}, {0, (int)x.count()-deadline_timer_verification.timer_overhead}};
         expired = 0;
         expired |= !!setitimer(ITIMER_REAL, &enable, NULL);
      }
   }

   void deadline_timer::stop() {
      if(expired)
         return;
      struct itimerval disable = {{0, 0}, {0, 0}};
      setitimer(ITIMER_REAL, &disable, NULL);
   }

   deadline_timer::~deadline_timer() {
      stop();
   }

   void deadline_timer::timer_expired(int) {
      expired = 1;
   }
   volatile sig_atomic_t deadline_timer::expired = 0;
   bool deadline_timer::initialized = false;

   transaction_context::transaction_context( controller& c,
                                             const signed_transaction& t,
                                             const transaction_id_type& trx_id,
                                             fc::time_point s )
   :control(c)
   ,trx(t)
   ,id(trx_id)
   // TODO: removed by CyberWay
   // ,undo_session()
   ,chaindb_undo_session()
   ,trace(std::make_shared<transaction_trace>())
   ,start(s)
   ,billed_ram_bytes(trace->ram_bytes)
   ,net_usage(trace->net_usage)
   ,storage_bytes(trace->storage_bytes)
   ,pseudo_start(s)
   {
      if (!c.skip_db_sessions()) {
         // TODO: removed by CyberWay
         // undo_session = c.mutable_db().start_undo_session(true);
         chaindb_undo_session = c.chaindb().start_undo_session(true);
      }
      trace->id = id;
      trace->block_num = c.pending_block_state()->block_num;
      trace->block_time = c.pending_block_time();
      trace->producer_block_id = c.pending_producer_block_id();
      executed.reserve( trx.total_actions() );
      EOS_ASSERT( trx.transaction_extensions.size() == 0, unsupported_feature, "we don't support any extensions yet" );
   }

   void transaction_context::init(uint64_t initial_net_usage)
   {
      EOS_ASSERT( !is_initialized, transaction_exception, "cannot initialize twice" );
      const static int64_t large_number_no_overflow = std::numeric_limits<int64_t>::max()/2;

      const auto& cfg = control.get_global_properties().configuration;
      auto& rl = control.get_mutable_resource_limits_manager();

      net_limit = rl.get_block_limit(resource_limits::NET, cfg);
      objective_duration_limit = fc::microseconds( rl.get_block_limit(resource_limits::CPU, cfg) );

      _deadline = start + objective_duration_limit;

      // Possibly lower net_limit to the maximum net usage a transaction is allowed to be billed
      if( config::max_transaction_usage[resource_limits::NET] <= net_limit ) {
         net_limit = config::max_transaction_usage[resource_limits::NET];
         net_limit_due_to_block = false;
      }

      // Possibly lower objective_duration_limit to the maximum cpu usage a transaction is allowed to be billed
      if( config::max_transaction_usage[resource_limits::CPU] <= objective_duration_limit.count() ) {
         objective_duration_limit = fc::microseconds(config::max_transaction_usage[resource_limits::CPU]);
         billing_timer_exception_code = tx_cpu_usage_exceeded::code_value;
         _deadline = start + objective_duration_limit;
      }

      // Possibly lower net_limit to optional limit set in the transaction header
      uint64_t trx_specified_net_usage_limit = static_cast<uint64_t>(trx.max_net_usage_words.value) * 8;
      if( trx_specified_net_usage_limit > 0 && trx_specified_net_usage_limit <= net_limit ) {
         net_limit = trx_specified_net_usage_limit;
         net_limit_due_to_block = false;
      }

      // Possibly lower RAM limit to optional limit set in transaction header
      if( trx.max_ram_kbytes > 0 ) {
         ram_bytes_limit = uint64_t(trx.max_ram_kbytes) << 10;
      }

      // Possibly lower STORAGE limit to optional limit set in transaction header
      if( trx.max_storage_kbytes > 0 ) {
         storage_bytes_limit = uint64_t(trx.max_storage_kbytes) << 10;
      }

      // Possibly lower objective_duration_limit to optional limit set in transaction header
      if( trx.max_cpu_usage_ms > 0 ) {
         auto trx_specified_cpu_usage_limit = fc::milliseconds(trx.max_cpu_usage_ms);
         if( trx_specified_cpu_usage_limit <= objective_duration_limit ) {
            objective_duration_limit = trx_specified_cpu_usage_limit;
            billing_timer_exception_code = tx_cpu_usage_exceeded::code_value;
            _deadline = start + objective_duration_limit;
         }
      }

      initial_objective_duration_limit = objective_duration_limit;

      if( billed_cpu_time_us > 0 ) // could also call on explicit_billed_cpu_time but it would be redundant
         validate_cpu_usage_to_bill( billed_cpu_time_us, false ); // Fail early if the amount to be billed is too high

      using cyberway::chain::providebw;

      storage_providers.reserve(trx.actions.size());
      for( const auto& act : trx.actions ) {
         if (act.account == providebw::get_account() && act.name == providebw::get_name()) {
            add_storage_provider(act.data_as<providebw>());
         }
         for( const auto& auth : act.authorization ) {
// TODO: requestbw
//             const auto provided_bw_it = provided_bandwith_.find(auth.actor);
//             if(provided_bw_it != provided_bandwith_.end()) {
//                 bill_to_accounts.insert( provided_bw_it->second.get_provider() );
//             } else {
                 bill_to_accounts.insert( auth.actor );
//             }
         }
      }

      for( const auto& bw : storage_providers ) {
         bill_to_accounts.erase(bw.first);
      }

      available_resources.init(explicit_billed_cpu_time, rl, bill_to_accounts, control.pending_block_time());

      eager_net_limit = net_limit;

      //TODO:? net/cpu leeway

      billing_timer_duration_limit = _deadline - start;

      // Check if deadline is limited by caller-set deadline (only change deadline if billed_cpu_time_us is not set)
      if( explicit_billed_cpu_time || deadline < _deadline ) {
         _deadline = deadline;
         deadline_exception_code = deadline_exception::code_value;
      } else {
         deadline_exception_code = billing_timer_exception_code;
      }

      eager_net_limit = (eager_net_limit/8)*8; // Round down to nearest multiple of word size (8 bytes) so check_net_usage can be efficient

      if( initial_net_usage > 0 )
         add_net_usage( initial_net_usage );  // Fail early if current net usage is already greater than the calculated limit

      checktime(); // Fail early if deadline has already been exceeded

      if(control.skip_trx_checks())
         _deadline_timer.expired = 0;
      else
         _deadline_timer.start(_deadline);

      is_initialized = true;
   }

   void transaction_context::add_storage_provider(const cyberway::chain::providebw& bw) {
       EOS_ASSERT(bw.provider != bw.account, bw_provider_error,
           "Fail to set the provider ${provider} for the account ${account}, because it is the same account",
           ("account", bw.account)("provider", bw.provider));
       EOS_ASSERT(!bw.provider.empty(), bw_provider_error,
           "Fail to set a empty provider for the account ${account}",
           ("account", bw.account));
       EOS_ASSERT(!bw.account.empty(), bw_provider_error,
           "Fail to set the provider ${provider} for an empty account",
           ("provider", bw.provider));

       const auto itr = storage_providers.find(bw.account);
       EOS_ASSERT(itr == storage_providers.end(), bw_provider_error,
           "Fail to set the provider ${new_provider} for the account ${account}, "
           "because it already has the provider ${provider}",
           ("account", bw.account)("provider", itr->second)("new_provider", bw.provider));

       storage_providers.emplace(bw.account, bw.provider);
   }

   void transaction_context::init_for_implicit_trx( uint64_t initial_net_usage  )
   {
      published = control.pending_block_time();
      init( initial_net_usage);
   }

   void transaction_context::init_for_input_trx( uint64_t packed_trx_unprunable_size,
                                                 uint64_t packed_trx_prunable_size,
                                                 bool skip_recording )
   {
      const auto& cfg = control.get_global_properties().configuration;

      uint64_t discounted_size_for_pruned_data = packed_trx_prunable_size;
      if( cfg.context_free_discount_net_usage_den > 0
          && cfg.context_free_discount_net_usage_num < cfg.context_free_discount_net_usage_den )
      {
         discounted_size_for_pruned_data *= cfg.context_free_discount_net_usage_num;
         discounted_size_for_pruned_data =  ( discounted_size_for_pruned_data + cfg.context_free_discount_net_usage_den - 1)
                                                                                    / cfg.context_free_discount_net_usage_den; // rounds up
      }

      uint64_t initial_net_usage = static_cast<uint64_t>(cfg.base_per_transaction_net_usage)
                                    + packed_trx_unprunable_size + discounted_size_for_pruned_data;


      if( trx.delay_sec.value > 0 ) {
          // If delayed, also charge ahead of time for the additional net usage needed to retire the delayed transaction
          // whether that be by successfully executing, soft failure, hard failure, or expiration.
         initial_net_usage += static_cast<uint64_t>(cfg.base_per_transaction_net_usage)
                               + static_cast<uint64_t>(config::transaction_id_net_usage);
      }

      published = control.pending_block_time();
      is_input = true;
      if (!control.skip_trx_checks()) {
         control.validate_expiration(trx);
         control.validate_tapos(trx);
         validate_referenced_accounts(trx);
      }
      init( initial_net_usage);
      if (!skip_recording)
         record_transaction( id, trx.expiration ); /// checks for dupes
   }

   void transaction_context::init_for_deferred_trx( fc::time_point p )
   {
      published = p;
      trace->scheduled = true;
      apply_context_free = false;
      init( 0 );
   }

   void transaction_context::exec() {
      EOS_ASSERT( is_initialized, transaction_exception, "must first initialize" );

      if( apply_context_free ) {
         for( const auto& act : trx.context_free_actions ) {
            trace->action_traces.emplace_back();
            dispatch_action( trace->action_traces.back(), act, true );
         }
      }

      if( delay == fc::microseconds() ) {
         for( const auto& act : trx.actions ) {
            trace->action_traces.emplace_back();
            dispatch_action( trace->action_traces.back(), act );
         }
      } else {
         schedule_transaction();
      }
   }

   void transaction_context::finalize() {
      validate_bw_usage();
      control.get_mutable_resource_limits_manager().add_transaction_usage( bill_to_accounts,
         static_cast<uint64_t>(billed_cpu_time_us),
         net_usage,
         billed_ram_bytes,
         control.pending_block_time()); // can fail due to billed_ram_bytes
   }

   void transaction_context::validate_bw_usage() {
       EOS_ASSERT( is_initialized, transaction_exception, "must first initialize" );

       if( is_input ) {
          auto& am = control.get_mutable_authorization_manager();
          for( const auto& act : trx.actions ) {
             for( const auto& auth : act.authorization ) {
                am.update_permission_usage( am.get_permission(auth) );
             }
          }
       }
       int64_t block_time = control.pending_block_time().sec_since_epoch();
       auto& rl = control.get_mutable_resource_limits_manager();

       update_billed_ram_bytes();

       check_ram_usage();

       check_storage_usage();

       net_usage = ((net_usage + 7)/8)*8; // Round up to nearest multiple of word size (8 bytes)

       eager_net_limit = net_limit;

       check_net_usage();

       auto now = fc::time_point::now();
       trace->elapsed = now - start;

       update_billed_cpu_time( now );

       validate_cpu_usage_to_bill( billed_cpu_time_us );
   }

   void transaction_context::squash() {
      // TODO: removed by CyberWay
      // if (undo_session) undo_session->squash();
      if (chaindb_undo_session) chaindb_undo_session->squash();
   }

   void transaction_context::undo() {
      // TODO: removed by CyberWay
      // if (undo_session) undo_session->undo();
      if (chaindb_undo_session) chaindb_undo_session->undo();
   }

   void transaction_context::check_net_usage()const {
      if (!control.skip_trx_checks()) {
         if( BOOST_UNLIKELY(net_usage > eager_net_limit) ) {
            if ( net_limit_due_to_block ) {
               EOS_THROW( block_net_usage_exceeded,
                          "not enough space left in block: ${net_usage} > ${net_limit}",
                          ("net_usage", net_usage)("net_limit", eager_net_limit) );
            } else {
               EOS_THROW( tx_net_usage_exceeded,
                          "transaction net usage is too high: ${net_usage} > ${net_limit}",
                          ("net_usage", net_usage)("net_limit", eager_net_limit) );
            }
         }
      }
   }

   void transaction_context::check_ram_usage()const {
      EOS_ASSERT(control.skip_trx_checks() || explicit_billed_ram_bytes || !ram_bytes_limit || billed_ram_bytes < ram_bytes_limit,
         tx_ram_usage_exceeded, "transaction ram usage is too high: ${ram_usage} > ${ram_limit}",
         ("ram_usage", billed_ram_bytes)("ram_limit", ram_bytes_limit) );
   }

   void transaction_context::check_storage_usage()const {
      EOS_ASSERT(control.skip_trx_checks() || !storage_bytes_limit || storage_bytes < storage_bytes_limit,
         tx_storage_usage_exceeded, "transaction storage usage is too high: ${storage_usage} > ${storage_limit}",
         ("storage_usage", storage_bytes)("storage_limit", storage_bytes_limit) );
   }

   void transaction_context::checktime()const {

      if(BOOST_LIKELY(_deadline_timer.expired == false))
         return;
      auto now = fc::time_point::now();
      if( BOOST_UNLIKELY( now > _deadline ) ) {
         // edump((now-start)(now-pseudo_start));
         if( explicit_billed_cpu_time || deadline_exception_code == deadline_exception::code_value ) {
            EOS_THROW( deadline_exception, "deadline exceeded", ("now", now)("deadline", _deadline)("start", start) );
         } else if( deadline_exception_code == block_cpu_usage_exceeded::code_value ) {
            EOS_THROW( block_cpu_usage_exceeded,
                        "not enough time left in block to complete executing transaction",
                        ("now", now)("deadline", _deadline)("start", start)("billing_timer", now - pseudo_start) );
         } else if( deadline_exception_code == tx_cpu_usage_exceeded::code_value ) {
            EOS_THROW( tx_cpu_usage_exceeded,
                     "transaction was executing for too long",
                     ("now", now)("deadline", _deadline)("start", start)("billing_timer", now - pseudo_start) );
         } else if( deadline_exception_code == leeway_deadline_exception::code_value ) {
            EOS_THROW( leeway_deadline_exception,
                        "the transaction was unable to complete by deadline, "
                        "but it is possible it could have succeeded if it were allowed to run to completion",
                        ("now", now)("deadline", _deadline)("start", start)("billing_timer", now - pseudo_start) );
         }
         EOS_ASSERT( false,  transaction_exception, "unexpected deadline exception code" );
      }
      available_resources.check_cpu_usage((now - pseudo_start).count());
   }

   void transaction_context::pause_billing_timer() {
      if( explicit_billed_cpu_time || pseudo_start == fc::time_point() ) return; // either irrelevant or already paused

      auto now = fc::time_point::now();
      billed_time = now - pseudo_start;

      deadline_exception_code = deadline_exception::code_value; // Other timeout exceptions cannot be thrown while billable timer is paused.
      pseudo_start = fc::time_point();
      _deadline_timer.stop();
   }

   void transaction_context::resume_billing_timer() {
      if( explicit_billed_cpu_time || pseudo_start != fc::time_point() ) return; // either irrelevant or already running

      auto now = fc::time_point::now();
      pseudo_start = now - billed_time;
      if( (pseudo_start + billing_timer_duration_limit) <= deadline ) {
         _deadline = pseudo_start + billing_timer_duration_limit;
         deadline_exception_code = billing_timer_exception_code;
      } else {
         _deadline = deadline;
         deadline_exception_code = deadline_exception::code_value;
      }
      _deadline_timer.start(_deadline);
   }

   void transaction_context::validate_cpu_usage_to_bill( int64_t billed_us, bool check_minimum )const {
      if (!control.skip_trx_checks()) {
         if( check_minimum ) {
            const auto& cfg = control.get_global_properties().configuration;
            EOS_ASSERT( billed_us >= cfg.min_transaction_cpu_usage, transaction_exception,
                        "cannot bill CPU time less than the minimum of ${min_billable} us",
                        ("min_billable", cfg.min_transaction_cpu_usage)("billed_cpu_time_us", billed_us)
                      );
         }

         if( billing_timer_exception_code == block_cpu_usage_exceeded::code_value ) {
            EOS_ASSERT( billed_us <= objective_duration_limit.count(),
                        block_cpu_usage_exceeded,
                        "billed CPU time (${billed} us) is greater than the billable CPU time left in the block (${billable} us)",
                        ("billed", billed_us)("billable", objective_duration_limit.count())
                      );
         } else {
            EOS_ASSERT( billed_us <= objective_duration_limit.count(),
                        tx_cpu_usage_exceeded,
                        "billed CPU time (${billed} us) is greater than the maximum billable CPU time for the transaction (${billable} us)",
                        ("billed", billed_us)("billable", objective_duration_limit.count())
                     );
         }
      }
   }

   void transaction_context::add_storage_usage( const storage_payer_info& storage, const bool is_authorized ) {
      storage_bytes += storage.delta;
      check_storage_usage();

      auto now = fc::time_point::now();
      if (available_resources.update_storage_usage(storage)) {
         available_resources.check_cpu_usage((now - pseudo_start).count());
      }

      auto& rl = control.get_mutable_resource_limits_manager();
      rl.add_storage_usage(storage.payer, storage.delta, control.pending_block_slot(), is_authorized);
   }

   int64_t transaction_context::get_billed_cpu_time(fc::time_point now)const {
      if (explicit_billed_cpu_time){
         return billed_cpu_time_us;
      }
      const auto& cfg = control.get_global_properties().configuration;
      return std::max((now - pseudo_start).count(), static_cast<int64_t>(cfg.min_transaction_cpu_usage));
   }

   uint32_t transaction_context::update_billed_cpu_time( fc::time_point now ) {
      billed_cpu_time_us = get_billed_cpu_time(now);
      return static_cast<uint32_t>(billed_cpu_time_us);
   }

   uint64_t transaction_context::update_billed_ram_bytes() {
      if( !explicit_billed_ram_bytes ) {
         const auto& cfg = control.get_global_properties().configuration;
         billed_ram_bytes = chaindb_undo_session->calc_ram_bytes();
         billed_ram_bytes = ((billed_ram_bytes + 1023) >> 10) << 10; // Round up to nearest kbytes
         billed_ram_bytes = std::max(billed_ram_bytes, cfg.min_transaction_ram_usage);

         check_ram_usage();
         explicit_billed_ram_bytes = true;
      }
      return billed_ram_bytes;
   }

// TODO: requested bw, why provided ?
//   uint64_t transaction_context::get_provided_net_limit(account_name account) const {
//       const auto provided_bw_it = provided_bandwith_.find(account);
//
//       if (provided_bw_it == provided_bandwith_.end()) {
//           return 0;
//       }
//
//       return provided_bw_it->second.get_net_limit();
//   }
//
//   uint64_t transaction_context::get_provided_cpu_limit(account_name account) const {
//       const auto provided_bw_it = provided_bandwith_.find(account);
//
//       if (provided_bw_it == provided_bandwith_.end()) {
//           return 0;
//       }
//
//       return provided_bw_it->second.get_cpu_limit();
//   }
//
//   bool transaction_context::is_provided_bandwith_confirmed(account_name account) const {
//       const auto provided_bw_it = provided_bandwith_.find(account);
//
//       if (provided_bw_it == provided_bandwith_.end()) {
//           return 0;
//       }
//
//       return provided_bw_it->second.is_confirmed();
//   }
//
//   void transaction_context::set_provided_bandwith(std::map<account_name, provided_bandwith>&& bandwith) {
//       provided_bandwith_ = std::move(bandwith);
//   }
//
//   void transaction_context::set_provided_bandwith_limits(account_name account, uint64_t net_limit, uint64_t cpu_limit) {
//        provided_bandwith_[account].set_net_limit(net_limit);
//        provided_bandwith_[account].set_cpu_limit(cpu_limit);
//   }
//
//   void transaction_context::confirm_provided_bandwith_limits(account_name account, account_name provider) {
//        provided_bandwith_[account].confirm(provider);
//   }

   void transaction_context::dispatch_action( action_trace& trace, const action& a, account_name receiver, bool context_free, uint32_t recurse_depth ) {
      apply_context  acontext( control, *this, a, recurse_depth );
      acontext.context_free = context_free;
      acontext.receiver     = receiver;

      acontext.exec( trace );
   }

   void transaction_context::schedule_transaction() {
      // Charge ahead of time for the additional net usage needed to retire the delayed transaction
      // whether that be by successfully executing, soft failure, hard failure, or expiration.
      if( trx.delay_sec.value == 0 ) { // Do not double bill. Only charge if we have not already charged for the delay.
         const auto& cfg = control.get_global_properties().configuration;
         add_net_usage( static_cast<uint64_t>(cfg.base_per_transaction_net_usage)
                         + static_cast<uint64_t>(config::transaction_id_net_usage) ); // Will exit early if net usage cannot be payed.
      }

      auto first_auth = trx.first_authorizor();

      uint32_t trx_size = 0;
      auto& chaindb = control.chaindb();
      auto trx_table = chaindb.get_table<generated_transaction_object>();
      auto res = trx_table.emplace( get_storage_payer(first_auth), [&]( auto& gto ) {
        gto.trx_id      = id;
        gto.sender      = account_name(); /// delayed transactions have no sender
        gto.sender_id   = transaction_id_to_sender_id( gto.trx_id );
        gto.published   = control.pending_block_time();
        gto.delay_until = gto.published + delay;
        gto.expiration  = gto.delay_until + fc::seconds(control.get_global_properties().configuration.deferred_trx_expiration_window);
        trx_size = gto.set( trx );
      });

// TODO: Removed by CyberWay
//      add_ram_usage( cgto.payer, (config::billable_size_v<generated_transaction_object> + trx_size) );
   }

   void transaction_context::record_transaction( const transaction_id_type& id, fc::time_point_sec expire ) {
      auto trx_idx = control.chaindb().get_index<transaction_object, by_trx_id>();
      auto itr = trx_idx.find(id);
      EOS_ASSERT(trx_idx.end() == itr, tx_duplicate, "duplicate transaction ${id}", ("id", id ));

      trx_idx.emplace([&](transaction_object& transaction) {
         transaction.trx_id     = id;
         transaction.expiration = expire;
      });
   } /// record_transaction

   void transaction_context::validate_referenced_accounts(const transaction& trx) const {
      auto& chaindb = control.chaindb();
      const auto& auth_manager = control.get_authorization_manager();

      for( const auto& a : trx.context_free_actions ) {
         auto* code = chaindb.find<account_object>(a.account);
         EOS_ASSERT( code != nullptr, transaction_exception,
                     "action's code account '${account}' does not exist", ("account", a.account) );
         EOS_ASSERT( a.authorization.size() == 0, transaction_exception,
                     "context-free actions cannot have authorizations" );
      }

      bool one_auth = false;
      for( const auto& a : trx.actions ) {
         auto* code = chaindb.find<account_object>(a.account);
         EOS_ASSERT( code != nullptr, transaction_exception,
                     "action's code account '${account}' does not exist", ("account", a.account) );
         for( const auto& auth : a.authorization ) {
            one_auth = true;
            auto* actor = chaindb.find<account_object>(auth.actor);
            EOS_ASSERT( actor  != nullptr, transaction_exception,
                        "action's authorizing actor '${account}' does not exist", ("account", auth.actor) );
            EOS_ASSERT( auth_manager.find_permission(auth) != nullptr, transaction_exception,
                        "action's authorizations include a non-existent permission: {permission}",
                        ("permission", auth) );
         }
      }
      EOS_ASSERT( one_auth, tx_no_auths, "transaction must have at least one authorization" );
   }

   const account_name& transaction_context::get_storage_provider(const account_name& owner) const {
      if (owner.empty()) {
          return owner;
      }

      auto itr = storage_providers.find(owner);
      if (storage_providers.end() == itr) {
          return owner;
      }
      return itr->second;
   }

   storage_payer_info transaction_context::get_storage_payer(const account_name& owner) {
      return {*this, owner, get_storage_provider(owner)};
   }

    void transaction_context::available_resources_t::init(bool ecpu_time, resource_limits_manager& rl, const flat_set<account_name>& accounts, fc::time_point pending_block_time) {
        explicit_cpu_time = ecpu_time;
        pricelist = rl.get_pricelist();
        auto& cpu_price = pricelist[resource_limits::CPU];
        rl.update_account_usage(accounts, block_timestamp_type(pending_block_time).slot);
        min_cpu_limit = UINT64_MAX;
        for (const auto& a : accounts) {
            auto balance = rl.get_account_balance(pending_block_time, a, pricelist, true);
            auto& lim = cpu_limits[a];
            lim = UINT64_MAX;
            if (cpu_price.numerator && balance < UINT64_MAX) {
                lim = safe_prop(balance, cpu_price.denominator, cpu_price.numerator);
            }
            min_cpu_limit = std::min(lim, min_cpu_limit);
        }
    }

    bool transaction_context::available_resources_t::update_storage_usage(const storage_payer_info& storage) {
        if (explicit_cpu_time || !storage.delta) {
            return false;
        }
        auto lim_itr = cpu_limits.find(storage.payer);
        if (lim_itr == cpu_limits.end()) {
            return false;
        }
        auto& lim = lim_itr->second;
        uint64_t delta_abs = std::abs(storage.delta);
        
        auto& storage_price = pricelist[resource_limits::STORAGE];
        auto& cpu_price     = pricelist[resource_limits::CPU];

        auto cost = safe_prop(delta_abs, storage_price.numerator, storage_price.denominator);
        auto cpu = cost ? (cpu_price.numerator ? safe_prop(cost, cpu_price.denominator, cpu_price.numerator) : UINT64_MAX) : 0;

        bool need_to_update_min = (lim == min_cpu_limit) && (storage.delta < 0);
        if (storage.delta > 0) {
            EOS_ASSERT(lim >= cpu, resource_exhausted_exception,
                "account ${a} has insufficient staked tokens: unspent cpu = ${b}, cost = ${c}, cpu equivalent = ${e}",
                ("a", storage.payer)("b", lim)("c", cost)("e", cpu));
            lim -= cpu;
        }
        else {
            lim = (UINT64_MAX - lim) > cpu ? lim + cpu : UINT64_MAX;
        }
        auto prev_min_cpu = min_cpu_limit;
        if (need_to_update_min) {
            min_cpu_limit = UINT64_MAX;
            for (const auto& b : cpu_limits) {
                min_cpu_limit = std::min(min_cpu_limit, b.second);
            }
        }
        else {
            min_cpu_limit = std::min(lim, min_cpu_limit);
        }
        return min_cpu_limit < prev_min_cpu;
    }

    void transaction_context::available_resources_t::add_net_usage(int64_t delta) {
        EOS_ASSERT(delta >= 0, transaction_exception, "SYSTEM: available_resources_t::add_net_usage, usage_delta < 0");
        auto& cpu_price = pricelist[resource_limits::CPU];
        if (explicit_cpu_time || !delta || !cpu_price.numerator) {
            return;
        }
        
        auto& net_price = pricelist[resource_limits::NET];
        
        auto cost = safe_prop(static_cast<uint64_t>(delta), net_price.numerator, net_price.denominator);
        auto cpu = safe_prop(cost, cpu_price.denominator, cpu_price.numerator);

        EOS_ASSERT(min_cpu_limit >= cpu, resource_exhausted_exception,
            "transaction costs too much; unspent cpu = ${b}, cost cpu equivalent = ${e}", ("b", min_cpu_limit)("e", cpu));
        min_cpu_limit = UINT64_MAX;
        for (auto& b : cpu_limits) {
            EOS_ASSERT(b.second >= cpu, transaction_exception, "SYSTEM: incorrect cpu limit");
            b.second -= cpu;
            min_cpu_limit = std::min(min_cpu_limit, b.second);
        }
    }

    void transaction_context::available_resources_t::check_cpu_usage(int64_t usage)const {
        EOS_ASSERT(explicit_cpu_time || min_cpu_limit >= usage, resource_exhausted_exception,
            "transaction costs too much; unspent cpu = ${b}, usage = ${u}", ("b", min_cpu_limit)("u", usage));
    }

    int64_t transaction_context::get_min_cpu_limit()const {
        auto ret_unsigned = available_resources.get_min_cpu_limit();
        return ret_unsigned > static_cast<uint64_t>(INT64_MAX) ? INT64_MAX : static_cast<int64_t>(ret_unsigned);
    }
} } /// eosio::chain
