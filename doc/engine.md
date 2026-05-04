### 一、 核心概念与使用流
1. **Event (事件)**: 底层由 `std::variant` 包装的业务事件。内置 `TickEvent`, `EnterEvent`, `ExitEvent`。
2. **Context (上下文)**: 存放业务数据和引擎指针，在所有状态和事件间传递。
3. **State / Listener (状态/监听器)**: 编写具体的 `on_event` 拦截和处理业务逻辑。
4. **Engine (引擎)**: 驱动状态机运转的单线程 Asio 事件循环。

**使用步骤：**
1. 定义业务事件，需要异步结果的继承 `AsyncEvent<T>`。
2. 继承 `BaseState` 或 `PureState`，实现多态重载函数：`ReturnType on_event(const SpecificEvent& e, Context& ctx)`（框架会自动通过 SFINAE 路由，无需手动转型）。
3. 实例化 `BaseEngine`，调用 `start()` 跑起状态机。
4. 外部或内部通过 `dispatch` 系列 API 派发事件。

---

### 二、 核心 API 参考

#### 1. Engine (引擎核心)
*   **生命周期**
    *   `start(initial_state, tick_interval_ms)`: 启动引擎、挂载初始状态、启动定时 Tick 和主事件循环线程。
    *   `stop()`: 停止事件循环和后台线程池。
    *   `step(next_state)`: 触发状态流转（自动派发 Exit 和 Enter 事件）。
*   **事件派发**
    *   `dispatch(e)`: 线程安全。将事件投递到主事件循环队尾。
    *   `dispatch_internal(e)`: 内部高优微队列。当前调用栈结束后立即执行（使用 `asio::defer`）。
    *   `dispatch_async(e, timeout_ms)` $\rightarrow$ `Future<T>`: 派发一个 `AsyncEvent` 并返回 Future，超时或业务调 `resolve` 时触发，e必须继承AsyncEvent。
*   **异步任务与等待**
    *   `post_background_task(task_func)` $\rightarrow$ `Future<T>`: 将耗时任务扔到后台线程池执行，执行完**绝对安全地**把结果送回主线程 Future。
    *   `wait_for(timeout_ms, lambdas...)` $\rightarrow$ `Future<bool>`: 在状态内直接挂起监听特定事件。Lambda 返回 `true` 结束监听，返回 `false` 继续监听。（内部自动提取 Lambda 参数类型进行匹配）。
*   **其他**
    *   `add_listener(listener)`: 添加独立于状态机的全局事件监听器。
    *   `get_context()`: 获取上下文。

#### 2. Event (事件处理)
*   **`AsyncEvent<ResultT>`** (异步事件基类)
    *   `resolve(ResultT val) const`: 业务处理完毕后调用，唤醒对应的 Future。
    *   `reject(exception_ptr / string) const`: 处理失败时调用。
    *   `is_awaited() const`: 判断外部是否正在等待（Promise 是否为空）。

#### 3. State & Listener (状态与监听)
*   **`BaseState`**: 允许带成员变量的普通状态。必须实现 `name()`。
*   **`PureState`**: **纯状态（无成员变量）**。通过 `::instance()` 获取自带空删除器的单例 shared_ptr，极大地节省内存分配开销。
*   **`BaseEventListener`**: 状态无关的全局监听器。

#### 4. Adapter (外部适配器)
*   **`BaseAdapter`**: 供外部模块继承，持有 Engine 指针，用于向状态机内部 `dispatch` 或 `dispatch_async` 注入外部事件。

---

### 三、 ⚠️ 极其重要的注意事项

1. **单线程无锁设计核心**：
   所有的 `on_event` 和 `handle_event` 都在**唯一的 Worker 线程**中串行执行，**严禁在 `on_event` 中执行阻塞/死循环操作**（如 sleep、锁等待、同步网络 IO），否则会导致整个状态机和 Tick 卡死。
2. **后台任务返回安全性**：
   如果使用了 `post_background_task` 在后台计算，**严禁**在后台任务的回调 Lambda 中直接修改 `Context` 或调用 `Event.resolve()`。框架已自动帮你封存在了主循环执行，业务层面只需关注返回 `ReturnType`。
3. **PureState 的致命陷阱**：
   继承自 `PureState` 的子类内部**绝对不可以定义任何成员变量**！它是作为全局 `static` 单例运行的，多状态流转时会导致数据串号污染。如果需要状态内数据，请用 `BaseState`。
4. **Engine 的析构**：
   `stop()` 中带有防误杀设计。状态机内部的方法（同一线程）如果调用会导致销毁自身的逻辑（如销毁持有 engine 的最外层对象），会抛出 `std::logic_error`，以防自身 Join 自身导致死锁。
5. **Wait_for 的覆盖规则**：
   `wait_for` 中传入的多个 Lambda 会组合成一个 `overloaded` 访问器。确保传入的 Lambda 处理的具体事件类型不重叠，并且入参必须是 `const EventType&`。

---

### 使用示例
```cpp
#include "dk/core.hpp" 

// 1. 定义业务事件与 Context
struct MyEvent { int val; };
using AppEvent = std::variant<dk::TickEvent, dk::EnterEvent, dk::ExitEvent, MyEvent>; // 包含内置事件
struct AppContext {}; 

// 2. 声明引擎
class AppEngine : public dk::BaseEngine<AppEvent, AppContext, AppEngine> {};

// 3. 定义具体状态（PureState 自动提供内存安全的 instance() 单例）
class StateA : public dk::PureState<AppEvent, AppContext, StateA> {
public:
    // 框架底层会通过 SFINAE 自动匹配并路由到这个方法
    std::shared_ptr<dk::IState<AppEvent, AppContext>> on_event(const MyEvent& e, AppContext& ctx) {
        std::cout << "StateA 收到 MyEvent，数值: " << e.val << std::endl;
        return nullptr; // 返回 nullptr 表示不切换状态
    }
};

int main() {
    auto engine = std::make_shared<AppEngine>();
    
    // 4. 启动引擎，挂载初始状态 StateA，设置 Tick 心跳为 1000ms
    engine->start(StateA::instance(), std::chrono::milliseconds(1000));
    
    // 5. 从外部线程安全地派发业务事件
    engine->dispatch(MyEvent{42});
    
    std::this_thread::sleep_for(std::chrono::seconds(1));
    engine->stop(); // 安全停止
    return 0;
}
```