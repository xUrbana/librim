cmake_minimum_required(VERSION 3.20)

include(FetchContent)

# Add GoogleTest via FetchContent
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(librim_test 
  main.cpp 
  test_dispatcher.cpp
  test_networking.cpp
)
target_link_libraries(librim_test
  GTest::gtest_main
  librim
)

include(GoogleTest)
gtest_discover_tests(librim_test)
