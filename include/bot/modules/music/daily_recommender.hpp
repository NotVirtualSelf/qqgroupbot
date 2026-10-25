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
     * @brief 获取当天的推荐内容 (在推送时被调用)
     * @param[out] out_item 抓取成功的推荐内容结构体
     * @param[out] err_msg 失败时的错误信息
     * @return bool 是否成功获取
     */
    virtual bool fetch_recommendation(DailyRecommendationItem& out_item, std::string& err_msg) = 0;

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
};

/**
 * @brief 网易云音乐每日推荐歌曲推送实现 (11:00 推送第一首，23:00 静默评价反馈)
 */
class NeteaseDailySongRecommender : public IDailyRecommender {
public:
    explicit NeteaseDailySongRecommender(std::shared_ptr<NeteaseClient> netease_client)
        : netease_client_(std::move(netease_client)) {}

    std::string task_name() const override {
        return "netease_daily_song";
    }

    std::string platform() const override {
        return "netease";
    }

    int push_hour() const override {
        return 11;
    }

    int push_minute() const override {
        return 0;
    }

    int eval_hour() const override {
        return 23;
    }

    int eval_minute() const override {
        return 0;
    }

    bool fetch_recommendation(DailyRecommendationItem& out_item, std::string& err_msg) override {
        if (!netease_client_) {
            err_msg = "netease_client is null";
            return false;
        }

        auto songs = netease_client_->get_daily_recommend_songs();
        if (songs.empty()) {
            err_msg = "No daily recommend songs returned (need netease cookie or network error)";
            return false;
        }

        // 选取日推列表的第一首
        const auto& first_song = songs[0];
        out_item.item_id = first_song.id;
        out_item.item_name = first_song.name;
        out_item.artist_name = first_song.artist_name;
        out_item.pic_url = first_song.pic_url;
        out_item.platform = "netease";
        out_item.extra_info = first_song.reason;
        return true;
    }

    std::vector<std::string> build_push_messages(const DailyRecommendationItem& item) override {
        // 1. 网易云音乐卡片 [CQ:music,type=163,id={song_id}]
        std::string card = "[CQ:music,type=163,id=" + std::to_string(item.item_id) + "]";
        // 2. 评价引导说明
        std::string prompt = "当日音乐日推为「" + item.item_name + "」- " + item.artist_name + "\n"
                           + "你可以通过表情回应「[CQ:face,id=478] / [CQ:face,id=479]」对该音乐做出评价";
        return { card, prompt };
    }

    bool execute_evaluation(const DailyRecommendationItem& item, DailyEvalAction action, std::string& result_msg) override {
        if (!netease_client_) {
            result_msg = "netease_client is null";
            return false;
        }

        switch (action) {
            case DailyEvalAction::Like: {
                bool ok = netease_client_->like_song(item.item_id, true);
                result_msg = ok ? "Successfully liked song on NetEase" : "Failed to like song on NetEase";
                return ok;
            }
            case DailyEvalAction::Dislike: {
                bool ok = netease_client_->dislike_recommend_song(item.item_id);
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
};

} // namespace bot::module::music
