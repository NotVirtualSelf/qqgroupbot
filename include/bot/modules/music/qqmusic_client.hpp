/**
 * @file qqmusic_client.hpp
 * @brief QQ 音乐 API 客户端定义
 * @details 封装 QQ 音乐 Smartbox 搜索、歌手详情、代表曲抓取、专辑/单曲曲目列表与总时长统计，
 *          包含 singerMID 与数字 ID 的双向自动解析与内存缓存机制。
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <mutex>

#include "bot/modules/music/netease_client.hpp"

namespace bot::module::music {

/**
 * @class QQMusicClient
 * @brief QQ 音乐 API 客户端封装 (基于 libcurl + glaze)
 */
class QQMusicClient {
public:
    /**
     * @brief 构造函数
     * @param custom_api_base 可选的自定义 API 基地址
     */
    explicit QQMusicClient(std::string custom_api_base = "");

    /**
     * @brief 析构函数
     */
    ~QQMusicClient();

    QQMusicClient(const QQMusicClient&) = delete;
    QQMusicClient& operator=(const QQMusicClient&) = delete;

    /**
     * @brief 搜索最关联的候选艺术家列表 (包含头像封面与最热门曲目)
     * @param name 搜索关键词
     * @param limit 返回候选最大数量 (默认 5)
     * @return std::vector<ArtistCandidate> 候选艺术家列表
     */
    std::vector<ArtistCandidate> search_candidates(const std::string& name, int limit = 5);

    /**
     * @brief 根据数字 ID 查询艺术家的最热门/代表单曲 (若无歌曲则返回 "<无>")
     * @param artist_id 艺术家数字 ID
     * @param artist_name 可选艺术家名称 (用于反查 mid)
     * @param pic_url 可选封面 URL (用于提取 mid)
     * @return std::string 代表曲目名称
     */
    std::string get_artist_top_song(int64_t artist_id, const std::string& artist_name = "", const std::string& pic_url = "");

    /**
     * @brief 根据 singer_mid 查询艺术家的最热门/代表单曲
     * @param singer_mid QQ 音乐歌手 mid 字符串
     * @return std::string 代表曲目名称
     */
    std::string get_artist_top_song_by_mid(const std::string& singer_mid);

    /**
     * @brief 根据数字 ID 查询艺术家的头像封面 URL
     * @param artist_id 艺术家数字 ID
     * @param artist_name 可选艺术家名称
     * @param pic_url 可选封面 URL
     * @return std::string 封面图片 URL
     */
    std::string get_artist_pic_url(int64_t artist_id, const std::string& artist_name = "", const std::string& pic_url = "");

    /**
     * @brief 根据 singer_mid 查询艺术家的头像封面 URL
     * @param singer_mid QQ 音乐歌手 mid 字符串
     * @return std::string 封面图片 URL
     */
    std::string get_artist_pic_url_by_mid(const std::string& singer_mid);

    /**
     * @brief 根据数字 ID 获取艺术家最新的专辑列表
     * @param artist_id 艺术家数字 ID
     * @param artist_name 可选艺术家名称
     * @param pic_url 可选封面 URL
     * @param limit 查询数量上限 (默认 10)
     * @return std::vector<AlbumItem> 专辑简要列表
     */
    std::vector<AlbumItem> get_artist_albums(int64_t artist_id, const std::string& artist_name = "", const std::string& pic_url = "", int limit = 10);

    /**
     * @brief 根据 singer_mid 获取艺术家最新的专辑列表
     * @param singer_mid QQ 音乐歌手 mid 字符串
     * @param limit 查询数量上限 (默认 10)
     * @return std::vector<AlbumItem> 专辑简要列表
     */
    std::vector<AlbumItem> get_artist_albums_by_mid(const std::string& singer_mid, int limit = 10);

    /**
     * @brief 根据专辑数字 ID 获取专辑完整信息与曲目详情
     * @param album_id 专辑数字 ID
     * @return std::optional<AlbumDetail> 专辑完整详情
     */
    std::optional<AlbumDetail> get_album_detail(int64_t album_id);

    /**
     * @brief 根据 album_mid 获取专辑完整信息与曲目详情
     * @param album_mid QQ 音乐专辑 mid 字符串
     * @return std::optional<AlbumDetail> 专辑完整详情
     */
    std::optional<AlbumDetail> get_album_detail_by_mid(const std::string& album_mid);

    /**
     * @brief 执行通用 HTTP GET 请求
     * @param url 请求 URL
     * @return std::string 响应文本
     */
    std::string http_get(const std::string& url);

    /**
     * @brief 执行通用 HTTP POST 请求 (用于 QQ 音乐 musicu.fcg 网关)
     * @param url 请求 URL
     * @param post_data 发送的 POST 字符串 (通常为 JSON)
     * @return std::string 响应文本
     */
    std::string http_post(const std::string& url, const std::string& post_data);

    /**
     * @brief 辅助方法：解析 singerMID (优先从本地缓存提取，其次从 pic_url 提取，再次通过搜索反查)
     * @param artist_id 艺术家数字 ID
     * @param artist_name 艺术家名称
     * @param pic_url 封面图片 URL
     * @return std::string 解析出的 singer_mid
     */
    std::string resolve_singer_mid(int64_t artist_id, const std::string& artist_name = "", const std::string& pic_url = "");

private:
    std::string custom_api_base_;
    std::string url_encode(const std::string& value);

    mutable std::mutex cache_mutex_;
    std::unordered_map<int64_t, std::string> id_to_mid_;
    std::unordered_map<std::string, int64_t> mid_to_id_;
};

} // namespace bot::module::music
