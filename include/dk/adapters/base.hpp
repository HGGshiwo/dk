#pragma once
#include <memory>

#include "../engine.hpp"
#include "../future.hpp"

namespace dk {
// 适配器基类，绑定特定的引擎类型
template <typename Context, typename DerivedEngine>
class BaseAdapter {
   protected:
    std::shared_ptr<BaseEngine<Context, DerivedEngine>> engine_;
    /*
     异步触发一个事件（通常是需要获取事件处理结果），底层是向boost::asio::post提交一个事件处理的任务，任务中如果调用AsyncEvent.resolve()则Future能够正确获取结果。
    */
    template <typename E>
    Future<typename E::ReturnType> dispatch_async(E e, uint32_t timeout_ms = 5000) {
        return engine_->dispatch_async(e, timeout_ms);
    }

    /*
        用boost::asio::post提交一个事件处理任务
    */
    void dispatch(std::any e) { engine_->dispatch(e); }

   public:
    BaseAdapter(std::shared_ptr<DerivedEngine> engine) : engine_(engine) {}
};
}  // namespace dk