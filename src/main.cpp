/**
 * @file main.cpp
 * @brief QQ 机器人主程序入口
 * @details 负责构建 Boost.Asio I/O 上下文、启动 WebSocket 服务端、挂载带 Echo 追踪的 RPC 客户端、
 *          装配并启动 MusicModule 业务模块、调度 C++20 协程消费流水线以及处理 SIGINT/SIGTERM 优雅停机。
 */

#include <iostream>
#include <string>
#include <chrono>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/as_tuple.hpp>
#include <glaze/glaze.hpp>

#include "bot/core/ws_server.hpp"
#include "bot/protocol/events.hpp"
#include "bot/protocol/actions.hpp"
#include "bot/protocol/rpc_client.hpp"
#include "bot/modules/music/music_module.hpp"

namespace net = boost::asio;

/**
 * @struct HeartbeatEvent
 * @brief OneBot 11 元事件 (心跳) 反射结构体
 */
struct HeartbeatEvent {
    std::string post_type;       ///< "meta_event"
    std::string meta_event_type; ///< "heartbeat"
    int64_t time{0};             ///< 事件时间戳
    int64_t self_id{0};          ///< 机器人 QQ 号
    int64_t interval{0};         ///< 心跳间隔 (毫秒)
};

/**
 * @brief 异步处理群消息业务逻辑 (在独立协程中运行，绝不阻塞主消息接收循环)
 * @param event 群消息事件对象
 * @param rpc_client RPC 客户端引用
 * @param music_module 音乐模块引用
 * @return net::awaitable<void>
 */
net::awaitable<void> handle_group_message(
    bot::protocol::GroupMessageEvent event,
    bot::protocol::RpcClient& rpc_client,
    bot::module::music::MusicModule& music_module)
{
    std::cout << "[Group " << event.group_id << "] "
              << event.sender.nickname << "(" << event.user_id << ") "
              << "[msg_id:" << event.message_id << "]: "
              << event.raw_message << std::endl;

    // 1. 优先交由音乐模块处理指令 (/关注, /取关, /关注列表, 序号选择)
    if (music_module.handle_group_message(event)) {
        co_return;
    }

    // 2. 基础测试指令：ping -> pong
    if (event.raw_message == "ping" || event.raw_message == "/ping") {
        std::cout << "[RPC] Invoking co_await rpc_client.send_group_msg_async for group "
                  << event.group_id << "..." << std::endl;

        auto start = std::chrono::steady_clock::now();

        auto rpc_result = co_await rpc_client.send_group_msg_async(
            event.group_id,
            "pong"
        );

        auto rtt = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start
        ).count();

        if (rpc_result.success) {
            std::cout << "[RPC Success] Group message delivered!"
                      << " -> Sent message_id: " << rpc_result.message_id
                      << " | Echo: " << rpc_result.echo
                      << " (RTT: " << rtt << "ms)" << std::endl;
        } else {
            std::cerr << "[RPC Failed] Error: " << rpc_result.error_message
                      << " | retcode: " << rpc_result.retcode
                      << " | Echo: " << rpc_result.echo << std::endl;
        }
    }

    co_return;
}

/**
 * @brief C++20 协程消费者：从分发通道持续消费业务报文 (非阻塞流水线)
 * @param channel 异步分发通道
 * @param server WebSocket 服务端引用
 * @param rpc_client RPC 客户端引用
 * @param music_module 音乐模块引用
 * @return net::awaitable<void>
 */
net::awaitable<void> message_consumer(
    std::shared_ptr<bot::core::MessageChannel> channel,
    bot::core::WsServer& server,
    bot::protocol::RpcClient& rpc_client,
    bot::module::music::MusicModule& music_module)
{
    std::cout << "[Consumer] Coroutine started, listening on dispatch channel..." << std::endl;
    auto executor = co_await net::this_coro::executor;

    while (true) {
        auto [ec, raw_json] = co_await channel->async_receive(net::as_tuple(net::use_awaitable));
        if (ec) {
            std::cout << "[Consumer] Channel closed or error: " << ec.message() << std::endl;
            break;
        }

        // 1. 心跳事件处理
        if (raw_json.find("\"heartbeat\"") != std::string::npos) {
            HeartbeatEvent hb;
            if (!glz::read<bot::protocol::glaze_opts>(hb, raw_json)) {
                if (hb.self_id > 0) {
                    rpc_client.set_self_id(hb.self_id);
                }
                std::cout << "[Heartbeat] Self ID: " << hb.self_id 
                          << " | Interval: " << hb.interval << "ms | Connection Healthy." << std::endl;
            }
            continue;
        }

        // 2. 解析群消息事件 (派发给独立协程，保持主循环即时接收)
        if (auto group_event = bot::protocol::GroupMessageEvent::parse(raw_json)) {
            if (group_event->self_id > 0) {
                rpc_client.set_self_id(group_event->self_id);
            }
            net::co_spawn(executor, handle_group_message(*group_event, rpc_client, music_module), net::detached);
            continue;
        }

        // 3. 解析私聊消息事件
        if (auto priv_event = bot::protocol::PrivateMessageEvent::parse(raw_json)) {
            if (priv_event->self_id > 0) {
                rpc_client.set_self_id(priv_event->self_id);
            }
            std::cout << "[Private] "
                      << priv_event->sender.nickname << "(" << priv_event->user_id << ") "
                      << "[msg_id:" << priv_event->message_id << "]: "
                      << priv_event->raw_message << std::endl;

            // 派发给音乐模块处理管理员管理与扫码登录等私聊交互
            if (music_module.handle_private_message(*priv_event)) {
                continue;
            }

            if (priv_event->raw_message == "ping" || priv_event->raw_message == "/ping") {
                bot::protocol::SendPrivateMsgAction reply;
                reply.params.user_id = priv_event->user_id;
                reply.params.message = "pong";
                if (auto json = glz::write_json(reply)) {
                    server.send(*json);
                    std::cout << "[Action] Replied pong to user " << priv_event->user_id << std::endl;
                }
            }
            continue;
        }

        // 4. 解析群表情回应事件 (点赞/踩)
        if (auto reaction_event = bot::protocol::GroupReactionEvent::parse(raw_json)) {
            if (reaction_event->self_id > 0) {
                rpc_client.set_self_id(reaction_event->self_id);
            }
            music_module.handle_group_reaction(*reaction_event);
            continue;
        }

        // 5. 其他类型通知与请求报文
        std::cout << "[Incoming] " << (raw_json.size() > 140 ? raw_json.substr(0, 140) + "..." : raw_json) << std::endl;
    }
}

int main() {
    std::cout << "==================================================" << std::endl;
    std::cout << "QQ Bot Modern C++ (Beast + Glaze + MusicModule)" << std::endl;
    std::cout << "==================================================" << std::endl;

    try {
        net::io_context ioc;

        // 1. 创建 WebSocket 服务端
        bot::core::WsServer server(ioc, 3001, "0.0.0.0");

        // 2. 创建带 Echo 追踪的 RPC 客户端
        bot::protocol::RpcClient rpc_client(ioc, [&](std::string msg) {
            server.send(std::move(msg));
        });

        // 支持通过环境变量预设机器人自身 QQ 号 (用于展示自身头像)
        const char* env_qq = std::getenv("BOT_QQ");
        if (!env_qq) env_qq = std::getenv("QQ_BOT_UIN");
        if (!env_qq) env_qq = std::getenv("BOT_UIN");
        if (env_qq && *env_qq) {
            try {
                int64_t qqid = std::stoll(env_qq);
                if (qqid > 0) {
                    rpc_client.set_self_id(qqid);
                    std::cout << "[Main] Initialized Bot self ID from environment: " << qqid << std::endl;
                }
            } catch (...) {}
        }

        // 3. 创建并启动音乐订阅监控模块 (存储路径 data/bot_music.db，默认轮询周期 20 分钟)
        bot::module::music::MusicModule music_module(ioc, rpc_client, "data/bot_music.db", std::chrono::minutes(20));
        if (!music_module.start()) {
            std::cerr << "[Main] Warning: MusicModule failed to initialize" << std::endl;
        }

        // 4. 注册前置拦截器：在报文到达底层第一瞬间由 RPC 客户端优先拦截并唤醒挂起的协程
        server.set_message_interceptor([&](const std::string& raw_msg) -> bool {
            return rpc_client.handle_response(raw_msg);
        });

        // 5. 挂载 C++20 协程消费者
        auto channel = server.dispatch_channel();
        net::co_spawn(ioc, message_consumer(channel, server, rpc_client, music_module), net::detached);

        // 6. 启动服务端
        server.start();

        // 7. 注册信号优雅退出 (Ctrl+C)
        net::signal_set signals(ioc, SIGINT, SIGTERM);
        signals.async_wait([&](auto, int signo) {
            std::cout << "\n[Main] Caught signal " << signo << ", shutting down..." << std::endl;
            music_module.stop();
            server.stop();
            ioc.stop();
        });

        // 8. 运行事件循环
        ioc.run();

    } catch (const std::exception& e) {
        std::cerr << "[Main] Fatal Exception: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "[Main] Server exited cleanly." << std::endl;
    return 0;
}
