/**
 * @file ws_server.cpp
 * @brief 基于 Boost.Beast 与 C++20 协程的异步 WebSocket 服务端实现
 * @details 处理客户端连接生命周期、握手、并发会话管理、读写队列与信道分发。
 */

#include "bot/core/ws_server.hpp"

#include <iostream>
#include <mutex>
#include <unordered_set>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

namespace bot::core {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;

class WsSession;

class WsServerImpl : public std::enable_shared_from_this<WsServerImpl> {
public:
    WsServerImpl(net::io_context& ioc, uint16_t port, std::string host)
        : ioc_(ioc)
        , port_(port)
        , host_(std::move(host))
        , endpoint_(net::ip::make_address(host_), port_)
        , dispatch_channel_(std::make_shared<MessageChannel>(ioc, 2048))
    {}

    ~WsServerImpl();

    void start();
    void stop();
    net::awaitable<void> run();

    void add_session(std::shared_ptr<WsSession> session) {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_.insert(std::move(session));
    }

    void remove_session(const std::shared_ptr<WsSession>& session) {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_.erase(session);
    }

    void set_message_handler(MessageHandler handler) {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        handler_ = std::move(handler);
    }

    MessageHandler get_message_handler() const {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        return handler_;
    }

    void set_message_interceptor(MessageInterceptor interceptor) {
        std::lock_guard<std::mutex> lock(interceptor_mutex_);
        interceptor_ = std::move(interceptor);
    }

    MessageInterceptor get_message_interceptor() const {
        std::lock_guard<std::mutex> lock(interceptor_mutex_);
        return interceptor_;
    }

    void send(const std::string& message);

    std::shared_ptr<MessageChannel> dispatch_channel() const {
        return dispatch_channel_;
    }

    size_t connection_count() const {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        return sessions_.size();
    }

    uint16_t port() const { return port_; }
    const std::string& host() const { return host_; }

private:
    net::io_context& ioc_;
    uint16_t port_;
    std::string host_;
    tcp::endpoint endpoint_;
    std::unique_ptr<tcp::acceptor> acceptor_;
    std::shared_ptr<MessageChannel> dispatch_channel_;

    mutable std::mutex handler_mutex_;
    MessageHandler handler_;

    mutable std::mutex interceptor_mutex_;
    MessageInterceptor interceptor_;

    mutable std::mutex sessions_mutex_;
    std::unordered_set<std::shared_ptr<WsSession>> sessions_;

    std::atomic<bool> is_running_{false};
};

/**
 * @brief WebSocket 连接会话，负责单个客户端的收发与心跳
 */
class WsSession : public std::enable_shared_from_this<WsSession> {
public:
    WsSession(tcp::socket socket,
              std::shared_ptr<MessageChannel> dispatch_channel,
              std::shared_ptr<WsServerImpl> server)
        : ws_(std::move(socket))
        , dispatch_channel_(std::move(dispatch_channel))
        , server_(server)
        , out_channel_(ws_.get_executor(), 1024)
    {
        boost::system::error_code ec;
        auto ep = ws_.next_layer().socket().remote_endpoint(ec);
        if (!ec) {
            client_id_ = ep.address().to_string() + ":" + std::to_string(ep.port());
        } else {
            client_id_ = "unknown";
        }
    }

    void start() {
        net::co_spawn(ws_.get_executor(), session_task(), net::detached);
    }

    void close() {
        if (!is_alive_.exchange(false)) {
            return;
        }

        out_channel_.close();
        boost::system::error_code ec;
        beast::get_lowest_layer(ws_).socket().close(ec);
    }

    void send(const std::string& msg) {
        if (!is_alive_) return;
        out_channel_.try_send(boost::system::error_code{}, msg);
    }

    const std::string& client_id() const {
        return client_id_;
    }

private:
    net::awaitable<void> session_task() {
        auto self = shared_from_this();

        // 1. 心跳配置：设置超时与自动 Keep-Alive Ping
        websocket::stream_base::timeout opt{
            std::chrono::seconds(20),  // 握手超时
            std::chrono::seconds(60),  // 空闲超时 (自动发送 PING 保持长连接)
            true                       // keep_alive_pings = true (开启底层 ping/pong 保持心跳)
        };
        ws_.set_option(opt);

        // 2. 帧控制回调：监听 WebSocket 底层 ping/pong 状态
        ws_.control_callback([this](websocket::frame_type kind, beast::string_view) {
            if (kind == websocket::frame_type::ping) {
                // 收到 NapCat 客户端发来的底层 PING，Beast 会自动回复 PONG
            } else if (kind == websocket::frame_type::pong) {
                // 收到客户端对我们 PING 的 PONG 响应，长连接健康
            }
        });

        // 3. 装饰响应头 (标识服务端名称)
        ws_.set_option(websocket::stream_base::decorator([](websocket::response_type& res) {
            res.set(beast::http::field::server, "QQBot-Modern-Cpp/1.0");
        }));

        // 4. 执行异步 WebSocket 握手
        auto [accept_ec] = co_await ws_.async_accept(net::as_tuple(net::use_awaitable));
        if (accept_ec) {
            std::cerr << "[WsSession] Handshake error from " << client_id_ << ": " << accept_ec.message() << std::endl;
            cleanup();
            co_return;
        }

        std::cout << "[WsSession] NapCat client connected successfully! [" << client_id_ << "]" << std::endl;

        // 5. 启动并发写协程
        net::co_spawn(ws_.get_executor(), writer_task(), net::detached);

        // 6. 读主循环
        beast::flat_buffer buffer;
        while (is_alive_) {
            auto [read_ec, bytes_transferred] = co_await ws_.async_read(buffer, net::as_tuple(net::use_awaitable));
            if (read_ec) {
                if (read_ec == websocket::error::closed) {
                    std::cout << "[WsSession] Client disconnected normally [" << client_id_ << "]" << std::endl;
                } else if (read_ec != net::error::operation_aborted) {
                    std::cerr << "[WsSession] Read error [" << client_id_ << "]: " << read_ec.message() << std::endl;
                }
                break;
            }

            // 处理接收到的文本报文 (OneBot 11 JSON)
            if (ws_.got_text()) {
                std::string message_text = beast::buffers_to_string(buffer.data());
                buffer.consume(buffer.size());

                // 优先执行前置拦截器 (如 RPC Echo 回包唤醒)
                bool intercepted = false;
                if (auto server = server_.lock()) {
                    auto interceptor = server->get_message_interceptor();
                    if (interceptor) {
                        try {
                            intercepted = interceptor(message_text);
                        } catch (const std::exception& e) {
                            std::cerr << "[WsSession] Interceptor exception: " << e.what() << std::endl;
                        }
                    }
                }

                // 若未被拦截，则正常推送到事件分发通道与业务回调
                if (!intercepted) {
                    if (dispatch_channel_) {
                        dispatch_channel_->try_send(boost::system::error_code{}, message_text);
                    }

                    if (auto server = server_.lock()) {
                        auto handler = server->get_message_handler();
                        if (handler) {
                            try {
                                handler(std::move(message_text));
                            } catch (const std::exception& e) {
                                std::cerr << "[WsSession] MessageHandler exception: " << e.what() << std::endl;
                            }
                        }
                    }
                }
            } else {
                buffer.consume(buffer.size());
            }
        }

        cleanup();
    }

    net::awaitable<void> writer_task() {
        while (is_alive_) {
            auto [ec, msg] = co_await out_channel_.async_receive(net::as_tuple(net::use_awaitable));
            if (ec) {
                break;
            }

            ws_.text(true);
            auto [write_ec, bytes] = co_await ws_.async_write(net::buffer(msg), net::as_tuple(net::use_awaitable));
            if (write_ec) {
                if (write_ec != net::error::operation_aborted) {
                    std::cerr << "[WsSession] Write error [" << client_id_ << "]: " << write_ec.message() << std::endl;
                }
                break;
            }
        }

        cleanup();
    }

    void cleanup() {
        if (!is_alive_.exchange(false)) {
            return;
        }

        out_channel_.close();

        boost::system::error_code ec;
        beast::get_lowest_layer(ws_).socket().close(ec);

        if (auto server = server_.lock()) {
            server->remove_session(shared_from_this());
        }
    }

    websocket::stream<beast::tcp_stream> ws_;
    std::shared_ptr<MessageChannel> dispatch_channel_;
    std::weak_ptr<WsServerImpl> server_;
    net::experimental::channel<void(boost::system::error_code, std::string)> out_channel_;
    std::string client_id_;
    std::atomic<bool> is_alive_{true};
};

// 延迟定义 WsServerImpl 成员函数 (在 WsSession 完整定义之后)
WsServerImpl::~WsServerImpl() {
    stop();
}

void WsServerImpl::start() {
    if (is_running_.exchange(true)) {
        return;
    }
    net::co_spawn(ioc_, run(), net::detached);
}

void WsServerImpl::stop() {
    if (!is_running_.exchange(false)) {
        return;
    }

    boost::system::error_code ec;
    if (acceptor_ && acceptor_->is_open()) {
        acceptor_->close(ec);
    }

    std::unordered_set<std::shared_ptr<WsSession>> sessions_copy;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_copy = sessions_;
        sessions_.clear();
    }

    for (const auto& session : sessions_copy) {
        session->close();
    }

    std::cout << "[WsServer] Server stopped on " << host_ << ":" << port_ << std::endl;
}

net::awaitable<void> WsServerImpl::run() {
    is_running_ = true;
    boost::system::error_code ec;

    acceptor_ = std::make_unique<tcp::acceptor>(ioc_);
    acceptor_->open(endpoint_.protocol(), ec);
    if (ec) {
        std::cerr << "[WsServer] Failed to open acceptor: " << ec.message() << std::endl;
        is_running_ = false;
        co_return;
    }

    acceptor_->set_option(net::socket_base::reuse_address(true), ec);
    acceptor_->bind(endpoint_, ec);
    if (ec) {
        std::cerr << "[WsServer] Failed to bind to " << host_ << ":" << port_ << ": " << ec.message() << std::endl;
        is_running_ = false;
        co_return;
    }

    acceptor_->listen(net::socket_base::max_listen_connections, ec);
    if (ec) {
        std::cerr << "[WsServer] Failed to listen: " << ec.message() << std::endl;
        is_running_ = false;
        co_return;
    }

    std::cout << "[WsServer] Listening on " << host_ << ":" << port_
              << " (Awaiting NapCatQQ reverse WebSocket connections)..." << std::endl;

    while (is_running_) {
        auto [accept_ec, socket] = co_await acceptor_->async_accept(net::as_tuple(net::use_awaitable));
        if (accept_ec) {
            if (accept_ec == net::error::operation_aborted) {
                break;
            }
            std::cerr << "[WsServer] Accept error: " << accept_ec.message() << std::endl;
            continue;
        }

        auto session = std::make_shared<WsSession>(
            std::move(socket),
            dispatch_channel_,
            shared_from_this()
        );

        add_session(session);
        session->start();
    }
}

void WsServerImpl::send(const std::string& message) {
    std::unordered_set<std::shared_ptr<WsSession>> sessions_copy;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_copy = sessions_;
    }

    for (const auto& session : sessions_copy) {
        session->send(message);
    }
}

// WsServer 对外接口实现
WsServer::WsServer(net::io_context& ioc, uint16_t port, std::string host)
    : impl_(std::make_shared<WsServerImpl>(ioc, port, std::move(host)))
{}

WsServer::~WsServer() = default;
WsServer::WsServer(WsServer&&) noexcept = default;
WsServer& WsServer::operator=(WsServer&&) noexcept = default;

void WsServer::start() {
    impl_->start();
}

void WsServer::stop() {
    impl_->stop();
}

net::awaitable<void> WsServer::run() {
    return impl_->run();
}

std::shared_ptr<MessageChannel> WsServer::dispatch_channel() {
    return impl_->dispatch_channel();
}

void WsServer::set_message_handler(MessageHandler handler) {
    impl_->set_message_handler(std::move(handler));
}

void WsServer::set_message_interceptor(MessageInterceptor interceptor) {
    impl_->set_message_interceptor(std::move(interceptor));
}

void WsServer::send(std::string message) {
    impl_->send(message);
}

size_t WsServer::connection_count() const {
    return impl_->connection_count();
}

uint16_t WsServer::port() const {
    return impl_->port();
}

const std::string& WsServer::host() const {
    return impl_->host();
}

} // namespace bot::core
