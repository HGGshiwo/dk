#pragma once
#include <any>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>

#include "./utils.hpp"

namespace dk {
template <typename T, typename = void>
struct has_allowed_events : std::false_type {};
template <typename T>
struct has_allowed_events<T, std::void_t<typename T::AllowedEvents>> : std::true_type {};

// 严苛的探针：检查类中是否写了匹配类型的 on_event
template <typename T, typename Event, typename Context, typename = void>
struct has_on_event : std::false_type {};
template <typename T, typename Event, typename Context>
struct has_on_event<
    T, Event, Context,
    std::void_t<decltype(std::declval<T>().on_event(std::declval<const Event&>(), std::declval<Context&>()))>>
    : std::true_type {};

template <typename T>
constexpr std::string_view get_type_name() {
#if defined(__clang__)
    constexpr std::string_view p = __PRETTY_FUNCTION__;
    return p.substr(p.find("T = ") + 4, p.find_last_of(']') - (p.find("T = ") + 4));
#elif defined(__GNUC__)
    constexpr std::string_view p = __PRETTY_FUNCTION__;
    return p.substr(p.find("with T = ") + 9, p.find_last_of(']') - (p.find("with T = ") + 9));
#elif defined(_MSC_VER)
    constexpr std::string_view p = __FUNCSIG__;
    return p.substr(p.find("get_type_name<") + 14, p.find_last_of('>') - (p.find("get_type_name<") + 14));
#else
    return "Unknown";
#endif
}

template <typename Context>
class IState;

// 状态跳转指令 (支持参数携带和延迟构造)
template <typename Context>
class StateAction {
   public:
    enum class Type { UNHANDLED, HANDLED, TRANSITION };
    Type type = Type::UNHANDLED;

    std::function<void(std::vector<std::shared_ptr<IState<Context>>>& out_path,
                       const std::vector<std::shared_ptr<IState<Context>>>& active_path)>
        path_builder;

    static StateAction unhandled() { return {Type::UNHANDLED, nullptr}; }

    static StateAction handled() { return {Type::HANDLED, nullptr}; }

    template <typename TargetState, typename... Tuples>
    static StateAction step(Tuples&&... tuples) {
        StateAction action{Type::TRANSITION, nullptr};
        // 将多个层级的 tuple 打包存起来
        auto args_pack = std::make_tuple(std::forward<Tuples>(tuples)...);
        action.path_builder = [args_pack](auto& out_path, const auto& active_path) {
            std::apply(
                [&out_path, &active_path](auto&&... unpacked_tuples) {
                    TargetState::build_path(out_path, active_path, unpacked_tuples...);
                },
                args_pack);
        };
        return action;
    }
};

// IState 接口返回 StateAction<Context>
template <typename Context>
class IState {
   public:
    IState<Context>* parent_ptr = nullptr;
    virtual ~IState() = default;
    virtual StateAction<Context> handle_event(const std::any& event, Context& ctx) = 0;
    virtual std::string name() const = 0;
    virtual std::string_view name_view() const = 0;
    virtual void on_enter(Context& ctx) {}
    virtual void on_exit(Context& ctx) {}
};

// 这个辅助函数的作用是：接收一整包参数，但只取前 N 个传给父状态
template <typename ParentType, typename OutPath, typename ActivePath, typename ArgsTuple, std::size_t... Is>
void call_parent_build_path(OutPath& out, const ActivePath& act, const ArgsTuple& args_tuple,
                            std::index_sequence<Is...>) {
    // std::get<Is> 会根据传入的 index_sequence，把 0 到 N-1 的参数解包出来传给父类
    ParentType::build_path(out, act, std::get<Is>(args_tuple)...);
}

/*
# 分析子类的AllowedEvents，根据状态进行分发，调用对应的on_event
子类使用：继承IEventHandler，
*/
template <typename BaseInterface, typename Context, typename ReturnType, typename Derived>
class IEventHandler : public BaseInterface {
   public:
    ReturnType handle_event(const std::any& event, Context& ctx) override {
        // 延迟推导类型，避开 CRTP 不完整类型问题
        static_assert(has_allowed_events<Derived>::value,
                      "FATAL ERROR: Missing AllowedEvents definition in your State/Listener! Example:\n"
                      "    using AllowedEvents = std::tuple<EventA, EventB>;\n"
                      "If it handles NO events, explicitly write:\n"
                      "    using AllowedEvents = std::tuple<>;");
        using EventTuple = typename Derived::AllowedEvents;
        // 分支 1：如果业务期望返回 void
        if constexpr (std::is_void_v<ReturnType>) {
            // 黑魔法：传一个空指针过去，只利用类型推导，彻底避免实例化 EventTuple 导致要求事件具有默认构造函数！
            dispatch_void_impl(event, ctx, static_cast<EventTuple*>(nullptr));
            return;
        }
        // 分支 2：如果业务期望有返回值
        else {
            ReturnType result;
            if constexpr (std::is_same_v<ReturnType, StateAction<Context>>) {
                result = StateAction<Context>::unhandled();
            } else {
                result = ReturnType{};
            }
            dispatch_ret_impl(event, ctx, result, static_cast<EventTuple*>(nullptr));
            return result;
        }
    }

   private:
    // === 新增两个 Helper 方法，专门用来解包 Tuple 里的类型 (Ts...)，避开 GCC 7 崩溃 Bug ===
    template <typename... Ts>
    bool dispatch_void_impl(const std::any& event, Context& ctx, std::tuple<Ts...>*) {
        // 直接在类型包上展开，没有 auto&&，没有 decltype，极其纯粹，GCC 绝对不会崩
        return (try_handle_void<Ts>(event, ctx) || ...);
    }

    template <typename R, typename... Ts>
    bool dispatch_ret_impl(const std::any& event, Context& ctx, R& result, std::tuple<Ts...>*) {
        return (try_handle_ret<Ts>(event, ctx, result) || ...);
    }

    // 专门处理期望返回 void 的分支
    template <typename E>
    bool try_handle_void(const std::any& event, Context& ctx) {
        if (const E* e = std::any_cast<E>(&event)) {
            static_assert(has_on_event<Derived, E, Context>::value,
                          "FATAL: Declared event in AllowedEvents but missing on_event(const Event&, Context&)!");
            static_cast<Derived*>(this)->on_event(*e, ctx);
            return true;
        }
        return false;
    }
    // 专门处理期望有返回值的的分支
    template <typename E, typename R>
    bool try_handle_ret(const std::any& event, Context& ctx, R& out_result) {
        if (const E* e = std::any_cast<E>(&event)) {
            static_assert(has_on_event<Derived, E, Context>::value,
                          "FATAL: Declared event in AllowedEvents but missing on_event(const Event&, Context&)!");
            using ActualRet = decltype(static_cast<Derived*>(this)->on_event(*e, ctx));
            if constexpr (std::is_void_v<ActualRet> && std::is_same_v<R, StateAction<Context>>) {
                static_cast<Derived*>(this)->on_event(*e, ctx);
                out_result = StateAction<Context>::handled();
            } else {
                out_result = static_cast<Derived*>(this)->on_event(*e, ctx);
            }
            return true;
        }
        return false;
    }
};

/*
# 使用方法
```
using namespace dk;
struct MyContext { int health = 100; };
// Contex, 自身, 父状态
class PlayerState : public BaseState<MyContext, PlayerState, void> {
public:
    class Grounded;
    class Airborne;
    // 只需要在这里声明本状态关心的事件
    using AllowedEvents = std::tuple<TickEvent>;
    StateAction on_event(const TickEvent& e, MyContext& ctx) {
        return StateAction::handled();
    }
};
class PlayerState::Grounded : public BaseState<MyContext, Grounded, PlayerState> {
public:
    struct JumpEvent { float force; };
    // 清晰地列出该子状态能处理的局部事件
    using AllowedEvents = std::tuple<TickEvent, JumpEvent>;
    StateAction on_event(const TickEvent& e, MyContext& ctx) {
        return StateAction::unhandled();
    }
    StateAction on_event(const JumpEvent& e, MyContext& ctx) {
        std::cout << "Jumped with " << e.force << std::endl;
        return step<Airborne>(9.8f);
    }
};
```
*/
template <typename Context, typename Derived, typename Parent>
class BaseState : public IEventHandler<IState<Context>, Context, StateAction<Context>, Derived> {
   public:
    using ParentState = Parent;

    static constexpr std::string_view static_name() {
        // 利用你原有的宏或模板提取类型名
        return get_type_name<Derived>();
    }

    // 实现基类的虚函数，直接桥接给静态函数
    std::string_view name_view() const override {
        // 这里的玄机：通过 Derived:: 来调用，允许子类覆盖静态名字！
        return Derived::static_name();
    }

    // 实现基类的虚函数，直接桥接给静态函数
    std::string name() const override {
        // 这里的玄机：通过 Derived:: 来调用，允许子类覆盖静态名字！
        return std::string(Derived::static_name());
    }

    // 提供带参跳转能力
    template <typename TargetState, typename... Args>
    StateAction<Context> step(Args&&... args) {
        return StateAction<Context>::template step<TargetState>(std::forward<Args>(args)...);
    }
    template <typename P = ParentState, typename = std::enable_if_t<!std::is_same_v<P, void>>>
    P* parent() {
        return static_cast<P*>(this->parent_ptr);
    }

    // 静态路径构建 (支持层级参数传递与复用：按 父 -> 子 顺序传参)
    template <typename... Tuples>
    static void build_path(std::vector<std::shared_ptr<IState<Context>>>& out_path,
                           const std::vector<std::shared_ptr<IState<Context>>>& active_path, const Tuples&... args) {
        // 1. 获取传入的所有 tuple 数量，并打包为引用的元组
        constexpr size_t NUM_ARGS = sizeof...(Tuples);
        auto args_tuple = std::tie(args...);
        // 2. 递归构建父状态 (将前 NUM_ARGS - 1 个参数传给父状态)
        if constexpr (!std::is_same_v<ParentState, void>) {
            if constexpr (NUM_ARGS > 1) {
                // 调用外部的辅助函数，截取前面的参数给父节点
                call_parent_build_path<ParentState>(out_path, active_path, args_tuple,
                                                    std::make_index_sequence<NUM_ARGS - 1>{});
            } else {
                // 传进来的参数耗尽了（或者根本没传），父状态按空参数处理
                ParentState::build_path(out_path, active_path);
            }
        }
        // 3. 提取当前状态的参数 (永远取最后一个 tuple 给自己)
        auto current_tuple = [&]() {
            if constexpr (NUM_ARGS > 0) {
                return std::get<NUM_ARGS - 1>(args_tuple);  // 准确拿到最后一个参数
            } else {
                return std::tuple<>{};  // 没参数时给个空的
            }
        }();
        // 4. 判断当前层级是否可以复用 (运行期判断)
        size_t current_depth = out_path.size();
        std::string_view my_name = Derived::static_name();
        if (current_depth < active_path.size() && active_path[current_depth]->name_view() == my_name) {
            // 【命中缓存】：直接复用原有的智能指针，跳过构造！
            out_path.push_back(active_path[current_depth]);
        } else {
            // 【未命中缓存】：必须真正执行构造
            auto state = std::apply(
                [](auto&&... unpacked_args) -> std::shared_ptr<IState<Context>> {
                    // 此时 current_tuple 里的参数就是精准给当前状态的了
                    if constexpr (std::is_constructible_v<Derived, decltype(unpacked_args)...>) {
                        return std::make_shared<Derived>(std::forward<decltype(unpacked_args)>(unpacked_args)...);
                    } else {
                        // 不能匹配返回 nullptr，交给后面的安全校验处理
                        return nullptr;
                    }
                },
                current_tuple);

            // 运行期安全校验
            if (!state) {
                meta_utils::print_type<decltype(current_tuple)>();
                throw std::logic_error(
                    "FATAL: State '" + std::string(my_name) +
                    "' cannot be reused, but appropriate constructor arguments were NOT provided in step()!");
            }
            out_path.push_back(state);
        }
    }
};

}  // namespace dk