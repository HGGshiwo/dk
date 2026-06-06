#pragma once

#include <boost/asio.hpp>
#include <chrono>
#include <memory>
#include <thread>

#include "./ITimeProvider.hpp"

namespace dk {

class AsioTimeProvider : public ITimeProvider {
   private:
    boost::asio::io_context& io_;

   public:
    explicit AsioTimeProvider(boost::asio::io_context& io) : io_(io) {}

    double now() override {
        auto current_time = std::chrono::steady_clock::now();
        auto duration = current_time.time_since_epoch();
        return std::chrono::duration<double>(duration).count();
    }

    void sleep_for(double seconds) override { std::this_thread::sleep_for(std::chrono::duration<double>(seconds)); }

    std::function<void()> set_timeout(double seconds, std::function<void()> callback) override {
        auto timer = std::make_shared<boost::asio::steady_timer>(io_);
        timer->expires_after(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(seconds)));

        timer->async_wait([timer, callback](const boost::system::error_code& ec) {
            if (ec != boost::asio::error::operation_aborted) {
                callback();
            }
        });

        return [timer]() { timer->cancel(); };
    }

    std::function<void()> start_ticker(double interval_seconds, std::function<void()> callback) override {
        auto timer = std::make_shared<boost::asio::steady_timer>(io_);
        auto interval =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(interval_seconds));

        // 用 std::shared_ptr 存一个 std::function 解决自引用问题
        auto handler_ptr = std::make_shared<std::function<void(const boost::system::error_code&)>>();
        *handler_ptr = [timer, interval, callback, handler_ptr](const boost::system::error_code& ec) {
            if (ec != boost::asio::error::operation_aborted) {
                callback();
                timer->expires_after(interval);
                timer->async_wait(*handler_ptr);
            }
        };

        timer->expires_after(interval);
        timer->async_wait(*handler_ptr);

        return [timer, handler_ptr]() { timer->cancel(); };
    }
};

}  // namespace dk
