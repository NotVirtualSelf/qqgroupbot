/**
 * @file database.hpp
 * @brief SQLite 数据库持久化存储层定义
 * @details 负责管理艺术家订阅关系、新专辑/单曲推送防重记录、24 小时跨平台排重、
 *          每日音乐推荐推送与表情互动投票统计、管理员白名单与系统全局配置。
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <sqlite3.h>

namespace bot::module::music {

/**
 * @struct ArtistSubscription
 * @brief 艺术家订阅实体记录
 */
struct ArtistSubscription {
    int64_t group_id{0};             ///< 订阅的 QQ 群号
    int64_t artist_id{0};            ///< 艺术家在对应平台上的唯一 ID
    std::string artist_name;         ///< 艺术家名称
    std::string platform{"netease"}; ///< 音乐平台标识 ("netease" 或 "qqmusic")
    int64_t created_at{0};           ///< 关注创建时的 Unix 时间戳 (秒)
    std::string pic_url;             ///< 艺术家头像/封面 URL
    int64_t cover_updated_at{0};     ///< 头像最后静默刷新的 Unix 时间戳 (秒)
};

/**
 * @struct PushedSong
 * @brief 已推送单曲记录实体
 */
struct PushedSong {
    int64_t song_id{0};              ///< 单曲 ID
    int64_t artist_id{0};            ///< 艺术家 ID
    std::string song_name;           ///< 歌曲名称
    int64_t publish_time{0};         ///< 发行时间戳 (毫秒或秒)
    int64_t pushed_at{0};            ///< 机器人推送记录的时间戳 (秒)
};

/**
 * @struct PushedAlbum
 * @brief 已推送专辑记录实体 (包含平台与跨平台查重字段)
 */
struct PushedAlbum {
    int64_t album_id{0};             ///< 专辑 ID
    int64_t artist_id{0};            ///< 艺术家 ID
    std::string album_name;          ///< 专辑名称
    std::string artist_name;         ///< 艺术家名称
    std::string platform{"netease"}; ///< 来源音乐平台
    int64_t publish_time{0};         ///< 发行时间戳 (毫秒或秒)
    int64_t pushed_at{0};            ///< 推送记录的时间戳 (秒)
};

/**
 * @struct DailyPushedRecord
 * @brief 每日音乐推荐推送与投票记录实体
 */
struct DailyPushedRecord {
    int64_t id{0};                   ///< 数据库自增主键 ID
    std::string task_name;           ///< 推荐任务名 (如 "netease_daily_song")
    int64_t group_id{0};             ///< 推送目标群号
    int64_t message_id{0};           ///< 平台分配的消息 ID (用于表情回应定位)
    int64_t item_id{0};              ///< 推荐曲目/专辑 ID
    std::string item_name;           ///< 推荐曲目/专辑名称
    std::string artist_name;         ///< 艺术家名称
    std::string platform;            ///< 音乐平台标识
    std::string pushed_date;         ///< 推送日期字符串 (YYYY-MM-DD)
    int64_t pushed_at{0};            ///< 推送时间戳 (秒)
    int evaluated{0};                ///< 是否已完成评价结算 (0 未结算，1 已结算)
    int upvotes{0};                  ///< 点赞票数 (「对的对的」)
    int downvotes{0};                ///< 踩票数 (「不对不对」)
};

/**
 * @class MusicDatabase
 * @brief SQLite 数据存储管理类 (RAII 资源管控 + 线程安全)
 */
class MusicDatabase {
public:
    /**
     * @brief 构造函数，打开或创建 SQLite 数据库文件
     * @param db_path SQLite 数据库本地文件路径 (默认为 "data/bot_music.db")
     */
    explicit MusicDatabase(const std::string& db_path = "data/bot_music.db");

    /**
     * @brief 析构函数，关闭底层 SQLite 连接
     */
    ~MusicDatabase();

    MusicDatabase(const MusicDatabase&) = delete;
    MusicDatabase& operator=(const MusicDatabase&) = delete;

    /**
     * @brief 初始化数据表结构 (自动执行建表与增量迁移)
     * @return bool 成功返回 true
     */
    bool init();

    /**
     * @brief 添加群与艺术家的订阅映射 (可指定音乐平台与封面)
     * @param group_id 订阅群号
     * @param artist_id 平台艺术家 ID
     * @param artist_name 艺术家名称
     * @param platform 平台标识 ("netease" 或 "qqmusic")
     * @param pic_url 艺术家封面图片 URL
     * @return bool 成功返回 true
     */
    bool add_subscription(int64_t group_id, int64_t artist_id, const std::string& artist_name, const std::string& platform = "netease", const std::string& pic_url = "");

    /**
     * @brief 更新某艺术家的封面 URL 及更新时间 (同步更新该艺术家在所有群的订阅记录)
     * @param artist_id 平台艺术家 ID
     * @param pic_url 最新的封面 URL
     * @param platform 可选指定平台
     * @param updated_at 更新时间戳 (0 表示使用当前系统时间)
     * @return bool 成功返回 true
     */
    bool update_artist_pic_url(int64_t artist_id, const std::string& pic_url, const std::string& platform = "", int64_t updated_at = 0);

    /**
     * @brief 根据艺术家名字取消订阅 (单个群，可选指定平台)
     * @param group_id 群号
     * @param artist_name 艺术家名字
     * @param platform 可选指定平台
     * @return bool 成功返回 true
     */
    bool remove_subscription(int64_t group_id, const std::string& artist_name, const std::string& platform = "");

    /**
     * @brief 根据 artist_id 取消订阅 (针对某群，可选指定平台)
     * @param group_id 群号
     * @param artist_id 平台艺术家 ID
     * @param platform 可选指定平台
     * @return bool 成功返回 true
     */
    bool remove_subscription_by_id(int64_t group_id, int64_t artist_id, const std::string& platform = "");

    /**
     * @brief 查询某群下符合艺术家名字的订阅记录 (可选指定平台)
     * @param group_id 群号
     * @param artist_name 艺术家名字
     * @param platform 可选指定平台
     * @return std::vector<ArtistSubscription> 匹配的订阅记录列表
     */
    std::vector<ArtistSubscription> find_subscriptions(int64_t group_id, const std::string& artist_name, const std::string& platform = "");

    /**
     * @brief 查询某群关注的所有艺术家列表
     * @param group_id 群号
     * @return std::vector<ArtistSubscription> 关注列表
     */
    std::vector<ArtistSubscription> get_group_subscriptions(int64_t group_id);

    /**
     * @brief 查询系统中所有不重复的已订阅艺术家列表 (用于后台轮询监控)
     * @return std::vector<ArtistSubscription> 去重后的艺术家列表
     */
    std::vector<ArtistSubscription> get_all_unique_artists();

    /**
     * @brief 查询关注某位艺术家的所有群号列表 (可选指定平台)
     * @param artist_id 平台艺术家 ID
     * @param platform 平台标识
     * @return std::vector<int64_t> 群号列表
     */
    std::vector<int64_t> get_subscribers_for_artist(int64_t artist_id, const std::string& platform = "");

    /**
     * @brief 查询关注某位艺术家的所有群号列表 (按艺术家名称跨平台去重联合查询)
     * @param artist_name 艺术家名称
     * @return std::vector<int64_t> 去重后的群号列表
     */
    std::vector<int64_t> get_subscribers_for_artist_name(const std::string& artist_name);

    /**
     * @brief 检查某群是否已关注指定艺术家 (根据群号、艺术家ID、平台进行联合判重)
     * @param group_id 群号
     * @param artist_id 平台艺术家 ID
     * @param platform 平台标识
     * @return bool 已关注返回 true
     */
    bool is_subscribed(int64_t group_id, int64_t artist_id, const std::string& platform = "netease");

    /**
     * @brief 检查某首歌是否已经推送过
     * @param song_id 单曲 ID
     * @return bool 已推送过返回 true
     */
    bool is_song_pushed(int64_t song_id);

    /**
     * @brief 记录某首歌已完成推送
     * @param song_id 单曲 ID
     * @param artist_id 艺术家 ID
     * @param song_name 单曲名称
     * @param publish_time 发行时间戳
     * @return bool 成功返回 true
     */
    bool record_pushed_song(int64_t song_id, int64_t artist_id, const std::string& song_name, int64_t publish_time = 0);

    /**
     * @brief 检查某张专辑是否已经推送过 (可选指定平台)
     * @param album_id 专辑 ID
     * @param platform 平台标识
     * @return bool 已推送过返回 true
     */
    bool is_album_pushed(int64_t album_id, const std::string& platform = "");

    /**
     * @brief 记录某张专辑已完成推送 (支持指定平台与艺术家名字)
     * @param album_id 专辑 ID
     * @param artist_id 艺术家 ID
     * @param album_name 专辑名称
     * @param publish_time 发行时间戳
     * @param platform 平台标识
     * @param artist_name 艺术家名称
     * @return bool 成功返回 true
     */
    bool record_pushed_album(int64_t album_id, int64_t artist_id, const std::string& album_name,
                            int64_t publish_time = 0, const std::string& platform = "netease",
                            const std::string& artist_name = "");

    /**
     * @brief 检查某位艺术家的同名专辑是否在最近 (默认 1 天 = 86400 秒) 内已在其他平台推送过
     * @details 跨平台 24 小时排重机制：同一作品在不同平台发布时，一天内仅推送首个检测到的平台
     * @param artist_name 艺术家名称
     * @param album_name 专辑名称
     * @param within_seconds 时间窗口 (秒，默认 86400)
     * @param current_platform 当前正在检查的平台标识
     * @return bool 若最近已在其他平台推送过返回 true
     */
    bool is_album_recently_pushed_cross_platform(const std::string& artist_name,
                                                 const std::string& album_name,
                                                 int64_t within_seconds = 86400,
                                                 const std::string& current_platform = "");

    /**
     * @brief 检查某位艺术家是否已有已推送专辑记录 (用于新关注时的预热防刷屏判定)
     * @param artist_id 艺术家 ID
     * @return bool 存在历史推送记录返回 true
     */
    bool has_artist_pushed_albums(int64_t artist_id);

    /**
     * @brief 设置全局配置项键值对 (如 netease_cookie)
     * @param key 配置键名
     * @param value 配置键值
     * @return bool 成功返回 true
     */
    bool set_config(const std::string& key, const std::string& value);

    /**
     * @brief 查询全局配置项键值对
     * @param key 配置键名
     * @param default_val 不存在时的缺省值
     * @return std::string 配置值
     */
    std::string get_config(const std::string& key, const std::string& default_val = "");

    /**
     * @brief 开启或关闭某群的每日音乐推送
     * @param group_id 群号
     * @param enabled 是否开启
     * @return bool 成功返回 true
     */
    bool set_daily_music_enabled(int64_t group_id, bool enabled);

    /**
     * @brief 检查某群是否开启了每日音乐推送
     * @param group_id 群号
     * @return bool 已开启返回 true
     */
    bool is_daily_music_enabled(int64_t group_id);

    /**
     * @brief 获取所有已开启每日音乐推送的群列表
     * @return std::vector<int64_t> 群号列表
     */
    std::vector<int64_t> get_daily_music_groups();

    /**
     * @brief 记录每日音乐推荐消息发送与关联元数据
     * @param task_name 推荐任务名
     * @param group_id 群号
     * @param message_id 平台分配的消息 ID
     * @param item_id 推荐内容 ID
     * @param item_name 推荐曲目/专辑名
     * @param artist_name 艺术家名称
     * @param platform 平台标识
     * @param date 日期字符串 (YYYY-MM-DD)
     * @param timestamp 发送时间戳 (秒)
     * @return int64_t 自增记录 ID
     */
    int64_t record_daily_push(const std::string& task_name, int64_t group_id, int64_t message_id,
                              int64_t item_id, const std::string& item_name, const std::string& artist_name,
                              const std::string& platform, const std::string& date, int64_t timestamp = 0);

    /**
     * @brief 根据 message_id 查询每日推荐推送记录
     * @param message_id 消息 ID
     * @return std::optional<DailyPushedRecord> 存在则返回记录实体
     */
    std::optional<DailyPushedRecord> get_daily_push_by_message_id(int64_t message_id);

    /**
     * @brief 获取某群最新的一条每日推荐推送记录
     * @param group_id 群号
     * @param task_name 可选任务过滤
     * @return std::optional<DailyPushedRecord> 最新记录
     */
    std::optional<DailyPushedRecord> get_latest_daily_push(int64_t group_id, const std::string& task_name = "");

    /**
     * @brief 获取某群今天指定日期的第一条推荐推送记录 (如果 date 为空则默认为今天)
     * @param group_id 群号
     * @param date 日期字符串 (YYYY-MM-DD)
     * @return std::optional<DailyPushedRecord> 首条日推记录
     */
    std::optional<DailyPushedRecord> get_today_daily_push(int64_t group_id, const std::string& date = "");

    /**
     * @brief 获取尚未执行评价收集结算的推送记录列表
     * @param task_name 任务名过滤
     * @return std::vector<DailyPushedRecord> 未结算记录
     */
    std::vector<DailyPushedRecord> get_unevaluated_daily_pushes(const std::string& task_name = "");

    /**
     * @brief 标记某条推送记录已完成评价处理并更新最终赞踩票数
     * @param record_id 记录 ID
     * @param upvotes 点赞票数
     * @param downvotes 踩票数
     * @return bool 成功返回 true
     */
    bool mark_daily_push_evaluated(int64_t record_id, int upvotes, int downvotes);

    /**
     * @brief 记录或更新用户对每日推荐消息的表情回应
     * @param message_id 目标日推消息 ID
     * @param user_id 用户 QQ 号
     * @param reaction_type 回应类型 (1 为点赞/对的对的, -1 为踩/不对不对)
     * @return bool 成功返回 true
     */
    bool record_daily_reaction(int64_t message_id, int64_t user_id, int reaction_type);

    /**
     * @brief 移除用户对每日推荐消息的表情回应 (用户撤销表情时调用)
     * @param message_id 目标日推消息 ID
     * @param user_id 用户 QQ 号
     * @return bool 成功返回 true
     */
    bool remove_daily_reaction(int64_t message_id, int64_t user_id);

    /**
     * @brief 清空每日推荐消息的表情回应记录 (message_id 为 0 时清空全部)
     * @param message_id 目标消息 ID
     * @return bool 成功返回 true
     */
    bool clear_daily_reactions(int64_t message_id = 0);

    /**
     * @brief 查询某群在指定日期是否已经结算过日推评价
     * @param group_id 群号
     * @param date 日期字符串 (YYYY-MM-DD，为空则默认为今天)
     * @return bool 已结算返回 true
     */
    bool is_daily_evaluated(int64_t group_id, const std::string& date = "");

    /**
     * @brief 标记某群在指定日期的日推评价已结算
     * @param group_id 群号
     * @param date 日期字符串 (YYYY-MM-DD，为空则默认为今天)
     * @return bool 成功返回 true
     */
    bool set_daily_evaluated(int64_t group_id, const std::string& date = "");

    /**
     * @brief 清理某群在指定日期的每日推送历史与标记 (用于重置或测试)
     * @param group_id 群号 (0 表示全部)
     * @param date 日期字符串
     * @return bool 成功返回 true
     */
    bool clear_daily_push_records(int64_t group_id = 0, const std::string& date = "");

    /**
     * @brief 清理某群在指定日期的日推结算标记
     * @param group_id 群号 (0 表示全部)
     * @param date 日期字符串
     * @return bool 成功返回 true
     */
    bool clear_daily_evaluation(int64_t group_id = 0, const std::string& date = "");

    /**
     * @brief 恢复某群在指定日期的日推状态为初始状态 (清空当日推送、清空关联表情投票、清空结算标记)
     * @param group_id 群号
     * @param date 日期字符串
     * @return bool 成功返回 true
     */
    bool reset_daily_push_status(int64_t group_id, const std::string& date = "");

    /**
     * @brief 获取某条每日推荐消息的点赞与踩统计数
     * @param message_id 目标消息 ID
     * @return std::pair<int, int> {upvotes, downvotes}
     */
    std::pair<int, int> get_daily_push_votes(int64_t message_id);

    /**
     * @brief 检查指定 QQ 是否为管理员 (白名单)
     * @param user_id 用户 QQ 号
     * @return bool 是管理员返回 true
     */
    bool is_admin(int64_t user_id);

    /**
     * @brief 添加管理员至白名单
     * @param user_id 用户 QQ 号
     * @return bool 成功返回 true
     */
    bool add_admin(int64_t user_id);

    /**
     * @brief 从白名单移除管理员 (硬性管理员不可移除)
     * @param user_id 用户 QQ 号
     * @return bool 成功返回 true
     */
    bool remove_admin(int64_t user_id);

    /**
     * @brief 查询所有管理员 QQ 号列表
     * @return std::vector<int64_t> 管理员 QQ 列表
     */
    std::vector<int64_t> get_admins();

    /**
     * @brief 检查系统中是否已配置过至少一位管理员
     * @return bool 存在管理员返回 true
     */
    bool has_any_admin();

    /**
     * @brief 判断指定 QQ 是否属于代码中写死的硬性管理员白名单 (不可被指令移除)
     * @param user_id 用户 QQ 号
     * @return bool 属于硬性管理员返回 true
     */
    static bool is_hardcoded_admin(int64_t user_id);

    /**
     * @brief 获取硬性管理员代码白名单引用 (不可通过指令移除，仅限代码中维护)
     * @return const std::vector<int64_t>& 硬性管理员列表
     */
    static const std::vector<int64_t>& get_hardcoded_admins();

private:
    std::string db_path_;
    sqlite3* db_{nullptr};
    mutable std::mutex mutex_;
};

} // namespace bot::module::music
