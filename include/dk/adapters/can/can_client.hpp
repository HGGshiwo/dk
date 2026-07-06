#ifndef CAN_CLIENT_H
#define CAN_CLIENT_H

#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

// SocketCAN 专属头文件 (仅限 Linux)
#include <linux/can.h>
#include <linux/can/raw.h>

class CanClient {
   public:
    // 构造函数：传入 CAN 接口名，例如 "can0" 或 "vcan0"
    CanClient(const std::string& iface_name) {
        // 1. 创建 SocketCAN 原生套接字
        sockfd_ = socket(PF_CAN, SOCK_RAW, CAN_RAW);
        if (sockfd_ < 0) {
            throw std::runtime_error("Failed to create CAN socket");
        }

        // 2. 获取网络接口的索引 (将 "can0" 转为系统能识别的 index)
        struct ifreq ifr;
        std::strncpy(ifr.ifr_name, iface_name.c_str(), IFNAMSIZ - 1);
        if (ioctl(sockfd_, SIOCGIFINDEX, &ifr) < 0) {
            close(sockfd_);
            throw std::runtime_error("Failed to find CAN interface: " +
                                     iface_name);
        }

        // 3. 绑定 Socket 到指定的 CAN 接口
        struct sockaddr_can addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.can_family = AF_CAN;
        addr.can_ifindex = ifr.ifr_ifindex;

        if (bind(sockfd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sockfd_);
            throw std::runtime_error("Failed to bind CAN socket");
        }
    }

    ~CanClient() {
        if (sockfd_ >= 0) {
            close(sockfd_);
        }
    }

    // 禁用拷贝
    CanClient(const CanClient&) = delete;
    CanClient& operator=(const CanClient&) = delete;

    // --- 发送数据接口 ---

    // 专门针对 CAN 帧的发送函数
    ssize_t send_frame(uint32_t can_id, const uint8_t* data, uint8_t dlc) {
        if (dlc > 8) return -1;

        struct can_frame frame;
        std::memset(&frame, 0, sizeof(frame));

        // 【关键排查点】：确定是标准帧还是扩展帧
        // 如果你的 can_id 大于 0x7FF (即扩展帧)，需要打上标志位：
        if (can_id > 0x7FF) {
            frame.can_id = can_id | CAN_EFF_FLAG;
        } else {
            frame.can_id = can_id;
        }

        frame.can_dlc = dlc;
        std::memcpy(frame.data, data, dlc);

        ssize_t sent_bytes = write(sockfd_, &frame, sizeof(struct can_frame));

        // 【关键排查点】：打印详细错误信息
        if (sent_bytes < 0) {
            std::cerr << "Write failed! errno: " << errno << " ("
                      << std::strerror(errno) << ")" << std::endl;
        } else if (sent_bytes != sizeof(struct can_frame)) {
            std::cerr << "Incomplete write!" << std::endl;
        }

        return sent_bytes;
    }

   private:
    int sockfd_;
};

#endif  // CAN_CLIENT_H