#!/bin/bash

# 1. 切换到脚本所在的父目录 (也就是 dk 目录)
# 加上 || exit 1，如果目录不存在直接退出，防止误操作
cd "$(dirname "$0")/.." || exit 1

# 2. 安全地创建并进入 build 目录
rm -rf build || exit 1
mkdir -p build
cd build || exit 1

# 3. 此时我们确信自己 100% 在 dk/build 里面了
# 这里的 .. 指的是 dk 目录，找的是 dk/CMakeLists.txt
cmake -DDK_TEST=ON .. || exit 1

# 4. 编译
make -j16 || exit 1