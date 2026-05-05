#pragma once
#include <boost/exception/all.hpp>
#include <boost/stacktrace.hpp>
#include <sstream>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#define BOOST_STACKTRACE_USE_WINDBG
#else
#define BOOST_STACKTRACE_USE_ADDR2LINE
#endif

namespace dk {
// 1. 定义 Boost 异常携带属性的 Tag
using stacktrace_tag = boost::error_info<struct tag_stacktrace, boost::stacktrace::stacktrace>;

// 2. 定义我们的优雅异常基类
class TraceableException : public virtual std::runtime_error, public virtual boost::exception {
   public:
    // 构造函数：接收错误信息，并【自动】捕获当前调用栈
    explicit TraceableException(const std::string& msg) : std::runtime_error(msg) {
        // 将当前的调用栈附加到 Boost 异常对象中
        *this << stacktrace_tag(boost::stacktrace::stacktrace());
    }

    // 类似 Python 的 format_exc() 方法，返回完整的字符串
    std::string format_exc() const {
        std::ostringstream oss;

        // 提取错误信息
        oss << "Exception: " << this->what() << "\n";

        // 提取并格式化调用栈
        const boost::stacktrace::stacktrace* st = boost::get_error_info<stacktrace_tag>(*this);
        if (st) {
            oss << "Traceback (most recent call last):\n" << *st;
        } else {
            oss << "Traceback: [No stacktrace available]\n";
        }

        return oss.str();
    }
};
}  // namespace dk