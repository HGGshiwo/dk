set(CMAKE_IGNORE_PATH "/home/hgg/miniconda")

# 寻找标准 GTest
find_package(GTest REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)

add_executable(test_dk_core test_core.cpp)
add_executable(test_dk_future test_future.cpp)
target_link_libraries(
    test_dk_core PRIVATE dk GTest::gtest_main pthread 
    asan_config
)

# 链接库
target_link_libraries(test_dk_future PRIVATE dk
    GTest::gtest_main
    Boost::system
    Threads::Threads
    asan_config
)

# 注册标准 CTest
add_test(NAME TestDkCore COMMAND test_dk_core)
add_test(NAME TestDkFuture COMMAND test_dk_future)