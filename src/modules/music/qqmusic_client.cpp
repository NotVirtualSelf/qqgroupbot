/**
 * @file qqmusic_client.cpp
 * @brief QQ 音乐 API 客户端底层实现
 * @details 负责调用 QQ 音乐 Smartbox 搜索网关、musicu.fcg 复合接口及歌手/专辑详情抓取。
 */

#include "bot/modules/music/qqmusic_client.hpp"
#include "bot/protocol/events.hpp"

#include <iostream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <cctype>
#include <curl/curl.h>
#include <glaze/glaze.hpp>

namespace bot::module::music {

namespace detail_qq {

size_t curl_write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    auto* s = static_cast<std::string*>(userp);
    s->append(static_cast<char*>(contents), total_size);
    return total_size;
}

// UTF-8 安全截断
std::string truncate_utf8(const std::string& str, size_t max_chars) {
    size_t char_count = 0;
    size_t byte_index = 0;
    size_t truncate_byte_pos = 0;
    size_t target_cutoff = (max_chars > 3) ? (max_chars - 2) : max_chars;

    while (byte_index < str.size()) {
        unsigned char c = static_cast<unsigned char>(str[byte_index]);
        size_t char_len = 1;
        if ((c & 0x80) == 0) char_len = 1;
        else if ((c & 0xE0) == 0xC0) char_len = 2;
        else if ((c & 0xF0) == 0xE0) char_len = 3;
        else if ((c & 0xF8) == 0xF0) char_len = 4;

        if (byte_index + char_len > str.size()) break;

        byte_index += char_len;
        ++char_count;

        if (char_count == target_cutoff) {
            truncate_byte_pos = byte_index;
        }
    }

    if (char_count > max_chars) {
        if (truncate_byte_pos == 0) truncate_byte_pos = byte_index;
        std::string res = str.substr(0, truncate_byte_pos);
        while (!res.empty() && (res.back() == ' ' || res.back() == '/')) {
            res.pop_back();
        }
        return res + "...";
    }
    return str;
}

int64_t parse_date_to_timestamp(const std::string& date_str) {
    if (date_str.empty()) return 0;
    std::tm tm = {};
    if (std::sscanf(date_str.c_str(), "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) == 3) {
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        time_t t = timegm(&tm);
        if (t != -1) {
            return static_cast<int64_t>(t) * 1000;
        }
    }
    return 0;
}

// Glaze 模型定义
struct QQSingerItemJson {
    int64_t singerID{0};
    std::string singerMID;
    std::string singerName;
    std::optional<std::string> singerPic;
};

struct QQSearchResultJson {
    struct Data {
        struct Singer {
            std::vector<QQSingerItemJson> list;
        } singer;
    } data;
};

struct QQSmartboxItemJson {
    std::string id;
    std::string mid;
    std::string name;
    std::optional<std::string> pic;
};

struct QQSmartboxResultJson {
    struct Data {
        struct Singer {
            int count{0};
            std::vector<QQSmartboxItemJson> itemlist;
        } singer;
    } data;
};

struct QQSingerNameJson {
    std::string name;
};

struct QQSongDetailJson {
    int64_t id{0};
    std::string name;
    std::vector<QQSingerNameJson> singer;
};

struct QQSingerDetailResponseJson {
    struct SingerDetail {
        int code{0};
        struct Data {
            std::vector<QQSongDetailJson> songlist;
        } data;
    } singerDetail;
};

struct QQAlbumSingerJson {
    std::string singer_name;
};

struct QQLatestSongJson {
    int song_count{0};
};

struct QQAlbumItemJson {
    int64_t albumid{0};
    std::string album_mid;
    std::string album_name;
    std::string pub_time;
    std::string singer_name;
    std::vector<QQAlbumSingerJson> singers;
    std::optional<QQLatestSongJson> latest_song;
};

struct QQSingerAlbumResponseJson {
    struct SingerAlbum {
        int code{0};
        struct Data {
            std::vector<QQAlbumItemJson> list;
        } data;
    } singerAlbum;
};

struct QQAlbumBasicInfoJson {
    int64_t albumID{0};
    std::string albumMid;
    std::string albumName;
    std::string publishDate;
};

struct QQAlbumSingerDetailJson {
    std::string name;
};

struct QQAlbumDetailJson {
    std::optional<QQAlbumBasicInfoJson> basicInfo;
    struct Singer {
        std::vector<QQAlbumSingerDetailJson> singerList;
    } singer;
};

struct QQSongInfoSingerJson {
    std::string name;
};

struct QQSongInfoJson {
    int64_t id{0};
    std::string mid;
    std::string name;
    int64_t interval{0};
    std::vector<QQSongInfoSingerJson> singer;
};

struct QQAlbumSongItemJson {
    QQSongInfoJson songInfo;
};

struct QQAlbumResponseJson {
    struct DetailWrapper {
        int code{0};
        QQAlbumDetailJson data;
    } albumDetail;
    struct SonglistWrapper {
        int code{0};
        struct SonglistData {
            std::vector<QQAlbumSongItemJson> songList;
        } data;
    } albumSonglist;
};

} // namespace detail_qq

QQMusicClient::QQMusicClient(std::string custom_api_base)
    : custom_api_base_(std::move(custom_api_base))
{
    static bool curl_initialized = false;
    if (!curl_initialized) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl_initialized = true;
    }
}

QQMusicClient::~QQMusicClient() = default;

std::string QQMusicClient::url_encode(const std::string& value) {
    CURL* curl = curl_easy_init();
    if (!curl) return value;
    char* encoded = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.length()));
    std::string result;
    if (encoded) {
        result = encoded;
        curl_free(encoded);
    }
    curl_easy_cleanup(curl);
    return result;
}

std::string QQMusicClient::http_get(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, detail_qq::curl_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36");

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        std::cerr << "[QQMusicClient] curl_easy_perform() failed: " << curl_easy_strerror(res) << " | URL: " << url << std::endl;
    }
    curl_easy_cleanup(curl);
    return response;
}

std::string QQMusicClient::http_post(const std::string& url, const std::string& post_data) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Referer: https://y.qq.com/");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(post_data.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, detail_qq::curl_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36");

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        std::cerr << "[QQMusicClient] POST failed: " << curl_easy_strerror(res) << " | URL: " << url << std::endl;
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

std::string QQMusicClient::resolve_singer_mid(int64_t artist_id, const std::string& artist_name, const std::string& pic_url) {
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = id_to_mid_.find(artist_id);
        if (it != id_to_mid_.end() && !it->second.empty()) {
            return it->second;
        }
    }

    if (!pic_url.empty()) {
        auto pos = pic_url.find("M000");
        if (pos != std::string::npos && pos + 4 + 14 <= pic_url.size()) {
            std::string mid = pic_url.substr(pos + 4, 14);
            bool valid = true;
            for (char c : mid) {
                if (!std::isalnum(static_cast<unsigned char>(c))) {
                    valid = false;
                    break;
                }
            }
            if (valid) {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                id_to_mid_[artist_id] = mid;
                mid_to_id_[mid] = artist_id;
                return mid;
            }
        }
    }

    if (!artist_name.empty()) {
        auto cands = search_candidates(artist_name, 5);
        for (const auto& c : cands) {
            if (c.id == artist_id && !c.mid.empty()) {
                return c.mid;
            }
        }
    }

    return "";
}

std::vector<ArtistCandidate> QQMusicClient::search_candidates(const std::string& name, int limit) {
    std::vector<ArtistCandidate> candidates;

    // 优先尝试官方前端实时检索接口 smartbox_new.fcg (高可用、响应迅速、格式稳定)
    std::string smartbox_url = !custom_api_base_.empty() 
        ? custom_api_base_ + "/smartbox?key=" + url_encode(name)
        : "https://c.y.qq.com/splcloud/fcgi-bin/smartbox_new.fcg?key=" + url_encode(name) + "&format=json";

    std::string smart_body = http_get(smartbox_url);
    if (!smart_body.empty()) {
        detail_qq::QQSmartboxResultJson sb_res;
        auto ec = glz::read<bot::protocol::glaze_opts>(sb_res, smart_body);
        if (!ec && !sb_res.data.singer.itemlist.empty()) {
            for (const auto& item : sb_res.data.singer.itemlist) {
                if (candidates.size() >= static_cast<size_t>(limit)) break;
                ArtistCandidate cand;
                try {
                    cand.id = std::stoll(item.id);
                } catch (...) {
                    cand.id = 0;
                }
                cand.name = item.name;
                cand.mid = item.mid;
                cand.platform = "qqmusic";
                if (!item.mid.empty()) {
                    cand.pic_url = "https://y.qq.com/music/photo_new/T001R300x300M000" + item.mid + ".jpg";
                } else if (item.pic && !item.pic->empty()) {
                    cand.pic_url = *item.pic;
                }
                if (cand.id > 0 && !cand.mid.empty()) {
                    std::lock_guard<std::mutex> lock(cache_mutex_);
                    id_to_mid_[cand.id] = cand.mid;
                    mid_to_id_[cand.mid] = cand.id;
                }
                cand.top_song = get_artist_top_song_by_mid(item.mid);
                candidates.push_back(std::move(cand));
            }
            if (!candidates.empty()) {
                return candidates;
            }
        }
    }

    // 后备方案：client_search_cp
    std::string url;
    if (!custom_api_base_.empty()) {
        url = custom_api_base_ + "/search?w=" + url_encode(name) + "&t=9&n=" + std::to_string(limit);
    } else {
        url = "https://c.y.qq.com/soso/fcgi-bin/client_search_cp?w=" + url_encode(name)
            + "&t=9&format=json&n=" + std::to_string(limit);
    }

    std::string json_body = http_get(url);
    if (json_body.empty()) {
        return candidates;
    }

    detail_qq::QQSearchResultJson res;
    auto ec = glz::read<bot::protocol::glaze_opts>(res, json_body);
    if (ec) {
        std::cerr << "[QQMusicClient] Failed to parse search JSON: " << glz::format_error(ec, json_body) << std::endl;
        return candidates;
    }

    const auto& list = res.data.singer.list;
    for (const auto& item : list) {
        ArtistCandidate cand;
        cand.id = item.singerID;
        cand.name = item.singerName;
        cand.mid = item.singerMID;
        cand.platform = "qqmusic";

        if (!item.singerMID.empty()) {
            cand.pic_url = "https://y.qq.com/music/photo_new/T001R300x300M000" + item.singerMID + ".jpg";
        } else if (item.singerPic && !item.singerPic->empty()) {
            cand.pic_url = *item.singerPic;
        }

        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            id_to_mid_[item.singerID] = item.singerMID;
            mid_to_id_[item.singerMID] = item.singerID;
        }

        cand.top_song = get_artist_top_song_by_mid(item.singerMID);
        candidates.push_back(std::move(cand));
    }

    return candidates;
}

std::string QQMusicClient::get_artist_top_song(int64_t artist_id, const std::string& artist_name, const std::string& pic_url) {
    std::string mid = resolve_singer_mid(artist_id, artist_name, pic_url);
    if (mid.empty()) return "<无>";
    return get_artist_top_song_by_mid(mid);
}

std::string QQMusicClient::get_artist_top_song_by_mid(const std::string& singer_mid) {
    if (singer_mid.empty()) return "<无>";

    std::string gateway = !custom_api_base_.empty() ? custom_api_base_ + "/musicu" : "https://u.y.qq.com/cgi-bin/musicu.fcg";
    std::string post_data = "{\"comm\":{\"cv\":4747474,\"ct\":24,\"format\":\"json\",\"platform\":\"yqq.json\"},"
                            "\"singerDetail\":{\"module\":\"music.web_singer_info_svr\",\"method\":\"get_singer_detail_info\","
                            "\"param\":{\"singermid\":\"" + singer_mid + "\",\"sin\":0,\"num\":5}}}";

    std::string json_body = http_post(gateway, post_data);
    if (json_body.empty()) return "<无>";

    detail_qq::QQSingerDetailResponseJson resp;
    auto ec = glz::read<bot::protocol::glaze_opts>(resp, json_body);
    if (ec || resp.singerDetail.code != 0 || resp.singerDetail.data.songlist.empty()) {
        return "<无>";
    }

    const auto& song = resp.singerDetail.data.songlist[0];
    std::string artists_str;
    for (const auto& a : song.singer) {
        if (!a.name.empty()) {
            if (!artists_str.empty()) artists_str += " / ";
            artists_str += a.name;
        }
    }

    if (artists_str.empty()) {
        return song.name;
    }

    artists_str = detail_qq::truncate_utf8(artists_str, 20);
    return artists_str + " - " + song.name;
}

std::string QQMusicClient::get_artist_pic_url(int64_t artist_id, const std::string& artist_name, const std::string& pic_url) {
    std::string mid = resolve_singer_mid(artist_id, artist_name, pic_url);
    if (mid.empty()) return "";
    return get_artist_pic_url_by_mid(mid);
}

std::string QQMusicClient::get_artist_pic_url_by_mid(const std::string& singer_mid) {
    if (singer_mid.empty()) return "";
    return "https://y.qq.com/music/photo_new/T001R300x300M000" + singer_mid + ".jpg";
}

std::vector<AlbumItem> QQMusicClient::get_artist_albums(int64_t artist_id, const std::string& artist_name, const std::string& pic_url, int limit) {
    std::string mid = resolve_singer_mid(artist_id, artist_name, pic_url);
    if (mid.empty()) return {};
    return get_artist_albums_by_mid(mid, limit);
}

std::vector<AlbumItem> QQMusicClient::get_artist_albums_by_mid(const std::string& singer_mid, int limit) {
    std::vector<AlbumItem> result;
    if (singer_mid.empty()) return result;

    std::string gateway = !custom_api_base_.empty() ? custom_api_base_ + "/musicu" : "https://u.y.qq.com/cgi-bin/musicu.fcg";
    std::string post_data = "{\"comm\":{\"cv\":4747474,\"ct\":24,\"format\":\"json\",\"platform\":\"yqq.json\"},"
                            "\"singerAlbum\":{\"module\":\"music.web_singer_info_svr\",\"method\":\"get_singer_album\","
                            "\"param\":{\"singermid\":\"" + singer_mid + "\",\"begin\":0,\"num\":" + std::to_string(limit) + ",\"order\":\"time\"}}}";

    std::string json_body = http_post(gateway, post_data);
    if (json_body.empty()) return result;

    detail_qq::QQSingerAlbumResponseJson resp;
    auto ec = glz::read<bot::protocol::glaze_opts>(resp, json_body);
    if (ec || resp.singerAlbum.code != 0) {
        std::cerr << "[QQMusicClient] Failed to parse singer albums: " << (ec ? glz::format_error(ec, json_body) : "code != 0") << std::endl;
        return result;
    }

    for (const auto& a : resp.singerAlbum.data.list) {
        AlbumItem item;
        item.id = a.albumid;
        item.name = a.album_name;
        if (!a.album_mid.empty()) {
            item.pic_url = "https://y.qq.com/music/photo_new/T002R300x300M000" + a.album_mid + ".jpg";
        }
        item.publish_time = detail_qq::parse_date_to_timestamp(a.pub_time);
        if (a.latest_song) {
            item.size = a.latest_song->song_count;
        }
        item.artist_name = !a.singers.empty() ? a.singers[0].singer_name : a.singer_name;
        result.push_back(std::move(item));
    }

    return result;
}

std::optional<AlbumDetail> QQMusicClient::get_album_detail(int64_t album_id) {
    if (album_id <= 0) return std::nullopt;

    std::string gateway = !custom_api_base_.empty() ? custom_api_base_ + "/musicu" : "https://u.y.qq.com/cgi-bin/musicu.fcg";
    std::string post_data = "{\"comm\":{\"cv\":4747474,\"ct\":24,\"format\":\"json\",\"platform\":\"yqq.json\"},"
                            "\"albumDetail\":{\"module\":\"music.musichallAlbum.AlbumInfoServer\",\"method\":\"GetAlbumDetail\","
                            "\"param\":{\"albumId\":" + std::to_string(album_id) + "}},"
                            "\"albumSonglist\":{\"module\":\"music.musichallAlbum.AlbumSongList\",\"method\":\"GetAlbumSongList\","
                            "\"param\":{\"albumId\":" + std::to_string(album_id) + ",\"begin\":0,\"num\":100}}}";

    std::string json_body = http_post(gateway, post_data);
    if (json_body.empty()) return std::nullopt;

    detail_qq::QQAlbumResponseJson resp;
    auto ec = glz::read<bot::protocol::glaze_opts>(resp, json_body);
    if (ec || resp.albumDetail.code != 0 || !resp.albumDetail.data.basicInfo) {
        std::cerr << "[QQMusicClient] Failed to parse album detail: " << (ec ? glz::format_error(ec, json_body) : "code != 0") << std::endl;
        return std::nullopt;
    }

    const auto& basic = *resp.albumDetail.data.basicInfo;
    AlbumDetail detail;
    detail.id = basic.albumID != 0 ? basic.albumID : album_id;
    detail.album_mid = basic.albumMid;
    detail.name = basic.albumName;
    if (!basic.albumMid.empty()) {
        detail.pic_url = "https://y.qq.com/music/photo_new/T002R300x300M000" + basic.albumMid + ".jpg";
    }
    detail.publish_time = detail_qq::parse_date_to_timestamp(basic.publishDate);

    std::string artists_str;
    for (const auto& s : resp.albumDetail.data.singer.singerList) {
        if (!s.name.empty()) {
            if (!artists_str.empty()) artists_str += " / ";
            artists_str += s.name;
        }
    }
    detail.artist_name = artists_str;

    for (const auto& item : resp.albumSonglist.data.songList) {
        AlbumTrack track;
        track.id = item.songInfo.id;
        track.name = item.songInfo.name;
        track.duration_ms = item.songInfo.interval * 1000;

        std::string track_arts;
        for (const auto& ts : item.songInfo.singer) {
            if (!ts.name.empty()) {
                if (!track_arts.empty()) track_arts += " / ";
                track_arts += ts.name;
            }
        }
        track.artist_name = !track_arts.empty() ? track_arts : detail.artist_name;
        detail.total_duration_ms += track.duration_ms;
        detail.tracks.push_back(std::move(track));
    }

    return detail;
}

std::optional<AlbumDetail> QQMusicClient::get_album_detail_by_mid(const std::string& album_mid) {
    if (album_mid.empty()) return std::nullopt;

    std::string gateway = !custom_api_base_.empty() ? custom_api_base_ + "/musicu" : "https://u.y.qq.com/cgi-bin/musicu.fcg";
    std::string post_data = "{\"comm\":{\"cv\":4747474,\"ct\":24,\"format\":\"json\",\"platform\":\"yqq.json\"},"
                            "\"albumDetail\":{\"module\":\"music.musichallAlbum.AlbumInfoServer\",\"method\":\"GetAlbumDetail\","
                            "\"param\":{\"albumMid\":\"" + album_mid + "\"}},"
                            "\"albumSonglist\":{\"module\":\"music.musichallAlbum.AlbumSongList\",\"method\":\"GetAlbumSongList\","
                            "\"param\":{\"albumMid\":\"" + album_mid + "\",\"begin\":0,\"num\":100}}}";

    std::string json_body = http_post(gateway, post_data);
    if (json_body.empty()) return std::nullopt;

    detail_qq::QQAlbumResponseJson resp;
    auto ec = glz::read<bot::protocol::glaze_opts>(resp, json_body);
    if (ec || resp.albumDetail.code != 0 || !resp.albumDetail.data.basicInfo) {
        std::cerr << "[QQMusicClient] Failed to parse album detail by mid: " << (ec ? glz::format_error(ec, json_body) : "code != 0") << std::endl;
        return std::nullopt;
    }

    const auto& basic = *resp.albumDetail.data.basicInfo;
    AlbumDetail detail;
    detail.id = basic.albumID;
    detail.album_mid = basic.albumMid;
    detail.name = basic.albumName;
    if (!basic.albumMid.empty()) {
        detail.pic_url = "https://y.qq.com/music/photo_new/T002R300x300M000" + basic.albumMid + ".jpg";
    }
    detail.publish_time = detail_qq::parse_date_to_timestamp(basic.publishDate);

    std::string artists_str;
    for (const auto& s : resp.albumDetail.data.singer.singerList) {
        if (!s.name.empty()) {
            if (!artists_str.empty()) artists_str += " / ";
            artists_str += s.name;
        }
    }
    detail.artist_name = artists_str;

    for (const auto& item : resp.albumSonglist.data.songList) {
        AlbumTrack track;
        track.id = item.songInfo.id;
        track.name = item.songInfo.name;
        track.duration_ms = item.songInfo.interval * 1000;

        std::string track_arts;
        for (const auto& ts : item.songInfo.singer) {
            if (!ts.name.empty()) {
                if (!track_arts.empty()) track_arts += " / ";
                track_arts += ts.name;
            }
        }
        track.artist_name = !track_arts.empty() ? track_arts : detail.artist_name;
        detail.total_duration_ms += track.duration_ms;
        detail.tracks.push_back(std::move(track));
    }

    return detail;
}

} // namespace bot::module::music
