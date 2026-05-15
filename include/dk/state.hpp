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

// 前置声明
template <typename Context>
class IState;
template <typename Context>
class IStatePathBuilder {
   public:
    virtual ~IStatePathBuilder() = default;
    // 检查当前深度的状态是否可以复用
    virtual bool try_reuse(std::string_view state_name) = 0;
    // 获取当前内存分配游标
    virtual size_t get_memory_mark() const = 0;
    // 分配内存
    virtual void* allocate_bytes(size_t size, size_t alignment) = 0;
    // 注册新构造的状态，并提供其析构函数擦除
    virtual void push_new_state(IState<Context>* state, void (*destroy_fn)(IState<Context>*),
                                size_t original_offset) = 0;
    // 辅助函数：定位 new 构造状态
    template <typename T, typename... Args>
    void emplace(Args&&... args) {
        // 1. 在分配内存【之前】，精准记录下内存池的状态
        size_t rollback_mark = get_memory_mark();

        // 2. 分配并构造
        void* mem = allocate_bytes(sizeof(T), alignof(T));
        if (!mem) throw std::bad_alloc();
        T* instance = new (mem) T(std::forward<Args>(args)...);

        // 3. 把状态和【准确的回退点】一起注册进去
        push_new_state(instance, [](IState<Context>* ptr) { static_cast<T*>(ptr)->~T(); }, rollback_mark);
    }
};

template <typename ParentType, typename Builder, typename ArgsTuple, std::size_t... Is>
void call_parent_build_path(Builder& builder, const ArgsTuple& args_tuple, std::index_sequence<Is...>) {
    ParentType::build_path(builder, std::get<Is>(args_tuple)...);
}
template <typename Context>
class StateAction {
   public:
    enum class Type { UNHANDLED, HANDLED, TRANSITION };
    Type type = Type::UNHANDLED;
    // 重点：签名大幅度简化
    std::function<void(IStatePathBuilder<Context>& builder)> path_builder;
    static StateAction unhandled() { return {Type::UNHANDLED, nullptr}; }
    static StateAction handled() { return {Type::HANDLED, nullptr}; }
    template <typename TargetState, typename... Tuples>
    static StateAction step(Tuples&&... tuples) {
        StateAction action{Type::TRANSITION, nullptr};
        auto args_pack = std::make_tuple(std::forward<Tuples>(tuples)...);

        action.path_builder = [args_pack](IStatePathBuilder<Context>& builder) {
            std::apply([&builder](auto&&... unpacked_tuples) { TargetState::build_path(builder, unpacked_tuples...); },
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

    template <typename... Tuples>
    static void build_path(IStatePathBuilder<Context>& builder, const Tuples&... args) {
        constexpr size_t NUM_ARGS = sizeof...(Tuples);
        auto args_tuple = std::tie(args...);
        // 1. 递归构建父状态
        if constexpr (!std::is_same_v<ParentState, void>) {
            if constexpr (NUM_ARGS > 1) {
                call_parent_build_path<ParentState>(builder, args_tuple, std::make_index_sequence<NUM_ARGS - 1>{});
            } else {
                ParentState::build_path(builder);
            }
        }
        // 2. 提取当前层级参数
        auto current_tuple = [&]() {
            if constexpr (NUM_ARGS > 0)
                return std::get<NUM_ARGS - 1>(args_tuple);
            else
                return std::tuple<>{};
        }();
        // // 【修复重点】：提前在泛型 Lambda 外部提取好类型，避开 GCC 捕获推导 Bug
        // using CurrentTupleType = decltype(current_tuple);
        // 3. 判断复用还是构造
        std::string_view my_name = Derived::static_name();
        if (builder.try_reuse(my_name)) {
            // 【命中缓存】：底层 Builder 内部会自动推进深度，什么都不用做
        } else {
            // 【未命中缓存】：直接利用 builder 在内存池上分配并构造
            std::apply(
                [&](auto&&... unpacked_args) {
                    if constexpr (std::is_constructible_v<Derived, decltype(unpacked_args)...>) {
                        builder.template emplace<Derived>(std::forward<decltype(unpacked_args)>(unpacked_args)...);
                    } else {
                        // 使用外部提前定义好的 CurrentTupleType
                        // meta_utils::print_type<CurrentTupleType>();

                        // 或者更精准一点，打印出 unpacked_args 到底被推导成了什么类型组合：
                        meta_utils::print_type<std::tuple<decltype(unpacked_args)...>>();

                        throw std::logic_error("FATAL: Cannot construct state '" + std::string(my_name) + "'.");
                    }
                },
                current_tuple);
        }
    }
};
}  // namespace dk