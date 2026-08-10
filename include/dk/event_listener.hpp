#pragma once
#include "./state.hpp"
#include "./utils.hpp"
#include "event_box.hpp"

namespace dk {
template <typename Context>
class IEventListener : public TimeTracker {
   public:
    virtual StateAction<Context> handle_event(const EventBox& event,
                                              Context& ctx) = 0;
    virtual ~IEventListener() = default;

    virtual void on_tick(double dt, Context& ctx) {}

    void internal_tick(double dt, Context& ctx) {
        this->update_time(dt);
        on_tick(dt, ctx);
    }
};

// 继承 IEventHandler，返回类型指定为 StateAction<Context>
template <typename Context, typename Derived>
class BaseEventListener : public IEventHandler<IEventListener<Context>, Context,
                                               StateAction<Context>, Derived> {
};
}  // namespace dk