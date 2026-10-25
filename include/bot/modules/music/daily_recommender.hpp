/**
 * @file daily_recommender.hpp
 * @brief 每日音乐/专辑推荐策略接口与网易云日推实现
 * @details 采用面向对象的多态抽象设计，分离每日推荐的具体平台抓取、卡片排版与点赞/踩反馈逻辑，
 *          提供低耦合扩展能力（如未来扩展 17:00 每日推荐专辑等）。
 */

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <iostream>
#include "bot/modules/music/database.hpp"
#include "bot/modules/music/netease_client.hpp"

namespace bot::module::music {

/**
 * @enum DailyEvalAction
 * @brief 每日推荐结算反馈动作枚举
 */
enum class DailyEvalAction {
    None,     ///< 保持现状，不执行操作
    Like,     ///< 点赞 / 喜欢该曲目
    Dislike   ///< 踩 / 减少推荐
};

/**
 * @struct DailyRecommendationItem
 * @brief 每日推荐项基础数据结构
 */
struct DailyRecommendationItem {
    int64_t item_id{0};          ///< 推荐内容 ID (单曲 ID 或专辑 ID)
    std::string item_name;       ///< 推荐标题 (歌曲名/专辑名)
    std::string artist_name;     ///< 艺术家名称
    std::string platform{"netease"}; ///< 平台标识 (如 "netease", "qqmusic")
    std::string pic_url;         ///< 封面图片 URL
    std::string extra_info;      ///< 补充元数据 (如推荐理由等)
};

/**
 * @class IDailyRecommender
 * @brief 每日推荐策略接口基类 (开放式多态设计)
 */
class IDailyRecommender {
public:
    virtual ~IDailyRecommender() = default;

    /**
     * @brief 获取唯一任务标识
     * @return std::string 例如 "netease_daily_song", "netease_daily_album"
     */
    virtual std::string task_name() const = 0;

    /**
     * @brief 获取平台标识
     * @return std::string 例如 "netease", "qqmusic"
     */
    virtual std::string platform() const = 0;

    /**
     * @brief 获取每日推送触发时间 (24小时制，小时: 0~23)
     * @return int 小时数值
     */
    virtual int push_hour() const = 0;

    /**
     * @brief 获取每日推送触发时间 (分钟: 0~59)
     * @return int 分钟数值，默认 0
     */
    virtual int push_minute() const { return 0; }

    /**
     * @brief 获取每日评价收集时间 (24小时制，小时: 0~23)
     * @return int 小时数值
     */
    virtual int eval_hour() const = 0;

    /**
     * @brief 获取每日评价收集时间 (分钟: 0~59)
     * @return int 分钟数值，默认 0
     */
    virtual int eval_minute() const { return 0; }

    /**
     * @brief 获取平台支持的每日推荐槽位上限 (默认为 35)
     * @return int 最大槽位数
     */
    virtual int max_recommend_slots() const {
        return NeteasePlatformConfig::MAX_RECOMMEND_SLOTS;
    }

    /**
     * @brief 获取当天的推荐内容 (在推送时被调用)
     * @param[out] out_item 抓取成功的推荐内容结构体
     * @param[out] err_msg 失败时的错误信息
     * @return bool 是否成功获取
     */
    virtual bool fetch_recommendation(DailyRecommendationItem& out_item, std::string& err_msg) = 0;

    /**
     * @brief 获取当天的推荐内容 (支持传入指定账号 Cookie)
     * @param[out] out_item 抓取成功的推荐内容结构体
     * @param[out] err_msg 失败时的错误信息
     * @param cookie 指定账号 Cookie 凭据
     * @return bool 是否成功获取
     */
    virtual bool fetch_recommendation_with_cookie(DailyRecommendationItem& out_item, std::string& err_msg, const std::string& cookie) {
        (void)cookie;
        return fetch_recommendation(out_item, err_msg);
    }

    /**
     * @brief 获取当天的推荐歌曲完整列表 (支持传入指定账号 Cookie)
     * @param[out] out_items 抓取成功的推荐列表
     * @param[out] err_msg 失败时的错误信息
     * @param cookie 指定账号 Cookie 凭据
     * @return bool 是否成功获取
     */
    virtual bool fetch_recommendations_list_with_cookie(std::vector<DailyRecommendationItem>& out_items, std::string& err_msg, const std::string& cookie) {
        DailyRecommendationItem item;
        if (fetch_recommendation_with_cookie(item, err_msg, cookie)) {
            out_items.push_back(item);
            return true;
        }
        return false;
    }

    /**
     * @brief 获取指定槽位当天的推荐内容 (支持传入指定账号 Cookie 与槽位索引)
     * @param[out] out_item 抓取成功的推荐单曲
     * @param[out] err_msg 失败时的错误信息
     * @param cookie 指定账号 Cookie 凭据
     * @param slot 槽位索引 (0..max_recommend_slots-1)
     * @return bool 是否成功获取
     */
    virtual bool fetch_recommendation_with_slot(DailyRecommendationItem& out_item, std::string& err_msg, const std::string& cookie, int slot) {
        std::vector<DailyRecommendationItem> items;
        if (!fetch_recommendations_list_with_cookie(items, err_msg, cookie) || items.empty()) {
            return false;
        }
        if (slot < 0) slot = 0;
        out_item = items[slot % items.size()];
        return true;
    }

    /**
     * @brief 构建推送消息列表 (返回要发送的消息列表，通常包含音乐分享卡片与评价引导)
     * @param item 推荐数据项
     * @return std::vector<std::string> 消息文本/CQ 码列表
     */
    virtual std::vector<std::string> build_push_messages(const DailyRecommendationItem& item) = 0;

    /**
     * @brief 执行评价反馈操作 (如点赞或减少推荐)
     * @param item 目标推荐项
     * @param action 评价动作 (Like / Dislike / None)
     * @param[out] result_msg 执行结果反馈信息
     * @return bool 是否成功执行
     */
    virtual bool execute_evaluation(const DailyRecommendationItem& item, DailyEvalAction action, std::string& result_msg) = 0;

    /**
     * @brief 执行评价反馈操作 (支持传入指定账号 Cookie)
     * @param item 目标推荐项
     * @param action 评价动作 (Like / Dislike / None)
     * @param[out] result_msg 执行结果反馈信息
     * @param cookie 指定账号 Cookie 凭据
     * @return bool 是否成功执行
     */
    virtual bool execute_evaluation_with_cookie(const DailyRecommendationItem& item, DailyEvalAction action, std::string& result_msg, const std::string& cookie) {
        (void)cookie;
        return execute_evaluation(item, action, result_msg);
    }
};

/**
 * @brief 网易云音乐每日推荐歌曲推送实现 (11:00 推送第一首，23:00 静默评价反馈)
 */
class NeteaseDailySongRecommender : public IDailyRecommender {
public:
    explicit NeteaseDailySongRecommender(std::shared_ptr<NeteaseClient> netease_client,
                                         int push_hour = NeteasePlatformConfig::DEFAULT_PUSH_HOUR,
                                         int push_minute = NeteasePlatformConfig::DEFAULT_PUSH_MINUTE,
                                         int eval_hour = NeteasePlatformConfig::DEFAULT_EVAL_HOUR,
                                         int eval_minute = NeteasePlatformConfig::DEFAULT_EVAL_MINUTE)
        : netease_client_(std::move(netease_client)),
          push_hour_(push_hour), push_minute_(push_minute),
          eval_hour_(eval_hour), eval_minute_(eval_minute) {}

    std::string task_name() const override {
        return "netease_daily_song";
    }

    std::string platform() const override {
        return "netease";
    }

    int max_recommend_slots() const override {
        return NeteasePlatformConfig::MAX_RECOMMEND_SLOTS;
    }

    int push_hour() const override {
        return push_hour_;
    }

    int push_minute() const override {
        return push_minute_;
    }

    int eval_hour() const override {
        return eval_hour_;
    }

    int eval_minute() const override {
        return eval_minute_;
    }

    void set_push_time(int hour, int minute) {
        push_hour_ = hour;
        push_minute_ = minute;
    }

    void set_eval_time(int hour, int minute) {
        eval_hour_ = hour;
        eval_minute_ = minute;
    }

    bool fetch_recommendations_list_with_cookie(std::vector<DailyRecommendationItem>& out_items, std::string& err_msg, const std::string& cookie) override {
        if (!netease_client_) {
            err_msg = "netease_client is null";
            return false;
        }

        auto songs = netease_client_->get_daily_recommend_songs(cookie);
        if (songs.empty()) {
            err_msg = "No daily recommend songs returned (need netease cookie or network error)";
            return false;
        }

        out_items.clear();
        for (const auto& song : songs) {
            DailyRecommendationItem item;
            item.item_id = song.id;
            item.item_name = song.name;
            item.artist_name = song.artist_name;
            item.pic_url = song.pic_url;
            item.platform = "netease";
            item.extra_info = song.reason;
            out_items.push_back(std::move(item));
        }
        return true;
    }

    bool fetch_recommendation(DailyRecommendationItem& out_item, std::string& err_msg) override {
        return fetch_recommendation_with_cookie(out_item, err_msg, "");
    }

    bool fetch_recommendation_with_cookie(DailyRecommendationItem& out_item, std::string& err_msg, const std::string& cookie) override {
        return fetch_recommendation_with_slot(out_item, err_msg, cookie, 0);
    }

    bool fetch_recommendation_with_slot(DailyRecommendationItem& out_item, std::string& err_msg, const std::string& cookie, int slot) override {
        std::vector<DailyRecommendationItem> items;
        if (!fetch_recommendations_list_with_cookie(items, err_msg, cookie) || items.empty()) {
            return false;
        }
        if (slot < 0) slot = 0;
        out_item = items[slot % items.size()];
        return true;
    }

    std::vector<std::string> build_push_messages(const DailyRecommendationItem& item) override {
        // 1. 网易云音乐卡片 [CQ:music,type=163,id={song_id}]
        std::string card = "[CQ:music,type=163,id=" + std::to_string(item.item_id) + "]";
        // 2. 评价引导说明 (先发卡片再发引导说明)
        std::string prompt = "今日音乐推荐为\n「" + item.item_name + "」- " + item.artist_name + "\n"
                           + "你可以通过表情回应「[CQ:face,id=478] / [CQ:face,id=479]」对该音乐做出评价";
        return { card, prompt };
    }

    bool execute_evaluation(const DailyRecommendationItem& item, DailyEvalAction action, std::string& result_msg) override {
        return execute_evaluation_with_cookie(item, action, result_msg, "");
    }

    bool execute_evaluation_with_cookie(const DailyRecommendationItem& item, DailyEvalAction action, std::string& result_msg, const std::string& cookie) override {
        if (!netease_client_) {
            result_msg = "netease_client is null";
            return false;
        }

        switch (action) {
            case DailyEvalAction::Like: {
                bool ok = netease_client_->like_song(item.item_id, true, cookie);
                result_msg = ok ? "Successfully liked song on NetEase" : "Failed to like song on NetEase";
                return ok;
            }
            case DailyEvalAction::Dislike: {
                bool ok = netease_client_->dislike_recommend_song(item.item_id, cookie);
                result_msg = ok ? "Successfully reduced recommendation on NetEase" : "Failed to reduce recommendation on NetEase";
                return ok;
            }
            case DailyEvalAction::None:
            default:
                result_msg = "No action needed";
                return true;
        }
    }

    std::shared_ptr<NeteaseClient> get_client() const {
        return netease_client_;
    }

private:
    std::shared_ptr<NeteaseClient> netease_client_;
    int push_hour_{9};
    int push_minute_{0};
    int eval_hour_{23};
    int eval_minute_{40};
};

} // namespace bot::module::music
