#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <chrono>
#include <future>
#include <string>
#include <thread>

#include "dk/future.hpp"  // 引入你的 Future 框架头文件

using namespace dk;

// ============================================================================
// 测试夹具 (Test Fixture)：为每个 Test 提供独立、干净的运行时环境
// ============================================================================
class DkFutureTest : public ::testing::Test {
   protected:
    boost::asio::io_context ioc_;
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_guard_;
    std::thread worker_thread_;
    std::unique_ptr<IAsyncRuntime> runtime_;

    // 定义 AsioRuntime 内部类
    class AsioRuntime : public IAsyncRuntime {
        boost::asio::io_context& ioc_;

       public:
        std::thread::id thread_id;
        explicit AsioRuntime(boost::asio::io_context& ioc) : ioc_(ioc) {}

        std::thread::id get_thread_id() override { return thread_id; }

        void post_future_task(std::function<void()> task) override { boost::asio::post(ioc_, std::move(task)); }

        std::function<void()> set_future_timeout(uint32_t ms, std::function<void()> on_timeout) override {
            auto timer = std::make_shared<boost::asio::steady_timer>(ioc_);
            timer->expires_after(std::chrono::milliseconds(ms));
            timer->async_wait([on_timeout, timer](const boost::system::error_code& ec) {
                if (!ec) on_timeout();
            });
            return [timer]() { timer->cancel(); };
        }
    };

    void SetUp() override {
        // 1. 初始化 Runtime
        runtime_ = std::make_unique<AsioRuntime>(ioc_);
        // 2. 保证 io_context 不会因为没任务而立刻退出
        work_guard_ = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
            boost::asio::make_work_guard(ioc_));
        // 3. 启动后台事件循环
        worker_thread_ = std::thread([this]() { ioc_.run(); });
        if (auto* asio_rt = dynamic_cast<AsioRuntime*>(runtime_.get())) {
            asio_rt->thread_id = worker_thread_.get_id();
        }
    }

    void TearDown() override {
        // 测试结束后，清理环境
        work_guard_->reset();  // 释放工作锁
        ioc_.stop();           // 停止事件循环
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
    }
};

// ============================================================================
// 测试用例 1：基础链式调用与数据流转
// ============================================================================
TEST_F(DkFutureTest, BasicChain) {
    std::promise<void> test_done;  // 用于阻塞 GTest 等待异步完成
    std::string final_result;

    Promise<int>::resolve(runtime_.get(), 10)
        .then([](int val) { return val * 2; })
        .then([](int val) { return std::string("Result: ") + std::to_string(val); })
        .then([&](std::string str) {
            final_result = str;     // 记录结果
            test_done.set_value();  // 唤醒主线程
            return true;
        });

    // 阻塞等待异步链条执行完毕（设置 1 秒超时防死锁）
    ASSERT_EQ(test_done.get_future().wait_for(std::chrono::seconds(1)), std::future_status::ready);

    // GTest 断言
    EXPECT_EQ(final_result, "Result: 20");
}

// ============================================================================
// 测试用例 2：异步嵌套自动展平 (Flattening)
// ============================================================================
TEST_F(DkFutureTest, FutureFlattening) {
    std::promise<void> test_done;
    std::string final_result;

    Promise<int>::resolve(runtime_.get(), 100)
        .then([this](int val) {
            // 返回内部的新 Future
            auto p = std::make_shared<Promise<std::string>>(runtime_.get());

            // 模拟后台耗时操作
            std::thread([p]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                p->resolve("Flatten Magic!");
            }).detach();

            return p->get_future();
        })
        .then([&](std::string res) {
            final_result = res;
            test_done.set_value();
            return true;
        });

    ASSERT_EQ(test_done.get_future().wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_EQ(final_result, "Flatten Magic!");
}

// ============================================================================
// 测试用例 3：合作式取消机制
// ============================================================================
TEST_F(DkFutureTest, Cancellation) {
    std::promise<void> test_done;
    bool step1_executed = false;
    bool error_caught = false;

    CancellationTokenSource cts;
    auto token = cts.get_token();

    // 在注册前立刻取消
    cts.cancel();

    dk::Promise<int>::resolve(runtime_.get(), 1)
        .with_cancellation(token)
        .then([&](int val, CancellationToken t) {
            step1_executed = true;  // 如果 token 机制生效，这行绝对不会执行
            return val;
        })
        .catch_error([&](std::exception_ptr e) {
            try {
                std::rethrow_exception(e);
            } catch (const CancelledException&) {
                error_caught = true;  // 成功捕获取消异常
            }
            test_done.set_value();
        });

    ASSERT_EQ(test_done.get_future().wait_for(std::chrono::seconds(1)), std::future_status::ready);

    // GTest 断言
    EXPECT_FALSE(step1_executed) << "任务已经被取消，不应该执行 then 的回调！";
    EXPECT_TRUE(error_caught) << "必须在 catch_error 中捕获到 CancelledException！";
}

// ============================================================================
// 测试用例 4：超时拦截机制
// ============================================================================
TEST_F(DkFutureTest, TimeoutInterceptor) {
    std::promise<void> test_done;
    bool error_caught = false;

    auto p = std::make_shared<Promise<int>>(runtime_.get());

    p->get_future()
        .timeout(50)  // 👈 设置 50 毫秒极短超时
        .then([](int val) {
            ADD_FAILURE() << "超时保护失效，竟然执行了正常回调！";
            return val;
        })
        .catch_error([&](std::exception_ptr e) {
            try {
                std::rethrow_exception(e);
            } catch (const TimeoutException&) {
                error_caught = true;
            }
            test_done.set_value();
        });

    // 模拟后台线程：耗时 200 毫秒才完成任务 (早就超时了)
    auto t = std::thread([p]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        p->resolve(99);
    });

    // 留出足够的时间让超时触发
    ASSERT_EQ(test_done.get_future().wait_for(std::chrono::seconds(1)), std::future_status::ready);

    EXPECT_TRUE(error_caught) << "必须捕获到 TimeoutException！";
    if (t.joinable()) t.join();
    // 等待resolve完成，防止崩溃
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

// ============================================================================
// 测试用例 5：成功执行时，自动拆除定时炸弹
// ============================================================================
TEST_F(DkFutureTest, TimeoutCancelledOnSuccess) {
    std::promise<void> test_done;
    bool success_executed = false;

    auto p = std::make_shared<Promise<int>>(runtime_.get());

    p->get_future()
        .timeout(500)  // 👈 希望上一步(p的future)的时间不要多余500ms
        .then([&](int val) {
            success_executed = true;
            test_done.set_value();
            return val;
        })
        .catch_error([&](std::exception_ptr e) { ADD_FAILURE() << "任务已经成功完成，不应该触发 catch！"; });

    // 后台瞬间完成 (10ms)，不应该触发 500ms 的超时
    auto t = std::thread([p]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        p->resolve(99);
    });

    ASSERT_EQ(test_done.get_future().wait_for(std::chrono::seconds(1)), std::future_status::ready);

    EXPECT_TRUE(success_executed);
    if (t.joinable()) t.join();
}