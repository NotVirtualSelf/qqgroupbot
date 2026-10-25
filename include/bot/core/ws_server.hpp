/**
 * @file ws_server.hpp
 * @brief 基于 Boost.Beast 与 C++20 协程的高性能异步 WebSocket 服务端定义
 * @details 负责监听 NapCat (OneBot 11 协议客户端) 的反向 WebSocket 连接，
 *          提供基于 Boost.Asio Channel 的协程报文分发、RPC 前置拦截与多会话广播能力。
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/io_context.hpp>

namespace bot::core {

/**
 * @brief 异步报文分发通道类型 (基于 Boost.Asio C++20 协程通道)
 * @details 容量为 2048 的无锁/异步队列通道，供上层流水线协程通过 async_receive 持续消费收到的报文。
 */
using MessageChannel = boost::asio::experimental::channel<void(boost::system::error_code, std::string)>;

/**
 * @brief 回调式报文处理器类型定义
 */
using MessageHandler = std::function<void(std::string)>;

/**
 * @brief 报文前置拦截器类型定义
 * @details 在报文推入分发通道之前执行。若返回 true，表示该报文已被上层组件消费（例如 RPC 回包），
 *          无需再推入通道广播，从而避免事件污染。
 */
using MessageInterceptor = std::function<bool(const std::string&)>;

class WsServerImpl;

/**
 * @class WsServer
 * @brief 基于 Boost.Beast 与 C++20 协程的高性能异步 WebSocket 服务端
 * @details 采用 PIMPL 模式封装底层实现细节，提供线程安全的会话管理与消息收发接口。
 */
class WsServer {
public:
    /**
     * @brief 构造 WebSocket 服务端
     * @param ioc 异步 I/O 上下文引用
     * @param port 监听端口号（默认 3001）
     * @param host 监听主机地址（默认 "0.0.0.0"）
     */
    explicit WsServer(boost::asio::io_context& ioc, uint16_t port = 3001, std::string host = "0.0.0.0");

    /**
     * @brief 析构函数，释放底层服务端资源
     */
    ~WsServer();

    WsServer(const WsServer&) = delete;
    WsServer& operator=(const WsServer&) = delete;
    WsServer(WsServer&&) noexcept;
    WsServer& operator=(WsServer&&) noexcept;

    /**
     * @brief 启动监听服务 (以后台协程方式运行，不阻塞调用线程)
     */
    void start();

    /**
     * @brief 停止服务并优雅断开所有当前活跃的客户端连接
     */
    void stop();

    /**
     * @brief 协程监听主循环
     * @details 供在外部协程中 co_await server.run() 形式使用
     * @return boost::asio::awaitable<void>
     */
    boost::asio::awaitable<void> run();

    /**
     * @brief 获取报文分发通道引用
     * @return std::shared_ptr<MessageChannel> 异步消费通道
     */
    std::shared_ptr<MessageChannel> dispatch_channel();

    /**
     * @brief 设置回调函数形式的全局报文处理器
     * @param handler 报文处理回调
     */
    void set_message_handler(MessageHandler handler);

    /**
     * @brief 设置前置拦截器 (如 RPC 异步回包过滤)
     * @param interceptor 拦截器函数
     */
    void set_message_interceptor(MessageInterceptor interceptor);

    /**
     * @brief 向已连接的客户端发送文本报文 (如 OneBot 11 API 请求)
     * @param message 待发送的文本内容 (通常为 JSON 字符串)
     */
    void send(std::string message);

    /**
     * @brief 获取当前活动客户端连接数量
     * @return size_t 客户端连接数
     */
    size_t connection_count() const;

    /**
     * @brief 获取当前服务监听端口
     * @return uint16_t 端口号
     */
    uint16_t port() const;

    /**
     * @brief 获取当前服务监听主机地址
     * @return const std::string& 主机地址
     */
    const std::string& host() const;

private:
    std::shared_ptr<WsServerImpl> impl_; ///< 底层 PIMPL 实现指针
};

} // namespace bot::core
