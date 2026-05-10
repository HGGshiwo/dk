#pragma once
#include <boost/smart_ptr/shared_ptr.hpp>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <vector>

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

template <typename T>
struct extract_ros_msg {
    using type = T;  // 默认情况：如果是普通对象，直接返回
};
// 匹配 std::shared_ptr<const T> (ROS 2 常用)
template <typename T>
struct extract_ros_msg<std::shared_ptr<const T>> {
    using type = T;
};
// 匹配 boost::shared_ptr<const T> (ROS 1 常用 ConstPtr 底层类型)
// 注意：如果使用 ROS 1，请取消下方注释并引入 boost 头文件

template <typename T>
struct extract_ros_msg<boost::shared_ptr<const T>> {
    using type = T;
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
struct callable_traits : callable_traits<decltype(&std::decay_t<T>::operator())> {};
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

template <typename T>
class thread_safe {
   private:
    T data_;
    mutable std::shared_mutex mtx_;

   public:
    template <typename... Args>
    thread_safe(Args&&... args) : data_(std::forward<Args>(args)...) {}
    // ==========================================
    // 接口 1：写操作（独占锁）
    // 强制要求传递 T& (或 auto&)，否则直接编译报错！
    // ==========================================
    template <typename Func>
    decltype(auto) write(Func&& func) {
        // 1. 确保函数可以用 T&（左值引用）调用
        static_assert(std::is_invocable_v<Func, T&>, "write callback error: Argument must be T&!");
        // 2. 核心魔法：确保函数【不能】用 T（右值）调用
        // 这完美拦截了传值拷贝 `[](T x)` 和常引用 `[](const T& x)`
        static_assert(!std::is_invocable_v<Func, T>, "write callback error: Argument must be T& or auto&!");
        std::unique_lock<std::shared_mutex> lock(mtx_);
        // 完美转发传入的 callable 对象
        return std::forward<Func>(func)(data_);
    }
    // ==========================================
    // 接口 2：读操作（共享锁）
    // ==========================================
    template <typename Func>
    decltype(auto) read(Func&& func) const {
        // 提供友好的编译期错误提示
        static_assert(std::is_invocable_v<Func, const T&>, "read callback error: Argument must be const T&!");
        std::shared_lock<std::shared_mutex> lock(mtx_);
        return std::forward<Func>(func)(data_);
    }
    T get() const {
        std::shared_lock<std::shared_mutex> lock(mtx_);  // 注意：get 应该用共享锁(读锁)
        return data_;
    }
    void set(T data) {
        std::unique_lock<std::shared_mutex> lock(mtx_);
        data_ = std::move(data);  // 优化：使用 std::move 减少一次拷贝
    }
};
}  // namespace dk