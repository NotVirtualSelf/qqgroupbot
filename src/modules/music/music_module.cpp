/**
 * @file music_module.cpp
 * @brief 音乐订阅与新歌监控推送主业务模块实现
 * @details 实现了群命令交互路由、候选防错消歧卡片生成、新专辑/单曲轮询与严格逐个推送、
 *          私聊管理员鉴权与网易云扫码登录流程。
 */

#include "bot/modules/music/music_module.hpp"

#include <iostream>
#include <sstream>
#include <chrono>
#include <cctype>
#include <algorithm>
#include <filesystem>
#include <set>

namespace bot::module::music {

namespace {

std::string trim(const std::string& str) {
    auto first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    auto last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

std::string to_lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::vector<bot::protocol::ForwardNode> strip_cq_images(const std::vector<bot::protocol::ForwardNode>& nodes) {
    auto result = nodes;
    for (auto& node : result) {
        std::string& s = node.data.content;
        size_t pos = 0;
        while ((pos = s.find("[CQ:image", pos)) != std::string::npos) {
            size_t end_pos = s.find(']', pos);
            if (end_pos == std::string::npos) {
                break;
            }
            if (pos > 0 && s[pos - 1] == '\n') {
                s.erase(pos - 1, end_pos - (pos - 1) + 1);
                pos = pos - 1;
            } else {
                s.erase(pos, end_pos - pos + 1);
            }
        }
        if (s.empty()) {
            s = "（暂无封面）";
        }
    }
    return result;
}

std::string escape_cq(const std::string& str) {
    std::string res;
    for (char c : str) {
        if (c == '&') res += "&amp;";
        else if (c == '[') res += "&#91;";
        else if (c == ']') res += "&#93;";
        else if (c == ',') res += "&#44;";
        else res += c;
    }
    return res;
}

// 解码下一个 UTF-8 字符，返回 (codepoint, byte_len)
std::pair<uint32_t, size_t> decode_utf8(const std::string& str, size_t index) {
    if (index >= str.size()) return {0, 0};
    unsigned char c = static_cast<unsigned char>(str[index]);
    if (c < 0x80) {
        return {c, 1};
    } else if ((c & 0xE0) == 0xC0) {
        if (index + 1 >= str.size()) return {c, 1};
        uint32_t cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(str[index + 1]) & 0x3F);
        return {cp, 2};
    } else if ((c & 0xF0) == 0xE0) {
        if (index + 2 >= str.size()) return {c, 1};
        uint32_t cp = ((c & 0x0F) << 12) |
                      ((static_cast<unsigned char>(str[index + 1]) & 0x3F) << 6) |
                      (static_cast<unsigned char>(str[index + 2]) & 0x3F);
        return {cp, 3};
    } else if ((c & 0xF8) == 0xF0) {
        if (index + 3 >= str.size()) return {c, 1};
        uint32_t cp = ((c & 0x07) << 18) |
                      ((static_cast<unsigned char>(str[index + 1]) & 0x3F) << 12) |
                      ((static_cast<unsigned char>(str[index + 2]) & 0x3F) << 6) |
                      (static_cast<unsigned char>(str[index + 3]) & 0x3F);
        return {cp, 4};
    }
    return {c, 1};
}

// 计算单个 Unicode 字符的视觉显示宽度 (半角字符=1, 全角/CJK/Emoji=2)
int get_code_point_width(uint32_t cp) {
    if (cp < 0x80) return 1;
    if ((cp >= 0x1100 && cp <= 0x115F) ||
        (cp >= 0x2E80 && cp <= 0xA4CF && cp != 0x303F) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) ||
        (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE10 && cp <= 0xFE19) ||
        (cp >= 0xFE30 && cp <= 0xFE6F) ||
        (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||
        (cp >= 0x1F000 && cp <= 0x1FFFF) ||
        (cp >= 0x20000 && cp <= 0x3FFFF)) {
        return 2;
    }
    return 1;
}

// 计算 UTF-8 字符串的总视觉显示宽度
int get_str_display_width(const std::string& str) {
    int total = 0;
    size_t i = 0;
    while (i < str.size()) {
        auto [cp, len] = decode_utf8(str, i);
        total += get_code_point_width(cp);
        i += (len > 0 ? len : 1);
    }
    return total;
}

// 格式化艺术家单行内容：例如 ┃[网易云] Porter Robinson
// 当整行显示宽度超过 max_line_width (默认 26，针对手机 QQ 聊天气泡不折行的大众且保守选择) 时截断并追加 ...
std::string format_artist_line(const std::string& plat_tag, const std::string& artist_name, int max_line_width = 26) {
    std::string prefix = "┃[" + plat_tag + "] ";
    int prefix_w = get_str_display_width(prefix);
    int max_name_w = (max_line_width > prefix_w) ? (max_line_width - prefix_w) : 0;

    int name_w = get_str_display_width(artist_name);
    if (name_w <= max_name_w) {
        return prefix + artist_name;
    }

    int target_w = (max_name_w >= 3) ? (max_name_w - 3) : 0;
    std::string truncated_name;
    int curr_w = 0;
    size_t i = 0;
    while (i < artist_name.size()) {
        auto [cp, len] = decode_utf8(artist_name, i);
        int w = get_code_point_width(cp);
        if (curr_w + w > target_w) {
            break;
        }
        truncated_name.append(artist_name, i, len);
        curr_w += w;
        i += (len > 0 ? len : 1);
    }
    return prefix + truncated_name + "...";
}

// 构造关注结果卡片文本
std::string format_follow_response(const std::vector<std::pair<std::string, std::string>>& added,
                                   const std::vector<std::pair<std::string, std::string>>& skipped) {
    std::ostringstream oss;
    if (!added.empty()) {
        oss << "┏⊕ Added━";
        for (const auto& item : added) {
            oss << "\n" << format_artist_line(item.first, item.second);
        }
        if (!skipped.empty()) {
            oss << "\n┣⊗ Skipped━";
            for (const auto& item : skipped) {
                oss << "\n" << format_artist_line(item.first, item.second);
            }
        }
    } else if (!skipped.empty()) {
        oss << "┏⊗ Skipped━";
        for (const auto& item : skipped) {
            oss << "\n" << format_artist_line(item.first, item.second);
        }
    }
    return oss.str();
}

// 构造取关结果卡片文本
std::string format_unfollow_response(const std::vector<std::pair<std::string, std::string>>& removed) {
    std::ostringstream oss;
    oss << "┏⊖ Removed━";
    for (const auto& item : removed) {
        oss << "\n" << format_artist_line(item.first, item.second);
    }
    return oss.str();
}

std::string get_platform_display_tag(const std::string& platform) {
    if (platform == "qqmusic" || platform == "qq") {
        return "QQ音乐";
    }
    return "网易云";
}

std::string get_platform_full_name(const std::string& platform) {
    if (platform == "qqmusic" || platform == "qq") {
        return "QQ音乐";
    }
    return "网易云音乐";
}

} // namespace

MusicModule::MusicModule(boost::asio::io_context& ioc,
                         bot::protocol::RpcClient& rpc_client,
                         std::string db_path,
                         std::chrono::seconds poll_interval,
                         std::chrono::seconds cover_update_interval)
    : ioc_(ioc)
    , rpc_client_(rpc_client)
    , db_(std::move(db_path))
    , netease_(std::make_shared<NeteaseClient>())
    , poll_interval_(poll_interval)
    , timer_(ioc)
    , cover_update_interval_(cover_update_interval)
    , cover_timer_(ioc)
{
    daily_manager_ = std::make_unique<DailyMusicManager>(ioc_, rpc_client_, db_);
    daily_manager_->register_recommender(std::make_shared<NeteaseDailySongRecommender>(netease_));
}

MusicModule::~MusicModule() {
    stop();
}

bool MusicModule::start() {
    if (!db_.init()) {
        std::cerr << "[MusicModule] Failed to initialize database" << std::endl;
        return false;
    }

    std::string saved_cookie = db_.get_config("netease_cookie");
    if (!saved_cookie.empty()) {
        netease_->set_cookie(saved_cookie);
        std::cout << "[MusicModule] Loaded NetEase cookie from database." << std::endl;
    }

    is_running_ = true;
    schedule_timer();
    schedule_cover_timer();
    if (daily_manager_) {
        daily_manager_->start();
    }
    std::cout << "[MusicModule] MusicModule started! Polling interval: "
              << poll_interval_.count() << "s, Daily cover update interval: "
              << cover_update_interval_.count() << "s." << std::endl;
    return true;
}

void MusicModule::stop() {
    if (!is_running_.exchange(false)) {
        return;
    }
    if (daily_manager_) {
        daily_manager_->stop();
    }
    cancel_qr_login_session();
    boost::system::error_code ec;
    timer_.cancel(ec);
    cover_timer_.cancel(ec);
}

bool MusicModule::handle_group_reaction(const bot::protocol::GroupReactionEvent& event) {
    if (daily_manager_) {
        daily_manager_->handle_reaction(event);
        return true;
    }
    return false;
}

void MusicModule::schedule_timer() {
    if (!is_running_) return;

    timer_.expires_after(poll_interval_);
    timer_.async_wait([this](const boost::system::error_code& ec) {
        on_timer(ec);
    });
}

void MusicModule::on_timer(const boost::system::error_code& ec) {
    if (ec || !is_running_) {
        return;
    }

    check_new_albums_once();
    schedule_timer();
}

void MusicModule::schedule_cover_timer() {
    if (!is_running_) return;

    cover_timer_.expires_after(cover_update_interval_);
    cover_timer_.async_wait([this](const boost::system::error_code& ec) {
        on_cover_timer(ec);
    });
}

void MusicModule::on_cover_timer(const boost::system::error_code& ec) {
    if (ec || !is_running_) {
        return;
    }

    update_artist_covers_once(false);
    schedule_cover_timer();
}

void MusicModule::update_artist_covers_once(bool force) {
    auto artists = db_.get_all_unique_artists();
    if (artists.empty()) {
        return;
    }

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    std::cout << "[MusicModule] Updating artist covers (daily silent refresh)..." << std::endl;

    for (const auto& artist : artists) {
        // 如果未开启强制更新，且上次更新在 24 小时 (86400 秒) 之内，则跳过
        if (!force && artist.cover_updated_at > 0 && (now - artist.cover_updated_at) < 86400) {
            continue;
        }

        std::string new_pic_url;
        if (artist.platform == "qqmusic" || artist.platform == "qq") {
            new_pic_url = qqmusic_.get_artist_pic_url(artist.artist_id, artist.artist_name, artist.pic_url);
        } else if (artist.platform == "netease" || artist.platform.empty()) {
            new_pic_url = netease_->get_artist_pic_url(artist.artist_id);
        }

        if (!new_pic_url.empty()) {
            db_.update_artist_pic_url(artist.artist_id, new_pic_url, artist.platform, now);
        } else {
            // 保持原封面不变，但刷新检查时间戳
            db_.update_artist_pic_url(artist.artist_id, artist.pic_url, artist.platform, now);
        }
    }
}

std::string MusicModule::format_track_duration(int64_t duration_ms) {
    int64_t sec = duration_ms / 1000;
    int64_t m = sec / 60;
    int64_t s = sec % 60;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02lld分%02lld秒", static_cast<long long>(m), static_cast<long long>(s));
    return std::string(buf);
}

std::string MusicModule::format_total_duration(int64_t duration_ms) {
    int64_t total_sec = duration_ms / 1000;
    int64_t h = total_sec / 3600;
    int64_t m = (total_sec % 3600) / 60;
    int64_t s = total_sec % 60;
    char buf[64];
    if (h > 0) {
        std::snprintf(buf, sizeof(buf), "%lld小时%02lld分%02lld秒",
                      static_cast<long long>(h), static_cast<long long>(m), static_cast<long long>(s));
    } else {
        std::snprintf(buf, sizeof(buf), "%02lld分%02lld秒",
                      static_cast<long long>(m), static_cast<long long>(s));
    }
    return std::string(buf);
}

std::vector<bot::protocol::ForwardNode> MusicModule::build_album_forward_nodes(const AlbumDetail& album) {
    std::vector<bot::protocol::ForwardNode> nodes;

    // 只有一首单曲时：判定为新单曲卡片
    if (album.tracks.size() == 1) {
        const auto& track = album.tracks[0];

        // 1. 封面：（封面）
        {
            bot::protocol::ForwardNode node;
            node.data.name = "封面";
            node.data.uin = "2854196306";
            node.data.content = !album.pic_url.empty() ? ("[CQ:image,file=" + album.pic_url + "]") : "（暂无封面）";
            nodes.push_back(std::move(node));
        }

        // 2. 单曲：「歌曲名」
        {
            bot::protocol::ForwardNode node;
            node.data.name = "单曲";
            node.data.uin = "2854196306";
            std::string song_name = !track.name.empty() ? track.name : album.name;
            node.data.content = "「" + song_name + "」";
            nodes.push_back(std::move(node));
        }

        // 3. 创作者：「所有艺术家」
        {
            bot::protocol::ForwardNode node;
            node.data.name = "创作者";
            node.data.uin = "2854196306";
            std::string creator = !track.artist_name.empty() ? track.artist_name : album.artist_name;
            node.data.content = "「" + creator + "」";
            nodes.push_back(std::move(node));
        }

        // 4. 基本信息：「单曲时长」
        {
            bot::protocol::ForwardNode node;
            node.data.name = "基本信息";
            node.data.uin = "2854196306";
            node.data.content = "「" + format_track_duration(track.duration_ms) + "」";
            nodes.push_back(std::move(node));
        }

        return nodes;
    }

    // 多首曲目时：判定为新专辑卡片
    // 1. 封面
    {
        bot::protocol::ForwardNode node;
        node.data.name = "封面";
        node.data.uin = "2854196306";
        node.data.content = !album.pic_url.empty() ? ("[CQ:image,file=" + album.pic_url + "]") : "（暂无封面）";
        nodes.push_back(std::move(node));
    }

    // 2. 专辑
    {
        bot::protocol::ForwardNode node;
        node.data.name = "专辑";
        node.data.uin = "2854196306";
        node.data.content = "「" + album.name + "」";
        nodes.push_back(std::move(node));
    }

    // 3. 创作者
    {
        bot::protocol::ForwardNode node;
        node.data.name = "创作者";
        node.data.uin = "2854196306";
        node.data.content = "「" + album.artist_name + "」";
        nodes.push_back(std::move(node));
    }

    // 4. 基本信息
    {
        bot::protocol::ForwardNode node;
        node.data.name = "基本信息";
        node.data.uin = "2854196306";
        node.data.content = "「" + std::to_string(album.tracks.size()) + "首 · " + format_total_duration(album.total_duration_ms) + "」";
        nodes.push_back(std::move(node));
    }

    // 5 ~ N+4. 各曲目：曲目 #1、曲目 #2...
    for (size_t i = 0; i < album.tracks.size(); ++i) {
        const auto& track = album.tracks[i];
        bot::protocol::ForwardNode node;
        node.data.name = "曲目 #" + std::to_string(i + 1);
        node.data.uin = "2854196306";
        std::string creator = !track.artist_name.empty() ? track.artist_name : album.artist_name;
        node.data.content = track.name + "\n- " + creator + "\n- " + format_track_duration(track.duration_ms);
        nodes.push_back(std::move(node));
    }

    return nodes;
}

void MusicModule::check_new_songs_once(int64_t target_group) {
    check_new_albums_once(target_group);
}

void MusicModule::check_new_albums_once(int64_t target_group) {
    std::cout << "[MusicModule] Checking for new albums (target_group=" << target_group << ")..." << std::endl;

    std::vector<ArtistSubscription> artists_to_check;
    if (target_group != 0) {
        auto group_subs = db_.get_group_subscriptions(target_group);
        if (group_subs.empty()) {
            rpc_client_.send_group_msg(target_group, "𝐢 | 未查询到新歌");
            return;
        }
        std::set<std::pair<int64_t, std::string>> seen;
        for (const auto& sub : group_subs) {
            if (seen.insert({sub.artist_id, sub.platform}).second) {
                artists_to_check.push_back(sub);
            }
        }
    } else {
        artists_to_check = db_.get_all_unique_artists();
        if (artists_to_check.empty()) {
            std::cout << "[MusicModule] No artists currently subscribed across all groups." << std::endl;
            return;
        }
    }

    std::vector<AlbumPushItem> push_queue;
    int target_group_found_new = 0;

    for (const auto& artist : artists_to_check) {
        bool is_qq = (artist.platform == "qqmusic" || artist.platform == "qq");
        std::vector<AlbumItem> latest_albums;
        if (is_qq) {
            latest_albums = qqmusic_.get_artist_albums(artist.artist_id, artist.artist_name, artist.pic_url, 5);
        } else {
            latest_albums = netease_->get_artist_albums(artist.artist_id, 5);
        }

        for (const auto& album_item : latest_albums) {
            // 1. 本平台已推送排重校验
            if (db_.is_album_pushed(album_item.id, artist.platform)) {
                continue;
            }

            // 2. 跨平台多源排重校验：针对不同平台的同一张专辑，一天内 (86400秒) 只推送其中一个平台，决定依据为第一个轮询到结果的平台
            if (db_.is_album_recently_pushed_cross_platform(artist.artist_name, album_item.name, 86400, artist.platform)) {
                std::cout << "[MusicModule] Album [" << album_item.name << "] by [" << artist.artist_name
                          << "] was already pushed on another platform within 24h. Skipping broadcast for platform ["
                          << artist.platform << "]." << std::endl;
                // 标记本平台也已处理入库，避免未来跨天重新触发推送
                db_.record_pushed_album(album_item.id, artist.artist_id, album_item.name, album_item.publish_time,
                                        artist.platform, artist.artist_name);
                continue;
            }

            // 发现新专辑发布！并且是首个轮询到结果的平台！
            std::cout << "[MusicModule] New album detected! [" << album_item.name
                      << "] by " << artist.artist_name << " on platform [" << artist.platform
                      << "] (Album ID: " << album_item.id << ")" << std::endl;

            // 立即记录入库避免重复推送
            db_.record_pushed_album(album_item.id, artist.artist_id, album_item.name, album_item.publish_time,
                                    artist.platform, artist.artist_name);

            // 查询专辑完整详情与曲目列表
            std::optional<AlbumDetail> album_opt;
            if (is_qq) {
                album_opt = qqmusic_.get_album_detail(album_item.id);
            } else {
                album_opt = netease_->get_album_detail(album_item.id);
            }

            AlbumDetail detail;
            if (album_opt) {
                detail = std::move(*album_opt);
            } else {
                detail.id = album_item.id;
                detail.name = album_item.name;
                detail.pic_url = album_item.pic_url;
                detail.publish_time = album_item.publish_time;
                detail.artist_name = !album_item.artist_name.empty() ? album_item.artist_name : artist.artist_name;
            }

            if (detail.artist_name.empty()) {
                detail.artist_name = artist.artist_name;
            }

            // 查询所有订阅了该艺术家的群 (跨平台联合去重通知)
            std::vector<int64_t> subscriber_groups;
            if (target_group != 0) {
                subscriber_groups = db_.get_subscribers_for_artist_name(artist.artist_name);
                if (subscriber_groups.empty()) {
                    subscriber_groups = db_.get_subscribers_for_artist(artist.artist_id, artist.platform);
                }
                if (std::find(subscriber_groups.begin(), subscriber_groups.end(), target_group) == subscriber_groups.end()) {
                    subscriber_groups.push_back(target_group);
                }
                target_group_found_new++;
            } else {
                subscriber_groups = db_.get_subscribers_for_artist_name(artist.artist_name);
                if (subscriber_groups.empty()) {
                    subscriber_groups = db_.get_subscribers_for_artist(artist.artist_id, artist.platform);
                }
            }

            std::string platform_display = get_platform_full_name(artist.platform);

            // 1. 发送单独前置提示语：
            // 如果新专辑只有一首单曲，判定为该艺术家发了一首新单曲，其余均判定为发布新专辑
            bool is_single = (detail.tracks.size() == 1);
            std::string intro_text = is_single
                ? ("关注的艺术家【" + artist.artist_name + "】在【" + platform_display + "】发布了新单曲！")
                : ("关注的艺术家【" + artist.artist_name + "】在【" + platform_display + "】发布了新专辑！");

            // 2. 构造合并转发卡片节点
            auto forward_nodes = build_album_forward_nodes(detail);

            // 3. 构造平台音乐分享 CQ 码
            std::string share_msg;
            if (is_qq) {
                if (detail.tracks.size() == 1) {
                    share_msg = "[CQ:music,type=qq,id=" + std::to_string(detail.tracks[0].id) + "]";
                } else {
                    std::string album_link = !detail.album_mid.empty()
                        ? ("https://y.qq.com/n/ryqq/albumDetail/" + detail.album_mid)
                        : ("https://y.qq.com/n/ryqq/albumDetail/" + std::to_string(detail.id));
                    share_msg = "[CQ:music,type=custom,url=" + album_link
                        + ",audio=" + escape_cq(album_link)
                        + ",title=" + escape_cq(detail.name)
                        + ",content=" + escape_cq(detail.artist_name)
                        + (!detail.pic_url.empty() ? (",image=" + escape_cq(detail.pic_url)) : "")
                        + "]";
                }
            } else {
                if (detail.tracks.size() == 1) {
                    share_msg = "[CQ:music,type=163,id=" + std::to_string(detail.tracks[0].id) + "]";
                } else {
                    std::string audio_url = !detail.tracks.empty()
                        ? ("https://music.163.com/song/media/outer/url?id=" + std::to_string(detail.tracks[0].id) + ".mp3")
                        : ("https://music.163.com/#/album?id=" + std::to_string(detail.id));
                    share_msg = "[CQ:music,type=custom,url=https://music.163.com/#/album?id=" + std::to_string(detail.id)
                        + ",audio=" + escape_cq(audio_url)
                        + ",title=" + escape_cq(detail.name)
                        + ",content=" + escape_cq(detail.artist_name)
                        + (!detail.pic_url.empty() ? (",image=" + escape_cq(detail.pic_url)) : "")
                        + "]";
                }
            }

            for (int64_t gid : subscriber_groups) {
                push_queue.push_back(AlbumPushItem{
                    .gid = gid,
                    .intro_text = intro_text,
                    .forward_nodes = forward_nodes,
                    .share_msg = share_msg
                });
            }
        }
    }

    if (target_group != 0 && target_group_found_new == 0) {
        rpc_client_.send_group_msg(target_group, "𝐢 | 未查询到新歌");
        return;
    }

    if (!push_queue.empty()) {
        dispatch_push_queue(std::move(push_queue), 0);
    }
}

void MusicModule::dispatch_push_queue(std::vector<AlbumPushItem> queue, size_t index) {
    if (index >= queue.size()) {
        return;
    }

    const auto& item = queue[index];

    // 1. 发送前置文本通知 (如：关注的艺术家xxx发布了新专辑/新单曲！)
    rpc_client_.send_group_msg(item.gid, item.intro_text);

    // 2. 发送合并转发卡片
    // 为避免 OneBot 回包丢失卡死队列，设置 8 秒超时保护
    auto advance_called = std::make_shared<std::atomic<bool>>(false);
    auto timeout_timer = std::make_shared<boost::asio::steady_timer>(ioc_);

    auto advance_to_next = [this, queue, index, advance_called, timeout_timer]() mutable {
        if (!advance_called->exchange(true)) {
            boost::system::error_code ec;
            timeout_timer->cancel(ec);
            dispatch_push_queue(std::move(queue), index + 1);
        }
    };

    timeout_timer->expires_after(std::chrono::seconds(8));
    timeout_timer->async_wait([advance_to_next, index](const boost::system::error_code& ec) mutable {
        if (!ec) {
            std::cerr << "[MusicModule] Warning: Album push timed out at index " << index << ", advancing queue." << std::endl;
            advance_to_next();
        }
    });

    rpc_client_.send_group_forward_msg(item.gid, item.forward_nodes,
        [this, item, advance_to_next](bot::protocol::SendGroupMsgResult result) mutable {
            auto on_forward_completed = [this, item, advance_to_next]() mutable {
                // 3. 合并转发卡片发送完毕后，发送平台音乐分享卡片
                rpc_client_.send_group_msg(item.gid, item.share_msg,
                    [advance_to_next](bot::protocol::SendGroupMsgResult) mutable {
                        // 4. 当前新歌全部内容(前置文本+转发卡片+音乐分享)全部发送完毕后，才触发推送下一个！
                        advance_to_next();
                    });
            };

            if (!result.success) {
                std::cerr << "[MusicModule] Failed to send album forward card: " << result.error_message
                          << ". Retrying without images..." << std::endl;
                auto text_nodes = strip_cq_images(item.forward_nodes);
                rpc_client_.send_group_forward_msg(item.gid, text_nodes,
                    [on_forward_completed](bot::protocol::SendGroupMsgResult) mutable {
                        on_forward_completed();
                    });
                return;
            }

            on_forward_completed();
        });
}

bool MusicModule::handle_candidate_selection(const bot::protocol::GroupMessageEvent& event, const std::string& text) {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    auto it = pending_selections_.find(event.group_id);
    if (it == pending_selections_.end()) {
        return false;
    }

    auto now = std::chrono::steady_clock::now();
    if (now > it->second.expire_at) {
        pending_selections_.erase(it);
        return false;
    }

    // 仅响应发起该操作的用户（避免群内其他成员发数字误触）
    if (event.user_id != it->second.user_id) {
        return false;
    }

    if (text == "取消" || text == "/取消" || text == ".取消") {
        pending_selections_.erase(it);
        rpc_client_.send_group_msg(event.group_id, "➥ | 操作已取消");
        return true;
    }

    if (it->second.type == PendingType::Unfollow) {
        // 取关冲突交互：支持 "all" / "ALL" / "全部"，或者序号多选 (如 "1 2 4")
        if (text == "all" || text == "ALL" || text == "全部") {
            auto candidates = it->second.candidates;
            std::string platform = it->second.platform;
            pending_selections_.erase(it);

            std::vector<std::pair<std::string, std::string>> removed;
            for (const auto& cand : candidates) {
                db_.remove_subscription_by_id(event.group_id, cand.id, cand.platform);
                std::string plat_tag = get_platform_display_tag(cand.platform);
                removed.push_back({plat_tag, cand.name});
            }
            rpc_client_.send_group_msg(event.group_id, format_unfollow_response(removed));
            return true;
        }

        // 解析可能的数字序号列表 (如 "1", "1 2", "1 2 4")
        std::istringstream iss(text);
        std::string token;
        std::vector<int> tokens;
        bool all_digits = true;
        while (iss >> token) {
            bool is_num = !token.empty() && std::all_of(token.begin(), token.end(), [](char c) {
                return std::isdigit(static_cast<unsigned char>(c));
            });
            if (!is_num) {
                all_digits = false;
                break;
            }
            try {
                tokens.push_back(std::stoi(token));
            } catch (...) {
                all_digits = false;
                break;
            }
        }

        if (all_digits && !tokens.empty()) {
            const auto& candidates = it->second.candidates;
            std::vector<int> selected_indices;
            std::vector<bool> seen(candidates.size(), false);
            bool out_of_range = false;

            for (int val : tokens) {
                int idx = val - 1;
                if (idx < 0 || idx >= static_cast<int>(candidates.size())) {
                    out_of_range = true;
                    break;
                }
                if (!seen[idx]) {
                    seen[idx] = true;
                    selected_indices.push_back(idx);
                }
            }

            if (out_of_range || selected_indices.empty()) {
                rpc_client_.send_group_msg(event.group_id, "✕ | 序号无效，请重新输入");
                return true;
            }

            auto saved_candidates = it->second.candidates;
            std::string platform = it->second.platform;
            pending_selections_.erase(it);

            std::vector<std::pair<std::string, std::string>> removed;
            for (size_t i = 0; i < selected_indices.size(); ++i) {
                const auto& cand = saved_candidates[selected_indices[i]];
                db_.remove_subscription_by_id(event.group_id, cand.id, cand.platform);
                std::string plat_tag = get_platform_display_tag(cand.platform);
                removed.push_back({plat_tag, cand.name});
            }
            rpc_client_.send_group_msg(event.group_id, format_unfollow_response(removed));
            return true;
        }

        return false;
    }

    if (it->second.type == PendingType::Follow) {
        // 支持 "all" / "ALL" / "全部" 全部关注
        if (text == "all" || text == "ALL" || text == "全部") {
            auto candidates = it->second.candidates;
            std::string platform = it->second.platform;
            pending_selections_.erase(it);

            std::vector<std::pair<std::string, std::string>> added;
            std::vector<std::pair<std::string, std::string>> skipped;
            for (size_t i = 0; i < candidates.size(); ++i) {
                const auto& chosen = candidates[i];
                std::string chosen_plat = chosen.platform.empty() ? platform : chosen.platform;
                std::string item_tag = get_platform_display_tag(chosen_plat);
                if (db_.is_subscribed(event.group_id, chosen.id, chosen_plat)) {
                    skipped.push_back({item_tag, chosen.name});
                } else {
                    db_.add_subscription(event.group_id, chosen.id, chosen.name, chosen_plat, chosen.pic_url);
                    // 首次关注预热：将该艺术家历史前 15 张专辑记录为已推送，避免把历史专辑当新专辑推送刷屏
                    if (chosen_plat == "qqmusic" || chosen_plat == "qq") {
                        auto existing_albums = qqmusic_.get_artist_albums(chosen.id, chosen.name, chosen.pic_url, 15);
                        for (const auto& a : existing_albums) {
                            db_.record_pushed_album(a.id, chosen.id, a.name, a.publish_time, chosen_plat, chosen.name);
                        }
                    } else {
                        auto existing_albums = netease_->get_artist_albums(chosen.id, 15);
                        for (const auto& a : existing_albums) {
                            db_.record_pushed_album(a.id, chosen.id, a.name, a.publish_time, chosen_plat, chosen.name);
                        }
                    }
                    added.push_back({item_tag, chosen.name});
                }
            }

            rpc_client_.send_group_msg(event.group_id, format_follow_response(added, skipped));
            return true;
        }

        // 解析可能的数字序号列表 (如 "1", "1 2", "1 2 4")
        std::istringstream iss(text);
        std::string token;
        std::vector<int> tokens;
        bool all_digits = true;
        while (iss >> token) {
            bool is_num = !token.empty() && std::all_of(token.begin(), token.end(), [](char c) {
                return std::isdigit(static_cast<unsigned char>(c));
            });
            if (!is_num) {
                all_digits = false;
                break;
            }
            try {
                tokens.push_back(std::stoi(token));
            } catch (...) {
                all_digits = false;
                break;
            }
        }

        if (all_digits && !tokens.empty()) {
            const auto& candidates = it->second.candidates;
            std::vector<int> selected_indices;
            std::vector<bool> seen(candidates.size(), false);
            bool out_of_range = false;

            for (int val : tokens) {
                int idx = val - 1;
                if (idx < 0 || idx >= static_cast<int>(candidates.size())) {
                    out_of_range = true;
                    break;
                }
                if (!seen[idx]) {
                    seen[idx] = true;
                    selected_indices.push_back(idx);
                }
            }

            if (out_of_range || selected_indices.empty()) {
                rpc_client_.send_group_msg(event.group_id, "✕ | 序号无效，请重新输入");
                return true;
            }

            auto saved_candidates = it->second.candidates;
            std::string platform = it->second.platform;
            pending_selections_.erase(it);

            std::vector<std::pair<std::string, std::string>> added;
            std::vector<std::pair<std::string, std::string>> skipped;
            for (size_t i = 0; i < selected_indices.size(); ++i) {
                const auto& chosen = saved_candidates[selected_indices[i]];
                std::string chosen_plat = chosen.platform.empty() ? platform : chosen.platform;
                std::string item_tag = get_platform_display_tag(chosen_plat);
                if (db_.is_subscribed(event.group_id, chosen.id, chosen_plat)) {
                    skipped.push_back({item_tag, chosen.name});
                } else {
                    db_.add_subscription(event.group_id, chosen.id, chosen.name, chosen_plat, chosen.pic_url);
                    // 首次关注预热：将该艺术家历史前 15 张专辑记录为已推送，避免把历史专辑当新专辑推送刷屏
                    if (chosen_plat == "qqmusic" || chosen_plat == "qq") {
                        auto existing_albums = qqmusic_.get_artist_albums(chosen.id, chosen.name, chosen.pic_url, 15);
                        for (const auto& a : existing_albums) {
                            db_.record_pushed_album(a.id, chosen.id, a.name, a.publish_time, chosen_plat, chosen.name);
                        }
                    } else {
                        auto existing_albums = netease_->get_artist_albums(chosen.id, 15);
                        for (const auto& a : existing_albums) {
                            db_.record_pushed_album(a.id, chosen.id, a.name, a.publish_time, chosen_plat, chosen.name);
                        }
                    }
                    added.push_back({item_tag, chosen.name});
                }
            }

            rpc_client_.send_group_msg(event.group_id, format_follow_response(added, skipped));
            return true;
        }

        return false;
    }

    return false;
}

bool MusicModule::handle_group_message(const bot::protocol::GroupMessageEvent& event) {
    std::string text = trim(event.raw_message);
    if (text.empty()) return false;

    // 拦截群聊中的登录与管理员指令：全部静默处理，不回复
    if (text == "/登录" || text == ".登录" || text == "/login" || text == ".login" ||
        text == "/网易云登录" || text == ".网易云登录" || text == "/扫码登录" || text == ".扫码登录" ||
        text.rfind("/登录", 0) == 0 || text.rfind(".登录", 0) == 0 ||
        text.rfind("/login", 0) == 0 || text.rfind(".login", 0) == 0) {
        return true;
    }

    if (text == "/管理员" || text == ".管理员" || text == "/admin" || text == ".admin" ||
        text.rfind("/管理员", 0) == 0 || text.rfind(".管理员", 0) == 0 ||
        text.rfind("/admin", 0) == 0 || text.rfind(".admin", 0) == 0) {
        return true;
    }

    if (text == "/账号状态" || text == ".账号状态" || text == "/登录状态" || text == ".登录状态" ||
        text == "/退出登录" || text == ".退出登录" || text == "/logout" || text == ".logout") {
        return true;
    }

    // 0. 优先检查并处理群成员回复候选数字 (1-5) 或 取消
    if (handle_candidate_selection(event, text)) {
        return true;
    }

    // 1. 指令：/关注 [平台] <名字>
    bool is_sub_cmd = false;
    std::string arg_str;
    if (text.rfind("/关注", 0) == 0 || text.rfind(".关注", 0) == 0) {
        // 排除 /关注列表 和已弃用的 /关注艺术家
        if (text != "/关注列表" && text != ".关注列表" &&
            text.rfind("/关注列表", 0) != 0 && text.rfind(".关注列表", 0) != 0 &&
            text.rfind("/关注艺术家", 0) != 0 && text.rfind(".关注艺术家", 0) != 0) {
            std::string prefix = (text[0] == '/') ? "/关注" : ".关注";
            arg_str = trim(text.substr(prefix.length()));
            is_sub_cmd = true;
        }
    }

    if (is_sub_cmd) {
        // 检查当前群是否还有上一个未确认关注或取消的候选操作
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            auto it = pending_selections_.find(event.group_id);
            if (it != pending_selections_.end()) {
                auto now = std::chrono::steady_clock::now();
                if (now <= it->second.expire_at) {
                    rpc_client_.send_group_msg(event.group_id, "𝐢 | 请先完成或回复「取消」当前操作");
                    return true;
                } else {
                    // 已超时，移除过期状态
                    pending_selections_.erase(it);
                }
            }
        }

        if (arg_str.empty()) {
            rpc_client_.send_group_msg(event.group_id, "𝐢 | 用法：/关注 [平台] <名字>");
            return true;
        }

        // 解析 [平台] 与 <艺术家名字>
        std::string platform = "netease";
        std::string artist_name = arg_str;

        auto space_pos = arg_str.find_first_of(" \t");
        if (space_pos != std::string::npos) {
            std::string first_token = arg_str.substr(0, space_pos);
            std::string second_token = trim(arg_str.substr(space_pos + 1));
            std::string lower_token = to_lower(first_token);

            if (lower_token == "netease" || lower_token == "wyy" || lower_token == "163" ||
                lower_token == "网易云" || lower_token == "网易云音乐") {
                platform = "netease";
                artist_name = second_token;
            } else if (lower_token == "qqmusic" || lower_token == "qq" || lower_token == "qq音乐" || lower_token == "tx") {
                platform = "qqmusic";
                artist_name = second_token;
            } else if (lower_token == "kugou" || lower_token == "酷狗" ||
                       lower_token == "kuwo" || lower_token == "酷我" ||
                       lower_token == "spotify" || lower_token == "bilibili" || lower_token == "b站") {
                rpc_client_.send_group_msg(event.group_id, "✕ | 暂不支持该平台（目前仅支持网易云与QQ音乐）");
                return true;
            } else {
                // 第一个词非已知平台标识，将完整输入作为艺术家名称，平台默认网易云
                platform = "netease";
                artist_name = arg_str;
            }
        }

        if (artist_name.empty()) {
            rpc_client_.send_group_msg(event.group_id, "𝐢 | 用法：/关注 [平台] <名字>");
            return true;
        }

        if (platform != "netease" && platform != "qqmusic") {
            rpc_client_.send_group_msg(event.group_id, "✕ | 暂不支持该平台（目前仅支持网易云与QQ音乐）");
            return true;
        }

        // 搜索候选艺术家列表 (包含头像与最热代表曲，无曲目则返回<无>)
        std::vector<ArtistCandidate> candidates;
        if (platform == "qqmusic") {
            candidates = qqmusic_.search_candidates(artist_name, 5);
        } else {
            candidates = netease_->search_candidates(artist_name, 5);
        }

        if (candidates.empty()) {
            rpc_client_.send_group_msg(event.group_id, "✕ | 未找到相关艺术家");
            return true;
        }

        // 记录交互状态并设置 60 秒超时
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            PendingSelection pending;
            pending.user_id = event.user_id;
            pending.type = PendingType::Follow;
            pending.platform = platform;
            for (auto& c : candidates) {
                c.platform = platform;
            }
            pending.candidates = candidates;
            pending.expire_at = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            pending_selections_[event.group_id] = std::move(pending);
        }

        // 构造合并转发卡片节点
        std::string platform_display = get_platform_full_name(platform);
        std::vector<bot::protocol::ForwardNode> forward_nodes;

        // 节点 1：标题头
        {
            bot::protocol::ForwardNode title_node;
            title_node.data.name = "音乐搜索助手";
            title_node.data.uin = "2854196306";
            title_node.data.content = "🔍【" + platform_display + "】搜索结果：";
            forward_nodes.push_back(std::move(title_node));
        }

        // 节点 2 ~ N+1：各候选艺术家的详细图文信息 (艺术家名字 (ID) -> 艺术家图片 -> 相关音乐)
        for (size_t i = 0; i < candidates.size(); ++i) {
            const auto& cand = candidates[i];
            bot::protocol::ForwardNode node;
            node.data.name = "候选艺术家 #" + std::to_string(i + 1);
            node.data.uin = "2854196306";

            std::ostringstream node_oss;
            node_oss << cand.name << " (ID: " << cand.id << ")";
            if (!cand.pic_url.empty()) {
                node_oss << "\n[CQ:image,file=" << cand.pic_url << "]";
            }
            node_oss << "\n相关音乐：" << cand.top_song;
            node.data.content = node_oss.str();
            forward_nodes.push_back(std::move(node));
        }

        // 1. 先发送群合并转发卡片 (卡片内展示各候选艺术家信息)
        // 2. 待确认转发卡片发出后，再在群内发送轻量提示语。若因网络/图片加载失败，自动回退为纯文字卡片
        rpc_client_.send_group_forward_msg(event.group_id, forward_nodes,
            [this, group_id = event.group_id, forward_nodes](bot::protocol::SendGroupMsgResult result) {
                if (!result.success) {
                    std::cerr << "[MusicModule] Failed to send forward card: " << result.error_message
                              << " (retcode: " << result.retcode << "). Retrying without images..." << std::endl;
                    auto text_only_nodes = strip_cq_images(forward_nodes);
                    rpc_client_.send_group_forward_msg(group_id, text_only_nodes,
                        [this, group_id](bot::protocol::SendGroupMsgResult retry_res) {
                            if (!retry_res.success) {
                                std::cerr << "[MusicModule] Fallback forward card also failed: " << retry_res.error_message << std::endl;
                                {
                                    std::lock_guard<std::mutex> lock(pending_mutex_);
                                    pending_selections_.erase(group_id);
                                }
                                rpc_client_.send_group_msg(group_id, "✕ | 发送候选卡片失败，请稍后重试");
                                return;
                            }
                            {
                                std::lock_guard<std::mutex> lock(pending_mutex_);
                                auto it = pending_selections_.find(group_id);
                                if (it != pending_selections_.end()) {
                                    it->second.expire_at = std::chrono::steady_clock::now() + std::chrono::seconds(60);
                                }
                            }
                            rpc_client_.send_group_msg(group_id, "❐ | 确定需要关注的艺术家\n➫「序号 / all / 取消」");
                        });
                    return;
                }

                {
                    std::lock_guard<std::mutex> lock(pending_mutex_);
                    auto it = pending_selections_.find(group_id);
                    if (it != pending_selections_.end()) {
                        it->second.expire_at = std::chrono::steady_clock::now() + std::chrono::seconds(60);
                    }
                }
                rpc_client_.send_group_msg(group_id, "❐ | 确定需要关注的艺术家\n➫「序号 / all / 取消」");
            });

        return true;
    }

    // 2. 指令：/取关 [平台] <名字>
    bool is_unsub_cmd = false;
    std::string unsub_arg;
    if (text.rfind("/取关", 0) == 0 || text.rfind(".取关", 0) == 0) {
        // 排除已弃用的 /取关艺术家
        if (text.rfind("/取关艺术家", 0) != 0 && text.rfind(".取关艺术家", 0) != 0) {
            std::string prefix = (text[0] == '/') ? "/取关" : ".取关";
            unsub_arg = trim(text.substr(prefix.length()));
            is_unsub_cmd = true;
        }
    }

    if (is_unsub_cmd) {
        // 检查当前群是否还有上一个未确认关注或取关的候选操作
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            auto it = pending_selections_.find(event.group_id);
            if (it != pending_selections_.end()) {
                auto now = std::chrono::steady_clock::now();
                if (now <= it->second.expire_at) {
                    rpc_client_.send_group_msg(event.group_id, "𝐢 | 请先完成或回复「取消」当前操作");
                    return true;
                } else {
                    // 已超时，移除过期状态
                    pending_selections_.erase(it);
                }
            }
        }

        if (unsub_arg.empty()) {
            rpc_client_.send_group_msg(event.group_id, "𝐢 | 用法：/取关 [平台] <名字>");
            return true;
        }

        // 解析 [平台] 与 <艺术家名字>
        std::string platform = "";
        std::string artist_name = unsub_arg;

        auto space_pos = unsub_arg.find_first_of(" \t");
        if (space_pos != std::string::npos) {
            std::string first_token = unsub_arg.substr(0, space_pos);
            std::string second_token = trim(unsub_arg.substr(space_pos + 1));
            std::string lower_token = to_lower(first_token);

            if (lower_token == "netease" || lower_token == "wyy" || lower_token == "163" ||
                lower_token == "网易云" || lower_token == "网易云音乐") {
                platform = "netease";
                artist_name = second_token;
            } else if (lower_token == "qqmusic" || lower_token == "qq" || lower_token == "qq音乐" || lower_token == "tx") {
                platform = "qqmusic";
                artist_name = second_token;
            } else if (lower_token == "kugou" || lower_token == "酷狗" ||
                       lower_token == "kuwo" || lower_token == "酷我" ||
                       lower_token == "spotify" || lower_token == "bilibili" || lower_token == "b站") {
                platform = lower_token;
                artist_name = second_token;
            } else {
                // 第一个词非已知平台标识，整体作为艺术家名称
                platform = "";
                artist_name = unsub_arg;
            }
        }

        if (artist_name.empty()) {
            rpc_client_.send_group_msg(event.group_id, "𝐢 | 用法：/取关 [平台] <名字>");
            return true;
        }

        // 查询符合条件的订阅记录
        auto matched = db_.find_subscriptions(event.group_id, artist_name, platform);

        if (matched.empty()) {
            rpc_client_.send_group_msg(event.group_id, "✕ | 未关注该艺术家");
            return true;
        }

        if (matched.size() == 1) {
            // 唯一匹配，直接取关
            db_.remove_subscription_by_id(event.group_id, matched[0].artist_id, matched[0].platform);
            std::string plat_tag = get_platform_display_tag(matched[0].platform);
            std::vector<std::pair<std::string, std::string>> removed = {{plat_tag, matched[0].artist_name}};
            rpc_client_.send_group_msg(event.group_id, format_unfollow_response(removed));
            return true;
        }

        // 存在多个同名/冲突艺术家订阅，提取并展示合并转发卡片
        std::vector<ArtistCandidate> candidates;
        for (const auto& sub : matched) {
            ArtistCandidate cand;
            cand.id = sub.artist_id;
            cand.name = sub.artist_name;
            cand.platform = sub.platform;
            if (sub.platform == "qqmusic" || sub.platform == "qq") {
                cand.pic_url = sub.pic_url.empty() ? qqmusic_.get_artist_pic_url(sub.artist_id, sub.artist_name, sub.pic_url) : sub.pic_url;
                cand.top_song = qqmusic_.get_artist_top_song(sub.artist_id, sub.artist_name, sub.pic_url);
            } else {
                cand.pic_url = sub.pic_url.empty() ? netease_->get_artist_pic_url(sub.artist_id) : sub.pic_url;
                cand.top_song = netease_->get_artist_top_song(sub.artist_id);
            }
            candidates.push_back(std::move(cand));
        }

        // 记录取关待确认交互状态 (60 秒超时)
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            PendingSelection pending;
            pending.user_id = event.user_id;
            pending.type = PendingType::Unfollow;
            pending.platform = platform;
            pending.candidates = candidates;
            pending.expire_at = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            pending_selections_[event.group_id] = std::move(pending);
        }

        std::vector<bot::protocol::ForwardNode> forward_nodes;

        // 节点 1：标题头
        {
            bot::protocol::ForwardNode title_node;
            title_node.data.name = "音乐搜索助手";
            title_node.data.uin = "2854196306";
            title_node.data.content = "🔍 发现多个同名艺术家，请选择要取关的项：";
            forward_nodes.push_back(std::move(title_node));
        }

        // 节点 2 ~ N+1：各候选艺术家的详细图文信息 (与关注卡片格式完全一致)
        for (size_t i = 0; i < candidates.size(); ++i) {
            const auto& cand = candidates[i];
            bot::protocol::ForwardNode node;
            node.data.name = "候选艺术家 #" + std::to_string(i + 1);
            node.data.uin = "2854196306";

            std::ostringstream node_oss;
            node_oss << cand.name << " (ID: " << cand.id << ")";
            if (!cand.pic_url.empty()) {
                node_oss << "\n[CQ:image,file=" << cand.pic_url << "]";
            }
            node_oss << "\n相关音乐：" << cand.top_song;
            node.data.content = node_oss.str();
            forward_nodes.push_back(std::move(node));
        }

        // 1. 发送合并转发卡片
        // 2. 待确认转发卡片发出后，发送提示语。若因网络/图片加载失败，自动回退为纯文字卡片
        rpc_client_.send_group_forward_msg(event.group_id, forward_nodes,
            [this, group_id = event.group_id, forward_nodes](bot::protocol::SendGroupMsgResult result) {
                if (!result.success) {
                    std::cerr << "[MusicModule] Failed to send conflict card: " << result.error_message
                              << " (retcode: " << result.retcode << "). Retrying without images..." << std::endl;
                    auto text_only_nodes = strip_cq_images(forward_nodes);
                    rpc_client_.send_group_forward_msg(group_id, text_only_nodes,
                        [this, group_id](bot::protocol::SendGroupMsgResult retry_res) {
                            if (!retry_res.success) {
                                std::cerr << "[MusicModule] Fallback conflict card also failed: " << retry_res.error_message << std::endl;
                                {
                                    std::lock_guard<std::mutex> lock(pending_mutex_);
                                    pending_selections_.erase(group_id);
                                }
                                rpc_client_.send_group_msg(group_id, "✕ | 发送冲突卡片失败，请稍后重试");
                                return;
                            }
                            {
                                std::lock_guard<std::mutex> lock(pending_mutex_);
                                auto it = pending_selections_.find(group_id);
                                if (it != pending_selections_.end()) {
                                    it->second.expire_at = std::chrono::steady_clock::now() + std::chrono::seconds(60);
                                }
                            }
                            rpc_client_.send_group_msg(group_id, "❐ | 确定需要取关的艺术家\n➫「序号 / all / 取消」");
                        });
                    return;
                }

                {
                    std::lock_guard<std::mutex> lock(pending_mutex_);
                    auto it = pending_selections_.find(group_id);
                    if (it != pending_selections_.end()) {
                        it->second.expire_at = std::chrono::steady_clock::now() + std::chrono::seconds(60);
                    }
                }
                rpc_client_.send_group_msg(group_id, "❐ | 确定需要取关的艺术家\n➫「序号 / all / 取消」");
            });

        return true;
    }

    // 3. 指令：/关注列表
    if (text == "/关注列表" || text == ".关注列表") {
        auto subs = db_.get_group_subscriptions(event.group_id);
        if (subs.empty()) {
            rpc_client_.send_group_msg(event.group_id, "✕ | 当前暂无关注\n➫ 「/关注 [平台] <名字>」 添加");
            return true;
        }

        std::vector<bot::protocol::ForwardNode> forward_nodes;

        // 10 个艺术家组成一页，单页时转发人显示为“关注列表”，多页时为“关注列表页 #x”，展示格式为 [平台]艺术家名 (不展示ID)
        const size_t page_size = 10;
        size_t total_pages = (subs.size() + page_size - 1) / page_size;

        for (size_t p = 0; p < total_pages; ++p) {
            bot::protocol::ForwardNode page_node;
            page_node.data.name = (total_pages == 1) ? "关注列表" : ("关注列表页 #" + std::to_string(p + 1));
            page_node.data.uin = "2854196306";

            std::ostringstream page_oss;
            size_t start_idx = p * page_size;
            size_t end_idx = std::min(start_idx + page_size, subs.size());

            for (size_t i = start_idx; i < end_idx; ++i) {
                if (i > start_idx) {
                    page_oss << "\n";
                }
                std::string plat_tag = get_platform_display_tag(subs[i].platform);
                page_oss << "[" << plat_tag << "]" << subs[i].artist_name;
            }

            page_node.data.content = page_oss.str();
            forward_nodes.push_back(std::move(page_node));
        }

        rpc_client_.send_group_forward_msg(event.group_id, forward_nodes);
        return true;
    }

    // 3.5 指令：/查新歌 /检查新歌 /查歌 /新歌 /更新
    if (text == "/查新歌" || text == ".查新歌" ||
        text == "/检查新歌" || text == ".检查新歌" ||
        text == "/查歌" || text == ".查歌" ||
        text == "/新歌" || text == ".新歌" ||
        text == "/更新" || text == ".更新") {
        check_new_albums_once(event.group_id);
        return true;
    }

    // 4. 指令：/日推 [开启/关闭/状态/结算/重置] 或 /每日推荐
    if (text == "/日推" || text == ".日推" || text == "/每日推荐" || text == ".每日推荐" ||
        text.rfind("/日推", 0) == 0 || text.rfind(".日推", 0) == 0 ||
        text.rfind("/每日推荐", 0) == 0 || text.rfind(".每日推荐", 0) == 0) {

        std::string sub_arg;
        if (text.rfind("/日推", 0) == 0) sub_arg = trim(text.substr(7));
        else if (text.rfind(".日推", 0) == 0) sub_arg = trim(text.substr(7));
        else if (text.rfind("/每日推荐", 0) == 0) sub_arg = trim(text.substr(13));
        else if (text.rfind(".每日推荐", 0) == 0) sub_arg = trim(text.substr(13));

        if (sub_arg == "开启" || sub_arg == "on" || sub_arg == "enable" || sub_arg == "start") {
            db_.set_daily_music_enabled(event.group_id, true);
            rpc_client_.send_group_msg(event.group_id, "➥ | 已开启本群每日音乐推荐\n(每天 11:00 自动推送，23:00 反馈日推数据)");
            return true;
        } else if (sub_arg == "关闭" || sub_arg == "off" || sub_arg == "disable" || sub_arg == "stop") {
            db_.set_daily_music_enabled(event.group_id, false);
            rpc_client_.send_group_msg(event.group_id, "➥ | 已关闭本群每日音乐推荐");
            return true;
        } else if (sub_arg == "状态" || sub_arg == "status") {
            bool enabled = db_.is_daily_music_enabled(event.group_id);
            std::string today_str = DailyMusicManager::get_current_date_str();
            auto today_push = db_.get_today_daily_push(event.group_id, today_str);
            auto latest = today_push.has_value() ? today_push : db_.get_latest_daily_push(event.group_id);
            std::string msg = "┏ 每日音乐推荐状态 ━\n";
            msg += "┃状态：" + std::string(enabled ? "已开启" : "未开启 (可回复 /日推 开启)") + "\n";
            if (latest.has_value()) {
                auto [up, down] = db_.get_daily_push_votes(latest->message_id);
                msg += "┃今日/最新：「" + latest->item_name + "」- " + latest->artist_name + "\n";
                msg += "┃投票反馈：👍 " + std::to_string(up) + " / 👎 " + std::to_string(down) + "\n";
                msg += "┃评价状态：" + std::string(latest->evaluated ? "已结算" : "待 23:00 结算") + "\n";
            } else {
                msg += "┃今日/最新：暂无推送记录\n";
            }
            msg += "┃推送时刻：每天 11:00\n";
            msg += "┃评价时刻：每天 23:00";
            rpc_client_.send_group_msg(event.group_id, msg);
            return true;
        } else if (sub_arg == "结算" || sub_arg == "eval") {
            std::string today_str = DailyMusicManager::get_current_date_str();
            if (db_.is_daily_evaluated(event.group_id, today_str)) {
                rpc_client_.send_group_msg(event.group_id, "𝐢 | 当日日推评价已结算");
                return true;
            }
            daily_manager_->trigger_eval_now();
            db_.set_daily_evaluated(event.group_id, today_str);
            rpc_client_.send_group_msg(event.group_id, "➥ | 已手动执行日推评价结算");
            return true;
        } else if (sub_arg == "重置" || sub_arg == "恢复" || sub_arg == "reset") {
            std::string today_str = DailyMusicManager::get_current_date_str();
            db_.reset_daily_push_status(event.group_id, today_str);
            rpc_client_.send_group_msg(event.group_id, "➥ | 已恢复当日日推状态为初始状态");
            return true;
        } else {
            std::string today_str = DailyMusicManager::get_current_date_str();
            auto today_push = db_.get_today_daily_push(event.group_id, today_str);
            if (today_push.has_value() && today_push->message_id > 0) {
                // 后续触发：QQ群内对第一条日推信息（即转发的音乐）进行引用（回复）
                std::string reply_msg = "[CQ:reply,id=" + std::to_string(today_push->message_id) + "]"
                                      + "今日日推歌曲为「" + today_push->item_name + "」- " + today_push->artist_name;
                rpc_client_.send_group_msg(event.group_id, reply_msg);
                return true;
            }

            // 第一次触发：转发音乐卡片
            bool ok = daily_manager_->trigger_push_now("", event.group_id);
            if (!ok) {
                rpc_client_.send_group_msg(event.group_id, "✕ | 获取今日推荐失败，请检查网易云账号登录状态或网络");
            }
            return true;
        }
    }

    return false;
}

bool MusicModule::handle_private_message(const bot::protocol::PrivateMessageEvent& event) {
    std::string text = trim(event.raw_message);
    if (text.empty()) return false;

    // 容错规范化：折叠用户误输入的重复前缀斜杠或点号 (如 //登录 -> /登录)
    while (text.size() > 1 && ((text[0] == '/' && text[1] == '/') || (text[0] == '.' && text[1] == '.'))) {
        text.erase(0, 1);
    }

    // 1. 取消正在进行的扫码登录
    if (text == "取消" || text == "/取消" || text == ".取消" || text == "cancel" || text == "/cancel") {
        if (qr_session_ && qr_session_->user_id == event.user_id) {
            cancel_qr_login_session();
            rpc_client_.send_private_msg(event.user_id, "➥ | 已取消扫码登录流程");
            return true;
        }
    }

    bool is_admin_cmd = (text == "/管理员" || text == ".管理员" || text == "/admin" || text == ".admin" ||
                         text.rfind("/管理员", 0) == 0 || text.rfind(".管理员", 0) == 0 ||
                         text.rfind("/admin", 0) == 0 || text.rfind(".admin", 0) == 0);
    bool is_login_cmd = (text == "/登录" || text == ".登录" || text == "/login" || text == ".login" ||
                         text == "/网易云登录" || text == ".网易云登录" || text == "/扫码登录" || text == ".扫码登录" ||
                         text == "/cookie" || text == ".cookie" || text == "/设置cookie" || text == ".设置cookie" ||
                         text.rfind("/登录", 0) == 0 || text.rfind(".登录", 0) == 0 ||
                         text.rfind("/login", 0) == 0 || text.rfind(".login", 0) == 0 ||
                         text.rfind("/cookie", 0) == 0 || text.rfind(".cookie", 0) == 0 ||
                         text.rfind("/设置cookie", 0) == 0 || text.rfind(".设置cookie", 0) == 0);
    bool is_status_cmd = (text == "/账号状态" || text == ".账号状态" || text == "/登录状态" || text == ".登录状态" ||
                          text == "/status" || text == ".status");
    bool is_logout_cmd = (text == "/退出登录" || text == ".退出登录" || text == "/注销" || text == ".注销" ||
                          text == "/logout" || text == ".logout");

    // 若不是本模块支持的私聊指令，直接放行
    if (!is_admin_cmd && !is_login_cmd && !is_status_cmd && !is_logout_cmd) {
        return false;
    }

    // 2. 超级管理员首登机制：若当前数据库无任何管理员，首位发送 /登录 或 /管理员 的私聊用户静默成为初始超级管理员
    bool has_admins = db_.has_any_admin();
    if (!has_admins && (is_admin_cmd || is_login_cmd)) {
        db_.add_admin(event.user_id);
    }

    bool is_admin = db_.is_admin(event.user_id);

    // 3. 管理员命令：/管理员 或 /admin
    if (is_admin_cmd) {
        if (!is_admin) {
            rpc_client_.send_private_msg(event.user_id, "✕ | 权限不足");
            return true;
        }

        std::string sub_arg;
        if (text.rfind("/管理员", 0) == 0 || text.rfind(".管理员", 0) == 0) {
            sub_arg = trim(text.substr(std::string("/管理员").length()));
        } else if (text.rfind("/admin", 0) == 0 || text.rfind(".admin", 0) == 0) {
            sub_arg = trim(text.substr(std::string("/admin").length()));
        }

        if (sub_arg == "列表" || sub_arg == "list") {
            auto admins = db_.get_admins();
            std::string msg = "┏管理员列表 (共 " + std::to_string(admins.size()) + " 人)━\n";
            for (size_t i = 0; i < admins.size(); ++i) {
                msg += "┃• " + std::to_string(admins[i]);
                if (i + 1 < admins.size()) msg += "\n";
            }
            rpc_client_.send_private_msg(event.user_id, msg);
            return true;
        }

        auto space_pos = sub_arg.find_first_of(" \t");
        std::string action = (space_pos == std::string::npos) ? sub_arg : sub_arg.substr(0, space_pos);
        std::string target_str = (space_pos == std::string::npos) ? "" : trim(sub_arg.substr(space_pos + 1));

        if (action == "添加" || action == "add") {
            if (target_str.empty()) {
                rpc_client_.send_private_msg(event.user_id, "𝐢 | 用法：/管理员 添加 <QQ号>");
                return true;
            }
            try {
                int64_t target_qq = std::stoll(target_str);
                if (target_qq <= 10000) {
                    rpc_client_.send_private_msg(event.user_id, "✕ | QQ号不合法");
                    return true;
                }
                if (db_.is_admin(target_qq)) {
                    rpc_client_.send_private_msg(event.user_id, "➥ | 管理员列表已存在 " + std::to_string(target_qq));
                    return true;
                }
                if (db_.add_admin(target_qq)) {
                    rpc_client_.send_private_msg(event.user_id, "➥ | 管理员新增 " + std::to_string(target_qq));
                } else {
                    rpc_client_.send_private_msg(event.user_id, "✕ | 添加管理员失败，请检查数据库");
                }
            } catch (...) {
                rpc_client_.send_private_msg(event.user_id, "✕ | QQ号格式错误，请输入纯数字");
            }
            return true;
        } else if (action == "删除" || action == "移除" || action == "del" || action == "remove") {
            if (target_str.empty()) {
                rpc_client_.send_private_msg(event.user_id, "𝐢 | 用法：/管理员 删除 <QQ号>");
                return true;
            }
            try {
                int64_t target_qq = std::stoll(target_str);
                if (MusicDatabase::is_hardcoded_admin(target_qq)) {
                    rpc_client_.send_private_msg(event.user_id, "➥ | 不可移除管理员 " + std::to_string(target_qq));
                    return true;
                }
                if (!db_.is_admin(target_qq)) {
                    rpc_client_.send_private_msg(event.user_id, "✕ | QQ号 " + std::to_string(target_qq) + " 不在管理员列表中");
                    return true;
                }
                if (db_.remove_admin(target_qq)) {
                    rpc_client_.send_private_msg(event.user_id, "➥ | 管理员移除 " + std::to_string(target_qq));
                } else {
                    rpc_client_.send_private_msg(event.user_id, "✕ | 移除管理员失败，请检查数据库");
                }
            } catch (...) {
                rpc_client_.send_private_msg(event.user_id, "✕ | QQ号格式错误，请输入纯数字");
            }
            return true;
        } else {
            std::string help = "┏管理员指令帮助━\n";
            help += "┃• /管理员 列表 - 查看管理员名单\n";
            help += "┃• /管理员 添加 <QQ> - 添加管理员\n";
            help += "┃• /管理员 删除 <QQ> - 移除管理员\n";
            help += "┃• /登录 - 扫码登录网易云音乐\n";
            help += "┃• /登录 <Cookie> - 导入Cookie凭据\n";
            help += "┃• /账号状态 - 查看网易云登录态\n";
            help += "┃• /退出登录 - 退出当前网易云登录";
            rpc_client_.send_private_msg(event.user_id, help);
            return true;
        }
    }

    // 4. 扫码登录或直接导入 Cookie 命令：/登录 或 /login
    if (is_login_cmd) {
        if (!is_admin) {
            rpc_client_.send_private_msg(event.user_id, "✕ | 权限不足：仅限管理员登录网易云音乐账号，如需授权请联系管理员");
            return true;
        }

        std::string arg;
        const std::vector<std::string> prefixes = {
            "/设置cookie", ".设置cookie", "/网易云登录", ".网易云登录", "/扫码登录", ".扫码登录",
            "/cookie", ".cookie", "/登录", ".登录", "/login", ".login"
        };
        for (const auto& prefix : prefixes) {
            if (text.rfind(prefix, 0) == 0) {
                arg = trim(text.substr(prefix.size()));
                break;
            }
        }

        // 如果用户提供了 Cookie 参数 (例如 /登录 MUSIC_U=xxxx)
        if (!arg.empty()) {
            if (arg.find("MUSIC_U") != std::string::npos) {
                // 清洗 Cookie，提取干净的键值并滤除用户输入中残留的中文或非 ASCII 字符
                std::string clean_cookie;
                auto u_pos = arg.find("MUSIC_U=");
                if (u_pos != std::string::npos) {
                    size_t v_start = u_pos + 8;
                    size_t v_end = v_start;
                    while (v_end < arg.size() && (std::isalnum(static_cast<unsigned char>(arg[v_end])) ||
                                                  arg[v_end] == '_' || arg[v_end] == '-' ||
                                                  arg[v_end] == '%' || arg[v_end] == '.')) {
                        v_end++;
                    }
                    clean_cookie = "MUSIC_U=" + arg.substr(v_start, v_end - v_start);
                } else {
                    clean_cookie = arg;
                }

                auto csrf_pos = arg.find("__csrf=");
                if (csrf_pos != std::string::npos) {
                    size_t c_start = csrf_pos + 7;
                    size_t c_end = c_start;
                    while (c_end < arg.size() && (std::isalnum(static_cast<unsigned char>(arg[c_end])) ||
                                                  arg[c_end] == '_' || arg[c_end] == '-')) {
                        c_end++;
                    }
                    std::string csrf_val = arg.substr(c_start, c_end - c_start);
                    if (!csrf_val.empty()) {
                        clean_cookie += "; __csrf=" + csrf_val;
                    }
                }

                netease_->set_cookie(clean_cookie);
                db_.set_config("netease_cookie", clean_cookie);
                cancel_qr_login_session();

                auto status = netease_->check_login_status();
                if (status.is_logged_in) {
                    std::string vip_str = status.is_vip ? "VIP用户" : "普通用户";
                    std::string msg = "登录成功！\n";
                    msg += "方式：Cookie导入\n";
                    msg += "当前账号：" + status.nickname + "（UID：" + std::to_string(status.user_id) + "）\n";
                    msg += vip_str;
                    rpc_client_.send_private_msg(event.user_id, msg);
                    std::cout << "[MusicModule] Admin UID " << event.user_id << " imported valid NetEase cookie (User: " << status.nickname << ", UID: " << status.user_id << ", VIP: " << (status.is_vip ? "YES" : "NO") << ")." << std::endl;
                } else {
                    std::string msg = "登录失败！\n";
                    msg += "原因：该 Cookie 凭据未通过网易云服务器鉴权 (account: null)\n";
                    msg += "➫ 请在电脑网页访问 music.163.com 确认正常登录后，按 F12 复制最新的 MUSIC_U";
                    rpc_client_.send_private_msg(event.user_id, msg);
                    std::cout << "[MusicModule] Admin UID " << event.user_id << " imported cookie but verification returned unauthenticated." << std::endl;
                }
                return true;
            } else {
                std::string msg = "登录失败！\n";
                msg += "原因：未检测到核心凭据 MUSIC_U\n";
                msg += "➫ 请确保内容包含类似「MUSIC_U=...」字段";
                rpc_client_.send_private_msg(event.user_id, msg);
                return true;
            }
        }

        start_qr_login_session(event.user_id);
        return true;
    }

    // 5. 账号状态查询命令
    if (is_status_cmd) {
        std::string cookie = netease_->get_cookie();
        if (cookie.empty()) {
            cookie = db_.get_config("netease_cookie");
            if (!cookie.empty()) {
                netease_->set_cookie(cookie);
            }
        }
        if (cookie.empty() || cookie.find("MUSIC_U") == std::string::npos) {
            rpc_client_.send_private_msg(event.user_id, "登录态：未登录\n➫ 请管理员私聊机器人登录账号进行授权");
            return true;
        }

        auto status = netease_->check_login_status();
        if (status.is_logged_in) {
            std::string vip_str = status.is_vip ? "VIP用户" : "普通用户";
            std::string msg = "登录态：已登录\n";
            msg += "当前账号：" + status.nickname + "（UID：" + std::to_string(status.user_id) + "）\n";
            msg += vip_str;
            rpc_client_.send_private_msg(event.user_id, msg);
        } else {
            rpc_client_.send_private_msg(event.user_id, "登录态：未登录\n➫ 请管理员私聊机器人登录账号进行授权");
        }
        return true;
    }

    // 6. 退出登录命令
    if (is_logout_cmd) {
        if (!is_admin) {
            rpc_client_.send_private_msg(event.user_id, "✕ | 权限不足：仅限管理员执行该操作");
            return true;
        }
        netease_->set_cookie("");
        db_.set_config("netease_cookie", "");
        rpc_client_.send_private_msg(event.user_id, "➥ | 已清除网易云账号登录凭据并退出登录");
        return true;
    }

    return false;
}

void MusicModule::start_qr_login_session(int64_t user_id) {
    cancel_qr_login_session();

    auto key_res = netease_->get_qr_key();
    if (!key_res.success || key_res.unikey.empty()) {
        std::string err = key_res.error.empty() ? "网络请求超时或接口异常，请稍后重试" : key_res.error;
        rpc_client_.send_private_msg(user_id, "✕ | 获取登录二维码失败：" + err);
        return;
    }

    qr_session_ = std::make_unique<QrLoginSession>();
    qr_session_->user_id = user_id;
    qr_session_->unikey = key_res.unikey;
    qr_session_->poll_count = 0;
    qr_session_->prompted_confirm = false;
    qr_session_->png_path = "/tmp/netease_qr_" + std::to_string(user_id) + ".png";
    qr_session_->timer = std::make_unique<boost::asio::steady_timer>(ioc_);

    std::string base64_img = NeteaseClient::generate_qr_image_base64(key_res.url, qr_session_->png_path);

    std::string msg = "❐ | 网易云音乐扫码登录\n";
    msg += "请使用网易云音乐 APP 扫描下方二维码确认登录\n";
    msg += "（有效时间约 5 分钟，如未显示二维码请复制下方链接在手机打开）\n";
    if (!base64_img.empty()) {
        msg += "[CQ:image,file=base64://" + base64_img + "]\n";
    }
    msg += key_res.url + "\n";
    msg += "𝐢 | 发送「取消」可终止登录";

    rpc_client_.send_private_msg(user_id, msg);
    schedule_qr_poll();
}

void MusicModule::schedule_qr_poll() {
    if (!qr_session_ || !qr_session_->timer) return;

    qr_session_->timer->expires_after(std::chrono::seconds(2));
    qr_session_->timer->async_wait([this](const boost::system::error_code& ec) {
        on_qr_poll_timer(ec);
    });
}

void MusicModule::on_qr_poll_timer(const boost::system::error_code& ec) {
    if (ec || !qr_session_) return;

    qr_session_->poll_count++;
    if (qr_session_->poll_count > 150) {
        int64_t uid = qr_session_->user_id;
        cancel_qr_login_session();
        rpc_client_.send_private_msg(uid, "✕ | 二维码已超时失效，请重新发送 /登录");
        return;
    }

    auto check_res = netease_->check_qr_login(qr_session_->unikey);
    if (check_res.code == 803) {
        int64_t uid = qr_session_->user_id;
        std::string cookie = check_res.cookie;
        if (cookie.empty()) {
            cookie = netease_->get_cookie();
        }
        if (!cookie.empty()) {
            netease_->set_cookie(cookie);
            db_.set_config("netease_cookie", cookie);
        }
        cancel_qr_login_session();

        auto status = netease_->check_login_status();
        std::string nick = (status.is_logged_in && !status.nickname.empty()) ? status.nickname : (!check_res.nickname.empty() ? check_res.nickname : qr_session_->nickname);
        std::string uid_str = (status.is_logged_in && status.user_id > 0) ? std::to_string(status.user_id) : std::to_string(uid);
        std::string vip_str = status.is_vip ? "VIP用户" : "普通用户";

        std::string success_msg = "登录成功！\n";
        success_msg += "方式：扫码登录\n";
        success_msg += "当前账号：" + nick + "（UID：" + uid_str + "）\n";
        success_msg += vip_str;
        rpc_client_.send_private_msg(uid, success_msg);

        std::cout << "[MusicModule] QR login successfully finalized for UID: " << uid
                  << ", cookie length: " << cookie.size()
                  << ", has_MUSIC_U=" << (cookie.find("MUSIC_U") != std::string::npos ? "YES" : "NO")
                  << std::endl;
        return;
    } else if (check_res.code == 802) {
        if (!check_res.nickname.empty()) {
            qr_session_->nickname = check_res.nickname;
        }
        if (!check_res.avatar_url.empty()) {
            qr_session_->avatar_url = check_res.avatar_url;
        }
        // 扫码成功静默处理，不向私聊发送任何消息，直接继续轮询等待手机端确认
        schedule_qr_poll();
    } else if (check_res.code == 801) {
        schedule_qr_poll();
    } else if (check_res.code == 800) {
        int64_t uid = qr_session_->user_id;
        cancel_qr_login_session();
        rpc_client_.send_private_msg(uid, "✕ | 二维码已过期，登录已终止。如需登录请重新发送 /登录");
        return;
    } else if (check_res.code == 8821) {
        int64_t uid = qr_session_->user_id;
        cancel_qr_login_session();
        std::string warn_msg = "✕ | 网易云登录受阻：触发官方安全风控（错误码 8821：请切换其他登录方式或升级新版本再试）。\n";
        warn_msg += "💡 官方风控已限制当前网络环境下的扫码授权，推荐采用 Cookie 导入方式：\n";
        warn_msg += "1. 在浏览器打开并登录网易云官网 music.163.com\n";
        warn_msg += "2. 按 F12 打开开发者工具，在 Application -> Cookies 中复制 MUSIC_U\n";
        warn_msg += "3. 私聊发送：/登录 MUSIC_U=你的凭据内容";
        rpc_client_.send_private_msg(uid, warn_msg);
        return;
    } else {
        if (check_res.code != 801 && check_res.code != 802) {
            int64_t uid = qr_session_->user_id;
            cancel_qr_login_session();
            rpc_client_.send_private_msg(uid, "✕ | 扫码登录异常（错误码 " + std::to_string(check_res.code) + "：" + check_res.message + "），登录已终止。\n💡 建议使用 Cookie 导入方式：私聊发送 /登录 MUSIC_U=你的凭据");
            return;
        }
        schedule_qr_poll();
    }
}

void MusicModule::cancel_qr_login_session() {
    if (!qr_session_) return;
    boost::system::error_code ec;
    if (qr_session_->timer) {
        qr_session_->timer->cancel(ec);
    }
    if (!qr_session_->png_path.empty()) {
        std::filesystem::remove(qr_session_->png_path, ec);
    }
    qr_session_.reset();
}

} // namespace bot::module::music
