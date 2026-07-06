#pragma once
#include <boost/smart_ptr/shared_ptr.hpp>
#include <fstream>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace dk {

namespace meta_utils {

template <typename T>
void print_type() {
    // __PRETTY_FUNCTION__ 会在编译时生成包含真实类型 T 的字符串，但在运行时打印
    std::cout << "Type is: " << __PRETTY_FUNCTION__ << std::endl;
}

template <typename... T>
struct TypePrinter;  // 只有声明，没有实现

template <typename... Ts>
struct TypeList {};
// 2. 探测 TypeList 中的类型，是否有任何一个能处理 EventT
template <typename EventT, typename List>
struct can_handle;
template <typename EventT, typename... Fs>
// 用于判断一个函数列表是否有一个能接收参数
struct can_handle<EventT, TypeList<Fs...>> {
    // 在结构体里展开参数包，编译器绝对不会报错
    static constexpr bool value = (std::is_invocable_v<Fs, EventT> || ...);
};

// 1. 内部实现类：专门处理干净的类型（没有引用和外层 const）
template <typename T>
struct extract_ros_msg_impl {
    using type = T;  // 默认情况：如果是普通对象，直接返回
};

// 匹配 ROS2: std::shared_ptr<T> (非 const)
template <typename T>
struct extract_ros_msg_impl<std::shared_ptr<T>> {
    using type = T;
};

// 匹配 ROS2: std::shared_ptr<const T> (常用来接收不可变消息)
template <typename T>
struct extract_ros_msg_impl<std::shared_ptr<const T>> {
    using type = T;
};

#ifdef USE_ROS1
// 匹配 ROS1: boost::shared_ptr<T>
template <typename T>
struct extract_ros_msg_impl<boost::shared_ptr<T>> {
    using type = T;
};

// 匹配 ROS1: boost::shared_ptr<const T> (即 MsgType::ConstPtr)
template <typename T>
struct extract_ros_msg_impl<boost::shared_ptr<const T>> {
    using type = T;
};
#endif

// 2. 对外暴露的接口：先"净化"类型，再提取
template <typename T>
struct extract_ros_msg {
    // std::decay_t 会把 `const std::shared_ptr<const T>&` 变成
    // `std::shared_ptr<const T>`
    using decayed_type = typename std::decay<T>::type;

    // 把净化后的类型交给 impl 去精确匹配提取
    using type = typename extract_ros_msg_impl<decayed_type>::type;
};
/*
分析函数的入参和返回值，使用方法：

```
using Traits = meta_utils::callable_traits<decltype(my_lambda)>;
// 提取返回值类型
using Ret = Traits::return_type;
static_assert(std::is_same_v<Ret, float>, "返回值应该是 float");
// 提取第 0 个参数的类型
using Arg0 = Traits::arg_type<0>;
static_assert(std::is_same_v<Arg0, int>, "第0个参数应该是 int");
//获取参数总数
std::cout << "参数总数: " << Traits::arg_count << std::endl;
```
*/
template <typename T>
struct callable_traits
    : callable_traits<decltype(&std::decay_t<T>::operator())> {};
// 1. 匹配普通函数指针
template <typename R, typename... Args>
struct callable_traits<R (*)(Args...)> {
    using return_type = R;

    // 提取第 K 个参数的类型 (加了 decay_t 去除 const 和引用)
    template <std::size_t K>
    using arg_type = std::decay_t<std::tuple_element_t<K, std::tuple<Args...>>>;

    // 提取原汁原味的参数类型（保留 const 和 引用）
    template <std::size_t K>
    using exact_arg_type = std::tuple_element_t<K, std::tuple<Args...>>;

    // 附赠一个小功能：获取参数总个数
    static constexpr std::size_t arg_count = sizeof...(Args);
};

// 2. 匹配普通 Lambda 表达式 (const operator())
template <typename ClassType, typename R, typename... Args>
struct callable_traits<R (ClassType::*)(Args...) const> {
    using return_type = R;

    template <std::size_t K>
    using arg_type = std::decay_t<std::tuple_element_t<K, std::tuple<Args...>>>;
    static constexpr std::size_t arg_count = sizeof...(Args);

    template <std::size_t K>
    using exact_arg_type = std::tuple_element_t<K, std::tuple<Args...>>;
};
// 3. 匹配 mutable Lambda 表达式 (非 const operator())
template <typename ClassType, typename R, typename... Args>
struct callable_traits<R (ClassType::*)(Args...)> {
    using return_type = R;

    template <std::size_t K>
    using arg_type = std::decay_t<std::tuple_element_t<K, std::tuple<Args...>>>;
    static constexpr std::size_t arg_count = sizeof...(Args);

    template <std::size_t K>
    using exact_arg_type = std::tuple_element_t<K, std::tuple<Args...>>;
};
}  // namespace meta_utils

}  // namespace dk