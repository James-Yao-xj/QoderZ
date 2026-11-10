add_executable(qoderz_tests
  unit/common/error_test.cpp
  unit/common/log_test.cpp
)
target_link_libraries(qoderz_tests PRIVATE qoderz_core Catch2::Catch2WithMain)
target_compile_options(qoderz_tests PRIVATE -Wall -Wextra -Wpedantic -Wshadow)

include(Catch)
catch_discover_tests(qoderz_tests)
