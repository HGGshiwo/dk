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

// ================= dk 框架核心 =================
namespace dk {

// 引入类型萃取黑魔法：用于自动推导 Lambda 的参数类型
template <typename T>
struct lambda_traits : public lambda_traits<decltype(&T::operator())> {};
template <typename ClassType, typename ReturnType, typename ArgType>
struct lambda_traits<ReturnType (ClassType::*)(ArgType) const> {
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

struct TickEvent {};
struct EnterEvent {
    const std::string state_name;
};
struct ExitEvent {
    const std::string state_name;
};
struct StateChangeEvent {
    std::string prev;
    std::string cur;
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
            promise_->reject(std::make_exception_ptr(std::runtime_error(error_msg)));
        }
    }

    const bool is_settled() const { return promise_ == nullptr || promise_->state() != PromiseState::PENDING; }

    // 辅助方法：判断外界是否在等待
    bool is_awaited() const { return promise_ != nullptr; }
};

struct WaitNode {
    uint64_t id;
    std::function<bool(const std::any&)> predicate;
    std::shared_ptr<Promise<bool>> promise;
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

    std::atomic<bool> running_{false};

    std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_guard_;
    std::optional<boost::asio::steady_timer> tick_timer_;
    std::chrono::milliseconds tick_interval_;
    std::thread worker_thread_;  // 只需要这一个线程跑 io_context

    std::optional<std::unique_ptr<boost::asio::thread_pool>> workflow_pool_;
    std::thread::id event_thread_id_;  // 事件循环的thread id, 用于检测wait

   protected:
    Context ctx_;
    size_t active_depth_ = 0;
    std::array<StateNode, MaxDepth> active_path_;

    // Buffer for transition requested during an active transition (e.g., inside on_enter/on_exit)
    std::optional<StateAction<Context>> pending_transition_;
    using EventListenerPtr = std::shared_ptr<IEventListener<Context>>;
    std::vector<EventListenerPtr> listeners_;  // 和状态无关的监听器

    // 引入 Asio 的绝对核心：I/O 上下文
    boost::asio::io_context io_context_;
    // 保存所有正在等待的请求。使用 map 是为了 O(1) 的删除效率
    std::unordered_map<uint64_t, std::shared_ptr<WaitNode>> active_waits_;
    uint64_t next_wait_id_ = 0;

    int step_call_level_ = 0;  // step调用层数
    struct StepLevelGuard {
        int& level;
        StepLevelGuard(int& l) : level(l) { level++; }
        ~StepLevelGuard() { level--; }
    };

    virtual void process_internal(const std::any& e) = 0;

    virtual Future<bool> wait_internal(std::function<bool(const std::any&)> predicate, CancellationToken token) = 0;

    // IStatePathBuilder 接口实现（含生命周期事件派发）
    size_t get_memory_mark() const override { return current_offset_; }
    void* allocate_bytes(size_t size, size_t alignment) override {
        size_t aligned_offset = (current_offset_ + alignment - 1) & ~(alignment - 1);
        if (aligned_offset + size > MemorySize) {
            std::cout << "[Engeine]: allocate " << aligned_offset + size << " > " << MemorySize << std::endl;
            return nullptr;
        };
        void* ptr = memory_arena_ + aligned_offset;
        current_offset_ = aligned_offset + size;
        return ptr;
    }

    bool try_reuse(std::string_view state_name) override {
        if (!divergence_found_ && build_depth_ < active_depth_ &&
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

    void push_new_state(IState<Context>* state, void (*destroy_fn)(IState<Context>*), size_t original_offset) override {
        state->parent_ptr = (active_depth_ > 0) ? active_path_[active_depth_ - 1].state : nullptr;

        active_path_[active_depth_] = {state, destroy_fn, original_offset};
        active_depth_++;
        build_depth_++;
        // 【生命周期】：触发 Enter 事件和回调 (由于 step_call_level_ 大于 0，这里引发的跳转会被 pending 缓存)
        process_internal(std::make_any<EnterEvent>(EnterEvent{state->name()}));
        state->on_enter(ctx_);
    }

    void pop_state() {
        if (active_depth_ == 0) return;
        auto& node = active_path_[active_depth_ - 1];

        // 【生命周期】：触发 Exit 事件和回调
        node.state->on_exit(ctx_);
        process_internal(std::make_any<ExitEvent>(ExitEvent{node.state->name()}));
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
                // 初始化构建环境
                build_depth_ = 0;
                divergence_found_ = false;
                // 【魔法时刻】：一行代码自动完成 LCA 比对、旧状态 Pop 退栈、新状态 Push 入栈
                action.path_builder(*this);
                // 如果新路径比旧路径短，把多余的尾巴截断退出
                while (active_depth_ > build_depth_) {
                    pop_state();
                }
                std::string next_leaf = get_state_name();
                process_internal(std::make_any<StateChangeEvent>(StateChangeEvent{prev_leaf, next_leaf}));
                if (pending_transition_.has_value()) {
                    // 如果在上述的 pop_state(on_exit) 或 push_new_state(on_enter) 中发生了新的跳转
                    action = std::move(*pending_transition_);
                } else {
                    break;  // 跳转彻底结束
                }
            }
        } catch (const std::exception& e) {
            step_call_level_ = 0;
            std::cout << "[Engine]: transition error: " << e.what() << std::endl;
        }
    }

   private:
    template <typename Visitor, typename... Ts>
    static bool try_visit_any(Visitor& v, const std::any& any_event, bool& result_out, std::tuple<Ts...>) {
        bool handled = false;
        // 核心魔法：使用 auto 泛型 lambda 接收类型标签。
        // lambda 内部没有任何模板参数包 Ts！完全与外部解耦！
        auto check = [&](auto tag) -> bool {
            using T = typename decltype(tag)::type;  // 从标签中把真实的事件类型 T 提炼出来
            // 此时的 T 极其纯净，GCC 绝对不会报错
            if (const T* concrete_event = std::any_cast<T>(&any_event)) {
                using RetType = decltype(v(*concrete_event));

                // 自动适配 void 和 bool 返回值
                if constexpr (std::is_convertible_v<RetType, bool>) {
                    result_out = v(*concrete_event);
                } else {
                    v(*concrete_event);
                    result_out = true;
                }
                handled = true;
                return true;  // 匹配成功，短路终止后续的查找
            }
            return false;  // 类型不匹配，告诉外层继续找
        };
        // 完美的包展开：只展开函数调用 check(TypeTag<T1>{}) || check(TypeTag<T2>{}) ...
        (void)(check(TypeTag<Ts>{}) || ...);
        return handled;
    }

    void schedule_next_tick() {
        if (!running_) return;
        tick_timer_.value().expires_after(tick_interval_);
        tick_timer_.value().async_wait([this](const boost::system::error_code& ec) {
            if (!ec && running_) {
                this->process_internal(std::make_any<TickEvent>(TickEvent{}));
                this->schedule_next_tick();
            }
        });
    }

   public:
    ~IEngine() { stop(); };

    Context& get_context() { return ctx_; }

    void add_listener(EventListenerPtr listener) {
        boost::asio::post(io_context_, [this, listener]() { this->listeners_.push_back(listener); });
    }

    virtual void handle_event(const std::any& event, Context& ctx) = 0;

    boost::asio::io_context& get_ioc() { return io_context_; }

    std::thread::id get_thread_id() override { return event_thread_id_; }

    // 实现promise runtime
    void post_future_task(std::function<void()> task) override { boost::asio::post(io_context_, std::move(task)); }

    // 实现promise runtime
    std::function<void()> set_future_timeout(uint32_t ms, std::function<void()> on_timeout) override {
        auto timer = std::make_shared<boost::asio::steady_timer>(io_context_);
        timer->expires_after(std::chrono::milliseconds(ms));

        timer->async_wait([on_timeout](const boost::system::error_code& ec) {
            if (!ec) on_timeout();
        });
        // 返回取消句柄
        return [timer]() { timer->cancel(); };
    }

    // 线程安全的外部事件注入接口
    template <typename Event>
    void dispatch(Event e) {
        boost::asio::post(io_context_,
                          [this, ev = std::make_any<Event>(std::move(e))]() mutable { this->process_internal(ev); });
    }

    template <typename StateType, typename... Args>
    void step(Args&&... args) {
        // 利用 StateAction 的静态工厂完成“类型擦除”
        StateAction<Context> action = StateAction<Context>::template step<StateType>(std::forward<Args>(args)...);
        // 调用子类实现的非模板虚函数
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

    // === 适配 std::any 的新一代多重 Lambda 等待黑魔法 ===
    template <typename... Funcs>
    Future<bool> wait(CancellationToken token, Funcs&&... funcs) {
        auto user_visitor = overloaded{std::forward<Funcs>(funcs)...};
        // 提取所有 Lambda 的期望参数类型
        using ExpectedTypes = std::tuple<std::decay_t<typename lambda_traits<std::decay_t<Funcs>>::arg_type>...>;
        // 包装给底层的 std::any 侦听器
        auto any_wrapper = [v = std::move(user_visitor)](const std::any& any_event) mutable -> bool {
            bool result = false;
            // 通过 ExpectedTypes{} 空元组将类型包传递过去
            bool handled = try_visit_any(v, any_event, result, ExpectedTypes{});
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

    // For non-void ReturnType
    template <typename ReturnType, std::enable_if_t<!std::is_same<ReturnType, void>::value, int> = 0>
    Future<ReturnType> post_background_task(std::function<ReturnType()> task) {
        std::shared_ptr<IEngine> self_ptr(this, [](IEngine*) {});
        std::shared_ptr<Promise<ReturnType>> promise = std::make_shared<Promise<ReturnType>>(self_ptr);
        boost::asio::post(*this->workflow_pool_.value(), [task, engine = this, promise]() {
            // Execute background task
            ReturnType data = task();
            // Return result safely to main io_context
            boost::asio::post(engine->io_context_,
                              [data = std::move(data), promise]() mutable { promise->resolve(std::move(data)); });
        });
        return promise->get_future();
    }

    // ====== 引擎生命周期 ======
    template <typename RootState, typename... Args>
    void start(std::chrono::milliseconds tick_interval, Args&&... args) {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) return;
        tick_interval_ = tick_interval;

        // Use shared_ptr to prevent dangling reference if exception occurs later
        auto init_promise = std::make_shared<std::promise<void>>();
        std::future<void> init_future = init_promise->get_future();
        // Pass arguments into std::thread, lambda accepts them as thread_args
        worker_thread_ = std::thread(
            [this, init_promise](auto&&... thread_args) {
                bool promise_fulfilled = false;  // Track promise state to avoid std::future_error
                try {
                    event_thread_id_ = std::this_thread::get_id();
                    work_guard_.emplace(boost::asio::make_work_guard(io_context_));
                    tick_timer_.emplace(io_context_);
                    workflow_pool_.emplace(std::make_unique<boost::asio::thread_pool>(4));
                    schedule_next_tick();
                    // Forward the thread_args to the initial transition
                    execute_transition(StateAction<Context>::template step<RootState>(
                        std::forward<decltype(thread_args)>(thread_args)...));

                    init_promise->set_value();
                    promise_fulfilled = true;

                    io_context_.run();
                } catch (const std::exception& e) {
                    running_ = false;
                    std::cout << "[Engine]: fatal error: " << e.what() << std::endl;
                    if (!promise_fulfilled) {
                        init_promise->set_exception(std::current_exception());
                    }
                } catch (...) {
                    running_ = false;
                    std::cout << "[Engine]: Unknown exception type without what() interface.\n";
                    if (!promise_fulfilled) {
                        init_promise->set_exception(std::current_exception());
                    }
                }
            },
            std::forward<Args>(args)...);  // Standard way to pass args to std::thread
        init_future.get();
    }

    void stop() {
        // 原子级别判断并修改 running_
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false)) {
            return;
        }
        if (work_guard_.has_value()) {
            work_guard_.reset();  // 允许 io_context 退出
        }

        io_context_.stop();  // 立刻停止事件循环
        if (worker_thread_.joinable()) {
            // 判断是否是当前线程
            if (std::this_thread::get_id() != worker_thread_.get_id()) {
                worker_thread_.join();
            } else {
                throw std::logic_error("FATAL: Cannot destroy runtime from its own worker thread!");
            }
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

    void dispatch_internal(std::any e) {
        // asio::defer 告诉 Asio：这是一个后续任务，请优先在当前调用栈结束后执行！
        boost::asio::defer(io_context_, [this, e = std::move(e)]() mutable { this->process_internal(e); });
    }
};

// 引入 CRTP 允许外部重写 Engine 的 on_event
template <typename Context, typename DerivedEngine>
class BaseEngine : public IEventHandler<IEngine<Context>, Context, void, DerivedEngine>,
                   public std::enable_shared_from_this<BaseEngine<Context, DerivedEngine>> {
   private:
    // 使用子类类型作为模板参数，因为用到handle_event
    void process_internal(const std::any& e) {
        // 1. 触发 Wait 系统 (纯异步无状态修改，无需拦截)
        std::vector<std::shared_ptr<Promise<bool>>> to_resolve;
        for (auto it = this->active_waits_.begin(); it != this->active_waits_.end();) {
            if (it->second->promise->state() != PromiseState::PENDING) {
                it = this->active_waits_.erase(it);
            } else if (it->second->predicate(e)) {
                to_resolve.push_back(it->second->promise);  // 先收集
                it = this->active_waits_.erase(it);         // 安全删除
            } else {
                ++it;
            }
        }
        // 循环外再执行回调，此时无论外部怎么修改 active_waits_ 都是安全的
        for (auto& p : to_resolve) {
            p->resolve(true);
        }
        // =======================================================
        // === 开启事件处理周期的统一防护：所有显式跳转转为缓存 ===
        // =======================================================
        StateAction<Context> final_transition = StateAction<Context>::unhandled();
        // 2. 触发全局无状态监听器
        for (auto& listener : this->listeners_) {
            listener->handle_event(e, this->ctx_);
        }
        // 3. 触发引擎自身的拦截 (BaseEngine 继承自 IEventHandler)
        if (!this->pending_transition_.has_value()) {
            this->handle_event(e, this->ctx_);
        }
        // 4. HSM 事件冒泡核心：从数组尾部（叶子）向头部（根）冒泡
        if (!this->pending_transition_.has_value()) {
            for (int i = this->active_depth_ - 1; i >= 0; --i) {
                StateAction<Context> action = this->active_path_[i].state->handle_event(e, this->ctx_);
                if (action.type == StateAction<Context>::Type::TRANSITION) {
                    final_transition = action;
                    this->pending_transition_.reset();
                    break;
                } else if (this->pending_transition_.has_value()) {
                    final_transition = std::move(*(this->pending_transition_));
                    this->pending_transition_.reset();
                    break;
                } else if (action.type == StateAction<Context>::Type::HANDLED) {
                    break;
                }
            }
        } else {
            final_transition = std::move(*(this->pending_transition_));
            this->pending_transition_.reset();
        }
        // =======================================================
        // === 防护解除，统一结算状态树 ===
        // =======================================================

        // 如果最终决议需要发生跳转，在这里绝对安全地执行！
        if (final_transition.type == StateAction<Context>::Type::TRANSITION) {
            this->execute_transition(final_transition);
        }
    }

   public:
    using AllowedEvents = std::tuple<>;

    // 使用子类类型作为模板参数，因为使用了shared_from_this
    // 2. 异步派发：【在塞入队列前，给 Event 注入 Promise！】
    template <typename E>
    Future<typename E::ReturnType> dispatch_async(E e, uint32_t timeout_ms = 5000) {
        static_assert(std::is_base_of_v<AsyncEvent<typename E::ReturnType>, E>,
                      "Event must inherit from dk::AsyncEvent to use dispatch_async!");
        using R = typename E::ReturnType;
        auto promise = std::make_shared<Promise<R>>(this->shared_from_this());
        Future<R> future = promise->get_future();

        // 【关键注入】：赋予这个普通事件以异步返回的超能力
        e.promise_ = promise;
        if (timeout_ms > 0) {
            future = future.timeout(timeout_ms);
        }
        boost::asio::post(this->io_context_, [this, e = std::move(e)]() mutable { this->process_internal(e); });

        return future;
    }

    // 使用到了真实类型作为模板参数
    // 为若干个事件注册【一个】监听器，如果return False则继续监听
    Future<bool> wait_internal(std::function<bool(const std::any&)> predicate, CancellationToken token) override {
        auto promise = std::make_shared<Promise<bool>>(this->shared_from_this(), token);
        Future<bool> future = promise->get_future();
        boost::asio::post(this->io_context_, [this, predicate = std::move(predicate), promise]() {
            uint64_t id = ++(this->next_wait_id_);
            this->active_waits_[id] = std::make_shared<WaitNode>(WaitNode{id, std::move(predicate), promise});
        });
        return future;
    }
};

}  // namespace dk