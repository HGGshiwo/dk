set(CMAKE_IGNORE_PATH "/home/hgg/miniconda")

# 寻找标准 GTest
find_package(GTest REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)

add_executable(test_dk_core test_core.cpp)
add_executable(test_dk_future test_future.cpp)
add_executable(test_dk_http test_http.cpp)

dk_create_json_library(MyProjectJson "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(
    test_dk_core PRIVATE dk GTest::gtest_main pthread 
    asan_config
    MyProjectJson
)

# 链接库
target_link_libraries(test_dk_future PRIVATE dk
    GTest::gtest_main
    Boost::system
    Threads::Threads
    asan_config
    MyProjectJson
)

target_link_libraries(test_dk_http PRIVATE dk
    GTest::gtest_main
    GTest::gmock 
    Boost::system
    Threads::Threads
    asan_config
    MyProjectJson
)

# 注册标准 CTest
add_test(NAME TestDkCore COMMAND test_dk_core)
add_test(NAME TestDkFuture COMMAND test_dk_future)
add_test(NAME TestDkHttp COMMAND test_dk_http)