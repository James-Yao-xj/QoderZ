file(GLOB_RECURSE qoderz_test_sources CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/integration/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/support/*.cpp"

)

add_executable(qoderz_tests ${qoderz_test_sources})
target_link_libraries(qoderz_tests PRIVATE qoderz_core Catch2::Catch2WithMain)
target_include_directories(qoderz_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(qoderz_tests PRIVATE -Wall -Wextra -Wpedantic -Wshadow)

include(Catch)
catch_discover_tests(qoderz_tests)
