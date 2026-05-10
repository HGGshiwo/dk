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

// Engine的抽象接口
template <typename Context>
class IEngine : public IAsyncRuntime {
   protected:
    virtual Future<bool> wait_internal(std::function<bool(const std::any&)> predicate, CancellationToken token) = 0;

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

   public:
    virtual ~IEngine() = default;

    virtual Context& get_context() = 0;

    virtual void handle_event(const std::any& event, Context& ctx) = 0;

    virtual void dispatch_internal(std::any e) = 0;

    virtual void step_internal(const StateAction<Context>& action) = 0;

    template <typename StateType, typename... Args>
    void step(Args&&... args) {
        // 利用 StateAction 的静态工厂完成“类型擦除”
        StateAction<Context> action = StateAction<Context>::template step<StateType>(std::forward<Args>(args)...);
        // 调用子类实现的非模板虚函数
        this->step_internal(action);
    }

    // 获取当前的活跃路径 (从根到叶)
    virtual const std::vector<std::shared_ptr<IState<Context>>>& get_active_states() const = 0;

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

template <typename Derived>
struct BaseContext {
    // 存放业务上下文数据
    IEngine<Derived>* engine;
};

// 引入 CRTP 允许外部重写 Engine 的 on_event
template <typename Context, typename DerivedEngine>
class BaseEngine : public IEventHandler<IEngine<Context>, Context, void, DerivedEngine>,
                   public std::enable_shared_from_this<BaseEngine<Context, DerivedEngine>> {
   private:
    using StatePtr = std::shared_ptr<IState<Context>>;
    using EventListenerPtr = std::shared_ptr<IEventListener<Context>>;
    Context ctx_;
    std::vector<StatePtr> active_states_;
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
        std::function<bool(const std::any&)> predicate;
        std::shared_ptr<Promise<bool>> promise;
    };
    // 保存所有正在等待的请求。使用 map 是为了 O(1) 的删除效率
    std::unordered_map<uint64_t, std::shared_ptr<WaitNode>> active_waits_;
    uint64_t next_wait_id_ = 0;

    int step_call_level_ = 0;  // step调用层数
   private:
    // LCA (最近公共祖先) 状态树跳转计算
    void execute_transition(const StateAction<Context>& action) {
        try {
            if (step_call_level_ != 0) {
                throw std::logic_error("FATAL: Cannot change state when a transition is already in progress!");
            }
            step_call_level_++;
            std::vector<StatePtr> new_path;
            // 【修改点 4】：将当前的 active_states_ 传给 Builder
            action.path_builder(new_path, active_states_);
            // 2. 寻找新旧路径的最近公共祖先 (LCA)
            size_t lca_idx = 0;
            size_t min_len = std::min(active_states_.size(), new_path.size());

            // 【修改点 5】：直接使用指针比较，因为如果名字一致，Builder 直接推入了相同的指针
            while (lca_idx < min_len && active_states_[lca_idx] == new_path[lca_idx]) {
                lca_idx++;
            }
            std::string prev_leaf = active_states_.empty() ? "UNKNOWN" : active_states_.back()->name();
            // 3. 从旧叶子节点往上退出，直到 LCA 的子节点
            for (size_t i = active_states_.size(); i > lca_idx; --i) {
                auto s = active_states_[i - 1];
                s->on_exit(ctx_);
                process_internal(std::make_any<ExitEvent>(ExitEvent{s->name()}));
            }
            // 4. 从 LCA 的子节点往下进入，直到新叶子节点
            for (size_t i = lca_idx; i < new_path.size(); ++i) {
                auto s = new_path[i];
                s->parent_ptr = i >= 1 ? new_path[i - 1].get() : nullptr;  // 修改为.get()，因为这里是智能指针
                process_internal(std::make_any<EnterEvent>(EnterEvent{s->name()}));
                s->on_enter(ctx_);
            }
            std::string next_leaf = new_path.empty() ? "UNKNOWN" : new_path.back()->name();
            // 5. 替换当前的活跃路径
            active_states_ = std::move(new_path);
            step_call_level_--;
            // 触发全局状态改变事件
            process_internal(std::make_any<StateChangeEvent>(StateChangeEvent{prev_leaf, next_leaf}));
        } catch (const std::exception& e) {
            std::cout << "[Engine]: transition error: " << e.what() << std::endl;
        }
    }

    void process_internal(const std::any& e) {
        //  std::cout << "[Engine] Processing event of type: " << e.type().name() << std::endl;
        // 1. 触发 Wait 系统
        for (auto it = active_waits_.begin(); it != active_waits_.end();) {
            if (it->second->promise->state() != PromiseState::PENDING) {
                it = active_waits_.erase(it);
            } else if (it->second->predicate(e)) {
                it->second->promise->resolve(true);
                it = active_waits_.erase(it);
            } else {
                ++it;
            }
        }

        // 2. 触发全局无状态监听器
        for (auto& listener : listeners_) {
            listener->handle_event(e, ctx_);
        }

        // 3. 触发引擎自身的拦截 (BaseEngine 继承自 IEventHandler)
        this->handle_event(e, ctx_);

        // 4. HSM 事件冒泡核心：从【叶子节点】向【根节点】逐级探测
        for (auto it = active_states_.rbegin(); it != active_states_.rend(); ++it) {
            // std::cout << "[Engine] Dispatching to state: " << (*it)->name() << std::endl;
            StateAction<Context> action = (*it)->handle_event(e, ctx_);

            if (action.type == StateAction<Context>::Type::TRANSITION) {
                // 如果某个层级决定切换状态，立即执行切换并终止冒泡
                execute_transition(action);
                return;
            } else if (action.type == StateAction<Context>::Type::HANDLED) {
                // 如果某个层级消耗了该事件，终止冒泡
                return;
            }
            // 如果是 UNHANDLED，继续往上冒泡 (给父节点处理的机会)
        }
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
    virtual ~BaseEngine() { stop(); }

    using AllowedEvents = std::tuple<>;

    const std::vector<StatePtr>& get_active_states() const override { return active_states_; }

    Context& get_context() override { return ctx_; }

    boost::asio::io_context& get_ioc() { return io_context_; }

    void add_listener(EventListenerPtr listener) { listeners_.push_back(listener); }

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

    // 为若干个事件注册【一个】监听器，如果return False则继续监听
    Future<bool> wait_internal(std::function<bool(const std::any&)> predicate, CancellationToken token) override {
        auto promise = std::make_shared<Promise<bool>>(this, token);
        Future<bool> future = promise->get_future();
        boost::asio::post(io_context_, [this, predicate = std::move(predicate), promise]() {
            uint64_t id = ++next_wait_id_;
            active_waits_[id] = std::make_shared<WaitNode>(WaitNode{id, std::move(predicate), promise});
        });
        return future;
    }

    // ====== 引擎生命周期 ======
    template <typename RootState, typename... Args>
    void start(std::chrono::milliseconds tick_interval, Args&&... args) {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) return;

        tick_interval_ = tick_interval;
        std::promise<void> init_promise;
        std::future<void> init_future = init_promise.get_future();

        worker_thread_ = std::thread([this, &init_promise](auto... args) {
            try {
                this->ctx_.engine = this;
                event_thread_id_ = std::this_thread::get_id();
                work_guard_.emplace(boost::asio::make_work_guard(io_context_));
                tick_timer_.emplace(io_context_);
                workflow_pool_.emplace(std::make_unique<boost::asio::thread_pool>(4));

                schedule_next_tick();

                // 【核心变化 3】：启动时，利用伪造的 step 跳转到初始状态
                execute_transition(StateAction<Context>::template step<RootState>(std::forward<Args>(args)...));

                init_promise.set_value();
                io_context_.run();
            } catch (...) {
                running_ = false;
                init_promise.set_exception(std::current_exception());
            }
        });
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

    void step_internal(const StateAction<Context>& action) override {
        // 保留原先在 step() 里的线程安全检查
        if (std::this_thread::get_id() != event_thread_id_) {
            throw std::logic_error(
                "FATAL: step() must be called from the event loop thread! "
                "Use dispatch() or dispatch_async() instead.");
        }
        // 执行底层转换
        execute_transition(action);
    }

    void dispatch_internal(std::any e) override {
        // asio::defer 告诉 Asio：这是一个后续任务，请优先在当前调用栈结束后执行！
        boost::asio::defer(io_context_, [this, e = std::move(e)]() mutable { this->process_internal(e); });
    }
};

}  // namespace dk