configure_file(${CMAKE_CURRENT_SOURCE_DIR}/include/eosio/chain/core_symbol.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/include/eosio/chain/core_symbol.hpp)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/genesis_state_root_key.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/genesis_state_root_key.cpp)

file(GLOB HEADERS "include/eosio/chain/*.hpp"
                  "include/eosio/chain/webassembly/*.hpp"
                  "include/cyberway/chaindb/*.hpp"
                  "include/cyberway/chain/*.hpp"
                  "include/cyberway/genesis/*.hpp"
                  "${CMAKE_CURRENT_BINARY_DIR}/include/eosio/chain/core_symbol.hpp" )

include(ChaindbMongo)

## SORT .cpp by most likely to change / break compile
add_library( eosio_chain
             merkle.cpp
             name.cpp
             transaction.cpp
             block_header.cpp
             block_header_state.cpp
             block_state.cpp
             fork_database.cpp
             controller.cpp
             authorization_manager.cpp
             resource_limits.cpp
             stake.cpp
             block_log.cpp
             transaction_context.cpp
             eosio_contract.cpp
             eosio_contract_abi.cpp
             chain_config.cpp
             chain_id_type.cpp
             genesis_state.cpp
             ${CMAKE_CURRENT_BINARY_DIR}/genesis_state_root_key.cpp

#             chain_config.cpp
#             block_trace.cpp
              wast_to_wasm.cpp
              wasm_interface.cpp
              wasm_eosio_validation.cpp
              wasm_eosio_injection.cpp
              apply_context.cpp
              abi_serializer.cpp
              asset.cpp
              snapshot.cpp
              snapshot_controller.cpp

             webassembly/wavm.cpp
             webassembly/wabt.cpp

             chaindb/controller.cpp
             chaindb/names.cpp
             chaindb/undo_state.cpp
             chaindb/cache_map.cpp
             chaindb/journal.cpp
             chaindb/abi_info.cpp
             chaindb/storage_calculator.cpp
             chaindb/typed_name.cpp
             chaindb/account_abi_info.cpp
             chaindb/value_verifier.cpp
             chaindb/index_order_validator.cpp

             cyberway/domain_name.cpp
             cyberway/cyberway_contract.cpp
             genesis/genesis_import.cpp
             ${CHAINDB_SRCS}

#             get_config.cpp
#             global_property_object.cpp
#
#             contracts/chain_initializer.cpp


             transaction_metadata.cpp
             ${HEADERS}
             )

target_link_libraries( eosio_chain fc chainbase Logging IR WAST WASM Runtime
                       softfloat builtins wabt
                       ${CHAINDB_LIBS})
target_include_directories( eosio_chain
                            PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_BINARY_DIR}/include"
                                   "${CMAKE_CURRENT_SOURCE_DIR}/../wasm-jit/Include"
                                   "${CMAKE_SOURCE_DIR}/libraries/wabt"
                                   "${CMAKE_BINARY_DIR}/libraries/wabt"
                            PRIVATE ${CHAINDB_INCS}
                            )
target_compile_definitions( eosio_chain PRIVATE ${CHAINDB_DEFS})

install( TARGETS eosio_chain
   RUNTIME DESTINATION ${CMAKE_INSTALL_FULL_BINDIR}
   LIBRARY DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}
   ARCHIVE DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}
)
install( DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
      DESTINATION ${CMAKE_INSTALL_FULL_INCLUDEDIR}/
      FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h" PATTERN "webassembly" EXCLUDE
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/eosio/chain/core_symbol.hpp DESTINATION ${CMAKE_INSTALL_FULL_INCLUDEDIR}/eosio/chain)
#if(MSVC)
#  set_source_files_properties( db_init.cpp db_block.cpp database.cpp block_log.cpp PROPERTIES COMPILE_FLAGS "/bigobj" )
#endif(MSVC)
