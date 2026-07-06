#include "./include/test_http.hpp"

#include "dk_auto_json.hpp"

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;

// ============================================================================
// 2. 定义事件合集与上下文 (Engine 所需)
// ============================================================================
struct AppContext : dk::BaseContext<AppContext> {
    int trigger_count = 0;
};

// ============================================================================
// 3. 定义状态机与 Engine
// ============================================================================

class DummyState : public dk::BaseState<AppContext, DummyState, void> {
   public:
    using AllowedEvents = std::tuple<AsyncLoginEvent>;
    using StateAction = dk::StateAction<AppContext>;

    // 处理 HTTP 路由转发过来的 AsyncLoginEvent
    StateAction on_event(const AsyncLoginEvent& e, AppContext& ctx) {
        TestLoginResult result;
        if (e.username == "admin" && e.password == "123456") {
            result.success = true;
            result.token = "real_token_888";
        } else {
            result.success = false;
        }
        // 唤醒框架底层的 HTTP 响应处理
        e.resolve(result);
        return StateAction::handled();
    }
};

class TestEngine : public dk::BaseEngine<AppContext, TestEngine> {};

// ============================================================================
// 4. GTest 测试夹具 (真实网络集成测试)
// ============================================================================

class WebAdapterRealTestSuite : public ::testing::Test {
   protected:
    net::io_context ioc;
    std::shared_ptr<TestEngine> engine;
    // 使用文档中提到的真实 WebAdapter
    std::shared_ptr<dk::WebAdapter<AppContext, TestEngine>> adapter;
    std::thread ioc_thread;

    const short TEST_PORT = 18080;
    const std::string TEST_HOST = "127.0.0.1";

    void SetUp() override {
        engine = std::make_shared<TestEngine>();

        // 初始化真实的 WebAdapter
        adapter = std::make_shared<dk::WebAdapter<AppContext, TestEngine>>(
            engine, TEST_PORT);

        // 注册 HTTP 路由
        adapter->register_route<AsyncLoginEvent, TestLoginResult>(
            http::verb::post, "/api/login");

        // 注册 WebSocket 路由
        dk::WsEndpoint ws_endpoint;
        ws_endpoint.on_open =
            [](std::shared_ptr<dk::WsConnection> conn) -> void {
            conn->send(R"({"type": "welcome"})");
        };
        ws_endpoint.on_message = [](std::shared_ptr<dk::WsConnection> conn,
                                    std::string msg) {
            conn->send("Echo: " + msg);
            if (msg == "quit") {
                conn->close();
            }
        };
        adapter->register_ws_route("/ws/test", std::move(ws_endpoint));

        // 启动引擎
        engine->start<DummyState>(std::chrono::milliseconds(100));

        // 在后台线程启动 ASIO 事件循环，开始监听真实的 Socket 请求
        ioc_thread = std::thread([this]() {
            // 防止 ioc 立即退出
            auto work_guard = net::make_work_guard(ioc);
            ioc.run();
        });

        // 稍微等待一下确保端口绑定成功
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    void TearDown() override {
        if (engine) engine->stop();
        ioc.stop();  // 停止 ASIO 事件循环
        if (ioc_thread.joinable()) {
            ioc_thread.join();
        }
    }
};

// ============================================================================
// 5. 测试用例：测试真实的 HTTP 自动解析与返回
// ============================================================================
TEST_F(WebAdapterRealTestSuite, RealHttpConnectionTest) {
    // 1. 设置客户端独立的 IO Context
    net::io_context client_ioc;
    tcp::resolver resolver(client_ioc);
    beast::tcp_stream stream(client_ioc);

    // 2. 解析并连接到服务器
    auto const results = resolver.resolve(TEST_HOST, std::to_string(TEST_PORT));
    stream.connect(results);

    // 3. 构造真实的 HTTP POST 请求 (携带 JSON)
    http::request<http::string_body> req{http::verb::post, "/api/login", 11};
    req.set(http::field::host, TEST_HOST);
    req.set(http::field::content_type, "application/json");
    // 发送 admin / 123456
    req.body() = R"({"username": "admin", "password": "123456"})";
    req.prepare_payload();

    // 4. 发送请求
    http::write(stream, req);

    // 5. 接收响应
    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);

    // 6. 验证断言
    EXPECT_EQ(res.result(), http::status::ok);  // 期望状态码 200

    // 解析返回的 JSON (测试框架的自动序列化)
    auto res_json = nlohmann::json::parse(res.body());
    EXPECT_TRUE(res_json["success"].get<bool>());
    EXPECT_EQ(res_json["token"].get<std::string>(), "real_token_888");

    // 7. 优雅关闭
    beast::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, ec);
}

// ============================================================================
// 6. 测试用例：测试真实的 WebSocket 协议升级与双向通信
// ============================================================================
TEST_F(WebAdapterRealTestSuite, RealWebSocketConnectionTest) {
    net::io_context client_ioc;
    tcp::resolver resolver(client_ioc);

    // 使用 Beast 的 WebSocket 客户端流
    websocket::stream<tcp::socket> ws{client_ioc};

    // 1. 连接到服务器的 TCP 端口
    auto const results = resolver.resolve(TEST_HOST, std::to_string(TEST_PORT));
    net::connect(ws.next_layer(), results.begin(), results.end());

    // 2. 执行 WebSocket 握手 (协议升级)
    ws.handshake(TEST_HOST, "/ws/test");

    beast::flat_buffer buffer;

    // 3. 测试 on_open：读取服务端主动发来的欢迎消息
    ws.read(buffer);
    EXPECT_EQ(beast::buffers_to_string(buffer.data()),
              R"({"type": "welcome"})");
    buffer.consume(buffer.size());

    // 4. 测试 on_message：客户端发送 "hello"
    ws.write(net::buffer(std::string("hello")));

    // 验证服务端的 "Echo: hello"
    ws.read(buffer);
    EXPECT_EQ(beast::buffers_to_string(buffer.data()), "Echo: hello");
    buffer.consume(buffer.size());

    // 5. 测试 on_close：客户端发送 "quit" 触发服务端的 close()
    ws.write(net::buffer(std::string("quit")));

    // 读取 quit 的回音
    ws.read(buffer);
    EXPECT_EQ(beast::buffers_to_string(buffer.data()), "Echo: quit");
    buffer.consume(buffer.size());

    // 6. 验证连接被服务端关闭
    // 当服务端调用 close() 时，底层的 WebSocket 协议层会收到 close 帧
    beast::error_code ec;
    ws.read(buffer, ec);
    // 预期的错误码应当是 WebSocket 连接正常关闭
    EXPECT_EQ(ec, websocket::error::closed);
}