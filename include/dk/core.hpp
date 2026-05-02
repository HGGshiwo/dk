#pragma once
// #include <algorithm>
#include <atomic>
#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>
#include <chrono>
#include <condition_variable>
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
#include <typeinfo>
#include <unordered_map>
#include <variant>
#include <vector>

// ================= dk 框架核心 =================
namespace dk {

// 探测器：检查类型 T 是否包含 on_event(Event, Context)
template <typename T, typename E, typename C, typename = void>
struct has_on_event : std::false_type {};
template <typename T, typename E, typename C>
struct has_on_event<T, E, C, std::void_t<decltype(std::declval<T>().on_event(std::declval<E>(), std::declval<C>()))>>
    : std::true_type {};

// 提供给State的抽象接口
template <typename Event, typename Context>
class IEngine {
   public:
    // 投递后台工作流
    virtual void run_workflow(std::function<std::optional<Event>()> workflow) = 0;

    // 派发内部高优事件（微队列）
    virtual void dispatch_internal(Event e) = 0;

    // 根据任务类型调用on_event分发
    virtual void handle_event(const Event& e, Context& ctx) = 0;

   protected:
    // 保护析构，防止子状态手贱把引擎给 delete 掉
    ~IEngine() = default;
};

// 框架内置生命周期与心跳事件
struct TickEvent {};
struct EnterEvent {
    const std::string state_name;
};  // 携带状态名，方便全局拦截打印
struct ExitEvent {
    const std::string state_name;
};

template <typename ResultT>
class Promise {
    std::promise<ResultT> promise_;
    bool is_resolved_{false};

   public:
    Promise() = default;
    // 禁用拷贝
    Promise(const Promise&) = delete;
    Promise& operator=(const Promise&) = delete;
    ~Promise() {
        // 【核心逻辑】：如果对象析构时还没有被 resolve，自动抛出特定异常
        if (!is_resolved_) {
            promise_.set_exception(
                std::make_exception_ptr(std::runtime_error("AsyncEvent was discarded by State Machine (Unhandled).")));
        }
    }
    std::future<ResultT> get_future() { return promise_.get_future(); }

    void resolve(ResultT val) {
        if (!is_resolved_) {
            promise_.set_value(std::move(val));
            is_resolved_ = true;
        }
    }
    void reject(const std::exception_ptr& eptr) {
        if (!is_resolved_) {
            promise_.set_exception(eptr);
            is_resolved_ = true;
        }
    }
};

// 异步事件
template <typename ResultT>
struct AsyncEvent {
    using ReturnType = ResultT;

    // 【核心改造】：Event 本身持有 Promise！
    // 使用 mutable 是因为状态机的 on_event(const E& e) 接收的是 const 引用，
    // 而 resolve 本质上是一个不改变 Event 业务数据的副作用逻辑。
    mutable std::shared_ptr<Promise<ResultT>> promise_;
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

    // 辅助方法：判断外界是否在等待
    bool is_awaited() const { return promise_ != nullptr; }
};

template <typename Event, typename Derived>
struct BaseContext {
    // 存放业务上下文数据
    IEngine<Event, Derived>* engine;
};

// 调用handle_event时根据event类型, 给on_event分发
// BaseInterface: 抽象基类，必须有一个虚函数叫做hand_event
template <typename BaseInterface, typename Event, typename Context, typename ReturnType, typename DerivedChild>
class IEventHandler : public BaseInterface {
   public:
    ReturnType handle_event(const Event& event, Context& ctx) override {
        DerivedChild* child = static_cast<DerivedChild*>(this);

        return std::visit(
            [child, &ctx](const auto& e) -> ReturnType {
                // 1. 探测业务类是否实现了这个事件的处理
                if constexpr (has_on_event<DerivedChild, decltype(e), Context&>::value) {
                    // 如果期望返回 void，直接调用然后 return
                    if constexpr (std::is_void_v<ReturnType>) {
                        child->on_event(e, ctx);
                        return;
                    }
                    // 如果期望有返回值（如 shared_ptr），原样返回
                    else {
                        return child->on_event(e, ctx);
                    }

                }
                // 2. 自动兜底逻辑（业务类没写对应的 on_event）
                else {
                    // 如果期望返回 void，直接返回空
                    if constexpr (std::is_void_v<ReturnType>) {
                        return;
                    }
                    // 如果期望有返回值，返回该类型的【默认构造值】
                    // 对于 shared_ptr，ReturnType{} 就是 nullptr！
                    else {
                        return ReturnType{};
                    }
                }
            },
            event);
    }
};

// 和状态无关的事件监听器
template <typename Event, typename Context>
class IEventListener {
   public:
    virtual void handle_event(const Event& event, Context& ctx) = 0;
    virtual ~IEventListener() = default;
};

template <typename Event, typename Context, typename DerivedChild>
class BaseEventListener : public IEventHandler<IEventListener<Event, Context>, Event, Context, void, DerivedChild> {};

template <typename Event, typename Context>
class IState {
   public:
    virtual ~IState() = default;
    virtual std::shared_ptr<IState<Event, Context>> handle_event(const Event& event, Context& ctx) = 0;
    virtual const std::string name() const = 0;
};

template <typename Event, typename Context, typename DerivedChild>
class BaseState : public IEventHandler<IState<Event, Context>, Event, Context, std::shared_ptr<IState<Event, Context>>,
                                       DerivedChild> {
   public:
    virtual const std::string name() const override { return typeid(DerivedChild).name(); }
};

// 纯状态类，不允许有私有变量
template <typename Event, typename Context, typename DerivedChild>
class PureState : public BaseState<Event, Context, DerivedChild> {
   public:
    PureState() {
        static_assert(sizeof(DerivedChild) == sizeof(BaseState<Event, Context, DerivedChild>),
                      "FATAL: A PureState must NOT have member variables! Use BaseState instead.");
    }

    static std::shared_ptr<IState<Event, Context>> instance() {
        // 1. C++11 标准保证了局部静态变量的初始化是【绝对线程安全】的
        static DerivedChild static_instance;

        // 2. 核心魔法：返回一个 shared_ptr，但给它一个【空的删除器】！
        // 因为 static_instance 是静态区的变量，程序结束时系统会自动回收，
        // 如果 shared_ptr 默认去 delete 它，会导致 Core Dump（段错误）。
        // 传入 [](IState<Event, Context>*){} 告诉 shared_ptr：“引用计数清零时什么都不用做”。
        return std::shared_ptr<IState<Event, Context>>(&static_instance, [](IState<Event, Context>*) {});
    }
};

// 引入 CRTP 允许外部重写 Engine 的 on_event
template <typename Event, typename DerivedEngine, typename Context>
class BaseEngine : public std::enable_shared_from_this<BaseEngine<Event, DerivedEngine, Context>>,
                   public IEventHandler<IEngine<Event, Context>, Event, Context, void, DerivedEngine> {
    using StateType = IState<Event, Context>;
    using EventListenerPtr = std::shared_ptr<IEventListener<Event, Context>>;

    std::shared_ptr<StateType> state_;
    Context ctx_;

    std::mutex mtx_;
    std::queue<Event> external_queue_;
    std::queue<Event> internal_queue_;

    std::condition_variable cv_;
    std::atomic<bool> running_{false};
    std::thread worker_thread_;

    std::mutex timer_mtx_;
    std::condition_variable timer_cv_;
    std::thread timer_thread_;

    std::unique_ptr<boost::asio::thread_pool> workflow_pool_;

    std::vector<EventListenerPtr> listeners_;  // 和状态无关的监听器

   public:
    virtual ~BaseEngine() { stop(); }
    Context& get_context() { return ctx_; }

    void add_listener(EventListenerPtr listener) { listeners_.push_back(listener); }

    // 线程安全的外部事件注入接口
    void dispatch(Event e) {
        std::lock_guard<std::mutex> lock(mtx_);
        external_queue_.push(std::move(e));
        cv_.notify_one();
    }

    // 2. 异步派发：【在塞入队列前，给 Event 注入 Promise！】
    template <typename E>
    std::future<typename E::ReturnType> dispatch_async(E e) {
        static_assert(std::is_base_of_v<AsyncEvent<typename E::ReturnType>, E>,
                      "Event must inherit from dk::AsyncEvent to use dispatch_async!");
        using R = typename E::ReturnType;
        auto promise = std::make_shared<Promise<R>>();
        auto future = promise->get_future();

        // 【关键注入】：赋予这个普通事件以异步返回的超能力
        e.promise_ = promise;

        std::lock_guard<std::mutex> lock(mtx_);
        // 直接存入原生 Event，告别 EnvelopedEvent！
        external_queue_.push(Event{std::move(e)});
        cv_.notify_one();

        return future;
    }

    void start(std::shared_ptr<IState<Event, Context>> initial_state, std::chrono::milliseconds tick_interval) {
        if (running_) return;
        ctx_.engine = this;
        workflow_pool_ = std::make_unique<boost::asio::thread_pool>(4);

        transition(initial_state);
        running_ = true;
        // 1. 启动【工作线程】
        worker_thread_ = std::thread(&BaseEngine::run_loop, this);
        // 2. 启动【定时器线程】
        timer_thread_ = std::thread([this, tick_interval]() {
            while (running_) {
                std::unique_lock<std::mutex> lock(timer_mtx_);
                // wait_for 的魔法：
                // 如果休眠期间被 notify 唤醒，且 running_ 变为 false，它会立刻返回 true（中断睡眠）。
                // 如果睡满了 tick_interval 都没人叫它，它会返回 false（正常超时）。
                bool stopped = timer_cv_.wait_for(lock, tick_interval, [this]() { return !running_; });

                // 只有正常超时醒来，且引擎还在运行，才发送 Tick
                if (!stopped && running_) {
                    this->dispatch_internal(Event{dk::TickEvent{}});
                }
            }
        });
    }

    void stop() {
        if (!running_) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(timer_mtx_);
            running_ = false;
            timer_cv_.notify_all();
        }
        {
            std::lock_guard<std::mutex> lock(mtx_);
            cv_.notify_all();
        }

        if (timer_thread_.joinable()) timer_thread_.join();
        // 判断是否是当前线程
        if (worker_thread_.joinable() && std::this_thread::get_id() != worker_thread_.get_id()) {
            worker_thread_.join();
        } else {
            worker_thread_.detach();  // 如果是自身调用，只能 detach 交给系统回收
        }
        workflow_pool_->stop();
        workflow_pool_->join();
    }

    void run_workflow(std::function<std::optional<Event>()> workflow) override {
        if (!workflow_pool_) {
            // 可以打印一个日志，没有调用start
            return;
        }
        std::weak_ptr<BaseEngine> weak_this = this->weak_from_this();

        boost::asio::post(*workflow_pool_, [weak_this, workflow]() {
            auto result_event = workflow();
            if (result_event.has_value()) {
                if (auto shared_this = weak_this.lock()) {
                    shared_this->dispatch_internal(std::move(result_event.value()));
                }
            }
        });
    }

    void dispatch_internal(Event e) override {
        std::lock_guard<std::mutex> lock(mtx_);
        internal_queue_.push(std::move(e));
        cv_.notify_one();
    }

   private:
    // 状态流转闭环
    void transition(std::shared_ptr<StateType> next_state) {
        if (!next_state) return;

        if (state_) {
            Event exit_e = dk::ExitEvent{state_->name()};
            this->handle_event(exit_e, ctx_);    // 1. 全局拦截 Exit
            state_->handle_event(exit_e, ctx_);  // 2. 状态机处理 Exit
        }

        state_ = next_state;

        if (state_) {
            Event enter_e = dk::EnterEvent{state_->name()};
            this->handle_event(enter_e, ctx_);    // 3. 全局拦截 Enter
            state_->handle_event(enter_e, ctx_);  // 4. 状态机处理 Enter
        }
    }

    // 处理核心业务事件
    void process_internal(const Event& e) {
        for (auto& listener : listeners_) {
            listener->handle_event(e, ctx_);
        }
        this->handle_event(e, ctx_);  // 全局拦截器（可拦截业务事件和 Tick）

        if (!state_) return;
        auto next_state = state_->handle_event(e, ctx_);
        if (next_state && next_state != state_) {
            transition(next_state);
        }
    }

    void run_loop() {  // 👈 注意：再也不需要传入 tick_interval 了
        while (running_) {
            // ==========================================
            // 阶段 A：绝对排干内部微队列（最高优先级）
            // ==========================================
            while (true) {
                std::queue<Event> local_internal;
                {
                    std::lock_guard<std::mutex> lock(mtx_);
                    if (internal_queue_.empty()) break;
                    std::swap(local_internal, internal_queue_);
                }
                while (!local_internal.empty()) {
                    process_internal(std::move(local_internal.front()));
                    local_internal.pop();
                }
            }
            // ==========================================
            // 阶段 B：只取 1 个外部宏任务
            // ==========================================
            std::optional<Event> macro_event;
            {
                std::unique_lock<std::mutex> lock(mtx_);
                cv_.wait(lock, [this]() { return !external_queue_.empty() || !internal_queue_.empty() || !running_; });

                if (!running_) break;
                if (!internal_queue_.empty()) {
                    continue;  // 🚨 防线：定时器刚好塞入了 Tick，立马滚回去排干微队列！
                }
                if (!external_queue_.empty()) {
                    macro_event.emplace(std::move(external_queue_.front()));
                    external_queue_.pop();
                }
            }
            if (macro_event.has_value()) {
                process_internal(std::move(macro_event.value()));
            }
        }
    }
};

// 适配器基类，绑定特定的引擎类型
template <typename Event, typename EngineType>
class BaseAdapter {
   protected:
    std::shared_ptr<EngineType> engine_;

   public:
    BaseAdapter(std::shared_ptr<EngineType> engine) : engine_(engine) {}
};
}  // namespace dk