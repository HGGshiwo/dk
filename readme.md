# dk 框架使用指南

## 测试
```
# 1. 进入你包裹在 ROS 包内的框架独立目录
cd ~/catkin_ws/src/dankong/dk
# 2. 创建一个纯净的构建文件夹（隔离编译产物）
mkdir build && cd build
# 3. 运行 CMake，并强制开启我们之前设置的测试开关
cmake -DDK_BUILD_TESTS=ON ..
# 4. 编译（-j4 表示用 4 个核心加速编译）
make -j4
# 5. 运行生成的测试程序
./tests/test_dk_core
# （或者也可以使用 CTest 命令一键运行所有测试： ctest -V）
```

`dk` 是一个基于 C++17 的轻量级状态机与事件驱动框架。它通过 `std::variant` 实现无基类继承的事件系统，利用 CRTP 和 SFINAE 技术在编译期完成事件分发，并内置了微任务/宏任务双队列的事件循环。

## 1. 核心特性

* **类型安全与鸭子类型**：事件不需要继承任何公共基类。状态类只需按需实现 `on_event(Event, Context)`，框架会在编译期自动探测并路由，未处理的事件会被安全忽略。
* **双队列事件循环**：
  * **微队列 (内部事件)**：状态机内部触发的流转事件、Tick心跳。优先级最高，保证状态机逻辑的连贯性。
  * **宏队列 (外部事件)**：外部线程通过 `dispatch` 投递的事件。仅在微队列排干后处理。
* **零开销状态切换**：通过 `PureState` 模式，无成员变量的状态类可作为静态单例复用，消除状态切换时的内存分配 (`new/make_shared`) 开销。
* **内建异步支持**：支持外部线程向状态机发送事件并异步等待处理结果 (`Promise/Future` 模式)。
* **全局生命周期 Hook**：引擎层可拦截 `EnterEvent`、`ExitEvent` 及任意业务事件，方便统一打印日志或打点监控。

---

## 2. 快速上手

### 第一步：定义事件与上下文
将框架内置事件与业务事件打包成 `std::variant`。

```cpp
#include "dk.h"

// 1. 业务事件定义 (普通结构体即可)
struct EvPowerOn { int voltage; };
struct EvPowerOff {};

// 2. 聚合类型
using MyEvent = std::variant<
    dk::TickEvent, dk::EnterEvent, dk::ExitEvent, 
    EvPowerOn, EvPowerOff
>;

// 3. 上下文 (在所有状态间共享的数据)
struct MyContext : dk::BaseContext<MyEvent> {
    int current_voltage = 0;
};
```

### 第二步：定义状态
继承 `BaseState`。如果状态没有成员变量（推荐），请同时继承 `PureState` 以启用静态单例。

```cpp
class ActiveState; // 前置声明

class IdleState : 
    public dk::BaseState<MyEvent, MyContext, IdleState>,
    public dk::PureState<MyEvent, MyContext, IdleState> {
public:
    // 只需实现你关心的事件
    std::shared_ptr<dk::IState<MyEvent, MyContext>> on_event(const EvPowerOn& e, MyContext& ctx) {
        ctx.current_voltage = e.voltage;
        std::cout << "Powering on with " << e.voltage << "V\n";
      
        // 使用单例进行零开销切换
        return ActiveState::instance(); 
    }
};

class ActiveState : 
    public dk::BaseState<MyEvent, MyContext, ActiveState>,
    public dk::PureState<MyEvent, MyContext, ActiveState> {
public:
    std::shared_ptr<dk::IState<MyEvent, MyContext>> on_event(const dk::TickEvent& e, MyContext& ctx) {
        std::cout << "System is running...\n";
        return nullptr; // 返回 nullptr 表示保持当前状态
    }

    std::shared_ptr<dk::IState<MyEvent, MyContext>> on_event(const EvPowerOff& e, MyContext& ctx) {
        return IdleState::instance();
    }
};
```

### 第三步：定义引擎与运行
引擎负责管理队列和线程，可以在这里实现全局事件拦截。

```cpp
class MyEngine : public dk::BaseEngine<MyEvent, MyEngine, MyContext> {
public:
    // 全局拦截器：打印所有状态的进入日志
    void on_event(const dk::EnterEvent& e, MyContext&) {
        std::cout << "[Hook] Entered State: " << e.state_name << "\n";
    }
};

int main() {
    auto engine = std::make_shared<MyEngine>();
  
    // 启动引擎：传入初始状态，并设置 Tick 间隔(毫秒)
    engine->start(IdleState::instance(), std::chrono::milliseconds(1000));
  
    // 外部线程投递事件
    engine->dispatch(EvPowerOn{220});
  
    std::this_thread::sleep_for(std::chrono::seconds(3));
  
    engine->dispatch(EvPowerOff{});
  
    // 优雅停机
    engine->stop();
    return 0;
}
```

---

## 3. 高级特性

### 3.1 异步事件 (Async Event)
当外部线程需要同步等待状态机的处理结果时使用。

**定义事件：** 继承 `dk::AsyncEvent<返回值类型>`
```cpp
struct EvGetVoltage : dk::AsyncEvent<int> {};
```

**状态机内处理：** 调用 `e.resolve()` 或 `e.reject()`
```cpp
class ActiveState : ... {
public:
    auto on_event(const EvGetVoltage& e, MyContext& ctx) {
        e.resolve(ctx.current_voltage); // 填充结果
        return nullptr;
    }
};
```

**外部调用：** 使用 `dispatch_async` 并等待 `future`
```cpp
// 返回 std::future<int>
auto future = engine->dispatch_async(EvGetVoltage{}); 

try {
    int voltage = future.get(); // 阻塞等待状态机处理完毕
    std::cout << "Voltage is: " << voltage << "\n";
} catch (const std::exception& e) {
    std::cerr << "Event rejected or unhandled: " << e.what() << "\n";
}
```

### 3.2 投递后台工作流 (Workflow)
状态机主线程不能被阻塞。如果某个状态需要执行耗时 IO，可以使用 `engine->run_workflow`。

```cpp
class ActiveState : ... {
public:
    auto on_event(const EvStartDownload& e, MyContext& ctx) {
        // 投递到后台线程池执行
        ctx.engine->run_workflow([]() -> std::optional<MyEvent> {
            std::string data = fake_http_download(); // 耗时阻塞操作
            return EvDownloadComplete{data};         // 返回完成后要触发的新事件
        });
        return nullptr;
    }
};
```

---

## 4. 使用建议与避坑指南

1. **绝对不要在 `on_event` 中阻塞**
   `on_event` 运行在状态机的核心 Worker 线程中。任何 `sleep`、网络等待、长循环都会导致整个引擎卡死，无法处理后续队列和定时器。耗时操作请交由 `run_workflow` 处理。
2. **警惕内部队列死循环 (饥饿)**
   `dispatch_internal()` 投递的事件具有最高优先级。如果状态 A 触发了内部事件切换到状态 B，状态 B 又触发内部事件切回 A，会导致微队列永远无法排干，外部事件（宏队列）将被永久饿死。
3. **优先使用 `PureState`**
   尽可能让状态类保持无状态（将数据存在 `Context` 中）。这样可以使用 `PureState<...>::instance()` 返回单例指针，避免频繁切换状态导致的堆内存碎片。
4. **引擎析构与 `stop()`**
   * 不要强行 `delete` 引擎对象，应通过共享指针的生命周期管理。
   * 如果要在状态内部主动关闭状态机，请调用 `ctx.engine->stop()`，框架内部已处理了针对当前线程安全退出的逻辑。
5. **Context 的线程安全性**
   在 `on_event` 中读写 `Context` 是**绝对线程安全**的（受状态机单线程事件循环保护）。但如果外部线程（如其它适配器）直接持有并修改 `Context` 的指针，则必须自行加锁。建议所有数据修改都通过 `dispatch` 派发事件来完成。