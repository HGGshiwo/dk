#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <nlohmann/json.hpp>
#include <shared_mutex>
#include <string>
#include <type_traits>  // 必须包含这个
#include <vector>

namespace dk {

// ==========================================
// 1. C++17 SFINAE: 检查类型 T 是否支持 == 运算符
// ==========================================
template <typename T, typename = void>
struct is_equality_comparable : std::false_type {};

template <typename T>
struct is_equality_comparable<T, std::void_t<decltype(std::declval<T>() == std::declval<T>())>> : std::true_type {};

template <typename T>
inline constexpr bool is_equality_comparable_v = is_equality_comparable<T>::value;

class RateLimiter {
   public:
    explicit RateLimiter(double hz) : last_time_(std::chrono::steady_clock::now()), hz_(hz) {
        if (hz > 0) std::chrono::duration<double>(1.0 / hz);
    }

    bool check_and_update() {
        auto now = std::chrono::steady_clock::now();
        if (hz_ <= 0) return false;
        if (now - last_time_ >= interval_) {
            last_time_ = now;
            return true;
        }
        return false;
    }

   private:
    double hz_;
    std::chrono::duration<double> interval_;
    std::chrono::steady_clock::time_point last_time_;
};

// 抽象上报接口，供 Registry 统一调用
class IReportable {
   public:
    virtual ~IReportable() = default;
    // 尝试增量上报（脏位+限频）
    virtual void try_report(nlohmann::json& j) = 0;

    // 无视状态，强制写入当前完整数据
    virtual void append_full_state(nlohmann::json& j) const = 0;

    // 强制标记为脏
    virtual void mark_dirty() = 0;
};

// 状态注册表：自动收集所有需要上报的变量
class StateRegistry {
   private:
    std::vector<IReportable*> reportables_;

   public:
    void register_var(IReportable* var) { reportables_.push_back(var); }

    // 遍历所有注册的变量，尝试触发上报
    void report_all(nlohmann::json& j) {
        for (auto* var : reportables_) {
            var->try_report(j);
        }
    }

    // 获取当前所有数据的全量快照
    nlohmann::json get_full_state() const {
        nlohmann::json j;
        for (auto* var : reportables_) {
            var->append_full_state(j);
        }
        return j;
    }

    // 强制把所有变量标脏，下一次 tick 会引发全量广播
    void mark_all_dirty() {
        for (auto* var : reportables_) {
            var->mark_dirty();
        }
    }
};

template <typename T>
class ThreadVar {
   private:
    T data_;
    mutable std::shared_mutex mtx_;

   public:
    // 构造1：标准 JSON 键值对上报
    ThreadVar(T init_val = T{}) : data_(std::move(init_val)) {}

    template <typename U>
    ThreadVar& operator=(U&& value) {
        data_ = std::forward<U>(value);
        return *this;
    }

    template <typename Func>
    decltype(auto) write(Func&& func) {
        static_assert(std::is_invocable_v<Func, T&>, "write callback error: Argument must be T&!");
        static_assert(!std::is_invocable_v<Func, T>, "write callback error: Argument must be T& or auto&!");
        // 定义一个 RAII 结构体，在函数结束（无论正常返回还是抛异常）时设置 dirty_
        // 直接返回。如果 func 返回 void，decltype(auto) 也能推导为 void 并合法执行
        return [&]() -> decltype(auto) {
            std::unique_lock<std::shared_mutex> lock(mtx_);
            return std::forward<Func>(func)(data_);
        }();
    }

    template <typename Func>
    decltype(auto) read(Func&& func) const {
        static_assert(std::is_invocable_v<Func, const T&>, "read callback error: Argument must be const T&!");
        std::shared_lock<std::shared_mutex> lock(mtx_);
        return std::forward<Func>(func)(data_);
    }

    T get() const {
        std::shared_lock<std::shared_mutex> lock(mtx_);
        return data_;
    }

    void set(T data) {
        {
            std::unique_lock<std::shared_mutex> lock(mtx_);
            data_ = std::move(data);
        }
    }
};

// 全新升级的 TrackedVar：继承自 thread_safe 的思想，增加脏写和自动注册
template <typename T>
class TrackedVar : public IReportable {
   private:
    T data_;
    mutable std::shared_mutex mtx_;
    std::atomic<bool> dirty_{true};  // 初始默认为脏，保证第一次总能上报
    RateLimiter rate_;

    std::string key_;
    std::function<void(nlohmann::json&, const T&)> custom_serializer_;

   public:
    // 构造1：标准 JSON 键值对上报
    TrackedVar(StateRegistry& reg, std::string key, double hz, T init_val = T{})
        : data_(std::move(init_val)), rate_(hz), key_(std::move(key)) {
        reg.register_var(this);
    }

    // 构造2：自定义序列化（适用于 Vector3d 拆分成 lat, lon 等情况）
    TrackedVar(StateRegistry& reg, double hz, T init_val, std::function<void(nlohmann::json&, const T&)> serializer)
        : data_(std::move(init_val)), rate_(hz), custom_serializer_(std::move(serializer)) {
        reg.register_var(this);
    }

    // 无视脏位强制写入
    void append_full_state(nlohmann::json& j) const override {
        std::shared_lock<std::shared_mutex> lock(mtx_);
        if (custom_serializer_) {
            custom_serializer_(j, data_);
        } else {
            j[key_] = data_;
        }
    }

    // 强行标脏
    void mark_dirty() override { dirty_.store(true, std::memory_order_release); }

    template <typename Func>
    decltype(auto) write(Func&& func) {
        static_assert(std::is_invocable_v<Func, T&>, "write callback error: Argument must be T&!");
        static_assert(!std::is_invocable_v<Func, T>, "write callback error: Argument must be T& or auto&!");
        // 定义一个 RAII 结构体，在函数结束（无论正常返回还是抛异常）时设置 dirty_
        struct ScopeExit {
            std::atomic<bool>& dirty;
            ~ScopeExit() { dirty.store(true, std::memory_order_release); }
        } setter{dirty_};
        // 直接返回。如果 func 返回 void，decltype(auto) 也能推导为 void 并合法执行
        return [&]() -> decltype(auto) {
            std::unique_lock<std::shared_mutex> lock(mtx_);
            return std::forward<Func>(func)(data_);
        }();
    }

    template <typename Func>
    decltype(auto) read(Func&& func) const {
        static_assert(std::is_invocable_v<Func, const T&>, "read callback error: Argument must be const T&!");
        std::shared_lock<std::shared_mutex> lock(mtx_);
        return std::forward<Func>(func)(data_);
    }

    T get() const {
        std::shared_lock<std::shared_mutex> lock(mtx_);
        return data_;
    }

    void set(T data) {
        {
            std::unique_lock<std::shared_mutex> lock(mtx_);
            // C++17 写法：使用自定义的 Type Trait 判断是否支持 == 比较
            if constexpr (is_equality_comparable_v<T>) {
                if (data_ == data) return;  // 值未变，不触发脏写
            }
            data_ = std::move(data);
        }
        dirty_.store(true, std::memory_order_release);
    }

    // 实现 IReportable 接口：由 Registry 统一调用
    void try_report(nlohmann::json& j) override {
        if (!dirty_.load(std::memory_order_acquire)) return;
        if (!rate_.check_and_update()) return;

        dirty_.store(false, std::memory_order_release);  // 清除脏位

        std::shared_lock<std::shared_mutex> lock(mtx_);
        if (custom_serializer_) {
            custom_serializer_(j, data_);
        } else {
            j[key_] = data_;
        }
    }
};

}  // namespace dk
