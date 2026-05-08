#pragma once
#include <any>

#include "./state.hpp"

namespace dk {
template <typename Context>
class IEventListener {
   public:
    virtual void handle_event(const std::any& event, Context& ctx) = 0;
    virtual ~IEventListener() = default;
};

// 继承 IEventHandler，返回类型指定为 void
template <typename Context, typename Derived>
class BaseEventListener : public IEventHandler<IEventListener<Context>, Context, void, Derived> {};
}  // namespace dk