/**
 * @file events.hpp
 * @brief OneBot 11 协议事件结构体与 Glaze JSON 反射定义
 * @details 包含群消息、私聊消息、API 响应回包以及群消息表情回应事件的解析逻辑。
 */

#pragma once

#include <cstdint>
#include <string>
#include <optional>
#include <glaze/glaze.hpp>

namespace bot::protocol {

/**
 * @brief 统一使用的 Glaze 反射选项：忽略 JSON 中的未知/多余字段
 */
constexpr auto glaze_opts = glz::opts{.error_on_unknown_keys = false};

/**
 * @struct Sender
 * @brief 消息发送者详细信息
 */
struct Sender {
    int64_t user_id{0};       ///< 发送者 QQ 号
    std::string nickname;     ///< 发送者昵称
    std::string card;         ///< 群名片/备注 (群聊专有)
    std::string role;         ///< 角色: owner (群主) / admin (管理员) / member (群员)
    std::string sex;          ///< 性别: male / female / unknown
    int32_t age{0};           ///< 年龄
};

/**
 * @struct GroupMessageEvent
 * @brief 群消息事件结构体 (post_type="message", message_type="group")
 */
struct GroupMessageEvent {
    std::string post_type;       ///< 上报类型: "message"
    std::string message_type;    ///< 消息类型: "group"
    std::string sub_type;        ///< 消息子类型: "normal", "anonymous", "notice"
    int64_t time{0};             ///< 事件发生的 Unix 时间戳 (秒)
    int64_t self_id{0};          ///< 收到事件的机器人 QQ 号
    int64_t message_id{0};       ///< 消息唯一 ID
    int64_t group_id{0};         ///< 产生消息的群号
    int64_t user_id{0};          ///< 发送者 QQ 号
    std::string raw_message;     ///< 原始文本消息内容 (含 CQ 码)
    Sender sender;               ///< 发送者详细信息

    /**
     * @brief 辅助静态方法：快速尝试将 JSON 字符串解析为群消息事件
     * @param json_str 原始 JSON 字符串
     * @return std::optional<GroupMessageEvent> 解析成功返回事件对象，否则返回 std::nullopt
     */
    static std::optional<GroupMessageEvent> parse(const std::string& json_str) {
        GroupMessageEvent ev;
        auto ec = glz::read<glaze_opts>(ev, json_str);
        if (!ec && ev.post_type == "message" && ev.message_type == "group") {
            return ev;
        }
        return std::nullopt;
    }
};

/**
 * @struct PrivateMessageEvent
 * @brief 好友/私聊消息事件结构体 (post_type="message", message_type="private")
 */
struct PrivateMessageEvent {
    std::string post_type;       ///< 上报类型: "message"
    std::string message_type;    ///< 消息类型: "private"
    std::string sub_type;        ///< 消息子类型: "friend", "group", "other"
    int64_t time{0};             ///< 事件发生的 Unix 时间戳 (秒)
    int64_t self_id{0};          ///< 收到事件的机器人 QQ 号
    int64_t message_id{0};       ///< 消息唯一 ID
    int64_t user_id{0};          ///< 发送者 QQ 号
    std::string raw_message;     ///< 原始文本消息内容
    Sender sender;               ///< 发送者详细信息

    /**
     * @brief 辅助静态方法：快速尝试将 JSON 字符串解析为私聊消息事件
     * @param json_str 原始 JSON 字符串
     * @return std::optional<PrivateMessageEvent> 解析成功返回事件对象，否则返回 std::nullopt
     */
    static std::optional<PrivateMessageEvent> parse(const std::string& json_str) {
        PrivateMessageEvent ev;
        auto ec = glz::read<glaze_opts>(ev, json_str);
        if (!ec && ev.post_type == "message" && ev.message_type == "private") {
            return ev;
        }
        return std::nullopt;
    }
};

/**
 * @struct SendGroupMsgResponseData
 * @brief OneBot 11 发送消息 API 响应体中的基础返回数据
 */
struct SendGroupMsgResponseData {
    int64_t message_id{0};       ///< 成功发送后由协议端分配的消息 ID
};

/**
 * @struct RawApiResponse
 * @brief OneBot 11 通用 API 回包结构体 (带 echo 字段用于异步协程追踪)
 */
struct RawApiResponse {
    std::string status;          ///< 响应状态: "ok" 或 "failed"
    int32_t retcode{0};          ///< 返回码: 0 为成功，非 0 为错误码
    std::string message;         ///< 错误提示信息
    std::string wording;         ///< 补充提示文案
    std::string echo;            ///< 请求时携带的唯一追踪标识 (UUID)
    std::optional<SendGroupMsgResponseData> data; ///< 可选的数据负载
};

/**
 * @struct ReactionLikeItem
 * @brief 表情回应统计子项 (NapCat likes 结构)
 */
struct ReactionLikeItem {
    std::string emoji_id;        ///< 表情 ID 字符串
    int count{0};                ///< 当前表情计数 (0 表示已取消回应)
};

/**
 * @struct GroupReactionEvent
 * @brief 群消息表情回应通知事件 (notice_type="group_msg_emoji_like" 或 "reaction")
 * @details 负责归一化处理 NapCat 及不同 OneBot 协议端实现的群消息表情回应事件。
 */
struct GroupReactionEvent {
    std::string post_type;       ///< 上报类型: "notice"
    std::string notice_type;     ///< 通知类型: "group_msg_emoji_like", "reaction", "notify"
    std::string sub_type;        ///< 子类型: "add", "delete", "reaction"
    int64_t time{0};             ///< 事件发生的 Unix 时间戳 (秒)
    int64_t self_id{0};          ///< 机器人 QQ 号
    int64_t group_id{0};         ///< 群号
    int64_t user_id{0};          ///< 操作者 QQ 号
    int64_t message_id{0};       ///< 被表情回应的目标消息 ID
    std::string code;            ///< 表情 ID 或代码 (例如 "478", "479", "对的对的", "不对不对")
    std::string emoji_id;        ///< 兼容别名
    bool is_add{true};           ///< 操作类型: true 表示添加回应，false 表示取消回应

    /**
     * @struct RawReactionJson
     * @brief 原始表情回应 JSON 反射辅助结构体，兼顾多种上报字段变体
     */
    struct RawReactionJson {
        std::string post_type;
        std::string notice_type;
        std::optional<std::string> sub_type;
        int64_t time{0};
        int64_t self_id{0};
        int64_t group_id{0};
        int64_t user_id{0};
        int64_t message_id{0};
        std::optional<std::string> code;
        std::optional<std::string> emoji_id;
        std::optional<int64_t> face_id;
        std::optional<bool> is_add;
        std::vector<ReactionLikeItem> likes;
    };

    /**
     * @brief 解析原始 JSON 字符串为标准化的群表情回应事件
     * @param json_str 原始 JSON 字符串
     * @return std::optional<GroupReactionEvent> 成功返回归一化后的事件结构体
     */
    static std::optional<GroupReactionEvent> parse(const std::string& json_str) {
        RawReactionJson raw;
        auto ec = glz::read<glaze_opts>(raw, json_str);
        if (ec) return std::nullopt;

        if (raw.post_type != "notice") return std::nullopt;

        bool is_reaction = false;
        if (raw.notice_type == "group_msg_emoji_like" || raw.notice_type == "reaction") {
            is_reaction = true;
        } else if (raw.notice_type == "notify" && raw.sub_type && *raw.sub_type == "reaction") {
            is_reaction = true;
        }

        if (!is_reaction) return std::nullopt;

        GroupReactionEvent ev;
        ev.post_type = raw.post_type;
        ev.notice_type = raw.notice_type;
        if (raw.sub_type) ev.sub_type = *raw.sub_type;
        ev.time = raw.time;
        ev.self_id = raw.self_id;
        ev.group_id = raw.group_id;
        ev.user_id = raw.user_id;
        ev.message_id = raw.message_id;

        if (raw.code && !raw.code->empty()) {
            ev.code = *raw.code;
        } else if (raw.emoji_id && !raw.emoji_id->empty()) {
            ev.code = *raw.emoji_id;
        } else if (raw.face_id) {
            ev.code = std::to_string(*raw.face_id);
        } else if (!raw.likes.empty() && !raw.likes[0].emoji_id.empty()) {
            ev.code = raw.likes[0].emoji_id;
        }
        ev.emoji_id = ev.code;

        if (!raw.likes.empty() && raw.likes[0].count == 0) {
            ev.is_add = false;
        } else if (raw.is_add.has_value()) {
            ev.is_add = *raw.is_add;
        } else if (raw.sub_type) {
            ev.is_add = (*raw.sub_type != "delete" && *raw.sub_type != "remove");
        } else {
            ev.is_add = true;
        }

        return ev;
    }
};

} // namespace bot::protocol
