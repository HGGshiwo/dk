#pragma once

#include <spdlog/spdlog.h>

#include <boost/asio.hpp>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "dk/adapters/base.hpp"

namespace net = boost::asio;
using udp = net::ip::udp;

namespace dk {

template <typename Context, typename DerivedEngine, typename RouteId>
class UdpAdapter : public BaseAdapter<Context, DerivedEngine> {
   private:
    net::io_context& ioc_;
    udp::socket socket_;
    udp::endpoint sender_endpoint_;
    std::vector<uint8_t> recv_buffer_;

    std::function<RouteId(const std::vector<uint8_t>&)> router_;

    using ContextHandler = std::function<void(
        const std::vector<uint8_t>&,
        decltype(std::declval<DerivedEngine>().get_context()))>;
    std::map<RouteId, ContextHandler> context_routes_;

    using EventHandler = std::function<void(const std::vector<uint8_t>&)>;
    std::map<RouteId, EventHandler> event_routes_;

    void do_receive() {
        socket_.async_receive_from(
            net::buffer(recv_buffer_), sender_endpoint_,
            [this](boost::system::error_code ec, std::size_t bytes_recvd) {
                if (!ec && bytes_recvd > 0) {
                    handle_packet(bytes_recvd);
                } else if (ec && ec != net::error::operation_aborted) {
                    spdlog::error("[UdpAdapter Error] Receive failed: {}",
                                  ec.message());
                }

                if (socket_.is_open()) {
                    do_receive();
                }
            });
    }

    void handle_packet(std::size_t bytes_recvd) {
        std::vector<uint8_t> packet_data(recv_buffer_.begin(),
                                         recv_buffer_.begin() + bytes_recvd);

        RouteId id = router_(packet_data);

        if (auto it = context_routes_.find(id); it != context_routes_.end()) {
            it->second(packet_data, this->engine_->get_context());
        }

        if (auto it = event_routes_.find(id); it != event_routes_.end()) {
            it->second(packet_data);
        }
    }

   public:
    UdpAdapter(std::shared_ptr<DerivedEngine> engine, unsigned short int port,
               std::function<RouteId(const std::vector<uint8_t>&)> router)
        : BaseAdapter<Context, DerivedEngine>(engine),
          ioc_(engine->get_ioc()),
          socket_(engine->get_ioc()),
          router_(std::move(router)),
          recv_buffer_(65536) {  // Maximum safe UDP payload size

        boost::system::error_code ec;

        socket_.open(udp::v4(), ec);
        if (ec) {
            std::string err = "UDP Socket open error: " + ec.message();
            spdlog::error("[UdpAdapter Error] {}", err);
            throw std::runtime_error(err);
        }

        socket_.set_option(net::socket_base::reuse_address(true), ec);
        if (ec) {
            std::string err =
                "UDP Socket set reuse_address error: " + ec.message();
            spdlog::error("[UdpAdapter Error] {}", err);
            throw std::runtime_error(err);
        }

        socket_.bind({udp::v4(), port}, ec);
        if (ec) {
            std::string err = "Bind UDP port " + std::to_string(port) +
                              " failed: " + ec.message();
            spdlog::error("[UdpAdapter Error] {}", err);
            throw std::runtime_error(err);
        }

        spdlog::info("[UdpAdapter Info] Successfully listening on UDP port {}",
                     port);
        do_receive();
    }

    template <typename F>
    void bind_context(const RouteId& id, F&& cb) {
        context_routes_[id] = [cb = std::forward<F>(cb)](
                                  const std::vector<uint8_t>& data, auto& ctx) {
            cb(data, ctx);
        };
    }

    template <typename Callable>
    void bind_event(const RouteId& id, Callable&& translator) {
        event_routes_[id] = [this,
                             translator = std::forward<Callable>(translator)](
                                const std::vector<uint8_t>& data) {
            auto event = translator(data);
            this->engine_->dispatch(event);
        };
    }
};

}  // namespace dk