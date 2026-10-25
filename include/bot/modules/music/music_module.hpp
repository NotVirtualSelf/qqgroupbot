/**
 * @file music_module.hpp
 * @brief 音乐订阅与新歌监控推送主业务模块定义
 * @details 负责网易云与 QQ 音乐双平台的关注/取关会话状态机、合并转发候选消歧、
 *          新单曲/新专辑定时轮询与手动查询、严格顺序链式推送队列调度、
 *          每日日推业务集成、管理员白名单鉴权以及扫码登录会话管理。
 */

#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <set>
#include <mutex>
#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include "bot/protocol/events.hpp"
#include "bot/protocol/rpc_client.hpp"
#include "bot/modules/music/database.hpp"
#include "bot/modules/music/netease_client.hpp"
#include "bot/modules/music/qqmusic_client.hpp"
#include "bot/modules/music/daily_music_manager.hpp"

namespace bot::module::music {

/**
 * @enum PendingType
 * @brief 待群内交互确认的操作类型枚举
 */
enum class PendingType {
    Follow,   ///< 关注确认
    Unfollow  ///< 取消关注确认 (同名多项消歧)
};

/**
 * @struct PendingSelection
 * @brief 记录用户在某群正在等待交互确认的艺术家候选状态上下文
 */
struct PendingSelection {
    int64_t user_id{0};                               ///< 操作发起人 QQ 号
    PendingType type{PendingType::Follow};            ///< 操作类型
    std::string platform{"netease"};                  ///< 平台标识
    std::vector<ArtistCandidate> candidates;          ///< 候选艺术家列表
    std::chrono::steady_clock::time_point expire_at;  ///< 超时截止时间点 (默认 60 秒)
};

/**
 * @class MusicModule
 * @brief 音乐主业务模块 (命令路由、交互状态机与轮询调度)
 */
class MusicModule {
public:
    /**
     * @brief 构造函数
     * @param ioc 异步 I/O 上下文
     * @param rpc_client OneBot 11 RPC 客户端
     * @param db_path 本地 SQLite 数据库路径
     * @param poll_interval 新作品轮询周期 (默认 20 分钟)
     * @param cover_update_interval 艺术家头像静默更新周期 (默认 24 小时)
     */
    MusicModule(boost::asio::io_context& ioc,
                bot::protocol::RpcClient& rpc_client,
                std::string db_path = "data/bot_music.db",
                std::chrono::seconds poll_interval = std::chrono::minutes(20),
                std::chrono::seconds cover_update_interval = std::chrono::hours(24));

    /**
     * @brief 析构函数，停止所有异步定时器
     */
    ~MusicModule();

    MusicModule(const MusicModule&) = delete;
    MusicModule& operator=(const MusicModule&) = delete;

    /**
     * @brief 初始化数据库并启动定时轮询任务与封面静默更新任务
     * @return bool 成功返回 true
     */
    bool start();

    /**
     * @brief 停止所有定时轮询任务并重置运行状态
     */
    void stop();

    /**
     * @brief 处理群消息命令路由 (/关注, /取关, /关注列表, /查新歌, /日推, 候选数字选择)
     * @param event 群消息事件
     * @return bool 若成功匹配并消费了音乐命令返回 true，否则返回 false
     */
    bool handle_group_message(const bot::protocol::GroupMessageEvent& event);

    /**
     * @brief 处理群表情回应事件 (点赞/踩)
     * @param event 群表情回应事件
     * @return bool 成功处理返回 true
     */
    bool handle_group_reaction(const bot::protocol::GroupReactionEvent& event);

    /**
     * @brief 处理私聊消息命令路由 (/登录, /管理员 等)
     * @param event 私聊消息事件
     * @return bool 若成功匹配并消费了私聊命令返回 true，否则返回 false
     */
    bool handle_private_message(const bot::protocol::PrivateMessageEvent& event);

    /**
     * @brief 触发一次新专辑/单曲检查与严格顺序推送
     * @param target_group 目标群号 (0 表示轮询全局关注列表；非 0 仅查询指定群并在无新歌时回复“𝐢 | 未查询到新歌”)
     */
    void check_new_albums_once(int64_t target_group = 0);

    /**
     * @brief 触发一次新歌/新专辑检查与推送 (兼容包装)
     * @param target_group 目标群号
     */
    void check_new_songs_once(int64_t target_group = 0);

    /**
     * @brief 触发一次所有订阅艺术家的封面更新 (force=true 时忽略 24 小时间隔强制拉取)
     * @param force 是否强制更新
     */
    void update_artist_covers_once(bool force = false);

    /**
     * @brief 获取每日音乐推荐管理器引用
     * @return DailyMusicManager& 引用
     */
    DailyMusicManager& get_daily_manager() { return *daily_manager_; }

    /**
     * @brief 获取网易云音乐 API 客户端引用
     * @return NeteaseClient& 引用
     */
    NeteaseClient& get_netease_client() { return *netease_; }

    /**
     * @brief 获取音乐数据库引用
     * @return MusicDatabase& 引用
     */
    MusicDatabase& get_database() { return db_; }

    /**
     * @brief 格式化单曲时长为 xx分xx秒
     * @param duration_ms 毫秒数
     * @return std::string 格式化文本
     */
    static std::string format_track_duration(int64_t duration_ms);

    /**
     * @brief 格式化总时长 (小于1小时为 xx分xx秒，大于等于1小时为 x小时xx分xx秒)
     * @param duration_ms 毫秒数
     * @return std::string 格式化文本
     */
    static std::string format_total_duration(int64_t duration_ms);

    /**
     * @brief 构造新专辑/新单曲通知合并转发卡片节点列表
     * @param album 专辑/单曲详情
     * @return std::vector<bot::protocol::ForwardNode> 转发卡片节点列表
     */
    static std::vector<bot::protocol::ForwardNode> build_album_forward_nodes(const AlbumDetail& album);

    /**
     * @brief 判断机器人是否在指定的 QQ 群中
     * @param group_id QQ 群号
     * @return bool 在群中返回 true
     */
    bool is_bot_in_group(int64_t group_id) const;

    /**
     * @brief 记录机器人所在的群号 (用于动态入群发现或外部注入)
     * @param group_id QQ 群号
     */
    void add_known_group(int64_t group_id);

private:
    struct AlbumPushItem {
        int64_t gid;
        std::string intro_text;
        std::vector<bot::protocol::ForwardNode> forward_nodes;
        std::string share_msg;
    };

    void dispatch_push_queue(std::vector<AlbumPushItem> queue, size_t index);

    void schedule_timer();
    void on_timer(const boost::system::error_code& ec);

    void schedule_cover_timer();
    void on_cover_timer(const boost::system::error_code& ec);

    /// 检查并处理用户在群内的序号选择 (1-5) 或取消
    bool handle_candidate_selection(const bot::protocol::GroupMessageEvent& event, const std::string& text);

    struct QrLoginSession {
        int64_t user_id{0};
        int64_t target_group{0};
        std::string unikey;
        std::string png_path;
        int poll_count{0};
        bool prompted_confirm{false};
        std::string nickname;
        std::string avatar_url;
        std::unique_ptr<boost::asio::steady_timer> timer;
    };

    void start_qr_login_session(int64_t user_id, int64_t target_group);
    void schedule_qr_poll();
    void on_qr_poll_timer(const boost::system::error_code& ec);
    void cancel_qr_login_session();

    struct PendingShareConfirm {
        int64_t user_id{0};
        int64_t dest_group{0};
        int64_t src_group{0};
        std::chrono::steady_clock::time_point expire_at;
    };

    boost::asio::io_context& ioc_;
    bot::protocol::RpcClient& rpc_client_;
    MusicDatabase db_;
    std::shared_ptr<NeteaseClient> netease_;
    QQMusicClient qqmusic_;
    std::unique_ptr<DailyMusicManager> daily_manager_;
    std::unique_ptr<QrLoginSession> qr_session_;
    std::chrono::seconds poll_interval_;
    boost::asio::steady_timer timer_;
    std::chrono::seconds cover_update_interval_;
    boost::asio::steady_timer cover_timer_;
    std::atomic<bool> is_running_{false};

    mutable std::mutex pending_mutex_;
    std::unordered_map<int64_t, PendingSelection> pending_selections_; // group_id -> PendingSelection

    mutable std::mutex shares_mutex_;
    std::unordered_map<int64_t, PendingShareConfirm> pending_shares_; // user_id -> PendingShareConfirm

    mutable std::mutex groups_mutex_;
    std::set<int64_t> known_groups_;
};

} // namespace bot::module::music
