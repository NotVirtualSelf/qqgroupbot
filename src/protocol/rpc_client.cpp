/**
 * @file rpc_client.cpp
 * @brief 带 Echo 追踪的异步 OneBot 11 RPC 客户端实现
 * @details 实现了基于 UUID 的请求-响应关联匹配、回调与协程双通道唤醒及超时定时器。
 */

#include "bot/protocol/rpc_client.hpp"

#include <iostream>
#include <mutex>
#include <unordered_map>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <glaze/glaze.hpp>

namespace bot::protocol {

namespace net = boost::asio;

using ResultChannel = net::experimental::channel<void(boost::system::error_code, SendGroupMsgResult)>;

struct PendingEntry {
    SendGroupMsgCallback callback;
    std::shared_ptr<ResultChannel> channel;
};

class RpcClientImpl : public std::enable_shared_from_this<RpcClientImpl> {
public:
    RpcClientImpl(net::io_context& ioc, RpcClient::SendFunction send_fn)
        : ioc_(ioc)
        , send_fn_(std::move(send_fn))
    {}

    std::string generate_uuid() {
        std::lock_guard<std::mutex> lock(uuid_mutex_);
        return boost::uuids::to_string(uuid_gen_());
    }

    std::string send_group_msg(int64_t group_id,
                               const std::string& message,
                               SendGroupMsgCallback callback) {
        std::string echo = generate_uuid();

        if (callback) {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_requests_[echo] = PendingEntry{.callback = std::move(callback), .channel = nullptr};
        }

        SendGroupMsgAction action{
            .params = {
                .group_id = group_id,
                .message = message,
                .auto_escape = false
            },
            .echo = echo
        };

        if (auto json = glz::write_json(action)) {
            send_fn_(*json);
        } else {
            std::cerr << "[RpcClient] Failed to serialize SendGroupMsgAction" << std::endl;
        }

        return echo;
    }

    std::string send_private_msg(int64_t user_id,
                                 const std::string& message,
                                 SendGroupMsgCallback callback) {
        std::string echo = generate_uuid();

        if (callback) {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_requests_[echo] = PendingEntry{.callback = std::move(callback), .channel = nullptr};
        }

        SendPrivateMsgAction action{
            .params = {
                .user_id = user_id,
                .message = message,
                .auto_escape = false
            },
            .echo = echo
        };

        if (auto json = glz::write_json(action)) {
            send_fn_(*json);
        } else {
            std::cerr << "[RpcClient] Failed to serialize SendPrivateMsgAction" << std::endl;
        }

        return echo;
    }

    net::awaitable<SendGroupMsgResult> send_group_msg_async(
        int64_t group_id,
        const std::string& message,
        std::chrono::milliseconds timeout)
    {
        auto executor = co_await net::this_coro::executor;
        std::string echo = generate_uuid();
        auto channel = std::make_shared<ResultChannel>(executor, 1);

        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_requests_[echo] = PendingEntry{.callback = nullptr, .channel = channel};
        }

        SendGroupMsgAction action{
            .params = {
                .group_id = group_id,
                .message = message,
                .auto_escape = false
            },
            .echo = echo
        };

        if (auto json = glz::write_json(action)) {
            send_fn_(*json);
        } else {
            unregister_pending(echo);
            co_return SendGroupMsgResult{
                .success = false,
                .message_id = 0,
                .retcode = -3,
                .error_message = "Serialization failure",
                .echo = echo
            };
        }

        // 定时器实现超时控制
        auto timer = std::make_shared<net::steady_timer>(executor, timeout);
        timer->async_wait([this, echo, channel](const boost::system::error_code& ec) {
            if (!ec) {
                // 超时触发，取消并唤醒挂起的协程
                SendGroupMsgResult timeout_res{
                    .success = false,
                    .message_id = 0,
                    .retcode = -1,
                    .error_message = "RPC timeout: no response received from NapCat",
                    .echo = echo
                };
                channel->try_send(boost::system::error_code{}, timeout_res);
            }
        });

        // 挂起当前协程，等待回包到达或超时唤醒
        auto [ec, result] = co_await channel->async_receive(net::as_tuple(net::use_awaitable));
        timer->cancel();
        unregister_pending(echo);

        if (ec) {
            co_return SendGroupMsgResult{
                .success = false,
                .message_id = 0,
                .retcode = -2,
                .error_message = "Channel read error: " + ec.message(),
                .echo = echo
            };
        }

        co_return result;
    }

    std::string send_group_forward_msg(int64_t group_id,
                                       const std::vector<ForwardNode>& nodes,
                                       SendGroupMsgCallback callback) {
        std::string echo = generate_uuid();

        if (callback) {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_requests_[echo] = PendingEntry{.callback = std::move(callback), .channel = nullptr};
        }

        SendGroupForwardMsgAction action{
            .params = {
                .group_id = group_id,
                .messages = nodes
            },
            .echo = echo
        };

        if (auto json = glz::write_json(action)) {
            send_fn_(*json);
        } else {
            std::cerr << "[RpcClient] Failed to serialize SendGroupForwardMsgAction" << std::endl;
        }

        return echo;
    }

    net::awaitable<SendGroupMsgResult> send_group_forward_msg_async(
        int64_t group_id,
        const std::vector<ForwardNode>& nodes,
        std::chrono::milliseconds timeout)
    {
        auto executor = co_await net::this_coro::executor;
        std::string echo = generate_uuid();
        auto channel = std::make_shared<ResultChannel>(executor, 1);

        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_requests_[echo] = PendingEntry{.callback = nullptr, .channel = channel};
        }

        SendGroupForwardMsgAction action{
            .params = {
                .group_id = group_id,
                .messages = nodes
            },
            .echo = echo
        };

        if (auto json = glz::write_json(action)) {
            send_fn_(*json);
        } else {
            unregister_pending(echo);
            co_return SendGroupMsgResult{
                .success = false,
                .message_id = 0,
                .retcode = -3,
                .error_message = "Serialization failure",
                .echo = echo
            };
        }

        auto timer = std::make_shared<net::steady_timer>(executor, timeout);
        timer->async_wait([this, echo, channel](const boost::system::error_code& ec) {
            if (!ec) {
                SendGroupMsgResult timeout_res{
                    .success = false,
                    .message_id = 0,
                    .retcode = -1,
                    .error_message = "RPC timeout: no response received from NapCat",
                    .echo = echo
                };
                channel->try_send(boost::system::error_code{}, timeout_res);
            }
        });

        auto [ec, result] = co_await channel->async_receive(net::as_tuple(net::use_awaitable));
        timer->cancel();
        unregister_pending(echo);

        if (ec) {
            co_return SendGroupMsgResult{
                .success = false,
                .message_id = 0,
                .retcode = -2,
                .error_message = "Channel read error: " + ec.message(),
                .echo = echo
            };
        }

        co_return result;
    }

    bool handle_response(const std::string& raw_json) {
        if (raw_json.find("\"echo\"") == std::string::npos) {
            return false;
        }

        RawApiResponse resp;
        auto ec = glz::read<glaze_opts>(resp, raw_json);
        if (ec || resp.echo.empty()) {
            return false;
        }

        PendingEntry entry;
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            auto it = pending_requests_.find(resp.echo);
            if (it == pending_requests_.end()) {
                return false;
            }
            entry = std::move(it->second);
            pending_requests_.erase(it);
        }

        SendGroupMsgResult result{
            .success = (resp.status == "ok" && resp.retcode == 0),
            .message_id = resp.data ? resp.data->message_id : 0,
            .retcode = resp.retcode,
            .error_message = resp.message.empty() ? resp.wording : resp.message,
            .echo = resp.echo
        };

        if (entry.callback) {
            try {
                entry.callback(result);
            } catch (const std::exception& e) {
                std::cerr << "[RpcClient] Exception in callback for echo " << resp.echo << ": " << e.what() << std::endl;
            }
        }

        if (entry.channel) {
            entry.channel->try_send(boost::system::error_code{}, result);
        }

        return true;
    }

    void set_self_id(int64_t self_id) {
        if (self_id > 0) {
            self_id_.store(self_id);
        }
    }

    int64_t self_id() const {
        return self_id_.load();
    }

    std::string self_uin() const {
        int64_t id = self_id_.load();
        return id > 0 ? std::to_string(id) : "";
    }

    size_t pending_count() const {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        return pending_requests_.size();
    }

private:
    void unregister_pending(const std::string& echo) {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_requests_.erase(echo);
    }

    net::io_context& ioc_;
    RpcClient::SendFunction send_fn_;

    boost::uuids::random_generator uuid_gen_;
    std::mutex uuid_mutex_;

    mutable std::mutex pending_mutex_;
    std::unordered_map<std::string, PendingEntry> pending_requests_;
    std::atomic<int64_t> self_id_{0};
};

RpcClient::RpcClient(net::io_context& ioc, SendFunction send_fn)
    : impl_(std::make_shared<RpcClientImpl>(ioc, std::move(send_fn)))
{}

RpcClient::~RpcClient() = default;
RpcClient::RpcClient(RpcClient&&) noexcept = default;
RpcClient& RpcClient::operator=(RpcClient&&) noexcept = default;

std::string RpcClient::send_group_msg(int64_t group_id,
                                     const std::string& message,
                                     SendGroupMsgCallback callback) {
    return impl_->send_group_msg(group_id, message, std::move(callback));
}

std::string RpcClient::send_private_msg(int64_t user_id,
                                       const std::string& message,
                                       SendGroupMsgCallback callback) {
    return impl_->send_private_msg(user_id, message, std::move(callback));
}

boost::asio::awaitable<SendGroupMsgResult> RpcClient::send_group_msg_async(
    int64_t group_id,
    const std::string& message,
    std::chrono::milliseconds timeout) {
    return impl_->send_group_msg_async(group_id, message, timeout);
}

std::string RpcClient::send_group_forward_msg(int64_t group_id,
                                             const std::vector<ForwardNode>& nodes,
                                             SendGroupMsgCallback callback) {
    return impl_->send_group_forward_msg(group_id, nodes, std::move(callback));
}

boost::asio::awaitable<SendGroupMsgResult> RpcClient::send_group_forward_msg_async(
    int64_t group_id,
    const std::vector<ForwardNode>& nodes,
    std::chrono::milliseconds timeout) {
    return impl_->send_group_forward_msg_async(group_id, nodes, timeout);
}

bool RpcClient::handle_response(const std::string& raw_json) {
    return impl_->handle_response(raw_json);
}

size_t RpcClient::pending_count() const {
    return impl_->pending_count();
}

void RpcClient::set_self_id(int64_t self_id) {
    impl_->set_self_id(self_id);
}

int64_t RpcClient::self_id() const {
    return impl_->self_id();
}

std::string RpcClient::self_uin() const {
    return impl_->self_uin();
}

} // namespace bot::protocol
