# dk 框架开发指南：从入门到进阶

`dk` 是一个专为机器人系统设计的高性能 C++ 框架。它将**异步事件驱动引擎**与**层次化状态机 (HSM)** 完美结合，旨在解决复杂业务逻辑中的“状态爆炸”问题，并提供丝滑的非阻塞开发体验。

本指南将带你由浅入深，逐步掌握 `dk` 框架的核心能力。

---

## 1. 事件驱动模型：系统的“通信神经”

在 `dk` 框架中，各个组件之间不直接相互调用，而是通过**事件 (Event)** 进行通信。这极大降低了代码的耦合度。

### 1.1 基础 API：简单触发 (Fire-and-Forget)

这是最基础、最常用的事件派发方式。发送方只管把事件扔出去，不关心谁来处理，也不关心处理结果。

* **API**: `engine->dispatch(Event e)`
* **简单场景**：系统中的温度传感器每秒读取一次数据。它只需要将 `TemperatureUpdateEvent` 派发出去即可。UI 模块如果想刷新屏幕，监听这个事件就行了，传感器本身不需要知道 UI 模块的存在。

```cpp
// 传感器模块：只管发送，不问结果
engine->dispatch(TemperatureUpdateEvent{ current_temp });

```

### 💡 进阶：当业务需求变得复杂（需要等待结果）

**痛点引出**：假设发送的不是温度数据，而是 `StartCalibrationEvent`（开始校准电机）。发送方必须知道电机**什么时候校准完成**，以及**校准是否成功**，才能进行下一步（比如提示用户校准成功）。如果用传统回调函数，代码会被切得支离破碎（Callback Hell）。

**高级 API：异步触发与 Future/Promise 原语**

为了解决同步等待带来的线程阻塞问题，`dk` 提供了请求-响应（Request-Response）模型的异步 API。发送方派发一个带有“承诺”的异步事件，并拿到一个 `Future`，后续逻辑可以通过 `.then()` 流式编写。

* **API**: `Future<R> future = engine->dispatch_async(AsyncEvent<R> e)`

```cpp
// 发送方：派发异步事件，等待结果
engine->dispatch_async(StartCalibrationEvent{})
      .then([](bool success) {
          if (success) {
              LOG("电机校准成功，允许启动");
          }
      });

// 处理方（某状态内部）：处理完成后返回结果
void on_event(const StartCalibrationEvent& e, Context& ctx) {
    bool result = run_motor_calibration();
    e.resolve(result); // 兑现 Promise，触发发送方的 then 回调
}

```

---

## 2. 事件监听：从全局到局部

发送了事件，自然需要监听。`dk` 提供了从“大局”到“局部”的多种监听方式。

### 2.1 基础 API：全局监听与状态内监听

* **全局监听 (`add_listener`)**：无论系统当前处于什么状态，都会捕获该事件。
* **适用场景**：全局的日志记录器，或者底层的紧急急停模块（监听到 `EStopEvent` 直接切断动力）。


* **状态内监听 (`on_event`)**：这是最常用的业务开发方式。只有当系统处于特定状态时，事件才会被处理。
* **适用场景**：机器人只有处于 `IdleState`（空闲状态）时，才会响应 `MoveForwardEvent`（前进指令）。如果处于 `ChargingState`（充电状态），该事件会被自动忽略，避免了复杂的 `if-else` 判断。



### 💡 进阶：在异步流程中临时等待特定事件

**痛点引出**：在某个状态的 `on_enter` 初始化过程中，我们需要按顺序完成三件事：发指令给硬件 -> **等待硬件回复 Ack 事件** -> 继续后续逻辑。如果把这些逻辑拆分到 `on_event` 里，代码逻辑会变得极不连贯。

**高级 API：条件等待 (`wait`)**

`dk` 允许你在异步流中“挂起”一个监听器。当满足条件的事件到来时，自动触发回调并注销监听，让代码保持线性的阅读体验。

* **API**: `engine->wait<Event>(token, predicate_lambda)`

```cpp
void on_enter(Context& ctx) {
    // 1. 发送硬件启动指令
    send_hardware_start();

    // 2. 临时挂起，等待特定 ID 的硬件 Ack 事件
    engine->wait<HardwareAckEvent>(ctx.token, [](const HardwareAckEvent& e) {
        return e.id == EXPECTED_ID; // 只有 ID 匹配时才触发
    }).then([]() {
        // 3. 收到匹配的 Ack 后，执行后续逻辑
        LOG("硬件启动完成");
    });
}

```

---

## 3. 层次化状态机 (HSM)：管控复杂逻辑

当系统状态极多时，传统的平面状态机（Flat FSM）会导致各个状态之间连线错综复杂，引发“状态爆炸”。

### 3.1 基础概念：生命周期与数据隔离

将状态看作一个个独立的“房间”，`dk` 严格保证了进出的对称性：

* **`on_enter`**：进入房间时触发，用于分配内存、启动定时器。
* **`on_exit`**：离开房间时触发，用于清理定时器、释放连接。

**数据安全**：在 `dk` 中，状态特有的临时数据直接定义为状态类的成员变量。当状态退出时，该状态实例会被析构，数据自动销毁。这就彻底杜绝了上一次进入状态留下的“脏数据”污染下一次运行。

### 💡 进阶：面对海量状态的冗余代码

**痛点引出**：假设你的系统有 10 个不同的运行状态（前进、后退、抓取等）。现在需求增加：在任何运行状态下遇到 `LowBatteryEvent`（低电量事件），都要记录日志并跳转到充电状态。难道要在 10 个状态类里写 10 遍一样的处理逻辑吗？

**高级机制：父子状态继承与事件冒泡**

`dk` 允许状态嵌套（就像文件夹树一样）。你可以创建一个父状态 `BaseRunState` 包含低电量处理逻辑，然后让那 10 个具体状态继承它。

* **事件冒泡**：如果子状态 `MoveState` 收到了低电量事件，但它自己不处理（返回 `unhandled()`），引擎会自动把事件“冒泡”递交给父状态 `BaseRunState` 处理。
* 这极大提升了代码的复用性，让核心逻辑高内聚。

---

## 4. 底层架构：纳秒级的高性能内存栈

（注：这部分供需要了解框架性能极限的资深开发者阅读）

`dk` 框架之所以能在资源受限的嵌入式 ARM 或实时 Linux 环境下保持超低延迟，核心在于其独特的**状态内存栈 (Arena Memory Stack)** 设计。

传统状态机的每次状态切换通常伴随 `new` 和 `delete`，这在长期运行的机器人系统中会引发严重的堆内存碎片化问题。`dk` 的解决方案是完全模拟 C++ 的函数调用栈：

1. **预分配连续内存 (Zero Heap Allocation)**：引擎启动时申请一块连续内存（Arena）。系统运行期间，状态的创建全在这块内存上进行，全程零堆分配。
2. **LCA 跳转 (最近公共祖先)**：当从子状态 A 跳转到 另一个分支的子状态 B 时，引擎会计算树形结构上的最近公共祖先节点。
3. **内存回退与就地构造**：
* **退栈 (Pop)**：依次调用旧状态的析构函数，并将内存分配指针直接向后“拨回”。
* **入栈 (Push)**：在拨回后的指针位置，使用 **Placement New** 直接就地构造新状态。



这种极其紧凑的内存布局，使得状态数据完美契合 CPU Cache Line，极大提高了缓存命中率，让复杂状态树的切换开销保持在纳秒级别。

---

## 5. 最小使用示例 (Minimal Example)

### 5.1 层次化状态机模式 (HSM Mode)

下面是一个完整的、最小化的 C++ 示例，展示了如何定义 Context、事件、状态类，以及如何启动引擎和派发事件。

```cpp
#include <iostream>
#include <boost/asio.hpp>
#include <dk/engine.hpp>
#include <dk/AsioTimeProvider.hpp>

// 1. 定义上下文 (Context)，用于在状态机间共享数据
struct AppContext {
    int count = 0;
};

// 2. 定义两个测试事件
struct TickEvent {};
struct StartEvent {};

// 3. 前置声明状态类
class IdleState;
class RunningState;

// 4. 定义具体的状态类
// IdleState 继承自 BaseState，第三个模板参数 void 表示它是一个顶层根状态
class IdleState : public dk::BaseState<AppContext, IdleState, void> {
public:
    // 声明当前状态关心的事件列表
    using AllowedEvents = std::tuple<TickEvent, StartEvent>;
    using StateAction = dk::StateAction<AppContext>;

    // 处理 TickEvent
    StateAction on_event(const TickEvent& e, AppContext& ctx) {
        std::cout << "[IdleState] Received TickEvent. Count: " << ctx.count << std::endl;
        return StateAction::handled(); // 标记事件已处理，不发生状态跳转
    }

    // 处理 StartEvent
    StateAction on_event(const StartEvent& e, AppContext& ctx) {
        std::cout << "[IdleState] Received StartEvent. Transitioning to RunningState..." << std::endl;
        return step<RunningState>(); // 跳转到 RunningState
    }
};

// RunningState 同样是顶层状态
class RunningState : public dk::BaseState<AppContext, RunningState, void> {
public:
    using AllowedEvents = std::tuple<TickEvent>;
    using StateAction = dk::StateAction<AppContext>;

    StateAction on_event(const TickEvent& e, AppContext& ctx) {
        ctx.count++;
        std::cout << "[RunningState] Received TickEvent. New Count: " << ctx.count << std::endl;
        if (ctx.count >= 3) {
            std::cout << "[RunningState] Reached target count. Transitioning back to IdleState..." << std::endl;
            return step<IdleState>(); // 回退到 IdleState
        }
        return StateAction::handled();
    }
};

// 5. 定义状态机引擎 (Engine)
class MyEngine : public dk::BaseEngine<AppContext, MyEngine> {};

int main() {
    // 创建 Boost.Asio I/O 上下文
    boost::asio::io_context ioc;

    // 初始化时间提供器与引擎
    auto time_provider = std::make_shared<dk::AsioTimeProvider>(ioc);
    auto engine = std::make_shared<MyEngine>(ioc, time_provider);

    // 启动引擎：设置初始状态为 IdleState，心跳周期为 100ms
    engine->start<IdleState>(std::chrono::milliseconds(100));

    // 派发测试事件，观察状态机的运转与切换
    engine->dispatch(TickEvent{});   // 触发 IdleState 对 Tick 的处理
    engine->dispatch(StartEvent{});  // 触发 IdleState 跳转到 RunningState
    engine->dispatch(TickEvent{});   // 触发 RunningState 的计数自增
    engine->dispatch(TickEvent{});   // 触发 RunningState 的计数自增
    engine->dispatch(TickEvent{});   // 触发 RunningState 计数达到 3，跳转回 IdleState
    engine->dispatch(TickEvent{});   // 再次触发 IdleState 的处理

    // 启动 Boost.Asio 事件循环以执行上述事件
    ioc.run();

    return 0;
}
```

### 5.2 无状态机反应器模式 (Pure Event Reactor Mode)

如果你的业务场景非常简单，不需要维护复杂的状态流转（HSM），可以采用**纯事件反应器模式**。在此模式下，引擎无需绑定初始状态，你可以直接在引擎中接收并处理所有派发的事件。

```cpp
#include <iostream>
#include <boost/asio.hpp>
#include <dk/engine.hpp>
#include <dk/AsioTimeProvider.hpp>

// 1. 定义上下文 (Context)，用于在回调间共享数据
struct AppContext {
    int count = 0;
};

// 2. 定义测试事件
struct TickEvent {};
struct StartEvent {};

// 3. 定义引擎，直接继承 BaseEngine 并重写事件处理函数 (on_event)
class MyReactorEngine : public dk::BaseEngine<AppContext, MyReactorEngine> {
public:
    // 声明当前引擎关心的事件列表
    using AllowedEvents = std::tuple<TickEvent, StartEvent>;

    // 引擎层直接捕获并处理所有事件，无需经过任何状态机
    void on_event(const TickEvent& e, AppContext& ctx) {
        ctx.count++;
        std::cout << "[Reactor] Received TickEvent. Current Count: " << ctx.count << std::endl;
    }

    void on_event(const StartEvent& e, AppContext& ctx) {
        std::cout << "[Reactor] Received StartEvent. Doing some initialization..." << std::endl;
    }
};

int main() {
    boost::asio::io_context ioc;

    auto time_provider = std::make_shared<dk::AsioTimeProvider>(ioc);
    auto engine = std::make_shared<MyReactorEngine>(ioc, time_provider);

    // 启动引擎：不需要传入任何 RootState，只设置心跳周期
    engine->start(std::chrono::milliseconds(100));

    // 派发测试事件，直接由引擎的 on_event 捕获处理
    engine->dispatch(StartEvent{});
    engine->dispatch(TickEvent{});
    engine->dispatch(TickEvent{});

    // 运行事件循环
    ioc.run();

    return 0;
}
```