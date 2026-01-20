cmake_minimum_required(VERSION 3.14)
project(esp32_photoframe_tests)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fetch Google Test
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.14.0
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Enable testing
enable_testing()

# Add test executable
add_executable(
  utils_test
  test_utils.cpp
  ../main/testable_utils.c
)

# Link Google Test
target_link_libraries(
  utils_test
  GTest::gtest_main
)

# Include directories
target_include_directories(
  utils_test
  PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../main
)

# Discover tests
include(GoogleTest)
gtest_discover_tests(utils_test)
