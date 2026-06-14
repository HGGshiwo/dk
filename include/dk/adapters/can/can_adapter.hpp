#pragma once

#include <spdlog/spdlog.h>

#include <boost/asio.hpp>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

// SocketCAN 必要的 Linux 头文件
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include "dk/adapters/base.hpp"

namespace net = boost::asio;

namespace dk {

// 注意：对于 CAN，RouteId 通常直接就是 can_id (uint32_t)，
// 所以其实可以不需要外部传入 router 函数，直接用 frame.can_id 作为路由键即可。
template <typename Context, typename DerivedEngine, typename RouteId = uint32_t>
class CanAdapter : public BaseAdapter<Context, DerivedEngine> {
   private:
    net::io_context& ioc_;

    // 核心组件：用 posix::stream_descriptor 包装原生 SocketCAN 文件描述符
    net::posix::stream_descriptor stream_;

    // 接收缓冲区不再是一块连续的 byte 数组，而是 Linux 预定义的 can_frame 结构体
    struct can_frame recv_frame_;

    using ContextHandler =
        std::function<void(const std::vector<uint8_t>&, decltype(std::declval<DerivedEngine>().get_context()))>;
    std::map<RouteId, ContextHandler> context_routes_;

    using EventHandler = std::function<void(const std::vector<uint8_t>&)>;
    std::map<RouteId, EventHandler> event_routes_;

    void do_receive() {
        // 使用 async_read_some 读取一帧 CAN 数据
        stream_.async_read_some(net::buffer(&recv_frame_, sizeof(recv_frame_)),
                                [this](boost::system::error_code ec, std::size_t bytes_recvd) {
                                    if (!ec && bytes_recvd == sizeof(struct can_frame)) {
                                        handle_packet();
                                    } else if (ec && ec != net::error::operation_aborted) {
                                        spdlog::error("[CanAdapter Error] Receive failed: {}", ec.message());
                                    }

                                    if (stream_.is_open()) {
                                        do_receive();
                                    }
                                });
    }

    void handle_packet() {
        // 1. 提取路由 ID（直接使用 CAN ID）
        // 可选：过滤掉扩展帧标志位等信息 frame.can_id & CAN_EFF_MASK
        RouteId id = static_cast<RouteId>(recv_frame_.can_id);

        // 2. 将载荷转换为 vector 传递给回调，保持与 UdpAdapter 接口一致
        std::vector<uint8_t> payload(recv_frame_.data, recv_frame_.data + recv_frame_.can_dlc);

        if (auto it = context_routes_.find(id); it != context_routes_.end()) {
            it->second(payload, this->engine_->get_context());
        }

        if (auto it = event_routes_.find(id); it != event_routes_.end()) {
            it->second(payload);
        }
    }

   public:
    // 构造函数：只需传入网卡名称，例如 "can0"
    CanAdapter(std::shared_ptr<DerivedEngine> engine, const std::string& iface_name)
        : BaseAdapter<Context, DerivedEngine>(engine), ioc_(engine->get_ioc()), stream_(engine->get_ioc()) {
        // 1. 创建原生的 Linux SocketCAN
        int fd = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
        if (fd < 0) {
            std::string err = "Failed to create PF_CAN socket";
            spdlog::error("[CanAdapter Error] {}", err);
            throw std::runtime_error(err);
        }

        // 2. 解析网卡索引
        struct ifreq ifr;
        std::strncpy(ifr.ifr_name, iface_name.c_str(), IFNAMSIZ - 1);
        if (::ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
            ::close(fd);
            std::string err = "Failed to find CAN interface: " + iface_name;
            spdlog::error("[CanAdapter Error] {}", err);
            throw std::runtime_error(err);
        }

        // 3. 绑定 SocketCAN
        struct sockaddr_can addr = {0};
        addr.can_family = AF_CAN;
        addr.can_ifindex = ifr.ifr_ifindex;

        if (::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            ::close(fd);
            std::string err = "Failed to bind CAN interface: " + iface_name;
            spdlog::error("[CanAdapter Error] {}", err);
            throw std::runtime_error(err);
        }

        // 4. 将原生 fd 交给 Boost.Asio 管理
        boost::system::error_code ec;
        stream_.assign(fd, ec);
        if (ec) {
            ::close(fd);
            std::string err = "Failed to assign stream descriptor: " + ec.message();
            spdlog::error("[CanAdapter Error] {}", err);
            throw std::runtime_error(err);
        }

        spdlog::info("[CanAdapter Info] Successfully attached to CAN interface {}", iface_name);

        // 开始异步监听
        do_receive();
    }

    // 绑定路由和回调的逻辑与 UdpAdapter 完全一致
    template <typename F>
    void bind_context(const RouteId& id, F&& cb) {
        context_routes_[id] = [cb = std::forward<F>(cb)](const std::vector<uint8_t>& data, auto& ctx) {
            cb(data, ctx);
        };
    }

    template <typename Callable>
    void bind_event(const RouteId& id, Callable&& translator) {
        event_routes_[id] = [this, translator = std::forward<Callable>(translator)](const std::vector<uint8_t>& data) {
            auto event = translator(data);
            this->engine_->dispatch(event);
        };
    }
};

}  // namespace dk