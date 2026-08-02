#pragma once
// #include <algorithm>
#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "./event_listener.hpp"
#include "./future.hpp"
#include "./state.hpp"
#include "ITimeProvider.hpp"
#include "event_box.hpp"

// ================= dk 框架核心 =================
namespace dk {

// 引入类型萃取黑魔法：用于自动推导 Lambda 的参数类型
template <typename T>
struct lambda_traits : public lambda_traits<decltype(&T::operator())> {};
template <typename ClassType, typename ReturnType, typename ArgType>
struct lambda_traits<ReturnType (ClassType::*)(ArgType) const> {
    using arg_type = std::decay_t<ArgType>;  // 提取出具体的事件类型
};
template <typename ClassType, typename ReturnType, typename ArgType>
struct lambda_traits<ReturnType (ClassType::*)(ArgType)> {
    using arg_type = std::decay_t<ArgType>;  // 提取出具体的事件类型
};

template <class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

template <typename T>
struct TypeTag {
    using type = T;
};

struct StateChangeEvent {
    std::string prev;
    std::string cur;
    int prev_id;
    int cur_id;
};

// 异步事件
template <typename ResultT>
struct AsyncEvent {
    using ReturnType = ResultT;
    mutable std::shared_ptr<Promise<ResultT>> promise_;

    virtual ~AsyncEvent<ResultT>() = default;

    // 用户梦寐以求的 API：直接调用 e.resolve()!
    void resolve(ResultT val) const {
        if (promise_) {
            promise_->resolve(std::move(val));
        }
    }
    // 失败/异常时调用
    void reject(const std::exception_ptr& eptr) const {
        if (promise_) {
            promise_->reject(eptr);  // 将异常存入共享状态
        }
    }

    // 提供一个更方便的 reject 重载，直接传错误信息
    void reject(const std::string& error_msg) const {
        if (promise_) {
            promise_->reject(
                std::make_exception_ptr(std::runtime_error(error_msg)));
        }
    }

    const bool is_settled() const {
        return promise_ == nullptr ||
               promise_->state() != PromiseState::PENDING;
    }

    // 辅助方法：判断外界是否在等待
    bool is_awaited() const { return promise_ != nullptr; }
};

struct WaitNode {
    uint64_t id;
    std::function<bool(const EventBox&)> predicate;
    std::shared_ptr<Promise<bool>> promise;
};

struct StepLevelGuard {
    int& level;
    StepLevelGuard(int& l) : level(l) { level++; }
    ~StepLevelGuard() { level--; }
};

// Engine的抽象接口，不需要Engine的真实类型
template <typename Context, size_t MaxDepth = 10, size_t MemorySize = 65536>
class IEngine : public IAsyncRuntime, public IStatePathBuilder<Context> {
   private:
    struct StateNode {
        IState<Context>* state = nullptr;
        void (*destroy_fn)(IState<Context>*) = nullptr;
        size_t mem_offset = 0;
    };
    alignas(16) std::byte memory_arena_[MemorySize];
    size_t current_offset_ = 0;

    size_t build_depth_ = 0;
    bool divergence_found_ = false;

    // 当次跳转的重入控制
    bool current_force_full_reentry_ = false;
    std::function<bool(IState<Context>*)> current_reentry_predicate_;

    std::atomic<bool> running_{false};

    std::optional<boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>>
        work_guard_;
    std::function<void()> tick_canceler_;
    std::chrono::milliseconds tick_interval_;

    std::optional<std::unique_ptr<boost::asio::thread_pool>> workflow_pool_;
    std::thread::id event_thread_id_;  // 事件循环的thread id, 用于检测wait

   protected:
    Context ctx_;
    size_t active_depth_ = 0;
    std::array<StateNode, MaxDepth> active_path_;

    // Buffer for transition requested during an active transition (e.g., inside
    // on_enter/on_exit)
    std::optional<StateAction<Context>> pending_transition_;
    using EventListenerPtr = std::shared_ptr<IEventListener<Context>>;
    std::vector<EventListenerPtr> listeners_;  // 和状态无关的监听器

    // 引入 Asio 的绝对核心：I/O 上下文
    boost::asio::io_context& io_context_;
    std::shared_ptr<ITimeProvider> time_provider_;

    // 保存所有正在等待的请求。使用 map 是为了 O(1) 的删除效率
    std::unordered_map<uint64_t, std::shared_ptr<WaitNode>> active_waits_;
    uint64_t next_wait_id_ = 0;

    int step_call_level_ = 0;  // step调用层数

    virtual void process_internal(const EventBox& e) = 0;

    virtual Future<bool> wait_internal(
        std::function<bool(const EventBox&)> predicate,
        CancellationToken token) = 0;

   public:
    IEngine(boost::asio::io_context& io,
            std::shared_ptr<ITimeProvider> time_provider)
        : io_context_(io), time_provider_(std::move(time_provider)) {}

    // IStatePathBuilder 接口实现（含生命周期事件派发）
    size_t get_memory_mark() const override { return current_offset_; }
    void* allocate_bytes(size_t size, size_t alignment) override {
        size_t aligned_offset =
            (current_offset_ + alignment - 1) & ~(alignment - 1);
        if (aligned_offset + size > MemorySize) {
            std::cout << "[Engeine]: allocate " << aligned_offset + size
                      << " > " << MemorySize << std::endl;
            return nullptr;
        };
        void* ptr = memory_arena_ + aligned_offset;
        current_offset_ = aligned_offset + size;
        return ptr;
    }

    bool try_reuse(std::string_view state_name) override {
        bool hit_reentry_breakpoint = current_force_full_reentry_;

        // +++ 新增：如果当前栈深度有节点，且命中目标类型，打断复用 +++
        if (!hit_reentry_breakpoint && current_reentry_predicate_ &&
            build_depth_ < active_depth_) {
            if (current_reentry_predicate_(active_path_[build_depth_].state)) {
                hit_reentry_breakpoint = true;
            }
        }

        // 只有没碰到断点，且名字匹配，才允许复用
        if (!hit_reentry_breakpoint && !divergence_found_ &&
            build_depth_ < active_depth_ &&
            active_path_[build_depth_].state->name_view() == state_name) {
            build_depth_++;
            return true;
        }

        if (!divergence_found_) {
            divergence_found_ = true;
            // 【分歧点发现】：立刻向上逐级退出旧节点
            while (active_depth_ > build_depth_) {
                pop_state();
            }
        }
        return false;
    }

    void push_new_state(IState<Context>* state,
                        void (*destroy_fn)(IState<Context>*),
                        size_t original_offset) override {
        state->parent_ptr = (active_depth_ > 0)
                                ? active_path_[active_depth_ - 1].state
                                : nullptr;

        active_path_[active_depth_] = {state, destroy_fn, original_offset};
        active_depth_++;
        build_depth_++;
        StateAction<Context> enter_action = state->internal_enter(ctx_);
        if (enter_action.type == StateAction<Context>::Type::TRANSITION) {
            pending_transition_ = std::move(enter_action);
        }
    }

    void pop_state() {
        if (active_depth_ == 0) return;
        auto& node = active_path_[active_depth_ - 1];

        // 【生命周期】：触发 Exit 事件和回调
        node.state->on_exit(ctx_);
        if (node.destroy_fn) node.destroy_fn(node.state);  // 析构

        current_offset_ = node.mem_offset;  // 完美退还内存
        active_depth_--;
    }

    // 核心：极度简化的状态跳转控制循环
    void execute_transition(StateAction<Context> action) {
        try {
            if (step_call_level_ != 0) {
                // 如果处于事件冒泡或生命周期回调期间，缓存跳转意图
                pending_transition_ = std::move(action);
                return;
            }
            StepLevelGuard guard(step_call_level_);
            while (action.path_builder) {
                pending_transition_.reset();

                std::string prev_leaf = get_state_name();
                int prev_leaf_id = get_state_id();

                // 初始化构建环境
                build_depth_ = 0;
                divergence_found_ = false;

                // +++ 装载当次跳转的重入配置 +++
                current_force_full_reentry_ = action.force_full_reentry;
                current_reentry_predicate_ =
                    std::move(action.reentry_predicate);

                // 【魔法时刻】：一行代码自动完成 LCA 比对、旧状态 Pop
                // 退栈、新状态 Push 入栈
                action.path_builder(*this);
                // 如果新路径比旧路径短，把多余的尾巴截断退出
                while (active_depth_ > build_depth_) {
                    pop_state();
                }
                std::string next_leaf = get_state_name();
                int next_leaf_id = get_state_id();
                process_internal(EventBox(StateChangeEvent{
                    prev_leaf, next_leaf, prev_leaf_id, next_leaf_id}));
                if (pending_transition_.has_value()) {
                    // 如果在上述的 pop_state(on_exit) 或
                    // push_new_state(on_enter) 中发生了新的跳转
                    action = std::move(*pending_transition_);
                } else {
                    break;  // 跳转彻底结束
                }
            }
        } catch (const std::exception& e) {
            // step_call_level_ = 0;
            std::cout << "[Engine]: transition error: " << e.what()
                      << std::endl;
        }
    }

   private:
    template <typename Visitor, typename... Ts>
    static bool try_visit_event(Visitor& v, const EventBox& ev,
                                bool& result_out, std::tuple<Ts...>) {
        bool handled = false;

        auto check = [&](auto tag) -> bool {
            using T = typename decltype(tag)::type;

            // 【改变在这里】：调用我们自己写的极速 cast
            if (const T* concrete_event = ev.cast_if_match<T>()) {
                using RetType = decltype(v(*concrete_event));

                if constexpr (std::is_convertible_v<RetType, bool>) {
                    result_out = v(*concrete_event);
                } else {
                    v(*concrete_event);
                    result_out = true;
                }
                handled = true;
                return true;
            }
            return false;
        };

        (void)(check(TypeTag<Ts>{}) || ...);
        return handled;
    }

    void schedule_next_tick() {
        if (!running_) return;
        double interval_sec =
            std::chrono::duration<double>(tick_interval_).count();

        // 记录上一帧的时间
        auto last_tick_time =
            std::make_shared<std::chrono::steady_clock::time_point>(
                std::chrono::steady_clock::now());

        tick_canceler_ = time_provider_->start_ticker(
            interval_sec, [this, last_tick_time]() {
                if (running_) {
                    // 1. 计算真实的 Delta Time
                    auto now = std::chrono::steady_clock::now();
                    double dt =
                        std::chrono::duration<double>(now - *last_tick_time)
                            .count();
                    *last_tick_time = now;

                    // 防止电脑休眠唤醒后 dt 巨大导致物理/逻辑穿模（可选）
                    if (dt > 0.1) dt = 0.1;

                    // 2. 调用安全的 Tick 分发
                    this->process_tick(dt);
                }
            });
    }

    // 专门用于安全分发 Tick 的函数
    void process_tick(double dt) {
        StateAction<Context> final_transition =
            StateAction<Context>::unhandled();
        {
            // 【开启护盾】：防止在 on_tick 内部直接跳转导致栈崩溃
            StepLevelGuard tick_guard(this->step_call_level_);

            // 1. 优先派发 Tick 给全局无状态监听器
            for (auto& listener : this->listeners_) {
                if (this->pending_transition_.has_value())
                    break;  // 一旦发生跳转，立刻中止后续派发
                listener->internal_tick(dt, this->ctx_);
            }

            // 2. HSM 冒泡：从最深层的状态向根状态派发 Tick
            if (!this->pending_transition_.has_value()) {
                for (int i = active_depth_ - 1; i >= 0; --i) {
                    // 调用状态的 internal_tick
                    StateAction<Context> action =
                        active_path_[i].state->internal_tick(dt, ctx_);

                    if (action.type == StateAction<Context>::Type::TRANSITION) {
                        final_transition = action;
                        break;
                    } else if (pending_transition_.has_value()) {
                        break;  // 拦截到了强行调用 engine->step()
                    } else if (action.type ==
                               StateAction<Context>::Type::HANDLED) {
                        break;  // 子状态吸收了 Tick，停止向上冒泡
                    }
                }
            }

            // 提取缓存的跳转
            if (pending_transition_.has_value()) {
                final_transition = std::move(*pending_transition_);
                pending_transition_.reset();
            }
        }  // 【护盾解除】

        // 安全地在循环外执行状态跳转
        if (final_transition.type == StateAction<Context>::Type::TRANSITION) {
            this->execute_transition(final_transition);
        }
    }

   public:
    ~IEngine() { stop(); };

    Context& get_context() { return ctx_; }

    void add_listener(EventListenerPtr listener) {
        boost::asio::post(io_context_, [this, listener]() {
            this->listeners_.push_back(listener);
        });
    }

    virtual void handle_event(const EventBox& event, Context& ctx) = 0;

    boost::asio::io_context& get_ioc() { return io_context_; }
    std::shared_ptr<ITimeProvider> get_time_provider() const {
        return time_provider_;
    }

    std::thread::id get_thread_id() override { return event_thread_id_; }

    // 实现promise runtime
    void post_future_task(std::function<void()> task) override {
        boost::asio::post(io_context_, std::move(task));
    }

    // 实现promise runtime
    std::function<void()> set_future_timeout(
        uint32_t ms, std::function<void()> on_timeout) override {
        return time_provider_->set_timeout(ms / 1000.0, std::move(on_timeout));
    }

    // 线程安全的外部事件注入接口
    template <typename Event>
    void dispatch(Event e) {
        boost::asio::post(io_context_,
                          [this, ev = EventBox(std::move(e))]() mutable {
                              this->process_internal(ev);
                          });
    }

    template <typename StateType, typename... Args>
    void step(Args&&... args) {
        // 利用 StateAction 的静态工厂完成“类型擦除”
        StateAction<Context> action =
            StateAction<Context>::template step<StateType>(
                std::forward<Args>(args)...);
        // 调用子类实现的非模板虚函数
        this->step_internal(action);
    }

    // 强制从指定节点重入
    template <typename TargetState, typename ReentryFromState, typename... Args>
    void step_reenter_from(Args&&... args) {
        StateAction<Context> action =
            StateAction<Context>::template step<TargetState>(
                std::forward<Args>(args)...)
                .template reenter_from<ReentryFromState>();
        this->step_internal(action);
    }

    // 强制全栈重入
    template <typename TargetState, typename... Args>
    void step_reenter_all(Args&&... args) {
        StateAction<Context> action =
            StateAction<Context>::template step<TargetState>(
                std::forward<Args>(args)...)
                .reenter_all();
        this->step_internal(action);
    }

    const std::vector<IState<Context>*> get_active_states_view() const {
        std::vector<IState<Context>*> view;
        view.reserve(active_depth_);
        for (size_t i = 0; i < active_depth_; ++i) {
            view.push_back(active_path_[i].state);
        }
        return view;
    }

    const std::string get_state_name() {
        if (active_depth_ > 0) {
            return active_path_[active_depth_ - 1].state->name();
        }
        return "UNKNOWN";
    }

    const int get_state_id() {
        if (active_depth_ > 0) {
            return active_path_[active_depth_ - 1].state->id();
        }
        return -1;
    }

    // 重载 1：处理只传“模板名”的情况 (如 is_active_state<WalkState>())
    template <template <typename...> class TargetTmpl>
    bool is_active_state() {
        auto states = get_active_states_view();
        for (auto* s : states) {
            if (!s) continue;
            if (dynamic_cast<TmplBase<TargetTmpl>*>(s) != nullptr) {
                return true;
            }
        }
        return false;
    }
    // 重载 2：处理传“具体类型”的情况 (如 InitState 或 WalkState<RobotContext>)
    template <typename TargetType>
    bool is_active_state() {
        using CleanType = std::decay_t<TargetType>;

        // 核心魔法：如果是 InitState，TargetCast 就是 InitState
        // 如果是 WalkState<Context>，TargetCast 就是 TmplBase<WalkState>
        using TargetCast = typename CastTargetType<CleanType>::type;

        auto states = get_active_states_view();
        for (auto* s : states) {
            if (!s) continue;
            // 完美匹配对应的 dynamic_cast！
            if (dynamic_cast<TargetCast*>(s) != nullptr) {
                return true;
            }
        }
        return false;
    }

    template <typename... Funcs>
    Future<bool> wait(CancellationToken token, Funcs&&... funcs) {
        auto user_visitor = overloaded{std::forward<Funcs>(funcs)...};
        // 提取所有 Lambda 的期望参数类型
        using ExpectedTypes = std::tuple<std::decay_t<
            typename lambda_traits<std::decay_t<Funcs>>::arg_type>...>;
        // 包装给底层的 EventBox 侦听器
        auto any_wrapper = [v = std::move(user_visitor)](
                               const EventBox& any_event) mutable -> bool {
            bool result = false;
            // 通过 ExpectedTypes{} 空元组将类型包传递过去
            bool handled =
                try_visit_event(v, any_event, result, ExpectedTypes{});
            return handled ? result : false;
        };
        return wait_internal(std::move(any_wrapper), std::move(token));
    }

    template <typename... Funcs>
    Future<bool> wait_for(uint32_t timeout_ms, Funcs&&... funcs) {
        auto source = std::make_shared<CancellationTokenSource>();
        auto token = source->get_token();
        if (timeout_ms > 0) source->cancel_after(timeout_ms, this);
        return wait(token, std::forward<Funcs>(funcs)...);
    }

    // 延迟若干秒（支持小数，如 1.5 秒）后在主事件线程执行任务
    // 返回一个函数，在执行前调用它可取消本次延迟任务
    std::function<void()> call_later(double seconds,
                                     std::function<void()> task) {
        return time_provider_->set_timeout(
            seconds, [this, task = std::move(task)]() {
                // 确保任务安全投递回 Engine 的事件主线程执行
                boost::asio::post(io_context_, std::move(task));
            });
    }

    // 延迟若干秒，返回 Future<void>，支持与原有异步体系配合
    Future<void> delay(double seconds) {
        auto dispatcher = [this](std::function<void()> cb) {
            this->post_future_task(std::move(cb));
        };
        auto timeout_scheduler = [this](uint32_t ms,
                                        std::function<void()> on_timeout) {
            return this->set_future_timeout(ms, std::move(on_timeout));
        };

        auto promise = std::make_shared<Promise<void>>(
            std::move(dispatcher), std::move(timeout_scheduler));

        // 用一个 std::shared_ptr 存放 cancellation 函数来延续其生命周期
        auto cancel_fn = std::make_shared<std::function<void()>>();

        // 设置定时器，定时到达后 resolve future
        *cancel_fn =
            time_provider_->set_timeout(seconds, [this, promise, cancel_fn]() {
                boost::asio::post(io_context_, [promise, cancel_fn]() {
                    if (promise->state() == PromiseState::PENDING) {
                        promise->resolve();
                    }
                });
            });

        return promise->get_future();
    }

    // 周期性循环调用：
    // - times > 0: 最多执行 times 次
    // - times < 0: 无限循环（必须内部返回 false 或被外部调用返回值取消）
    // - 支持 task 返回 bool：返回 false 可在内部立即安全终止循环！
    template <typename Func>
    std::function<void()> call_every(double interval_sec, int times,
                                     Func&& task) {
        if (times == 0) return []() {};

        struct ControlBlock {
            int remaining;
            bool cancelled{false};
            std::function<void()> current_cancel_fn;
        };
        auto ctrl = std::make_shared<ControlBlock>(ControlBlock{times});

        std::shared_ptr<std::function<void()>> step_fn =
            std::make_shared<std::function<void()>>();

        *step_fn = [this, interval_sec, task = std::forward<Func>(task), ctrl,
                    step_fn, times]() mutable {
            if (ctrl->cancelled || ctrl->remaining == 0) return;

            // --- 1. 智能适配返回值：判断内部是否要求中止 ---
            bool should_continue = true;
            if constexpr (std::is_invocable_v<Func, int>) {
                using RetType = std::invoke_result_t<Func, int>;
                int idx = times - ctrl->remaining;
                if constexpr (std::is_convertible_v<RetType, bool>) {
                    should_continue = task(idx);
                } else {
                    task(idx);
                }
            } else if constexpr (std::is_invocable_v<Func>) {
                using RetType = std::invoke_result_t<Func>;
                if constexpr (std::is_convertible_v<RetType, bool>) {
                    should_continue = task();
                } else {
                    task();
                }
            } else {
                static_assert(
                    std::is_invocable_v<Func> || std::is_invocable_v<Func, int>,
                    "task must accept either () or (int) as arguments!");
            }

            // --- 2. 扣减次数并判断终结条件 ---
            if (ctrl->remaining > 0) {
                ctrl->remaining--;
            }

            // 内部触发中止，或者已经达到了规定次数 -> 彻底收尾
            if (!should_continue || ctrl->remaining == 0) {
                ctrl->cancelled = true;
                return;
            }

            // --- 3. 还有剩余，预约下一次 ---
            if (!ctrl->cancelled) {
                ctrl->current_cancel_fn = time_provider_->set_timeout(
                    interval_sec, [this, ctrl, step_fn]() {
                        boost::asio::post(io_context_,
                                          [step_fn]() { (*step_fn)(); });
                    });
            }
        };

        // 立即向事件队列派发第一次执行
        boost::asio::post(io_context_, [step_fn]() { (*step_fn)(); });

        // 外部手动取消句柄
        return [ctrl]() {
            ctrl->cancelled = true;
            if (ctrl->current_cancel_fn) {
                ctrl->current_cancel_fn();
            }
        };
    }

    // 周期循环执行 times 次，并在【全部结束后】 Resolve 一个 Future<void>
    Future<void> repeat_async(double interval_sec, int times,
                              std::function<void(int current_idx)> task) {
        auto dispatcher = [this](std::function<void()> cb) {
            this->post_future_task(std::move(cb));
        };
        auto timeout_scheduler = [this](uint32_t ms,
                                        std::function<void()> on_timeout) {
            return this->set_future_timeout(ms, std::move(on_timeout));
        };

        auto promise = std::make_shared<Promise<void>>(
            std::move(dispatcher), std::move(timeout_scheduler));

        if (times <= 0) {
            promise->resolve();
            return promise->get_future();
        }

        // 用控制块记录循环索引
        struct LoopContext {
            int idx{0};
            int max_times;
            std::function<void()> schedule_next;
            std::function<void()> current_cancel_fn;
        };
        auto loop = std::make_shared<LoopContext>(LoopContext{0, times});

        loop->schedule_next = [this, interval_sec, task = std::move(task),
                               promise, loop]() {
            // 在安全事件循环中执行当次业务
            task(loop->idx);
            loop->idx++;

            // 判断结束条件
            if (loop->idx >= loop->max_times) {
                promise->resolve();  // ✅ 循环结束，触发链式下一阶！
                auto loop_copy = loop;
                boost::asio::post(io_context_, [loop_copy]() {
                    loop_copy->schedule_next = nullptr;
                    loop_copy->current_cancel_fn = nullptr;
                });
            } else {
                loop->current_cancel_fn =
                    time_provider_->set_timeout(interval_sec, [this, loop]() {
                        boost::asio::post(io_context_,
                                          [loop]() { loop->schedule_next(); });
                    });
            }
        };

        // 启动第一波
        boost::asio::post(io_context_, [loop]() { loop->schedule_next(); });

        return promise->get_future();
    }

    // For non-void ReturnType
    template <typename ReturnType,
              std::enable_if_t<!std::is_same<ReturnType, void>::value, int> = 0>
    Future<ReturnType> post_background_task(std::function<ReturnType()> task) {
        // 1. 直接构造 Dispatcher，通过捕获 this 指针派发
        // （前提是 Engine 实例的生命周期始终大于网络请求，通常都是如此）
        auto dispatcher = [engine = this](std::function<void()> cb) {
            engine->post_future_task(std::move(cb));
        };

        auto timeout_scheduler =
            [engine = this](uint32_t ms, std::function<void()> on_timeout) {
                return engine->set_future_timeout(ms, std::move(on_timeout));
            };

        // 2. 绕开伪 shared_ptr，直接注入调度器
        auto promise = std::make_shared<Promise<ReturnType>>(
            std::move(dispatcher), std::move(timeout_scheduler));

        // 3. 将任务放入工作线程池，并加上全面的异常保护
        boost::asio::post(*this->workflow_pool_.value(), [task, engine = this,
                                                          promise]() {
            try {
                // 强制同步执行实际的后台逻辑 (比如 HTTP 请求)
                ReturnType data = task();

                // 成功后，将结果切回主事件循环
                boost::asio::post(engine->io_context_,
                                  [data = std::move(data), promise]() mutable {
                                      promise->resolve(std::move(data));
                                  });
            } catch (...) {
                // 如果遭遇未知崩溃，将异常抛给 Catch_error 流
                std::exception_ptr e = std::current_exception();
                boost::asio::post(engine->io_context_,
                                  [e, promise]() { promise->reject(e); });
            }
        });

        return promise->get_future();
    }

    // ====== 引擎生命周期 ======
    template <typename RootState, typename... Args>
    void start(std::chrono::milliseconds tick_interval, Args&&... args) {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) return;
        tick_interval_ = tick_interval;

        // 不再起新线程，仅初始化数据和派发第一个事件
        work_guard_.emplace(boost::asio::make_work_guard(io_context_));
        workflow_pool_.emplace(std::make_unique<boost::asio::thread_pool>(4));
        schedule_next_tick();

        // 派发初始状态跳转（放入 io_context 队列）
        boost::asio::post(
            io_context_, [this, args = std::make_tuple(
                                    std::forward<Args>(args)...)]() mutable {
                // 真正开始处理事件时，记录执行该任务的线程ID作为
                // event_thread_id_
                event_thread_id_ = std::this_thread::get_id();
                std::apply(
                    [this](auto&&... unpacked_args) {
                        execute_transition(
                            StateAction<Context>::template step<RootState>(
                                std::forward<decltype(unpacked_args)>(
                                    unpacked_args)...));
                    },
                    std::move(args));
            });
    }

    void stop() {
        // 原子级别判断并修改 running_
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false)) {
            return;
        }
        if (tick_canceler_) {
            tick_canceler_();
            tick_canceler_ = nullptr;
        }
        if (work_guard_.has_value()) {
            work_guard_.reset();  // 允许 io_context 退出
        }
        if (workflow_pool_.has_value()) {
            workflow_pool_.value()->stop();
            workflow_pool_.value()->join();
        }
    }

    void step_internal(const StateAction<Context>& action) {
        // 保留原先在 step() 里的线程安全检查
        if (std::this_thread::get_id() != event_thread_id_) {
            throw std::logic_error(
                "FATAL: step() must be called from the event loop thread! "
                "Use dispatch() or dispatch_async() instead.");
        }
        // 执行底层转换
        execute_transition(action);
    }

    void dispatch_internal(EventBox e) {
        // asio::defer 告诉
        // Asio：这是一个后续任务，请优先在当前调用栈结束后执行！
        boost::asio::defer(io_context_, [this, e = std::move(e)]() mutable {
            this->process_internal(e);
        });
    }
};

// 引入 CRTP 允许外部重写 Engine 的 on_event
template <typename Context, typename DerivedEngine>
class BaseEngine
    : public IEventHandler<IEngine<Context>, Context, void, DerivedEngine>,
      public std::enable_shared_from_this<BaseEngine<Context, DerivedEngine>> {
   private:
    // 使用子类类型作为模板参数，因为用到handle_event
    void process_internal(const EventBox& e) {
        // 开启事件处理周期的统一防护
        StateAction<Context> final_transition =
            StateAction<Context>::unhandled();
        {
            StepLevelGuard event_guard(this->step_call_level_);
            // 1. 触发 Wait 系统
            std::vector<std::shared_ptr<Promise<bool>>> to_resolve;
            for (auto it = this->active_waits_.begin();
                 it != this->active_waits_.end();) {
                if (it->second->promise->state() != PromiseState::PENDING) {
                    it = this->active_waits_.erase(it);
                } else if (it->second->predicate(e)) {
                    to_resolve.push_back(it->second->promise);
                    it = this->active_waits_.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto& p : to_resolve) {
                p->resolve(true);  // 如果这里触发了 step()，会被缓存在
                                   // pending_transition_ 中
            }

            // 2. 触发全局无状态监听器
            for (auto& listener : this->listeners_) {
                if (this->pending_transition_.has_value())
                    break;  // 【短路优化】一旦发生跳转，立刻中止后续派发
                listener->handle_event(e, this->ctx_);
            }

            // 3. 触发引擎自身的拦截
            if (!this->pending_transition_.has_value()) {
                this->handle_event(e, this->ctx_);
            }

            // 4. HSM 事件冒泡核心
            if (!this->pending_transition_.has_value()) {
                for (int i = this->active_depth_ - 1; i >= 0; --i) {
                    StateAction<Context> action =
                        this->active_path_[i].state->handle_event(e,
                                                                  this->ctx_);

                    // 优先处理 return StateAction::step() 的正规跳转
                    if (action.type == StateAction<Context>::Type::TRANSITION) {
                        final_transition = action;
                        break;
                    }
                    // 拦截到了内部强行调用 engine->step() 产生的缓存跳转
                    else if (this->pending_transition_.has_value()) {
                        break;
                    }
                    // 事件被拦截吸收
                    else if (action.type ==
                             StateAction<Context>::Type::HANDLED) {
                        break;
                    }
                }
            }

            // 【终极收口】：如果在 Wait、Listener、Base 或 HSM 内部任何一处
            // 调用了 step()，统统在这里一次性提取！
            if (this->pending_transition_.has_value()) {
                final_transition = std::move(*(this->pending_transition_));
                this->pending_transition_.reset();
            }

        }  // 【护盾解除】：离开作用域，step_call_level_ 恢复为 0

        // 如果最终决议需要发生跳转，在这里绝对安全地执行！
        if (final_transition.type == StateAction<Context>::Type::TRANSITION) {
            this->execute_transition(final_transition);
        }
    }

   public:
    using AllowedEvents = std::tuple<>;
    using BaseHandler =
        IEventHandler<IEngine<Context>, Context, void, DerivedEngine>;
    using BaseHandler::BaseHandler;

    // 使用子类类型作为模板参数，因为使用了shared_from_this
    // 2. 异步派发：【在塞入队列前，给 Event 注入 Promise！】
    template <typename E>
    Future<typename E::ReturnType> dispatch_async(E e,
                                                  uint32_t timeout_ms = 5000) {
        static_assert(
            std::is_base_of_v<AsyncEvent<typename E::ReturnType>, E>,
            "Event must inherit from dk::AsyncEvent to use dispatch_async!");
        using R = typename E::ReturnType;
        auto promise = std::make_shared<Promise<R>>(this->shared_from_this());
        Future<R> future = promise->get_future();

        // 【关键注入】：赋予这个普通事件以异步返回的超能力
        e.promise_ = promise;
        if (timeout_ms > 0) {
            future = future.timeout(timeout_ms);
        }
        boost::asio::post(
            this->io_context_,
            [this, e = std::move(e)]() mutable { this->process_internal(e); });

        return future;
    }

    // 使用到了真实类型作为模板参数
    // 为若干个事件注册【一个】监听器，如果return False则继续监听
    // 该函数会立即注册，防止在同一个线程中错过随后的 dispatch
    Future<bool> wait_internal(std::function<bool(const EventBox&)> predicate,
                               CancellationToken token) override {
        auto promise =
            std::make_shared<Promise<bool>>(this->shared_from_this(), token);
        Future<bool> future = promise->get_future();

        auto register_task = [this, predicate = std::move(predicate),
                              promise]() mutable {
            uint64_t id = ++(this->next_wait_id_);
            this->active_waits_[id] = std::make_shared<WaitNode>(
                WaitNode{id, std::move(predicate), promise});
        };

        // 如果当前已经在引擎的工作线程中，立刻同步注册！防止错过随后的 dispatch
        if (std::this_thread::get_id() == this->get_thread_id()) {
            register_task();
        } else {
            // 如果是外部线程发起的 wait，再扔进队列
            boost::asio::post(this->io_context_, std::move(register_task));
        }

        return future;
    }
};

}  // namespace dk