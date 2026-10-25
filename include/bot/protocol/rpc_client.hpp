/**
 * @file rpc_client.hpp
 * @brief 带 Echo 追踪的异步 OneBot 11 RPC 客户端定义
 * @details 负责向 NapCat 协议端发送 API 请求（支持普通群消息、私聊消息及合并转发卡片），
 *          生成唯一 UUID (Echo) 进行双向关联，并支持回调以及 C++20 协程 (co_await) 两种消费模式。
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>

#include "bot/protocol/events.hpp"
#include "bot/protocol/actions.hpp"

namespace bot::protocol {

/**
 * @struct SendGroupMsgResult
 * @brief 发送消息 RPC 的最终执行结果
 */
struct SendGroupMsgResult {
    bool success{false};           ///< 是否发送成功 (status == "ok" 且 retcode == 0)
    int64_t message_id{0};         ///< 平台成功发送后分配的消息 ID
    int32_t retcode{0};            ///< OneBot 协议端返回码 (0 为正常)
    std::string error_message;     ///< 失败时的详细错误信息
    std::string echo;              ///< 关联原始请求的 UUID 追踪标识
};

/**
 * @brief RPC 回调函数类型定义
 */
using SendGroupMsgCallback = std::function<void(const SendGroupMsgResult&)>;

class RpcClientImpl;

/**
 * @class RpcClient
 * @brief 带有 Echo 追踪的 OneBot 11 异步 RPC 客户端
 * @details 功能特点：
 *          - 发起 API 调用时自动生成全局唯一 UUID (Echo)；
 *          - 支持回调函数触发 (Callback) 与 C++20 协程挂起/恢复 (co_await) 两种模式；
 *          - 内置超时保护，防止协议端未回包时协程永久悬挂；
 *          - 提供前置拦截匹配，消费对应的 RPC 回包并防止事件污染。
 */
class RpcClient {
public:
    using SendFunction = std::function<void(std::string)>;

    /**
     * @brief 构造 RPC 客户端
     * @param ioc 异步 I/O 上下文引用
     * @param send_fn 底层报文发送函数 (通常绑定到 WsServer::send)
     */
    explicit RpcClient(boost::asio::io_context& ioc, SendFunction send_fn);

    /**
     * @brief 析构函数
     */
    ~RpcClient();

    RpcClient(const RpcClient&) = delete;
    RpcClient& operator=(const RpcClient&) = delete;
    RpcClient(RpcClient&&) noexcept;
    RpcClient& operator=(RpcClient&&) noexcept;

    /**
     * @brief 发送群消息 (回调模式)
     * @param group_id 目标群号
     * @param message 消息内容
     * @param callback 收到回包时的回调 (可选)
     * @return std::string 生成的唯一 Echo (UUID)
     */
    std::string send_group_msg(int64_t group_id,
                               const std::string& message,
                               SendGroupMsgCallback callback = nullptr);

    /**
     * @brief 发送私聊消息 (回调模式)
     * @param user_id 目标用户 QQ 号
     * @param message 消息内容
     * @param callback 收到回包时的回调 (可选)
     * @return std::string 生成的唯一 Echo (UUID)
     */
    std::string send_private_msg(int64_t user_id,
                                 const std::string& message,
                                 SendGroupMsgCallback callback = nullptr);

    /**
     * @brief 发送群消息 (C++20 协程挂起/恢复模式)
     * @param group_id 目标群号
     * @param message 消息内容
     * @param timeout 超时时间 (默认 5 秒)
     * @return boost::asio::awaitable<SendGroupMsgResult> 协程等待的结果对象
     */
    boost::asio::awaitable<SendGroupMsgResult> send_group_msg_async(
        int64_t group_id,
        const std::string& message,
        std::chrono::milliseconds timeout = std::chrono::seconds(5)
    );

    /**
     * @brief 发送群合并转发卡片消息 (回调模式)
     * @param group_id 目标群号
     * @param nodes 转发节点列表
     * @param callback 收到回包时的回调 (可选)
     * @return std::string 生成的唯一 Echo (UUID)
     */
    std::string send_group_forward_msg(int64_t group_id,
                                       const std::vector<ForwardNode>& nodes,
                                       SendGroupMsgCallback callback = nullptr);

    /**
     * @brief 发送群合并转发卡片消息 (C++20 协程挂起/恢复模式)
     * @param group_id 目标群号
     * @param nodes 转发节点列表
     * @param timeout 超时时间 (默认 5 秒)
     * @return boost::asio::awaitable<SendGroupMsgResult> 协程等待的结果对象
     */
    boost::asio::awaitable<SendGroupMsgResult> send_group_forward_msg_async(
        int64_t group_id,
        const std::vector<ForwardNode>& nodes,
        std::chrono::milliseconds timeout = std::chrono::seconds(5)
    );

    /**
     * @brief 尝试处理接收到的文本报文
     * @param raw_json 收到的原始 JSON 字符串
     * @return true 如果该报文包含当前待匹配的 echo 并且已被成功处理
     * @return false 如果该报文不是当前待匹配的 RPC 响应
     */
    bool handle_response(const std::string& raw_json);

    /**
     * @brief 当前正在等待回包的 RPC 请求数量
     */
    size_t pending_count() const;

private:
    std::shared_ptr<RpcClientImpl> impl_;
};

} // namespace bot::protocol
