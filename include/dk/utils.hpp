#pragma once
#include <yaml-cpp/yaml.h>

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

// 递归：YAML 标量类型推断
static json yaml_to_json(const YAML::Node& node) {
    json j;
    switch (node.Type()) {
        case YAML::NodeType::Null:
            j = nullptr;
            break;
        case YAML::NodeType::Scalar: {
            try {
                return node.as<bool>();
            } catch (...) {
            }
            try {
                return node.as<int64_t>();
            } catch (...) {
            }
            try {
                return node.as<double>();
            } catch (...) {
            }
            return node.as<std::string>();
        }
        case YAML::NodeType::Sequence: {
            j = json::array();
            for (const auto& item : node) j.push_back(yaml_to_json(item));
            break;
        }
        case YAML::NodeType::Map: {
            j = json::object();
            for (const auto& kv : node) j[kv.first.as<std::string>()] = yaml_to_json(kv.second);
            break;
        }
        default:
            break;
    }
    return j;
}

/**
 * 核心流程: 读取 YAML -> 转为 JSON -> 注入 C++ 结构体补全默认值 -> 生成最终 JSON
 */
inline json load_and_complete(const std::string& yaml_path) {
    try {
        // 1. 读取 YAML
        YAML::Node yaml_root = YAML::LoadFile(yaml_path);

        // 2. 初步转为无类型推断缺失字段的 JSON
        json raw_json = yaml_to_json(yaml_root);

        return raw_json;

    } catch (const std::exception& e) {
        std::cerr << "配置加载失败: " << e.what() << std::endl;
        throw;
    }
}

/**
 * 直接加载 yaml 并生成 json 文件
 */
inline void generate_json_file(const std::string& yaml_path, const std::string& json_path) {
    json final_json = load_and_complete(yaml_path);
    std::ofstream out(json_path);
    if (out.is_open()) {
        out << final_json.dump(4);  // 4 空格缩进格式化
        std::cout << "✅ 配置文件已生成: " << json_path << std::endl;
    } else {
        std::cerr << "❌ 无法写入文件: " << json_path << std::endl;
    }
}
}  // namespace dk