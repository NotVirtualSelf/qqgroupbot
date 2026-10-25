/**
 * @file netease_client.hpp
 * @brief 网易云音乐 API 客户端定义
 * @details 负责网易云音乐平台的搜索消歧、热曲抓取、专辑/单曲详情获取、
 *          每日日推列表拉取、点赞/减少推荐操作、本地 Base64 二维码生成以及扫码登录全流程。
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

namespace bot::module::music {

/**
 * @struct ArtistCandidate
 * @brief 艺术家搜索候选项 (包含封面与最高热度曲目，用于关注/取关卡片展示)
 */
struct ArtistCandidate {
    int64_t id{0};               ///< 平台艺术家 ID
    std::string name;            ///< 艺术家名称
    std::string pic_url;         ///< 头像/封面 URL
    std::string top_song{"<无>"}; ///< 最热门/代表曲目名称
    std::string platform{"netease"}; ///< 音乐平台标识
    std::string mid;             ///< 字符串 ID (QQ 音乐 mid)
};


/**
 * @struct SongItem
 * @brief 歌曲基础条目信息
 */
struct SongItem {
    int64_t id{0};               ///< 单曲 ID
    std::string name;            ///< 单曲名称
    int64_t publish_time{0};     ///< 发行时间戳
    std::string album_name;      ///< 所属专辑名称
    std::string artist_name;     ///< 艺术家名称
};

/**
 * @struct AlbumItem
 * @brief 专辑基础条目信息
 */
struct AlbumItem {
    int64_t id{0};               ///< 专辑 ID
    std::string name;            ///< 专辑名称
    std::string pic_url;         ///< 专辑封面 URL
    int64_t publish_time{0};     ///< 发行时间戳
    int size{0};                 ///< 专辑包含的歌曲数量
    std::string artist_name;     ///< 艺术家名称
};

/**
 * @struct AlbumTrack
 * @brief 专辑内单曲详细信息
 */
struct AlbumTrack {
    int64_t id{0};               ///< 单曲 ID
    std::string name;            ///< 单曲名称
    std::string artist_name;     ///< 创作者列表
    int64_t duration_ms{0};      ///< 单曲时长 (毫秒)
};

/**
 * @struct AlbumDetail
 * @brief 专辑完整详情 (包含完整曲目列表与总时长统计)
 */
struct AlbumDetail {
    int64_t id{0};               ///< 专辑 ID
    std::string name;            ///< 专辑名称
    std::string pic_url;         ///< 专辑高清封面 URL
    int64_t publish_time{0};     ///< 发行时间戳
    std::string artist_name;     ///< 创作者名称
    std::vector<AlbumTrack> tracks; ///< 包含的曲目列表
    int64_t total_duration_ms{0};   ///< 专辑所有曲目总时长 (毫秒)
    std::string album_mid;       ///< 平台专辑字符串 ID
};

/**
 * @struct DailySongItem
 * @brief 每日推荐单曲条目
 */
struct DailySongItem {
    int64_t id{0};               ///< 单曲 ID
    std::string name;            ///< 单曲名称
    std::string artist_name;     ///< 艺术家名称
    std::string album_name;      ///< 专辑名称
    std::string pic_url;         ///< 封面 URL
    int64_t duration_ms{0};      ///< 单曲时长 (毫秒)
    std::string reason;          ///< 网易云官方推荐理由
};

/**
 * @struct QrKeyResult
 * @brief 网易云扫码登录二维码 Key 申请结果
 */
struct QrKeyResult {
    bool success{false};         ///< 是否成功申请
    std::string unikey;          ///< 扫码追踪 key
    std::string url;             ///< 扫码确认专属直链 URL
    std::string error;           ///< 失败错误信息
};

/**
 * @struct QrCheckResult
 * @brief 网易云二维码扫码状态轮询结果
 */
struct QrCheckResult {
    int code{0};                 ///< 状态码 (800: 过期, 801: 等待扫码, 802: 待确认, 803: 授权登录成功)
    std::string message;         ///< 状态描述文本
    std::string cookie;          ///< 登录成功后的 Cookie 凭据 (含 MUSIC_U)
    std::string nickname;        ///< 账号昵称
    std::string avatar_url;      ///< 账号头像 URL
};

/**
 * @struct HttpResult
 * @brief HTTP 请求详细响应结果
 */
struct HttpResult {
    int status_code{0};                      ///< HTTP 状态码
    std::string body;                        ///< 响应体文本
    std::string raw_headers;                 ///< 原始响应头文本
    std::vector<std::string> set_cookie_headers; ///< 提取的 Set-Cookie 头部列表
};

/**
 * @struct LoginStatus
 * @brief 网易云登录账号状态实体
 */
struct LoginStatus {
    bool is_logged_in{false};    ///< 是否已登录且凭据有效
    int64_t user_id{0};          ///< 账号 UID
    std::string nickname;        ///< 账号昵称
    std::string avatar_url;      ///< 账号头像 URL
    bool is_vip{false};          ///< 是否为 VIP 会员用户
};

namespace detail {
/**
 * @brief 解析并合并多个 Set-Cookie 头部、Body Cookie 及已有 Cookie
 * @param set_cookie_headers HTTP 响应的 Set-Cookie 头部数组
 * @param body_cookie 响应体中携带的 cookie 字符串
 * @param existing_cookie 现有的 Cookie 字符串
 * @return std::string 合并去重后的规范 Cookie 字符串
 */
std::string parse_and_combine_cookies(const std::vector<std::string>& set_cookie_headers,
                                      const std::optional<std::string>& body_cookie = std::nullopt,
                                      const std::string& existing_cookie = "");
} // namespace detail

/**
 * @class NeteaseClient
 * @brief 网易云音乐 API 客户端封装 (基于 libcurl + glaze)
 */
class NeteaseClient {
public:
    explicit NeteaseClient(std::string custom_api_base = "");
    ~NeteaseClient();

    NeteaseClient(const NeteaseClient&) = delete;
    NeteaseClient& operator=(const NeteaseClient&) = delete;

    /// 搜索最关联的候选艺术家列表 (包含头像与最热门曲目)
    std::vector<ArtistCandidate> search_candidates(const std::string& name, int limit = 5);

    /// 查询某位艺术家的最热门/代表单曲 (若无歌曲则返回 "<无>")
    std::string get_artist_top_song(int64_t artist_id);

    /// 查询某位艺术家的头像封面 URL
    std::string get_artist_pic_url(int64_t artist_id);

    /// 获取艺术家最新的歌曲列表
    std::vector<SongItem> get_artist_songs(int64_t artist_id, int limit = 10);

    /// 获取艺术家最新的专辑列表
    std::vector<AlbumItem> get_artist_albums(int64_t artist_id, int limit = 10);

    /// 获取专辑完整信息与曲目详情
    std::optional<AlbumDetail> get_album_detail(int64_t album_id);

    /// 获取每日推荐歌曲列表 (日推需登录 Cookie)
    std::vector<DailySongItem> get_daily_recommend_songs(const std::string& cookie = "");

    /// 喜欢/点赞歌曲 (like=true 为喜欢，false 为取消喜欢)
    bool like_song(int64_t song_id, bool like = true, const std::string& cookie = "");

    /// 日推减少推荐某歌曲 (Dislike / 踩 / 不感兴趣)
    bool dislike_recommend_song(int64_t song_id, const std::string& cookie = "");

    /// 获取网易云扫码登录 unikey 与登录 URL
    QrKeyResult get_qr_key();

    /// 检查网易云扫码登录状态 (800: 过期, 801: 等待扫码, 802: 待确认, 803: 成功)
    QrCheckResult check_qr_login(const std::string& unikey);

    /// 检查当前网易云登录凭据状态 (向网易云服务器验证账号登录态)
    LoginStatus check_login_status(const std::string& cookie = "");

    /// 生成二维码图片并返回 Base64 编码 (若 output_png_path 非空则同时保存到该文件)
    static std::string generate_qr_image_base64(const std::string& text, const std::string& output_png_path = "");

    /// 设置登录 Cookie (例如 MUSIC_U)
    void set_cookie(std::string cookie) { cookie_ = std::move(cookie); }
    const std::string& get_cookie() const { return cookie_; }

    /// 测试/模拟注入每日推荐列表 (用于测试和模拟验证)
    void set_mock_daily_songs(std::vector<DailySongItem> mock_songs) {
        mock_daily_songs_ = std::move(mock_songs);
    }
    const std::vector<DailySongItem>& get_mock_daily_songs() const { return mock_daily_songs_; }

    /// 测试/模拟注入二维码状态 (用于单元测试)
    void set_mock_qr_key(QrKeyResult mock_key) { mock_qr_key_ = std::move(mock_key); }
    void set_mock_qr_check(QrCheckResult mock_check) { mock_qr_check_ = std::move(mock_check); }

    /// 测试/模拟注入登录状态 (用于单元测试)
    void set_mock_login_status(LoginStatus mock_status) { mock_login_status_ = std::move(mock_status); }

    /// 执行通用 HTTP GET 请求
    std::string http_get(const std::string& url);

    /// 执行通用 HTTP POST 请求
    std::string http_post(const std::string& url, const std::string& post_data);

    /// 执行通用 HTTP POST 请求并返回详细响应 (包含响应头与 Set-Cookie 头部)
    HttpResult http_post_detailed(const std::string& url, const std::string& post_data);

    /// 执行网易云 Web 客户端专属 weapi 加密 POST 请求
    std::string weapi_post(const std::string& url, const std::string& json_payload, const std::string& cookie = "");

private:
    std::string custom_api_base_;
    std::string cookie_;
    std::vector<DailySongItem> mock_daily_songs_;
    std::optional<QrKeyResult> mock_qr_key_;
    std::optional<QrCheckResult> mock_qr_check_;
    std::optional<LoginStatus> mock_login_status_;
    std::string url_encode(const std::string& value);
};

} // namespace bot::module::music
