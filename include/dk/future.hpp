#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <variant>

#include "./exception.hpp"
#include "./utils.hpp"

namespace dk {
// ============================================================================
// 2. 抽象调度器定义
// ============================================================================
// 投递到微队列的方法: 接收一个无参函数，放入队列执行
using Dispatcher = std::function<void(std::function<void()>)>;

// 超时调度器: 接收(毫秒数, 回调函数)，返回一个可以用来【取消定时器】的函数
using TimeoutScheduler = std::function<std::function<void()>(uint32_t, std::function<void()>)>;
// ============================================================================
// 1. 基础组件：异常与取消令牌
// ============================================================================
struct CancelledException : public std::runtime_error {
    CancelledException() : std::runtime_error("Future chain cancelled!") {}
};
struct TimeoutException : public std::runtime_error {
    TimeoutException() : std::runtime_error("Future task timeout!") {}
};

// 如果有人在then中注册了void函数，则使用这个假的类型代替返回类型void
struct Unit {
    // 随便加个比较运算符，方便以后可能用得上
    bool operator==(const Unit&) const { return true; }
};

class IAsyncRuntime {
   public:
    virtual ~IAsyncRuntime() = default;
    // 强制子类必须实现：投递到微队列
    virtual void post_future_task(std::function<void()> task) = 0;
    // 强制子类必须实现：设置定时器并返回取消句柄
    virtual std::function<void()> set_future_timeout(uint32_t ms, std::function<void()> on_timeout) = 0;
    // 获取事件循环线程的id(这里假设只有一个事件循环线程)
    virtual std::thread::id get_thread_id() = 0;
};

class CancellationToken {
    std::shared_ptr<std::atomic<bool>> cancelled_;

   public:
    CancellationToken() = default;
    CancellationToken(std::shared_ptr<std::atomic<bool>> flag) : cancelled_(flag) {}
    bool is_cancelled() const { return cancelled_ && cancelled_->load(std::memory_order_relaxed); }
};

/* 不需要强制保留指针，非常安全 */
class CancellationTokenSource {
    std::shared_ptr<std::atomic<bool>> cancelled_;

   public:
    CancellationTokenSource() : cancelled_(std::make_shared<std::atomic<bool>>(false)) {}
    CancellationToken get_token() const { return CancellationToken(cancelled_); }
    void cancel() {
        if (cancelled_) cancelled_->store(true, std::memory_order_relaxed);
    }

    // ⭐ 新增：经过指定时间后自动触发 Cancel
    void cancel_after(uint32_t ms, TimeoutScheduler timeout_scheduler) {
        if (!timeout_scheduler) return;

        // 注意：这里需要捕获自身的弱引用或 shared_ptr 来确保生命周期安全
        timeout_scheduler(ms, [weak_flag = std::weak_ptr<std::atomic<bool>>(cancelled_)]() {
            if (auto flag = weak_flag.lock()) {
                flag->store(true, std::memory_order_relaxed);
            }
        });
    }

    void cancel_after(uint32_t ms, IAsyncRuntime* rt) {
        cancel_after(ms,
                     [rt](uint32_t m, std::function<void()> cb) { return rt->set_future_timeout(m, std::move(cb)); });
    }
};

// ============================================================================
// 3. 前置声明与类型萃取
// ============================================================================
template <typename T>
class Future;
template <typename T>
class Promise;

template <typename T>
struct is_future : std::false_type {};
template <typename T>
struct is_future<Future<T>> : std::true_type {};

// ============================================================================
// 4. 共享状态核心
// ============================================================================
enum class PromiseState { PENDING, FULFILLED, REJECTED };

template <typename T>
struct SharedState {
    std::mutex mtx;
    std::condition_variable cv;
    std::thread::id dispatcher_thread_id;
    PromiseState state = PromiseState::PENDING;
    std::optional<T> value;
    std::exception_ptr error;
    std::optional<CancellationToken> token;

    std::function<void(T)> on_resolve;
    std::function<void(std::exception_ptr)> on_reject;

    Dispatcher dispatcher;
    TimeoutScheduler timeout_scheduler;  // 可选的超时调度器
};

// ============================================================================
// 5. Future 类 (消费者)
// ============================================================================

namespace detail {
// 默认情况：不是 Future，直接返回 T 本身
template <typename T, bool IsFuture>
struct unwrap_future_type {
    using type = T;
};
// 特化情况：当 IsFuture 为 true 时，安全地提取内部的 value_type
// 编译器只有在匹配到这个特化时，才会去计算 T::value_type
template <typename T>
struct unwrap_future_type<T, true> {
    using type = typename T::value_type;
};
}  // namespace detail

template <typename T>
class Future {
    std::shared_ptr<SharedState<T>> state_;

   public:
    using value_type = T;
    explicit Future(std::shared_ptr<SharedState<T>> state) : state_(std::move(state)) {}

    // ========================================================================
    // 独立操作符：超时控制，给当前的future添加而不是下一个then
    // ========================================================================
    Future<T> timeout(uint32_t timeout_ms) {
        if (timeout_ms == 0 || !state_->timeout_scheduler) {
            return Future<T>(state_);  // 无超时，直接返回自身
        }

        auto next_promise = std::make_shared<Promise<T>>(state_->dispatcher, state_->timeout_scheduler);
        auto next_future = next_promise->get_future();

        // 设置定时器
        auto cancel_timer = state_->timeout_scheduler(
            timeout_ms, [next_promise]() { next_promise->reject(std::make_exception_ptr(TimeoutException())); });

        // 注册回调：如果原始 Future 先完成，则取消定时器并透传结果
        std::function<void(T)> wrapped_resolve = [next_promise, cancel_timer](T val) {
            if (cancel_timer) cancel_timer();
            next_promise->resolve(std::move(val));
        };

        std::function<void(std::exception_ptr)> wrapped_reject = [next_promise, cancel_timer](std::exception_ptr e) {
            if (cancel_timer) cancel_timer();
            next_promise->reject(e);
        };

        register_callbacks(std::move(wrapped_resolve), std::move(wrapped_reject));

        return next_future;
    }

    // ========================================================================
    // 独立操作符：取消预检 (代理 Future)
    // ========================================================================
    // ========================================================================
    // 独立操作符：挂载/替换 取消令牌
    // ========================================================================
    Future<T> with_cancellation(CancellationToken new_token) {
        // ⭐ 注意看这里：第三个参数不再是 state_->token，而是 new_token！
        // 这意味着从这里开始，下游所有的 future 都会遗传这个 new_token
        auto next_promise = std::make_shared<Promise<T>>(state_->dispatcher, state_->timeout_scheduler, new_token);
        auto next_future = next_promise->get_future();
        std::function<void(T)> wrapped_resolve = [next_promise, new_token](T val) {
            if (new_token.is_cancelled()) {
                next_promise->reject(std::make_exception_ptr(CancelledException()));
            } else {
                next_promise->resolve(std::move(val));
            }
        };
        std::function<void(std::exception_ptr)> wrapped_reject = [next_promise, new_token](std::exception_ptr e) {
            if (new_token.is_cancelled()) {
                next_promise->reject(std::make_exception_ptr(CancelledException()));
            } else {
                next_promise->reject(e);
            }
        };
        register_callbacks(std::move(wrapped_resolve), std::move(wrapped_reject));
        return next_future;
    }

    // 异常捕获方法
    template <typename CatchFunc>
    Future<T> catch_error(CatchFunc&& catch_cb) {
        auto next_promise = std::make_shared<Promise<T>>(state_->dispatcher, state_->timeout_scheduler);
        auto next_future = next_promise->get_future();

        std::function<void(T)> wrapped_resolve = [next_promise](T val) {
            next_promise->resolve(std::move(val));  // 正常流直接透传
        };

        std::function<void(std::exception_ptr)> wrapped_reject = [catch_cb = std::forward<CatchFunc>(catch_cb),
                                                                  next_promise](std::exception_ptr e) mutable {
            try {
                catch_cb(e);  // 让用户处理异常
                // 注意：这里为了简化，强行 reject 到底。如果想恢复，设计会更复杂。
                next_promise->reject(e);
            } catch (...) {
                next_promise->reject(std::current_exception());
            }
        };

        register_callbacks(std::move(wrapped_resolve), std::move(wrapped_reject));
        return next_future;
    }

    /*
        支持四种入参：0个参数，1个参数：上一个Future的返回值 or CacellationToken，
        2个参数：上一个Future的返回值 + CancellationToken
    */
    template <typename Func>
    auto then(Func&& cb) {
        // =====================================================================
        // 1. 编译期无损探测 (Probing)
        // 利用 C++17 的 is_invocable_v，完美避开泛型 Lambda 和重载函数的解析问题
        // =====================================================================
        static constexpr bool takes_both = std::is_invocable_v<Func, T, dk::CancellationToken>;
        static constexpr bool takes_t = std::is_invocable_v<Func, T>;
        static constexpr bool takes_token = std::is_invocable_v<Func, dk::CancellationToken>;
        static constexpr bool takes_none = std::is_invocable_v<Func>;
        static_assert(takes_both || takes_t || takes_token || takes_none,
                      "Callback signature is not supported! Must accept (T, Token), (T), (Token), or ().");
        // =====================================================================
        // 2. 惰性求值推导返回值类型 (Lazy Evaluation)
        // ⚠️ 极其关键：必须使用没有 _t 的 std::invoke_result，配合 std::conditional_t
        // 只有最终胜出的分支才会被 ::type，未胜出的分支即使参数不匹配也不会触发硬报错 (SFINAE 安全)
        // =====================================================================
        using RawRetTrait = std::conditional_t<
            takes_both, std::invoke_result<Func, T, dk::CancellationToken>,
            std::conditional_t<takes_t, std::invoke_result<Func, T>,
                               std::conditional_t<takes_token, std::invoke_result<Func, dk::CancellationToken>,
                                                  std::invoke_result<Func>>>>;

        // 安全提取返回值类型
        using RawRetType = typename RawRetTrait::type;
        // 3. 剥离可能的 Future 嵌套
        constexpr bool returns_future = is_future<RawRetType>::value;
        using InnerType = typename detail::unwrap_future_type<RawRetType, returns_future>::type;

        // 4. 定义安全的底层类型 (将 void 映射为 Unit)
        using SafeInnerType = std::conditional_t<std::is_void_v<InnerType>, Unit, InnerType>;

        // 创建下一个 Promise
        auto next_promise =
            std::make_shared<Promise<SafeInnerType>>(state_->dispatcher, state_->timeout_scheduler, state_->token);
        auto next_future = next_promise->get_future();
        // 5. 包装业务 Callback
        std::function<void(T)> wrapped_cb = [cb = std::forward<Func>(cb), next_promise,
                                             current_state = this->state_](T val) mutable {
            try {
                if (current_state->token && current_state->token->is_cancelled()) {
                    throw CancelledException();
                }
                // 🌟 完美的分发执行器
                auto execute_cb = [&]() -> decltype(auto) {
                    // 按照优先级尝试调用 (优先全都要)
                    if constexpr (takes_both) {
                        return cb(std::move(val), current_state->token.value_or(dk::CancellationToken{}));
                    } else if constexpr (takes_t) {
                        return cb(std::move(val));
                    } else if constexpr (takes_token) {
                        return cb(current_state->token.value_or(dk::CancellationToken{}));
                    } else if constexpr (takes_none) {
                        return cb();
                    }
                };
                std::shared_ptr<Promise<SafeInnerType>> local_p = next_promise;
                auto on_inner_error = [local_p](std::exception_ptr e) { local_p->reject(e); };
                if constexpr (returns_future) {
                    auto inner_future = execute_cb();
                    if constexpr (std::is_void_v<InnerType>) {
                        // 既然是 void，那就什么参数都不要接
                        auto on_inner_void = [local_p]() {
                            local_p->resolve(Unit{});
                            return Unit{};
                        };
                        inner_future.then(on_inner_void).catch_error(on_inner_error);
                    } else {
                        // 既然是有返回值，就严格要求只能传入 InnerType
                        auto on_inner_val = [local_p](InnerType inner_val) {
                            local_p->resolve(std::move(inner_val));
                            return inner_val;  // dummy return
                        };
                        inner_future.then(on_inner_val).catch_error(on_inner_error);
                    }
                } else {
                    if constexpr (std::is_void_v<RawRetType>) {
                        execute_cb();
                        local_p->resolve(Unit{});
                    } else {
                        auto res = execute_cb();
                        local_p->resolve(std::move(res));
                    }
                }
            } catch (...) {
                next_promise->reject(std::current_exception());
            }
        };
        std::function<void(std::exception_ptr)> wrapped_reject = [next_promise](std::exception_ptr e) {
            next_promise->reject(e);
        };
        register_callbacks(std::move(wrapped_cb), std::move(wrapped_reject));

        return next_future;
    }

    // ========================================================================
    // 独立操作符：Finally (无论成功或失败都会执行，用于清理/善后)
    // ========================================================================
    template <typename Func>
    Future<T> finally(Func cb) {
        // 1. 探测 cb 返回值类型（允许 finally 回调返回 Future 来实现异步等待清理）
        using RawRetType = std::invoke_result_t<Func>;
        constexpr bool returns_future = is_future<RawRetType>::value;
        using InnerType = typename detail::unwrap_future_type<RawRetType, returns_future>::type;
        auto next_promise = std::make_shared<Promise<T>>(state_->dispatcher, state_->timeout_scheduler);
        auto next_future = next_promise->get_future();
        // 2. 成功分支包装
        std::function<void(T)> wrapped_resolve = [cb, next_promise](T val) mutable {
            try {
                if constexpr (returns_future) {
                    auto res_future = cb();
                    // 核心技巧：使用 shared_ptr 包装 val，解决 T 可能是 Move-Only
                    // 且 std::function 要求 lambda 必须 CopyConstructible 的问题
                    auto val_ptr = std::make_shared<T>(std::move(val));

                    res_future
                        .then([next_promise, val_ptr](InnerType /* ignore */) {
                            // 清理完成，继续透传原来的值
                            next_promise->resolve(std::move(*val_ptr));
                            return 0;  // Dummy return，类型无所谓
                        })
                        .catch_error([next_promise](std::exception_ptr e) {
                            // 如果 finally 抛出新异常，则覆盖原结果
                            next_promise->reject(e);
                        });
                } else {
                    cb();                                   // 同步执行清理
                    next_promise->resolve(std::move(val));  // 继续透传原来的值
                }
            } catch (...) {
                next_promise->reject(std::current_exception());  // 同步抛错，覆盖原结果
            }
        };
        // 3. 失败分支包装
        std::function<void(std::exception_ptr)> wrapped_reject = [cb,
                                                                  next_promise](std::exception_ptr original_e) mutable {
            try {
                if constexpr (returns_future) {
                    auto res_future = cb();
                    res_future
                        .then([next_promise, original_e](InnerType /* ignore */) {
                            // 清理完成后，继续抛出原来的异常
                            next_promise->reject(original_e);
                            return 0;
                        })
                        .catch_error([next_promise](std::exception_ptr new_e) {
                            // 如果 finally 抛出新异常，新异常覆盖旧异常
                            next_promise->reject(new_e);
                        });
                } else {
                    cb();
                    next_promise->reject(original_e);  // 继续抛出原来的异常
                }
            } catch (...) {
                next_promise->reject(std::current_exception());  // 同步抛错，覆盖旧异常
            }
        };
        register_callbacks(std::move(wrapped_resolve), std::move(wrapped_reject));
        return next_future;
    }

    // ========================================================================
    // 同步阻塞方法
    // ========================================================================

    // 阻塞当前线程，直到 Future 完成 (Fulfilled 或 Rejected)
    void wait() const {
        std::unique_lock<std::mutex> lock(state_->mtx);  // 1. 先加锁，保护读取操作
        // 2. 只有当任务还没有完成时，在 dispatcher 线程里 wait 才会有死锁风险！
        // 如果已经完成（非 PENDING），直接让它顺畅通过即可。
        if (state_->state == PromiseState::PENDING) {
            if (state_->dispatcher_thread_id == std::this_thread::get_id()) {
                throw std::logic_error(
                    "FATAL: Do not call get() or wait() inside the async event loop thread! It will cause a deadlock.");
            }
        }
        state_->cv.wait(lock, [this]() { return state_->state != PromiseState::PENDING; });
    }

    // 阻塞等待指定时间（chrono 接口）
    template <class Rep, class Period>
    PromiseState wait_for(const std::chrono::duration<Rep, Period>& timeout_duration) const {
        std::unique_lock<std::mutex> lock(state_->mtx);
        state_->cv.wait_for(lock, timeout_duration, [this]() { return state_->state != PromiseState::PENDING; });
        return state_->state;
    }

    // 阻塞等待指定毫秒数（快捷接口）
    PromiseState wait_for(uint32_t timeout_ms) const { return wait_for(std::chrono::milliseconds(timeout_ms)); }

    // 阻塞等待并获取结果，如果有异常则重新抛出
    T get() {
        wait();  // 复用上面的 wait()

        // wait 结束后，状态必定不是 PENDING，此时无需再加锁
        if (state_->state == PromiseState::FULFILLED) {
            // 注意：这里使用 std::move，意味着 get() 只能调用一次，
            // 且后续 .then() 可能无法获取原值（与 std::future 行为保持一致）
            return std::move(state_->value.value());
        } else {
            // 将内部的 exception_ptr 重新在当前线程抛出
            std::rethrow_exception(state_->error);
        }
    }

   private:
    void register_callbacks(std::function<void(T)> on_res, std::function<void(std::exception_ptr)> on_rej) {
        std::unique_lock<std::mutex> lock(state_->mtx);
        if (state_->state == PromiseState::PENDING) {
            state_->on_resolve = std::move(on_res);
            state_->on_reject = std::move(on_rej);
        } else if (state_->state == PromiseState::FULFILLED) {
            T val = state_->value.value();
            state_->dispatcher([cb = std::move(on_res), val]() { cb(val); });
        } else {
            std::exception_ptr e = state_->error;
            state_->dispatcher([cb = std::move(on_rej), e]() { cb(e); });
        }
    }
};

// ============================================================================
// 6. Promise 类 (生产者)
// ============================================================================
template <typename T>
class Promise {
    std::shared_ptr<SharedState<T>> state_;

   public:
    // 1. 禁用拷贝语义（彻底阻断隐式拷贝引发的血案）
    Promise(const Promise&) = delete;
    Promise& operator=(const Promise&) = delete;

    // 2. 启用移动语义（转移所有权）
    Promise(Promise&&) = default;
    Promise& operator=(Promise&&) = default;

    // 构造注入 Dispatcher
    Promise(Dispatcher disp, TimeoutScheduler timeout_disp = nullptr,
            std::optional<CancellationToken> token = std::nullopt)
        : state_(std::make_shared<SharedState<T>>()) {
        state_->dispatcher = std::move(disp);
        state_->timeout_scheduler = std::move(timeout_disp);
        state_->token = std::move(token);
    }

    ~Promise() {
        if (!state_) {
            return;
        }
        // 需要加锁或者检查内部状态
        bool is_pending = false;
        {
            std::unique_lock<std::mutex> lock(state_->mtx);
            is_pending = (state_->state == PromiseState::PENDING);
        }

        if (is_pending) {
            reject(std::make_exception_ptr(
                TraceableException("Broken Promise: Event was dropped without being resolved or rejected.")));
        }
    }

    PromiseState state() { return state_->state; }

    explicit Promise(std::shared_ptr<IAsyncRuntime> rt, std::optional<CancellationToken> token = std::nullopt)
        : state_(std::make_shared<SharedState<T>>()) {
        // 捕获 weak_ptr，防止循环引用，同时保证调用时检查存活状态
        std::weak_ptr<IAsyncRuntime> weak_rt = rt;
        state_->dispatcher = [weak_rt](std::function<void()> task) {
            if (auto strong_rt = weak_rt.lock()) {  // 调用前检查 rt 是否还活着
                strong_rt->post_future_task(std::move(task));
            }
        };
        state_->timeout_scheduler = [weak_rt](uint32_t ms, std::function<void()> on_timeout) -> std::function<void()> {
            if (auto strong_rt = weak_rt.lock()) {
                return strong_rt->set_future_timeout(ms, std::move(on_timeout));
            }
            return []() {};  // 或者返回一个表示失败的 timer_id
        };
        state_->token = std::move(token);
    }

    Future<T> get_future() { return Future<T>(state_); }

    static Future<T> resolve(IAsyncRuntime* rt, T val, std::optional<CancellationToken> token = std::nullopt) {
        auto p = Promise<T>(rt, token);
        p.resolve(val);
        return p.get_future();
    }

    static Future<T> resolve(std::shared_ptr<IAsyncRuntime> rt, T val,
                             std::optional<CancellationToken> token = std::nullopt) {
        auto p = Promise<T>(rt, token);
        p.resolve(val);
        return p.get_future();
    }

    static Future<T> reject(IAsyncRuntime* rt, T val, std::optional<CancellationToken> token = std::nullopt) {
        auto p = Promise<T>(rt, token);
        p.reject(val);
        return p.get_future();
    }

    void resolve(T val) {
        std::function<void(T)> cb;
        {
            std::unique_lock<std::mutex> lock(state_->mtx);
            if (state_->state != PromiseState::PENDING) return;
            state_->state = PromiseState::FULFILLED;
            state_->value = val;
            cb = std::move(state_->on_resolve);
            state_->on_reject = nullptr;
            state_->on_resolve = nullptr;
        }
        state_->cv.notify_all();
        if (!state_->dispatcher) {
            throw std::runtime_error("ERROR: dispatcher is null in reject!");
        } else {
            if (cb) state_->dispatcher([cb, val]() { cb(val); });
        }
    }

    void reject(std::string error) { reject(std::make_exception_ptr(std::runtime_error(error))); }

    void reject(std::exception_ptr e) {
        std::function<void(std::exception_ptr)> cb;
        {
            std::unique_lock<std::mutex> lock(state_->mtx);
            if (state_->state != PromiseState::PENDING) return;
            state_->state = PromiseState::REJECTED;
            state_->error = e;
            cb = std::move(state_->on_reject);
            state_->on_reject = nullptr;
            state_->on_resolve = nullptr;
        }
        state_->cv.notify_all();
        if (!state_->dispatcher) {
            throw std::runtime_error("ERROR: dispatcher is null in reject!");
        } else {
            if (cb) state_->dispatcher([cb, e]() { cb(e); });
        }
    }
};
}  // namespace dk