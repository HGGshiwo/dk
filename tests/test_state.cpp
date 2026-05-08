#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "dk/engine.hpp"  // 替换为你的框架头文件路径

// ================== 1. 测试基础结构定义 ==================

// 测试事件
struct EvPing {};
struct EvPong {};
struct EvTriggerInternal {};
struct EvAsyncQuery : dk::AsyncEvent<int> {};  // 异步事件，返回 int

using TestEvent =
    std::variant<dk::TickEvent, dk::EnterEvent, dk::ExitEvent, EvPing, EvPong, EvTriggerInternal, EvAsyncQuery>;

// 测试上下文，用于记录测试结果
struct TestContext : dk::BaseContext<TestContext> {
    int ping_count = 0;
    std::vector<std::string> log;  // 记录执行顺序
    std::mutex log_mtx;
};

// ================== 2. 测试引擎与状态定义 ==================

class TestEngine : public dk::BaseEngine<TestContext, TestEngine> {
   public:
    // 测试全局 Hook：记录进出状态
    using AllowedEvent = std::tuple<dk::EnterEvent, dk::ExitEvent>;

    void on_event(const dk::EnterEvent& e, TestContext& ctx) {
        std::lock_guard<std::mutex> lock(ctx.log_mtx);
        ctx.log.push_back(std::string("ENTER_") + e.state_name);
    }
    void on_event(const dk::ExitEvent& e, TestContext& ctx) {
        std::lock_guard<std::mutex> lock(ctx.log_mtx);
        ctx.log.push_back(std::string("EXIT_") + e.state_name);
    }
};

class StateA : public dk::BaseState<TestContext, StateA, void> {
   public:
    using StateAction = dk::StateAction<TestContext>;
    using AllowedEvent = std::tuple<EvPing, EvAsyncQuery, EvTriggerInternal>;
    // 测试流转
    StateAction on_event(const EvPing&, TestContext& ctx);

    // 测试异步事件处理
    StateAction on_event(const EvAsyncQuery& e, TestContext& ctx);

    // 测试微队列派发
    StateAction on_event(const EvTriggerInternal&, TestContext& ctx);

    // 给 State 类返回一个简化的名字，方便看 log
    std::string name() const override { return "StateA"; }
};

class StateB : public dk::BaseState<TestContext, StateB, void> {
   public:
    using StateAction = dk::StateAction<TestContext>;
    using AllowedEvent = std::tuple<EvPong>;

    StateAction on_event(const EvPong&, TestContext& ctx);
    std::string name() const override { return "StateB"; }
};

// 测试流转
StateA::StateAction StateA::on_event(const EvPing&, TestContext& ctx) {
    std::lock_guard<std::mutex> lock(ctx.log_mtx);
    ctx.ping_count++;
    ctx.log.push_back("Ping_Handled");
    return step<StateB>();
}

// 测试异步事件处理
StateA::StateAction StateA::on_event(const EvAsyncQuery& e, TestContext& ctx) {
    std::lock_guard<std::mutex> lock(ctx.log_mtx);
    ctx.log.push_back("Async_Handled");
    e.resolve(42);  // 返回异步结果
    return StateA::StateAction::handled();
}

// 测试微队列派发
StateA::StateAction StateA::on_event(const EvTriggerInternal&, TestContext& ctx) {
    std::lock_guard<std::mutex> lock(ctx.log_mtx);
    ctx.log.push_back("Trigger_Macro");
    // 投递微队列（高优）
    ctx.engine->dispatch_internal(EvPing{});
    return StateA::StateAction::handled();
}

StateB::StateAction StateB::on_event(const EvPong&, TestContext& ctx) {
    std::lock_guard<std::mutex> lock(ctx.log_mtx);
    ctx.log.push_back("Pong_Handled");
    return step<StateA>();
}

// ================== 3. 测试用例 ==================

class DkFrameworkTest : public ::testing::Test {
   protected:
    std::shared_ptr<TestEngine> engine;

    void SetUp() override {
        engine = std::make_shared<TestEngine>();
        // 启动引擎，初始状态为 StateA，不触发定时器干扰测试
        engine->start<StateA>(std::chrono::hours(1));
    }

    void TearDown() override { engine->stop(); }
};

// 测试 1：基本的事件分发与状态流转
TEST_F(DkFrameworkTest, BasicStateTransitionAndHooks) {
    engine->dispatch(EvPing{});

    // 等待后台线程处理完毕 (简单的同步手段)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const auto& log = engine->get_context().log;  // 假设你在 Engine 里加了 get_context()

    std::lock_guard<std::mutex> lock(engine->get_context().log_mtx);
    EXPECT_EQ(engine->get_context().ping_count, 1);
    // 检查执行顺序：进入A -> 处理Ping -> 离开A -> 进入B
    ASSERT_GE(log.size(), 4);
    EXPECT_EQ(log[0], "ENTER_StateA");
    EXPECT_EQ(log[1], "Ping_Handled");
    EXPECT_EQ(log[2], "EXIT_StateA");
    EXPECT_EQ(log[3], "ENTER_StateB");
}

// 测试 2：微队列优先级测试 (dispatch_internal)
TEST_F(DkFrameworkTest, MicroQueuePriority) {
    // 连续投递两个宏任务
    engine->dispatch(EvTriggerInternal{});
    engine->dispatch(EvPong{});  // 这个任务应该被推迟，直到 EvTriggerInternal 触发的微任务执行完

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const auto& log = engine->get_context().log;
    std::lock_guard<std::mutex> lock(engine->get_context().log_mtx);
    // 核心逻辑：EvTriggerInternal 会触发内部事件 EvPing。
    // 框架要求必须先把 EvPing（内部微队列）处理完，再去处理后面的宏任务 EvPong。
    auto trigger_idx = std::find(log.begin(), log.end(), "Trigger_Macro") - log.begin();
    auto ping_idx = std::find(log.begin(), log.end(), "Ping_Handled") - log.begin();
    auto pong_idx = std::find(log.begin(), log.end(), "Pong_Handled") - log.begin();

    // 验证相对顺序：Trigger -> Ping(微队列抢占) -> Pong(宏队列滞后)
    EXPECT_LT(trigger_idx, ping_idx);
    EXPECT_LT(ping_idx, pong_idx);
}

// 测试 3：异步事件正常返回 (Promise/Future)
TEST_F(DkFrameworkTest, AsyncEventResolve) {
    // 发送异步事件
    dk::Future<int> fut = engine->dispatch_async(EvAsyncQuery{});

    // 阻塞等待状态机处理并 resolve
    // 设置超时防止死锁（500ms 内必须返回）
    auto status = fut.wait_for(500);
    ASSERT_EQ(status, dk::PromiseState::FULFILLED) << "Future timed out!";

    int result = fut.get();
    EXPECT_EQ(result, 42);  // StateA 中写死了 resolve(42)
}

// 测试 4：异步事件被丢弃/未处理时的异常抛出
TEST_F(DkFrameworkTest, AsyncEventUnhandledRejection) {
    // 切换到 StateB (StateB 没有处理 EvAsyncQuery 的代码)
    engine->dispatch(EvPing{});
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // 在 StateB 状态下发送异步请求
    dk::Future<int> fut = engine->dispatch_async(EvAsyncQuery{});

    // 状态机无法处理，Event 对象被析构，应该自动 Reject 抛出 runtime_error
    EXPECT_THROW({ fut.get(); }, std::runtime_error);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}