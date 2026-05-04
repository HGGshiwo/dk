# C++ Async Web/WebSocket Framework 说明文档

## 1. 架构总览 (Architecture Overview)
本框架基于 `Boost.Asio` 和 `Boost.Beast` 开发，是一个纯异步、事件驱动的底层通信适配器。
- **统一抽象路由**：所有连接入口均为 HTTP 请求。底层通过 `IProtocolHandler` 接口进行路由分发。
- **协议升级机制**：支持 HTTP 转长连接（如 WebSocket、SSE）。长连接 Handler 会调用 `release_socket()` 剥离并接管底层 TCP Socket，原 HTTP 会话安全销毁。
- **业务解耦**：业务层不接触任何底层的 Boost Socket 或 Buffer，只与高度抽象的接口（Struct、Future、WsConnection）交互。

## 2. 核心接口定义 (Core Interfaces)

### 2.1 初始化与启动 Adapter (Initialization and Startup)

WebAdapter 充当网络层（Boost.Asio）与业务逻辑层（Engine）之间的桥梁

```cpp
template <typename Event, typename Context, typename DerivedEngine>
class WebAdapter : public BaseAdapter<Event, Context, DerivedEngine> {
public:
    WebAdapter(boost::asio::io_context& ioc, short port, std::shared_ptr<DerivedEngine> engine);
    // ...
};
```
启动规范示例：

```cpp
// 实例化业务引擎 (需继承自框架的 BaseEngine)
auto engine = std::make_shared<MyBusinessEngine>();
// 实例化 Adapter (绑定 IO、端口与引擎)
WebAdapter<MyEvent, MyContext, MyBusinessEngine> adapter(ioc, 8080, engine);
// 4. 注册路由 (见下文)
// adapter.register_route...
// 5. 启动事件循环 (在线程中启动，非阻塞)
engine.start();
```

### 2.2 HTTP 路由注册 (基于 Struct 与 Future)
框架深度集成了 `nlohmann::json` 和自定义的 `dk::Future`。实现**自动解析、自动分发、自动序列化**。

```cpp
// 接口签名
template <typename SpecificEvent, typename SpecificResult>
void register_route(boost::beast::http::verb method, const std::string& path, uint32_t timeout_ms = 5000);
```
**行为规约：**
1. 框架自动将 HTTP POST Body (JSON) 映射为 `SpecificEvent` 结构体。
2. 事件投递给 Engine 处理，期望返回 `dk::Future<SpecificResult>`。
3. `SpecificResult` 自动序列化为 JSON 并作为 HTTP 200 返回。
4. 任何阶段的异常（JSON 缺字段、处理超时等）均会被框架捕获并转化为标准的 HTTP 400/500 错误响应。

### 2.3 WebSocket 路由注册
WebSocket 对业务层暴露为纯粹的事件回调机制和线程安全的发送接口。

```cpp
// 接口签名
void register_ws_route(const std::string& path, WsEndpoint endpoint);

// 回调结构体
struct WsEndpoint {
    std::function<void(std::shared_ptr<WsConnection>)> on_open;
    std::function<void(std::shared_ptr<WsConnection>, std::string)> on_message;
    std::function<void(std::shared_ptr<WsConnection>)> on_close;
};

// 抽象连接句柄 (暴露给业务层)
class WsConnection : public std::enable_shared_from_this<WsConnection> {
public:
    virtual void send(const std::string& msg) = 0; // 线程安全
    virtual void close() = 0;                      // 线程安全
};
```

---

## 3. 业务代码示例 (Usage Examples)

### 3.1 定义数据结构 (依赖 Nlohmann JSON)
**注意：必须为结构体定义宏，以支持序列化。编写结构体时，只需在上方添加专属标记 // @JSON_ENABLE，框架就会自动识别它并提取成员变量**

```cpp
// @JSON_ENABLE
struct LoginEvent {
    std::string username;
    std::string password;
};

// @JSON_ENABLE
struct LoginResult {
    bool success;
    std::string token;
};
```

在 CMake 文件中，调用框架提供的 dk_create_json_library 函数。它会一键扫描指定目录，并生成一个虚拟的“接口库”。

```cmake
add_subdirectory(thirdparty/dk)
# 调用函数，扫描用户自己的 src 目录
dk_create_json_library(UserGameJsonLib "${CMAKE_CURRENT_SOURCE_DIR}/src")
# 定义用户的程序
add_executable(my_game src/main.cpp)
# 链接框架 + 自动生成的 JSON 接口库
target_link_libraries(my_game PRIVATE 
    dk       # 链接框架
    UserGameJsonLib    # 链接刚才生成的 JSON 宏库
)
```
在main.cpp上添加生成的头文件
```cpp
#include "dk_auto_json.hpp" 
```

### 3.2 注册 HTTP API
```cpp
// 业务层只需这一行，框架包揽一切（反序列化 -> 投递 -> 异步等待 -> 序列化响应）
adapter.register_route<LoginEvent, LoginResult>(boost::beast::http::verb::post, "/api/login");
```

### 3.3 注册 WebSocket API
```cpp
WsEndpoint endpoint;
endpoint.on_open = [](std::shared_ptr<WsConnection> conn) {
    conn->send("{\"type\": \"welcome\"}");
};
endpoint.on_message = [](std::shared_ptr<WsConnection> conn, std::string msg) {
    // 业务处理逻辑
    conn->send("Echo: " + msg); 
};
adapter.register_ws_route("/ws/stream", std::move(endpoint));
```

---

## 4. LLM 代码生成注意事项 (CRITICAL GOTCHAS FOR LLMs)

如果你（大语言模型）需要基于此框架生成代码，**必须严格遵守以下规则**：

1. **绝对禁止阻塞 (No Blocking)**：
   底层基于单线程/线程池的 Asio Event Loop。业务代码在 HTTP Handler 或 WS Callback 中**绝对不能**调用 `std::this_thread::sleep_for` 或任何阻塞式 I/O。必须使用 `dk::Future` 的异步链式调用（`.then()`）。
2. **线程安全性 (Thread Safety)**：
   - 业务层持有的 `std::shared_ptr<WsConnection>` 可以在任意线程中调用 `send()` 和 `close()`。框架底层已经通过 `boost::asio::post` 和发送队列（Write Queue）保证了并发写入的安全性。
   - 不要试图在业务层自己加锁控制 `send()`，底层已处理。
3. **`shared_from_this` 多态陷阱**：
   - `WsConnection` 是基类并继承了 `std::enable_shared_from_this<WsConnection>`。
   - 在底层的派生类（如 `WsSessionImpl`）内部如果要获取派生类的智能指针，**不能**直接使用 `shared_from_this()`，必须使用预定义的 `derived_from_this()` 方法（内部通过 `std::static_pointer_cast` 转换），否则会引发 Asio 编译错误。
4. **异常处理 (Exception Handling)**：
   在 WebSocket 的 `on_message` 中如果执行 JSON 解析，必须使用 `try-catch` 捕获 `nlohmann::json::parse_error`，以免让底层 Asio 崩溃。
5. **协议拓展 (Extensibility)**：
   若需新增 SSE (Server-Sent Events) 支持，不要修改主流程，请继承 `IProtocolHandler`，在 `handle()` 中调用 `session->release_socket()` 获取底层的 `tcp::socket`，并手动写入 HTTP Header (`Content-Type: text/event-stream`) 即可。