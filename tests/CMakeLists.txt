set(CMAKE_IGNORE_PATH "/home/hgg/miniconda")
# 寻找标准 GTest
find_package(GTest REQUIRED)

add_executable(test_dk_core test_core.cpp)
target_link_libraries(test_dk_core PRIVATE dk GTest::gtest_main pthread)

# 注册标准 CTest
add_test(NAME TestDkCore COMMAND test_dk_core)