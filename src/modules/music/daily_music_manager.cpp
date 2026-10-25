/**
 * @file daily_music_manager.cpp
 * @brief 每日音乐推荐定时调度与表情评价结算管理器实现
 * @details 负责定时推送 (11:00)、提前手动日推识别与跳过、群表情回应计数统计与 23:00 静默反馈。
 */

#include "bot/modules/music/daily_music_manager.hpp"

#include <iostream>
#include <ctime>
#include <set>
#include <chrono>
#include <sstream>
#include <iomanip>

namespace bot::module::music {

DailyMusicManager::DailyMusicManager(boost::asio::io_context& ioc,
                                     bot::protocol::RpcClient& rpc_client,
                                     MusicDatabase& db)
    : ioc_(ioc)
    , rpc_client_(rpc_client)
    , db_(db)
{}

DailyMusicManager::~DailyMusicManager() {
    stop();
}

void DailyMusicManager::register_recommender(std::shared_ptr<IDailyRecommender> recommender) {
    if (!recommender) return;
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    recommenders_.push_back(recommender);

    auto ctx = std::make_unique<RecommenderTaskContext>();
    ctx->recommender = recommender;
    ctx->push_timer = std::make_unique<boost::asio::steady_timer>(ioc_);
    ctx->eval_timer = std::make_unique<boost::asio::steady_timer>(ioc_);

    if (is_running_) {
        schedule_push_for_task(*ctx);
        schedule_eval_for_task(*ctx);
    }
    task_contexts_.push_back(std::move(ctx));
}

const std::vector<std::shared_ptr<IDailyRecommender>>& DailyMusicManager::get_recommenders() const {
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    return recommenders_;
}

void DailyMusicManager::start() {
    if (is_running_.exchange(true)) {
        return;
    }

    std::lock_guard<std::mutex> lock(tasks_mutex_);
    for (auto& ctx : task_contexts_) {
        schedule_push_for_task(*ctx);
        schedule_eval_for_task(*ctx);
    }
    std::cout << "[DailyMusicManager] Started with " << task_contexts_.size()
              << " registered recommender task(s)." << std::endl;
}

void DailyMusicManager::stop() {
    if (!is_running_.exchange(false)) {
        return;
    }

    std::lock_guard<std::mutex> lock(tasks_mutex_);
    for (auto& ctx : task_contexts_) {
        boost::system::error_code ec;
        if (ctx->push_timer) ctx->push_timer->cancel(ec);
        if (ctx->eval_timer) ctx->eval_timer->cancel(ec);
    }
    std::cout << "[DailyMusicManager] Stopped." << std::endl;
}

void DailyMusicManager::schedule_push_for_task(RecommenderTaskContext& task_ctx) {
    if (!is_running_) return;

    auto delay = calculate_delay_until(task_ctx.recommender->push_hour(),
                                       task_ctx.recommender->push_minute());
    task_ctx.push_timer->expires_after(delay);
    task_ctx.push_timer->async_wait([this, ctx_ptr = &task_ctx](const boost::system::error_code& ec) {
        on_push_timer(ctx_ptr, ec);
    });

    std::cout << "[DailyMusicManager] Task [" << task_ctx.recommender->task_name()
              << "] next push in " << delay.count() << " seconds ("
              << task_ctx.recommender->push_hour() << ":"
              << std::setw(2) << std::setfill('0') << task_ctx.recommender->push_minute()
              << ")" << std::endl;
}

void DailyMusicManager::schedule_eval_for_task(RecommenderTaskContext& task_ctx) {
    if (!is_running_) return;

    auto delay = calculate_delay_until(task_ctx.recommender->eval_hour(),
                                       task_ctx.recommender->eval_minute());
    task_ctx.eval_timer->expires_after(delay);
    task_ctx.eval_timer->async_wait([this, ctx_ptr = &task_ctx](const boost::system::error_code& ec) {
        on_eval_timer(ctx_ptr, ec);
    });

    std::cout << "[DailyMusicManager] Task [" << task_ctx.recommender->task_name()
              << "] next eval in " << delay.count() << " seconds ("
              << task_ctx.recommender->eval_hour() << ":"
              << std::setw(2) << std::setfill('0') << task_ctx.recommender->eval_minute()
              << ")" << std::endl;
}

void DailyMusicManager::on_push_timer(RecommenderTaskContext* task_ctx, const boost::system::error_code& ec) {
    if (ec || !is_running_ || !task_ctx) {
        return;
    }

    std::cout << "[DailyMusicManager] Push timer triggered for task ["
              << task_ctx->recommender->task_name() << "]" << std::endl;
    execute_push(task_ctx->recommender);

    schedule_push_for_task(*task_ctx);
}

void DailyMusicManager::on_eval_timer(RecommenderTaskContext* task_ctx, const boost::system::error_code& ec) {
    if (ec || !is_running_ || !task_ctx) {
        return;
    }

    std::cout << "[DailyMusicManager] Evaluation timer triggered for task ["
              << task_ctx->recommender->task_name() << "]" << std::endl;
    execute_eval(task_ctx->recommender);

    schedule_eval_for_task(*task_ctx);
}

bool DailyMusicManager::trigger_push_now(const std::string& task_name, int64_t target_group) {
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    bool any_success = false;
    for (const auto& recommender : recommenders_) {
        if (task_name.empty() || recommender->task_name() == task_name) {
            bool ok = execute_push(recommender, target_group);
            if (ok) any_success = true;
        }
    }
    return any_success;
}

bool DailyMusicManager::trigger_eval_now(const std::string& task_name) {
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    bool any_success = false;
    for (const auto& recommender : recommenders_) {
        if (task_name.empty() || recommender->task_name() == task_name) {
            bool ok = execute_eval(recommender);
            if (ok) any_success = true;
        }
    }
    return any_success;
}

int64_t DailyMusicManager::record_push(const std::string& task_name, int64_t group_id, int64_t message_id,
                                      const DailyRecommendationItem& item, const std::string& date) {
    std::string d = date.empty() ? get_current_date_str() : date;
    return db_.record_daily_push(task_name, group_id, message_id,
                                 item.item_id, item.item_name, item.artist_name,
                                 item.platform, d);
}

void DailyMusicManager::handle_reaction(const bot::protocol::GroupReactionEvent& event) {
    // 检查此 message_id 是否属于某条每日推送消息
    auto push_opt = db_.get_daily_push_by_message_id(event.message_id);
    if (!push_opt.has_value()) {
        return; // 不是每日推荐的消息回应，忽略
    }

    // 检查表情类型
    // "对的对的": 478 或 /对的对的 或 对的对的
    // "不对不对": 479 或 /不对不对 或 不对不对
    bool is_like = (event.emoji_id == "478" || event.emoji_id == "/对的对的" || event.emoji_id == "对的对的");
    bool is_dislike = (event.emoji_id == "479" || event.emoji_id == "/不对不对" || event.emoji_id == "不对不对");

    if (!is_like && !is_dislike) {
        return; // 用户添加了其他表情，不计入点赞/踩
    }

    if (event.is_add) {
        int vote = is_like ? 1 : -1;
        db_.record_daily_reaction(event.message_id, event.user_id, vote);
        std::cout << "[DailyMusicManager] User " << event.user_id << " reacted with "
                  << (is_like ? "Like (对的对的)" : "Dislike (不对不对)")
                  << " to daily music message " << event.message_id << std::endl;
    } else {
        db_.remove_daily_reaction(event.message_id, event.user_id);
        std::cout << "[DailyMusicManager] User " << event.user_id << " unreacted from daily music message "
                  << event.message_id << std::endl;
    }
}

DailyEvalAction DailyMusicManager::evaluate_item_votes(const std::shared_ptr<IDailyRecommender>& recommender,
                                                      const DailyRecommendationItem& item,
                                                      int upvotes, int downvotes) {
    DailyEvalAction action = DailyEvalAction::None;
    // 规则 1: 如果点赞数量大于踩的数量+3，则点赞这首歌
    // 规则 2: 如果踩的数量大于等于点赞数量，则减少推荐这首歌
    // 规则 3: 其他情况下不对该音乐做任何处理
    if (upvotes > downvotes + 3) {
        action = DailyEvalAction::Like;
    } else if (downvotes >= upvotes) {
        action = DailyEvalAction::Dislike;
    } else {
        action = DailyEvalAction::None;
    }

    std::string result_msg;
    if (recommender) {
        recommender->execute_evaluation(item, action, result_msg);
    }

    std::cout << "[DailyMusicManager] Silent Evaluation: item [" << item.item_name
              << "] (ID=" << item.item_id << ", platform=" << item.platform
              << ") => Upvotes=" << upvotes << ", Downvotes=" << downvotes
              << ", Action=" << (action == DailyEvalAction::Like ? "LIKE" :
                                (action == DailyEvalAction::Dislike ? "DISLIKE" : "NONE"))
              << ", Result=" << result_msg << std::endl;

    return action;
}

bool DailyMusicManager::execute_push(const std::shared_ptr<IDailyRecommender>& recommender, int64_t target_group) {
    if (!recommender) return false;

    DailyRecommendationItem item;
    std::string err_msg;
    if (!recommender->fetch_recommendation(item, err_msg)) {
        std::cerr << "[DailyMusicManager] Failed to fetch recommendation for ["
                  << recommender->task_name() << "]: " << err_msg << std::endl;
        return false;
    }

    std::vector<int64_t> groups;
    if (target_group != 0) {
        groups.push_back(target_group);
    } else {
        groups = db_.get_daily_music_groups();
        if (groups.empty()) {
            // 如果尚未配置具体开启群，后备使用所有已关注过艺术家的群
            std::set<int64_t> unique_gids;
            auto artists = db_.get_all_unique_artists();
            for (const auto& a : artists) {
                auto gids = db_.get_subscribers_for_artist(a.artist_id, a.platform);
                unique_gids.insert(gids.begin(), gids.end());
            }
            groups.assign(unique_gids.begin(), unique_gids.end());
        }
    }

    if (groups.empty()) {
        std::cout << "[DailyMusicManager] No subscribed groups found for daily music push ["
                  << recommender->task_name() << "]." << std::endl;
        return false;
    }

    std::string today_str = get_current_date_str();
    auto msgs = recommender->build_push_messages(item);

    for (int64_t gid : groups) {
        if (msgs.empty()) continue;

        // 核心逻辑：如果在每天默认推送日推时间之前已经手动触发过日推，那么不再发送默认日推
        if (target_group == 0 && db_.get_today_daily_push(gid, today_str).has_value()) {
            std::cout << "[DailyMusicManager] Group " << gid
                      << " already pushed daily music today before default push. Skipping default push." << std::endl;
            continue;
        }

        // 1. 发送音乐卡片 (记录首条信息入库，用于后续引用回复与表情回应统计)
        rpc_client_.send_group_msg(gid, msgs[0], [this, task_name = recommender->task_name(), gid, item, today_str](const bot::protocol::SendGroupMsgResult& res) {
            if (res.success) {
                std::cout << "[DailyMusicManager] Pushed daily music to group " << gid
                          << ", message_id=" << res.message_id << std::endl;
                db_.record_daily_push(task_name, gid, res.message_id,
                                      item.item_id, item.item_name, item.artist_name,
                                      item.platform, today_str);
            } else {
                std::cerr << "[DailyMusicManager] Failed to push to group " << gid
                          << ": " << res.error_message << std::endl;
            }
        });

        // 2. 发送评价引导提示消息 (如：当日音乐日推为「歌曲名」- 艺术家名...)
        for (size_t m = 1; m < msgs.size(); ++m) {
            rpc_client_.send_group_msg(gid, msgs[m]);
        }
    }

    return true;
}

bool DailyMusicManager::execute_eval(const std::shared_ptr<IDailyRecommender>& recommender) {
    if (!recommender) return false;

    auto unevaluated = db_.get_unevaluated_daily_pushes(recommender->task_name());
    if (unevaluated.empty()) {
        std::cout << "[DailyMusicManager] No unevaluated pushes found for ["
                  << recommender->task_name() << "]." << std::endl;
        return true;
    }

    // 按 item_id 进行聚合，不同群中推的同一首歌合并投票数
    struct AggItem {
        DailyRecommendationItem item;
        std::vector<int64_t> record_ids;
        std::vector<int64_t> message_ids;
        std::vector<std::pair<int64_t, std::string>> group_dates;
        int total_upvotes{0};
        int total_downvotes{0};
    };

    std::unordered_map<int64_t, AggItem> item_map;
    for (const auto& rec : unevaluated) {
        auto [up, down] = db_.get_daily_push_votes(rec.message_id);
        auto& agg = item_map[rec.item_id];
        agg.item.item_id = rec.item_id;
        agg.item.item_name = rec.item_name;
        agg.item.artist_name = rec.artist_name;
        agg.item.platform = rec.platform;
        agg.record_ids.push_back(rec.id);
        agg.message_ids.push_back(rec.message_id);
        agg.group_dates.emplace_back(rec.group_id, rec.pushed_date);
        agg.total_upvotes += up;
        agg.total_downvotes += down;
    }

    for (const auto& [item_id, agg] : item_map) {
        evaluate_item_votes(recommender, agg.item, agg.total_upvotes, agg.total_downvotes);
        for (int64_t rec_id : agg.record_ids) {
            db_.mark_daily_push_evaluated(rec_id, agg.total_upvotes, agg.total_downvotes);
        }
        for (int64_t mid : agg.message_ids) {
            db_.clear_daily_reactions(mid);
        }
        for (const auto& [gid, pdate] : agg.group_dates) {
            db_.set_daily_evaluated(gid, pdate);
        }
    }

    return true;
}

std::chrono::seconds DailyMusicManager::calculate_delay_until(int target_hour, int target_minute) {
    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
#if defined(_WIN32)
    localtime_s(&local_tm, &now_c);
#else
    localtime_r(&now_c, &local_tm);
#endif

    std::tm target_tm = local_tm;
    target_tm.tm_hour = target_hour;
    target_tm.tm_min = target_minute;
    target_tm.tm_sec = 0;

    std::time_t target_c = std::mktime(&target_tm);
    if (target_c <= now_c) {
        // 今天目标时间已过，定在明天同一时间
        target_tm.tm_mday += 1;
        target_c = std::mktime(&target_tm);
    }
    return std::chrono::seconds(target_c - now_c);
}

std::string DailyMusicManager::get_current_date_str() {
    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
#if defined(_WIN32)
    localtime_s(&local_tm, &now_c);
#else
    localtime_r(&now_c, &local_tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&local_tm, "%Y-%m-%d");
    return oss.str();
}

} // namespace bot::module::music
