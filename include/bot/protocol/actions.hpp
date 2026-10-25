/**
 * @file actions.hpp
 * @brief OneBot 11 协议标准 API 动作请求结构体与参数定义
 * @details 包含发送群消息、发送私聊消息、发送合并转发消息等常用 API 请求封装。
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bot::protocol {

/**
 * @struct SendGroupMsgParams
 * @brief send_group_msg API 参数结构体
 */
struct SendGroupMsgParams {
    int64_t group_id{0};         ///< 目标群号
    std::string message;         ///< 消息内容 (支持 CQ 码)
    bool auto_escape{false};     ///< 是否作为纯文本发送 (true 不解析 CQ 码)
};

/**
 * @struct SendGroupMsgAction
 * @brief send_group_msg 动作请求结构体
 */
struct SendGroupMsgAction {
    std::string action = "send_group_msg"; ///< OneBot 11 API 动作名
    SendGroupMsgParams params;             ///< 动作参数
    std::string echo;                      ///< 异步回包追踪标识 (UUID)
};

/**
 * @struct SendPrivateMsgParams
 * @brief send_private_msg API 参数结构体
 */
struct SendPrivateMsgParams {
    int64_t user_id{0};          ///< 目标私聊用户 QQ 号
    std::string message;         ///< 消息内容 (支持 CQ 码)
    bool auto_escape{false};     ///< 是否作为纯文本发送
};

/**
 * @struct SendPrivateMsgAction
 * @brief send_private_msg 动作请求结构体
 */
struct SendPrivateMsgAction {
    std::string action = "send_private_msg"; ///< OneBot 11 API 动作名
    SendPrivateMsgParams params;               ///< 动作参数
    std::string echo;                        ///< 异步回包追踪标识 (UUID)
};

/**
 * @struct ForwardNodeData
 * @brief 合并转发自定义节点数据结构
 */
struct ForwardNodeData {
    std::string name;            ///< 转发气泡显示的发送者昵称
    std::string uin;             ///< 转发气泡显示的发送者 QQ 号
    std::string content;         ///< 节点文本内容 (支持 [CQ:image,file=...] 等 CQ 码)
};

/**
 * @struct ForwardNode
 * @brief 合并转发消息节点定义
 */
struct ForwardNode {
    std::string type = "node";   ///< 节点类型 (固定为 "node")
    ForwardNodeData data;        ///< 节点详细内容
};

/**
 * @struct SendGroupForwardMsgParams
 * @brief send_group_forward_msg API 参数结构体
 */
struct SendGroupForwardMsgParams {
    int64_t group_id{0};                 ///< 目标群号
    std::vector<ForwardNode> messages;   ///< 合并转发自定义节点列表
};

/**
 * @struct SendGroupForwardMsgAction
 * @brief send_group_forward_msg 动作请求结构体
 */
struct SendGroupForwardMsgAction {
    std::string action = "send_group_forward_msg"; ///< OneBot 11 API 动作名
    SendGroupForwardMsgParams params;                ///< 动作参数
    std::string echo;                                ///< 异步回包追踪标识 (UUID)
};

} // namespace bot::protocol
