#pragma once
// #include <algorithm>
#include <atomic>
#include <boost/asio.hpp>
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

#include "./future.hpp"

// ================= dk 框架核心 =================
namespace dk {

// 探测器：检查类型 T 是否包含 on_event(Event, Context)
template <typename T, typename E, typename C, typename = void>
struct has_on_event : std::false_type {};
template <typename T, typename E, typename C>
struct has_on_event<T, E, C, std::void_t<decltype(std::declval<T>().on_event(std::declval<E>(), std::declval<C>()))>>
    : std::true_type {};

// 引入类型萃取黑魔法：用于自动推导 Lambda 的参数类型
template <typename T>
struct lambda_traits : public lambda_traits<decltype(&T::operator())> {};
template <typename ClassType, typename ReturnType, typename ArgType>
struct lambda_traits<ReturnType (ClassType::*)(ArgType) const> {
    using arg_type = std::decay_t<ArgType>;  // 提取出具体的事件类型
};

// 提供给State的抽象接口
template <typename Event, typename Context>
class IEngine {
   public:
    // 临时注册事件回调，直到满足条件或者超时，EventType是具体的事件类型
    // 底层虚函数：接收总的 Variant 事件进行判断
    virtual Future<bool> wait_internal(std::function<bool(const Event&)> predicate, double timeout = 5000) = 0;
    // ================= 用户层魔法 API =================
    // 用户只需要写： engine->wait([](const SpecificEvent& e) { return e.id == 1; });
    template <typename F>
    Future<bool> wait(F&& predicate, double timeout = 5000) {
        // 1. 自动推导用户 Lambda 接收的是哪个具体的事件类型（比如 TickEvent）
        using SpecificEvent = typename lambda_traits<std::decay_t<F>>::arg_type;
        // 2. 包装用户的 predicate，抹平多态差异
        auto wrapper = [pred = std::forward<F>(predicate)](const Event& variant_event) -> bool {
            // 3. 核心：如果来的事件根本就不是我们等的那种事件，直接忽略！
            if (const auto* specific_event = std::get_if<SpecificEvent>(&variant_event)) {
                return pred(*specific_event);  // 类型匹配，执行业务判断
            }
            return false;  // 类型不匹配，当做条件不成立，继续在队列里等下一个事件
        };
        // 4. 将包装后的方法注册到底层
        return wait_internal(wrapper, timeout);
    }

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
template <typename Event, typename Context, typename DerivedEngine>
class BaseEngine : public std::enable_shared_from_this<BaseEngine<Event, Context, DerivedEngine>>,
                   public IEventHandler<IEngine<Event, Context>, Event, Context, void, DerivedEngine>,
                   public IAsyncRuntime {
    using StateType = IState<Event, Context>;
    using EventListenerPtr = std::shared_ptr<IEventListener<Event, Context>>;

    std::shared_ptr<StateType> state_;  // 当前的状态
    Context ctx_;

    std::atomic<bool> running_{false};
    std::vector<EventListenerPtr> listeners_;  // 和状态无关的监听器

    // 引入 Asio 的绝对核心：I/O 上下文
    boost::asio::io_context io_context_;
    std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_guard_;
    std::optional<boost::asio::steady_timer> tick_timer_;
    std::chrono::milliseconds tick_interval_;
    std::thread worker_thread_;  // 只需要这一个线程跑 io_context

    std::optional<std::unique_ptr<boost::asio::thread_pool>> workflow_pool_;
    std::thread::id event_thread_id_;  // 事件循环的thread id, 用于检测wait

    struct WaitNode {
        uint64_t id;
        std::function<bool(const Event&)> predicate;
        std::shared_ptr<Promise<bool>> promise;
        std::shared_ptr<boost::asio::steady_timer> timer;
    };
    // 保存所有正在等待的请求。使用 map 是为了 O(1) 的删除效率
    std::unordered_map<uint64_t, std::shared_ptr<WaitNode>> active_waits_;
    uint64_t next_wait_id_ = 0;

   public:
    virtual ~BaseEngine() { stop(); }
    Context& get_context() { return ctx_; }

    void add_listener(EventListenerPtr listener) { listeners_.push_back(listener); }

    std::thread::id get_thread_id() override { return event_thread_id_; }

    // 实现promise runtime
    void post_future_task(std::function<void()> task) { boost::asio::post(io_context_, std::move(task)); }

    // 实现promise runtime
    std::function<void()> set_future_timeout(uint32_t ms, std::function<void()> on_timeout) {
        auto timer = std::make_shared<boost::asio::steady_timer>(io_context_);
        timer->expires_after(std::chrono::milliseconds(ms));

        timer->async_wait([on_timeout](const boost::system::error_code& ec) {
            if (!ec) on_timeout();
        });
        // 返回取消句柄
        return [timer]() { timer->cancel(); };
    }

    // 线程安全的外部事件注入接口
    void dispatch(Event e) {
        boost::asio::post(io_context_, [this, e = std::move(e)]() mutable { this->process_internal(e); });
    }

    // 提交一个后台任务，在线程池而不是事件循环中进行
    template <typename ReturnType>
    Future<ReturnType> post_background_task(std::function<ReturnType()> task) {
        std::shared_ptr<Promise<ReturnType>> promise = std::make_shared<Promise<ReturnType>>(this);
        boost::asio::post(this->workflow_pool_.value(), [task, engine = this, promise]() {
            // --- 此时运行在【后台计算线程】，你可以随便算几秒，绝对不卡主线程 ---
            ReturnType data = task();
            // --- 算完啦！重点来了：必须安全地把结果送回主线程！ ---
            // 绝对不能在这里直接调 e.resolve() 或者修改 ctx，因为会破坏单线程无锁设计！
            // 利用引擎的主 io_context，把完成的事件 post 回去
            boost::asio::post(engine->io_context_, [data, promise]() {
                // --- 此时又回到了【主事件循环线程】！ ---
                // 极其安全地完成 Promise
                promise->resolve(data);
            });
        });
        return promise->get_futrue();
    }

    // 2. 异步派发：【在塞入队列前，给 Event 注入 Promise！】
    template <typename E>
    Future<typename E::ReturnType> dispatch_async(E e, uint32_t timeout_ms = 5000) {
        static_assert(std::is_base_of_v<AsyncEvent<typename E::ReturnType>, E>,
                      "Event must inherit from dk::AsyncEvent to use dispatch_async!");
        using R = typename E::ReturnType;
        auto promise = std::make_shared<Promise<R>>(this);
        Future<R> future = promise->get_future();

        // 【关键注入】：赋予这个普通事件以异步返回的超能力
        e.promise_ = promise;
        if (timeout_ms > 0) {
            future = future.timeout(timeout_ms);
        }
        boost::asio::post(io_context_, [this, e = std::move(e)]() mutable { this->process_internal(e); });

        return future;
    }

    Future<bool> wait_internal(std::function<bool(const Event&)> predicate, double timeout = 5000) override {
        auto promise = std::make_shared<Promise<bool>>(this);
        Future<bool> future = promise->get_future();
        // 将注册动作投入无锁的主循环
        boost::asio::post(io_context_, [this, predicate = std::move(predicate), promise, timeout]() {
            uint64_t id = ++next_wait_id_;
            auto node = std::make_shared<WaitNode>();
            node->id = id;
            node->predicate = std::move(predicate);
            node->promise = promise;

            node->timer = std::make_shared<boost::asio::steady_timer>(io_context_);
            node->timer->expires_after(std::chrono::milliseconds(static_cast<long long>(timeout)));

            node->timer->async_wait([this, id](const boost::system::error_code& ec) {
                if (ec == boost::asio::error::operation_aborted) return;

                auto it = active_waits_.find(id);
                if (it != active_waits_.end()) {
                    auto p = it->second->promise;
                    active_waits_.erase(it);
                    p->resolve(false);  // 超时未等到
                }
            });
            active_waits_[id] = node;
        });
        return future;
    }

    void start(std::shared_ptr<IState<Event, Context>> initial_state, std::chrono::milliseconds tick_interval) {
        if (running_) return;

        ctx_.engine = this;

        work_guard_.emplace(boost::asio::make_work_guard(io_context_));
        tick_timer_.emplace(io_context_);

        tick_interval_ = tick_interval;
        schedule_next_tick();

        workflow_pool_.emplace(std::make_unique<boost::asio::thread_pool>(4));
        transition(initial_state);
        running_ = true;

        worker_thread_ = std::thread([this]() {
            try {
                io_context_.run();
            } catch (const std::exception& e) {
                std::cerr << "io_context thread crashed: " << e.what() << std::endl;
            }
        });
        event_thread_id_ = worker_thread_.get_id();
    }

    void stop() {
        if (!running_) return;

        work_guard_.reset();  // 允许 io_context 退出
        io_context_.stop();   // 立刻停止

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

    void dispatch_internal(Event e) override {
        // asio::defer 告诉 Asio：这是一个后续任务，请优先在当前调用栈结束后执行！
        boost::asio::defer(io_context_, [this, e = std::move(e)]() mutable { this->process_internal(e); });
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
        // 遍历所有等待者，让它们判断当前事件是否符合条件
        // 注意：使用 C++11 的 erase 方式，防止迭代器失效
        for (auto it = active_waits_.begin(); it != active_waits_.end();) {
            // 传入 e 进行嗅探
            if (it->second->predicate(e)) {
                // 条件满足！
                it->second->timer->cancel();         // 取消超时定时器
                it->second->promise->resolve(true);  // 成功触发，返回 true
                it = active_waits_.erase(it);        // 从队列中移除（不需要再次排队了）
            } else {
                // 如果返回 false，什么都不做，迭代器前进（继续在队列中排队）
                ++it;
            }
        }

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

    void schedule_next_tick() {
        if (!running_) return;
        tick_timer_.value().expires_after(tick_interval_);
        tick_timer_.value().async_wait([this](const boost::system::error_code& ec) {
            if (!ec && running_) {
                // 触发 Tick 业务
                this->process_internal(Event{dk::TickEvent{}});
                // 循环排队下一次
                this->schedule_next_tick();
            }
        });
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