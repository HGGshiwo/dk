# `dk` Future/Promise 极简 API 参考与避坑指南

## 1. 核心类与接口
本库基于事件循环（需实现 `IAsyncRuntime` 接口进行微任务投递和定时器设置）。

*   **`CancellationTokenSource` / `CancellationToken`**：取消信号源与令牌。
*   **`Promise<T>`**：异步结果生产者。
*   **`Future<T>`**：异步结果消费者（支持链式调用）。

## 2. Promise<T> (生产者)
用于封装异步操作，触发成功或失败。
*   `Promise(IAsyncRuntime* rt)`: 构造绑定运行时引擎。
*   `Future<T> get_future()`: 获取关联的 Future。
*   `void resolve(T val)`: 标记成功并传值。
*   `void reject(std::exception_ptr e)`: 标记失败并传异常。

🚨 **避坑指南 (Caveats)：**
1.  **不可拷贝**：Promise 只能 Move。在 Lambda 中捕获时必须用 `[p = std::move(promise)]() mutable`。
2.  **Broken Promise**：如果 Promise 在销毁前既没有 resolve 也没有 reject，会触发析构函数异常（防挂起机制）。

## 3. Future<T> (消费者 & 链式流)
支持自动展平（Auto-flattening），即 `then` 返回 `Future<U>` 时，整体推导仍为 `Future<U>` 而非 `Future<Future<U>>`。

### 异步链式操作
*   `auto then(Func&& cb)`: 成功回调。`cb` 参数支持 `(T val)` 或 `(T val, CancellationToken token)`。
*   `Future<T> catch_error(CatchFunc&& cb)`: 失败回调。参数为 `std::exception_ptr`。
*   `auto finally(Func&& cb)`: 不管失败或者成功都会调用。`cb` 参数支持 `(T val)`。
*   `Future<T> timeout(uint32_t ms)`: 添加超时机制，超时产生 `dk::TimeoutException`，注意不会取消已经提交的任务，只对上游的一个任务计时。
*   `Future<T> with_cancellation(CancellationToken token)`: 注入取消令牌，取消产生 `dk::CancelledException`，会自动往下游传递。

### 同步阻塞操作 (Danger Zone)
*   `T get()`: 阻塞直到完成并返回值（或重新抛出异常）。
*   `void wait()` / `wait_for(ms)`: 仅阻塞等待。

🚨 **避坑指南 (Caveats)：**
1.  **死锁防御**：**严禁**在 `IAsyncRuntime` 事件循环所在线程调用 `get()` 或 `wait()`，会直接抛出 `std::logic_error`！
2.  **Move 语义消费**：`get()` 内部使用了 `std::move`，因此对同一个 Future **只能调用一次 `get()`**。
3.  **捕获生命周期**：`then` 里的 Lambda 是异步执行的，**严禁按引用捕获局部变量**（会导致悬垂引用），请按值捕获或使用智能指针。

---

## 4. 极简实战模板 (供 LLM 参考生成)

```cpp
// 1. 初始化与绑定
dk::CancellationTokenSource cts;
dk::Promise<int> promise(runtime_ptr);
dk::Future<int> future = promise.get_future();

// 2. 构造流式处理
future
    .with_cancellation(cts.get_token()) // 挂载取消令牌
    .timeout(5000)                      // 5秒超时
    .then([](int val) {                 // 步骤1: 普通转换
        return std::to_string(val);     // 自动推导为 Future<std::string>
    })
    .then([runtime_ptr](std::string s) { // 步骤2: 异步展平
        dk::Promise<bool> p(runtime_ptr);
        p.resolve(s == "42");
        return p.get_future();          // 自动展平为 Future<bool>
    })
    .catch_error([](std::exception_ptr e) {
        try { std::rethrow_exception(e); }
        catch(const dk::TimeoutException&) { /* 处理超时 */ }
        catch(const dk::CancelledException&) { /* 处理取消 */ }
    });

// 3. 异步触发
std::thread([p = std::move(promise)]() mutable {
    p.resolve(42); 
}).detach();
```