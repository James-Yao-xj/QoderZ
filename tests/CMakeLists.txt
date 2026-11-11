file(GLOB_RECURSE qoderz_test_sources CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/integration/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/support/*.cpp"
)

# The end-to-end tests spawn real processes and run in their own binary, so the unit suite stays
# fast and `qoderz_e2e` can be pointed at a separately built gateway.
list(REMOVE_ITEM qoderz_test_sources
  "${CMAKE_CURRENT_SOURCE_DIR}/integration/e2e_server_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/integration/process_runner.cpp"
)

add_executable(qoderz_tests ${qoderz_test_sources})
target_link_libraries(qoderz_tests PRIVATE qoderz_core Catch2::Catch2WithMain)
target_include_directories(qoderz_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(qoderz_tests PRIVATE -Wall -Wextra -Wpedantic -Wshadow)

# The stub upstream fixture is compiled into the end-to-end binary as well, so the executable the
# tests drive and the assertions about its protocol shapes cannot drift apart.
add_executable(qoderz_e2e
  "${CMAKE_CURRENT_SOURCE_DIR}/integration/e2e_server_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/integration/process_runner.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/support/stub_server.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/support/temp_dir.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/stub_upstream/stub_logic.cpp"
)
target_link_libraries(qoderz_e2e PRIVATE qoderz_core Catch2::Catch2WithMain)
target_include_directories(qoderz_e2e PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(qoderz_e2e PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
target_compile_definitions(qoderz_e2e PRIVATE
  QODERZ_BINARY="$<TARGET_FILE:qoderz>"
  QODERZ_STUB_UPSTREAM="$<TARGET_FILE:qoderz_stub_upstream>"
)

add_executable(qoderz_stub_upstream
  "${CMAKE_CURRENT_SOURCE_DIR}/stub_upstream/main.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/stub_upstream/server.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/stub_upstream/stub_logic.cpp"
)
target_link_libraries(qoderz_stub_upstream PRIVATE qoderz_core CLI11::CLI11 Boost::headers)
target_include_directories(qoderz_stub_upstream PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(qoderz_stub_upstream PRIVATE -Wall -Wextra -Wpedantic -Wshadow)

# The compile definitions above name both binaries as generator expressions, so the build order
# has to say it explicitly.
add_dependencies(qoderz_e2e qoderz qoderz_stub_upstream)

include(Catch)
catch_discover_tests(qoderz_tests)
catch_discover_tests(qoderz_e2e)
