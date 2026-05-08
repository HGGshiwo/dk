#pragma once
#include <any>
#include <functional>
#include <memory>
#include <string>
#include <tuple>

namespace dk {
// 1. 萃取器：探测子类是否定义了 AllowedEvents，如果没有则返回空 tuple
template <typename T, typename = void>
struct get_allowed_events {
    using type = std::tuple<>;
};

template <typename T>
struct get_allowed_events<T, std::void_t<typename T::AllowedEvents>> {
    using type = typename T::AllowedEvents;
};

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

    // 现在 Context 是合法的了！
    std::function<void(std::vector<std::shared_ptr<IState<Context>>>&)> path_builder;
    static StateAction unhandled() { return {Type::UNHANDLED, nullptr}; }
    static StateAction handled() { return {Type::HANDLED, nullptr}; }
    template <typename TargetState, typename... Args>
    static StateAction step(Args&&... args) {
        StateAction action{Type::TRANSITION, nullptr};
        // 延迟构造：将目标参数存入 Tuple
        auto args_tuple = std::make_tuple(std::forward<Args>(args)...);

        // 真正执行跳转时，调用 TargetState 内部的静态构造逻辑
        action.path_builder = [args_tuple](std::vector<std::shared_ptr<IState<Context>>>& path) {
            TargetState::build_path(path, args_tuple);
        };
        return action;
    }
};

// IState 接口返回 StateAction<Context>
template <typename Context>
class IState {
   public:
    virtual ~IState() = default;
    virtual StateAction<Context> handle_event(const std::any& event, Context& ctx) = 0;
    virtual std::string name() const = 0;
    virtual void on_enter(Context& ctx) {}
    virtual void on_exit(Context& ctx) {}
};

/*
# 分析子类的AllowedEvents，根据状态进行分发，调用对应的on_event
子类使用：继承IEventHandler，
*/
template <typename BaseInterface, typename Context, typename ReturnType, typename Derived>
class IEventHandler : public BaseInterface {
   public:
    ReturnType handle_event(const std::any& event, Context& ctx) override {
        // 延迟推导类型，避开 CRTP 不完整类型问题
        using EventTuple = typename get_allowed_events<Derived>::type;
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
    virtual std::string name() const override { return std::string(get_type_name<Derived>()); }
    // 提供带参跳转能力
    template <typename TargetState, typename... Args>
    StateAction<Context> step(Args&&... args) {
        return StateAction<Context>::template step<TargetState>(std::forward<Args>(args)...);
    }
    // 静态路径构建 (树形重构核心)
    template <typename Tuple>
    static void build_path(std::vector<std::shared_ptr<IState<Context>>>& path, const Tuple& tuple) {
        if constexpr (!std::is_same_v<ParentState, void>) {
            ParentState::build_path(path, std::make_tuple());
        }
        auto state = std::apply(
            [](auto&&... args) { return std::make_shared<Derived>(std::forward<decltype(args)>(args)...); }, tuple);
        path.push_back(state);
    }
};

}  // namespace dk