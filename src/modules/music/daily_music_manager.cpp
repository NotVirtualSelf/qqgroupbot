/**
 * @file daily_music_manager.cpp
 * @brief 每日音乐推荐定时调度与表情评价结算管理器实现
 * @details 负责定时推送 (11:00)、提前手动日推识别与跳过、群表情回应计数统计与 23:00 静默反馈。
 */

#include "bot/modules/music/daily_music_manager.hpp"

#include <iostream>
#include <ctime>
#include <set>
#include <unordered_set>
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

bool DailyMusicManager::trigger_eval_now(const std::string& task_name, int64_t target_group) {
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    bool any_success = false;
    for (const auto& recommender : recommenders_) {
        if (task_name.empty() || recommender->task_name() == task_name) {
            bool ok = execute_eval(recommender, target_group);
            if (ok) any_success = true;
        }
    }
    return any_success;
}

int64_t DailyMusicManager::record_push(const std::string& task_name, int64_t group_id, int64_t message_id,
                                       const DailyRecommendationItem& item, const std::string& date,
                                       int64_t card_message_id) {
    std::string d = date.empty() ? get_current_date_str() : date;
    return db_.record_daily_push(task_name, group_id, message_id,
                                 item.item_id, item.item_name, item.artist_name,
                                 item.platform, d, 0, card_message_id);
}

void DailyMusicManager::handle_reaction(const bot::protocol::GroupReactionEvent& event) {
    int64_t target_msg_id = event.message_id;

    // 检查此 message_id 是否属于某条每日推送消息（严格限制：仅收集转发卡片本身，不允许有其他的）
    auto push_opt = db_.get_daily_push_by_message_id(target_msg_id);
    if (!push_opt.has_value() || push_opt->message_id != target_msg_id) {
        return; // 不是每日推荐转发卡片的消息回应，严格忽略
    }
    target_msg_id = push_opt->message_id;

    std::cout << "[DailyMusicManager] Received reaction event: msg_id=" << event.message_id
              << " (target_msg_id=" << target_msg_id << "), user_id=" << event.user_id
              << ", emoji_id=" << event.emoji_id << ", code=" << event.code
              << ", is_add=" << event.is_add << std::endl;

    // 代表「点赞 / 喜爱 / 认可」的表情集合：
    // 包含各类手势夸赞、点赞、爱心、常用好评 Emoji 以及网络流行梗表情（🐛、🐋、🐍、🧊、✈️、/拜谢）
    static const std::unordered_set<std::string> LIKE_EMOJIS = {
        // --- 手势 / 点赞类 ---
        "76", "/赞", "赞",
        "201", "/点赞", "点赞",
        "364", "/超级赞", "超级赞",
        "389", "/太赞了", "太赞了",
        "447",
        "380", "/真棒", "真棒",
        "346", "/你真棒棒", "你真棒棒",
        "221", "/顶呱呱", "顶呱呱",
        "299", "/牛啊", "牛啊",
        "306", "/牛气冲天", "牛气冲天",
        "356", "/666", "666",
        "444", "/超级6", "超级6",
        "478", "/对的对的", "对的对的", "10478",
        "79", "/胜利", "胜利",
        "124", "/OK", "OK",
        "398", "/超级ok", "超级ok",
        "498", "/中!", "中!", "/中", "中",
        "449", "/+1", "+1",
        "99", "/鼓掌", "鼓掌",
        "375", "/超级鼓掌", "超级鼓掌",

        // --- 喜爱 / 爱心类 ---
        "66", "/爱心", "爱心",
        "122", "/爱你", "爱你",
        "42", "/爱情", "爱情",
        "319", "/比心", "比心",
        "445", "/比个心", "比个心",
        "484", "/比爱心", "比爱心",
        "318", "/崇拜", "崇拜",
        "472", "/心动", "心动",
        "473", "/爱意", "爱意",
        "506", "/捡到宝了", "捡到宝了",

        // --- 常见好评 Emoji 符号及编码 ---
        "👍", "128077", "/厉害", "厉害",
        "❤️", "\xe2\x9d\xa4", "\xe2\x9d\xa4\xef\xb8\x8f", "10084",
        "👏", "128079",
        "🎉", "127881", "320", "/庆祝", "庆祝",
        "🔥", "128293", "/火", "火",
        "👌", "128076",
        "✌", "✌️", "9996",

        // --- 网络流行梗额外好评表情 ---
        "🐛", "128027", "/虫", "虫",
        "🐋", "128011", "🐳", "128051", "400339", "/鲸鱼", "鲸鱼",
        "🐍", "128013", "400335",
        "🧊", "129482",
        "✈️", "✈", "9992", "/飞机", "飞机",
        "297", "10297", "/拜谢", "拜谢"
    };

    // 代表「踩 / 讨厌 / 差评」的表情集合：
    // 根据需求仅精准选取：/不对不对、/续标识、❔、/棒棒糖
    static const std::unordered_set<std::string> DISLIKE_EMOJIS = {
        // --- 1. /不对不对 ---
        "479", "/不对不对", "不对不对", "10479",

        // --- 2. /续标识 ---
        "424", "/续标识", "续标识", "10424",

        // --- 3. ❔ (问号/质疑：涵盖 QQ NT 各种上报变体) ---
        // QQ 官方表情配置 (QCid: 10068, EMCode: 401145, AQLid: 134)
        "10068", "401145", "134",
        // 白色问号 Unicode 字符及其 UTF-8 变体
        "❔", "\xe2\x9d\x95",
        // 红色问号 Unicode 及其编码
        "❓", "\xe2\x9d\x93", "10067", "401144",
        // 常见 ASCII / 全角问号字符及其十进制编码
        "?", "？", "63", "65311",
        // 经典小黄脸疑问与问号脸表情 (QQ face ID: 32 与 268)
        "32", "132", "/疑问", "疑问",
        "268", "10268", "/问号", "问号", "/问号脸", "问号脸",

        // --- 4. /棒棒糖 ---
        "147", "/棒棒糖", "棒棒糖", "400147", "127853", "🍭", "\xf0\x9f\x8d\xad"
    };

    bool is_like = LIKE_EMOJIS.contains(event.emoji_id) || (!event.code.empty() && LIKE_EMOJIS.contains(event.code));
    bool is_dislike = DISLIKE_EMOJIS.contains(event.emoji_id) || (!event.code.empty() && DISLIKE_EMOJIS.contains(event.code));

    if (!is_like && !is_dislike) {
        std::cout << "[DailyMusicManager] Reaction emoji_id=" << event.emoji_id
                  << " (code=" << event.code << ") ignored (not in like/dislike sets)" << std::endl;
        return; // 用户添加了其他表情，不计入点赞/踩
    }

    std::string emoji_key = !event.emoji_id.empty() ? event.emoji_id : event.code;

    if (event.is_add) {
        int vote = is_like ? 1 : -1;
        db_.record_daily_reaction(target_msg_id, event.user_id, emoji_key, vote);
        std::cout << "[DailyMusicManager] User " << event.user_id << " reacted with "
                  << (is_like ? "Like (" : "Dislike (") << emoji_key << ")"
                  << " to daily music message " << target_msg_id << std::endl;
    } else {
        db_.remove_daily_reaction(target_msg_id, event.user_id, emoji_key);
        std::cout << "[DailyMusicManager] User " << event.user_id << " unreacted ("
                  << emoji_key << ") from daily music message "
                  << target_msg_id << std::endl;
    }
}

DailyEvalAction DailyMusicManager::evaluate_item_votes(const std::shared_ptr<IDailyRecommender>& recommender,
                                                      const DailyRecommendationItem& item,
                                                      int upvotes, int downvotes,
                                                      const std::string& cookie) {
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
        recommender->execute_evaluation_with_cookie(item, action, result_msg, cookie);
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
    // 缓存每个账号拉取的日推歌曲列表 (key: account_uid 或 source_gid)
    std::unordered_map<int64_t, std::vector<DailyRecommendationItem>> cached_song_lists;
    bool any_success = false;

    for (int64_t gid : groups) {
        // 核心逻辑：如果在每天默认推送日推时间之前已经手动触发过日推，那么不再发送默认日推
        if (target_group == 0 && db_.get_today_daily_push(gid, today_str).has_value()) {
            std::cout << "[DailyMusicManager] Group " << gid
                      << " already pushed daily music today before default push. Skipping default push." << std::endl;
            continue;
        }

        // 查询该群绑定的账号 (自持或共享) 与分配的日推槽位
        auto acc_opt = db_.get_group_netease_account(gid);
        int64_t acc_uid = acc_opt.has_value() ? acc_opt->account_uid : 0;
        int slot = acc_opt.has_value() ? acc_opt->recommend_slot : 0;
        std::string cookie = acc_opt.has_value() ? acc_opt->cookie : "";

        if (cached_song_lists.find(acc_uid) == cached_song_lists.end()) {
            std::vector<DailyRecommendationItem> items;
            std::string err_msg;
            if (!recommender->fetch_recommendations_list_with_cookie(items, err_msg, cookie) || items.empty()) {
                std::cerr << "[DailyMusicManager] Failed to fetch recommendations for group "
                          << gid << " (UID " << acc_uid << "): " << err_msg << std::endl;
                continue;
            }
            cached_song_lists[acc_uid] = std::move(items);
        }

        const auto& songs = cached_song_lists[acc_uid];
        if (songs.empty()) continue;

        // 根据群分配的槽位固定映射到日推中的对应歌曲
        if (slot < 0) slot = 0;
        const auto& item = songs[slot % songs.size()];

        auto forward_nodes = recommender->build_push_forward_nodes(item, "每日推荐", rpc_client_.self_uin());
        if (forward_nodes.empty()) continue;

        any_success = true;

        rpc_client_.send_group_forward_msg(gid, forward_nodes, [this, task_name = recommender->task_name(), gid, item, today_str, forward_nodes](const bot::protocol::SendGroupMsgResult& res) {
            int64_t forward_msg_id = res.success ? res.message_id : 0;
            if (forward_msg_id > 0) {
                std::cout << "[DailyMusicManager] Pushed daily music forward card to group " << gid
                          << ", message_id=" << forward_msg_id << std::endl;
                db_.record_daily_push(task_name, gid, forward_msg_id,
                                      item.item_id, item.item_name, item.artist_name,
                                      item.platform, today_str, 0, forward_msg_id);
            } else {
                std::cerr << "[DailyMusicManager] Failed to send forward card to group " << gid
                          << ": " << res.error_message << ", retrying without images..." << std::endl;
                auto no_img_nodes = forward_nodes;
                for (auto& node : no_img_nodes) {
                    size_t pos = 0;
                    while ((pos = node.data.content.find("[CQ:image", pos)) != std::string::npos) {
                        size_t end_pos = node.data.content.find(']', pos);
                        if (end_pos == std::string::npos) break;
                        if (pos > 0 && node.data.content[pos - 1] == '\n') {
                            node.data.content.erase(pos - 1, end_pos - (pos - 1) + 1);
                            pos = pos - 1;
                        } else {
                            node.data.content.erase(pos, end_pos - pos + 1);
                        }
                    }
                }
                rpc_client_.send_group_forward_msg(gid, no_img_nodes, [this, task_name, gid, item, today_str](const bot::protocol::SendGroupMsgResult& retry_res) {
                    int64_t retry_msg_id = retry_res.success ? retry_res.message_id : 0;
                    if (retry_msg_id > 0) {
                        std::cout << "[DailyMusicManager] Pushed daily music forward card (no images) to group " << gid
                                  << ", message_id=" << retry_msg_id << std::endl;
                        db_.record_daily_push(task_name, gid, retry_msg_id,
                                              item.item_id, item.item_name, item.artist_name,
                                              item.platform, today_str, 0, retry_msg_id);
                    } else {
                        std::cerr << "[DailyMusicManager] Retry failed to send forward card to group " << gid
                                  << ": " << retry_res.error_message << std::endl;
                    }
                });
            }
        });
    }

    return any_success;
}

bool DailyMusicManager::execute_eval(const std::shared_ptr<IDailyRecommender>& recommender, int64_t target_group) {
    if (!recommender) return false;

    auto unevaluated = db_.get_unevaluated_daily_pushes(recommender->task_name());
    if (unevaluated.empty()) {
        std::cout << "[DailyMusicManager] No unevaluated pushes found for ["
                  << recommender->task_name() << "]." << std::endl;
        return true;
    }

    // 评价数据不互通：各群独立展示本群评价数据，独立结算
    for (const auto& rec : unevaluated) {
        if (target_group != 0 && rec.group_id != target_group) {
            continue;
        }

        auto acc_opt = db_.get_group_netease_account(rec.group_id);
        std::string cookie = acc_opt.has_value() ? acc_opt->cookie : "";

        auto [up, down] = db_.get_daily_push_votes(rec.message_id);
        DailyRecommendationItem item;
        item.item_id = rec.item_id;
        item.item_name = rec.item_name;
        item.artist_name = rec.artist_name;
        item.platform = rec.platform;

        evaluate_item_votes(recommender, item, up, down, cookie);
        db_.mark_daily_push_evaluated(rec.id, up, down);
        db_.clear_daily_reactions(rec.message_id);
        db_.set_daily_evaluated(rec.group_id, rec.pushed_date);
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

namespace {
std::optional<std::pair<int, int>> g_mock_current_hm;
} // namespace

void DailyMusicManager::set_mock_time(std::optional<std::pair<int, int>> mock_hm) {
    g_mock_current_hm = mock_hm;
}

std::pair<int, int> DailyMusicManager::get_current_time_hm() {
    if (g_mock_current_hm.has_value()) {
        return *g_mock_current_hm;
    }
    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
#if defined(_WIN32)
    localtime_s(&local_tm, &now_c);
#else
    localtime_r(&now_c, &local_tm);
#endif
    return {local_tm.tm_hour, local_tm.tm_min};
}

void DailyMusicManager::update_schedule_times(int push_h, int push_m, int eval_h, int eval_m) {
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    for (auto& ctx : task_contexts_) {
        if (!ctx || !ctx->recommender) continue;
        auto netease_rec = std::dynamic_pointer_cast<NeteaseDailySongRecommender>(ctx->recommender);
        if (netease_rec) {
            netease_rec->set_push_time(push_h, push_m);
            netease_rec->set_eval_time(eval_h, eval_m);
        }
        if (is_running_) {
            if (ctx->push_timer) {
                ctx->push_timer->cancel();
                schedule_push_for_task(*ctx);
            }
            if (ctx->eval_timer) {
                ctx->eval_timer->cancel();
                schedule_eval_for_task(*ctx);
            }
        }
    }
}

std::string DailyMusicManager::get_current_date_str(int update_hour, int update_minute) {
    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
#if defined(_WIN32)
    localtime_s(&local_tm, &now_c);
#else
    localtime_r(&now_c, &local_tm);
#endif

    auto [cur_h, cur_m] = get_current_time_hm();
    // 如果当前时间位于 00:00 ~ 更新时间 (如 06:00) 之间，则属于上一日日推周期
    if ((cur_h * 60 + cur_m) < (update_hour * 60 + update_minute)) {
        local_tm.tm_mday -= 1;
        std::mktime(&local_tm);
    }

    std::ostringstream oss;
    oss << std::put_time(&local_tm, "%Y-%m-%d");
    return oss.str();
}

} // namespace bot::module::music
