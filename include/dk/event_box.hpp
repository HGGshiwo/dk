#pragma once
#include <atomic>
#include <memory>
namespace dk {

class EventTypeId {
   private:
    // 静态原子计数器，用于在程序启动时自增分配 ID
    inline static std::atomic<uint32_t> counter_{1};

   public:
    template <typename T>
    static uint32_t get() {
        // 【核心魔法】：对于每一个不同的类型 T，这个 static
        // 变量只会被初始化一次！ 所以同一种类型的事件，永远返回同一个 ID。
        static uint32_t id = counter_.fetch_add(1);
        return id;
    }
};

struct EventBox {
    uint32_t type_id = 0;
    std::shared_ptr<void> data;  // 负责持有数据并自动正确析构

    EventBox() = default;

    // 万能构造函数（代替 std::make_any）
    template <typename T, typename = std::enable_if_t<
                              !std::is_same_v<std::decay_t<T>, EventBox>>>
    EventBox(T&& event) {
        using CleanT = std::decay_t<T>;
        type_id = EventTypeId::get<CleanT>();
        // shared_ptr<void> 会在内部分配控制块时，记录下 CleanT 的真实析构函数！
        data = std::make_shared<CleanT>(std::forward<T>(event));
    }

    // 极速零开销转换（代替 std::any_cast）
    template <typename T>
    const T* cast_if_match() const {
        using CleanT = std::decay_t<T>;
        // O(1) 的整数比对，比查 RTTI 表快一到两个数量级
        if (type_id != EventTypeId::get<CleanT>()) {
            return nullptr;
        }
        // 因为 ID 已经完美匹配，这里直接使用
        // static_cast，绝对安全且零运行时开销！
        return static_cast<const T*>(data.get());
    }
};

}  // namespace dk