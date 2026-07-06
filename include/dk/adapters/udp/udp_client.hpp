#ifndef UDP_CLIENT_H
#define UDP_CLIENT_H

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// --- 跨平台 Socket 头文件处理 ---
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
// Windows 下关闭 socket 的函数是 closesocket
#define CLOSE_SOCKET closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
// Linux/macOS 下关闭 socket 的函数是 close
#define CLOSE_SOCKET close
// 统一 Linux 下的 SOCKET 类型
typedef int SOCKET;
#define INVALID_SOCKET (SOCKET)(~0)
#define SOCKET_ERROR (-1)
#endif

class UdpClient {
   public:
    // 构造函数：传入目标 IP 和 端口
    UdpClient(const std::string& target_ip, uint16_t target_port) {
#ifdef _WIN32
        // Windows 网络环境初始化
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
#endif

        // 1. 创建 UDP Socket
        sockfd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd_ == INVALID_SOCKET) {
            throw std::runtime_error("Failed to create UDP socket");
        }

        // 2. 初始化目标地址结构体
        std::memset(&dest_addr_, 0, sizeof(dest_addr_));
        dest_addr_.sin_family = AF_INET;
        dest_addr_.sin_port = htons(target_port);

        // IP 地址转换
        if (inet_pton(AF_INET, target_ip.c_str(), &dest_addr_.sin_addr) <= 0) {
            CLOSE_SOCKET(sockfd_);
            throw std::invalid_argument("Invalid IP address format");
        }
    }

    // 析构函数：自动清理资源
    ~UdpClient() {
        if (sockfd_ != INVALID_SOCKET) {
            CLOSE_SOCKET(sockfd_);
        }
#ifdef _WIN32
        WSACleanup();
#endif
    }

    // 禁用拷贝构造和赋值操作（防止 Socket 被重复关闭）
    UdpClient(const UdpClient&) = delete;
    UdpClient& operator=(const UdpClient&) = delete;

    // --- 发送数据接口 ---

    // 1. 发送 std::vector<uint8_t> (核心需求)
    ssize_t send(const std::vector<uint8_t>& data) {
        if (data.empty()) return 0;
        return send_raw(data.data(), data.size());
    }

    // 2. 发送 std::string (便捷扩展)
    ssize_t send(const std::string& data) {
        if (data.empty()) return 0;
        return send_raw(reinterpret_cast<const uint8_t*>(data.data()),
                        data.size());
    }

    // 3. 发送原生指针数据 (底层调用)
    ssize_t send_raw(const uint8_t* data, size_t size) {
        ssize_t sent_bytes =
            sendto(sockfd_, reinterpret_cast<const char*>(data), size, 0,
                   (const struct sockaddr*)&dest_addr_, sizeof(dest_addr_));
        return sent_bytes;  // 返回实际发送的字节数，失败返回 -1 (SOCKET_ERROR)
    }

   private:
    SOCKET sockfd_;
    struct sockaddr_in dest_addr_;
};

#endif  // UDP_CLIENT_H