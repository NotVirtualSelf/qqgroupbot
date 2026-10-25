/**
 * @file daily_music_manager.hpp
 * @brief 每日音乐/专辑推荐定时调度与表情评价结算管理器定义
 * @details 负责定时推送（如每日 11:00）、群表情回应监听与点赞/踩票数统计、
 *          定时或手动评价结算（如每日 23:00 静默点赞/减少推荐）以及提前触发与状态重置逻辑。
 */

#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include "bot/protocol/events.hpp"
#include "bot/protocol/rpc_client.hpp"
#include "bot/modules/music/database.hpp"
#include "bot/modules/music/daily_recommender.hpp"

namespace bot::module::music {

/**
 * @struct RecommenderTaskContext
 * @brief 单个推荐任务的运行时上下文环境
 */
struct RecommenderTaskContext {
    std::shared_ptr<IDailyRecommender> recommender;        ///< 推荐策略实现
    std::unique_ptr<boost::asio::steady_timer> push_timer; ///< 每日推送定时器
    std::unique_ptr<boost::asio::steady_timer> eval_timer; ///< 每日结算定时器
};

/**
 * @class DailyMusicManager
 * @brief 每日音乐/专辑推荐定时调度与评价管理器
 * @details 采用基于 Boost.Asio steady_timer 的低负载定时唤醒机制，
 *          负责统筹推送、表情监听、结算闭环与生命周期管理。
 */
class DailyMusicManager {
public:
    /**
     * @brief 构造每日音乐管理器
     * @param ioc 异步 I/O 上下文引用
     * @param rpc_client OneBot 11 RPC 客户端引用
     * @param db 音乐数据库存储层引用
     */
    DailyMusicManager(boost::asio::io_context& ioc,
                      bot::protocol::RpcClient& rpc_client,
                      MusicDatabase& db);

    /**
     * @brief 析构函数，停止所有运行中的定时器
     */
    ~DailyMusicManager();

    DailyMusicManager(const DailyMusicManager&) = delete;
    DailyMusicManager& operator=(const DailyMusicManager&) = delete;

    /**
     * @brief 注册推荐策略 (开放式扩展，如网易云日推歌曲、未来17点日推专辑等)
     * @param recommender 推荐器智能指针
     */
    void register_recommender(std::shared_ptr<IDailyRecommender> recommender);

    /**
     * @brief 获取所有已注册的推荐策略列表
     * @return const std::vector<std::shared_ptr<IDailyRecommender>>& 策略列表
     */
    const std::vector<std::shared_ptr<IDailyRecommender>>& get_recommenders() const;

    /**
     * @brief 启动定时任务调度 (初始化推送与结算定时器)
     */
    void start();

    /**
     * @brief 停止定时任务调度并取消所有活动定时器
     */
    void stop();

    /**
     * @brief 处理群表情回应通知事件 (统计「对的对的」点赞与「不对不对」踩)
     * @param event 归一化后的群表情回应事件
     */
    void handle_reaction(const bot::protocol::GroupReactionEvent& event);

    /**
     * @brief 手动或即时触发一次推送
     * @param task_name 指定任务名（为空表示匹配首个任务）
     * @param target_group 目标群号（0 表示所有开启日推的群聊）
     * @return bool 是否成功触发
     */
    bool trigger_push_now(const std::string& task_name = "", int64_t target_group = 0);

    /**
     * @brief 手动或即时触发一次评价结算
     * @param task_name 指定任务名（为空表示匹配所有任务）
     * @return bool 是否成功触发
     */
    bool trigger_eval_now(const std::string& task_name = "");

    /**
     * @brief 记录某条日推消息与元数据 (供测试或上层即时推送复用)
     * @param task_name 推荐任务名
     * @param group_id 群号
     * @param message_id 消息 ID
     * @param item 推荐数据项
     * @param date 日期字符串 (格式: YYYY-MM-DD，为空则使用当天)
     * @return int64_t 数据库自增记录 ID
     */
    int64_t record_push(const std::string& task_name, int64_t group_id, int64_t message_id,
                        const DailyRecommendationItem& item, const std::string& date = "");

    /**
     * @brief 根据点赞与踩的票数对比决定评价动作并执行反馈
     * @param recommender 推荐器实例
     * @param item 推荐内容
     * @param upvotes 点赞票数
     * @param downvotes 踩票数
     * @return DailyEvalAction 最终执行的评价动作 (Like / Dislike / None)
     */
    DailyEvalAction evaluate_item_votes(const std::shared_ptr<IDailyRecommender>& recommender,
                                        const DailyRecommendationItem& item,
                                        int upvotes, int downvotes);

    /**
     * @brief 计算当前时刻距离下一个目标时刻 (hour:min:00) 的等待秒数
     * @param target_hour 目标小时 (0~23)
     * @param target_minute 目标分钟 (0~59)
     * @return std::chrono::seconds 等待时间
     */
    static std::chrono::seconds calculate_delay_until(int target_hour, int target_minute);

    /**
     * @brief 获取当前日期字符串 (格式: YYYY-MM-DD)
     * @return std::string 当天日期字符串
     */
    static std::string get_current_date_str();

private:
    void schedule_push_for_task(RecommenderTaskContext& task_ctx);
    void schedule_eval_for_task(RecommenderTaskContext& task_ctx);

    void on_push_timer(RecommenderTaskContext* task_ctx, const boost::system::error_code& ec);
    void on_eval_timer(RecommenderTaskContext* task_ctx, const boost::system::error_code& ec);

    bool execute_push(const std::shared_ptr<IDailyRecommender>& recommender, int64_t target_group = 0);
    bool execute_eval(const std::shared_ptr<IDailyRecommender>& recommender);

    boost::asio::io_context& ioc_;
    bot::protocol::RpcClient& rpc_client_;
    MusicDatabase& db_;
    std::atomic<bool> is_running_{false};

    mutable std::mutex tasks_mutex_;
    std::vector<std::shared_ptr<IDailyRecommender>> recommenders_;
    std::vector<std::unique_ptr<RecommenderTaskContext>> task_contexts_;
};

} // namespace bot::module::music
