#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

namespace dk {

template <typename T>
class thread_safe {
   private:
    T data_;
    mutable std::shared_mutex mtx_;  // 替换为读写锁

   public:
    template <typename... Args>
    thread_safe(Args&&... args) : data_(std::forward<Args>(args)...) {}

    // ==========================================
    // 接口 1：写操作（独占锁）
    // 传递 T&，允许修改数据
    // ==========================================
    template <typename Func>
    decltype(auto) write(Func&& func) {
        std::unique_lock<std::shared_mutex> lock(mtx_);  // 独占锁
        return func(data_);                              // 传递可变引用
    }

    // ==========================================
    // 接口 2：读操作（共享锁）
    // 传递 const T&，编译器强制禁止修改！
    // ==========================================
    template <typename Func>
    decltype(auto) read(Func&& func) const {
        std::shared_lock<std::shared_mutex> lock(mtx_);  // 共享锁
        return func(data_);                              // 传递常量引用
    }

    T get() const {
        std::unique_lock<std::shared_mutex> lock(mtx_);
        return data_;
    }

    void set(T data) {
        std::unique_lock<std::shared_mutex> lock(mtx_);
        ;
        data_ = data;
    }
};
}  // namespace dk