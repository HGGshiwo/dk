#pragma once

#include <functional>

namespace dk {

class ITimeProvider {
   public:
    virtual ~ITimeProvider() = default;

    // 返回 double 秒数
    virtual double now() = 0;

    // 同步休眠
    virtual void sleep_for(double seconds) = 0;

    // 单次异步延时
    virtual std::function<void()> set_timeout(
        double seconds, std::function<void()> callback) = 0;

    // 周期异步心跳
    virtual std::function<void()> start_ticker(
        double interval_seconds, std::function<void()> callback) = 0;
};

}  // namespace dk
