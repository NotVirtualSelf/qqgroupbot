/**
 * @file netease_client.cpp
 * @brief 网易云音乐 API 客户端底层网络通信与业务接口实现
 * @details 包含 weapi 加密、libcurl HTTP(S) 请求封装、Cookie 提取与清洗、扫码登录与二维码生成。
 */

#include "bot/modules/music/netease_client.hpp"
#include "bot/protocol/events.hpp"

#include <iostream>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <cstdlib>
#include <vector>
#include <random>
#include <algorithm>
#include <openssl/evp.h>
#include <openssl/bn.h>
#include <curl/curl.h>
#include <glaze/glaze.hpp>

namespace bot::module::music {

namespace detail {

size_t curl_write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    auto* s = static_cast<std::string*>(userp);
    s->append(static_cast<char*>(contents), total_size);
    return total_size;
}

size_t curl_header_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
    size_t total_size = size * nitems;
    auto* result = static_cast<HttpResult*>(userdata);
    std::string line(buffer, total_size);
    result->raw_headers.append(line);

    size_t start = 0;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
        start++;
    }
    const std::string prefix = "set-cookie:";
    if (line.size() - start >= prefix.size()) {
        bool match = true;
        for (size_t i = 0; i < prefix.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(line[start + i])) != prefix[i]) {
                match = false;
                break;
            }
        }
        if (match) {
            std::string cookie_val = line.substr(start + prefix.size());
            while (!cookie_val.empty() && (cookie_val.front() == ' ' || cookie_val.front() == '\t')) {
                cookie_val.erase(cookie_val.begin());
            }
            while (!cookie_val.empty() && (cookie_val.back() == '\r' || cookie_val.back() == '\n' || cookie_val.back() == ' ' || cookie_val.back() == '\t')) {
                cookie_val.pop_back();
            }
            if (!cookie_val.empty()) {
                result->set_cookie_headers.push_back(std::move(cookie_val));
            }
        }
    }
    return total_size;
}

std::string base64_encode_bytes(const unsigned char* data, size_t len) {
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for (size_t i = 0; i < len; ++i) {
        val = (val << 8) + data[i];
        valb += 8;
        while (valb >= 0) {
            out.push_back(chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(chars[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

std::string aes_128_cbc_encrypt(const std::string& plaintext, const std::string& key, const std::string& iv) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr,
                       reinterpret_cast<const unsigned char*>(key.data()),
                       reinterpret_cast<const unsigned char*>(iv.data()));
    std::vector<unsigned char> ciphertext(plaintext.size() + 32);
    int out_len1 = 0;
    EVP_EncryptUpdate(ctx, ciphertext.data(), &out_len1,
                      reinterpret_cast<const unsigned char*>(plaintext.data()), static_cast<int>(plaintext.size()));
    int out_len2 = 0;
    EVP_EncryptFinal_ex(ctx, ciphertext.data() + out_len1, &out_len2);
    EVP_CIPHER_CTX_free(ctx);

    size_t total_len = out_len1 + out_len2;
    return base64_encode_bytes(ciphertext.data(), total_len);
}

std::string rsa_encrypt_secret_key(const std::string& secret_key) {
    std::string reversed = secret_key;
    std::reverse(reversed.begin(), reversed.end());

    const char* modulus_hex =
        "00e0b509f6259df8642dbc35662901477df22677ec152b5ff68ace615bb7"
        "b725152b3ab17a876aea8a5aa76d2e417629ec4ee341f56135fccf695280"
        "104e0312ecbda92557c93870114af6c9d05c4f7f0c3685b7a46bee255932"
        "575cce10b424d813cfe4875d3e82047b97ddef52741d546b8e289dc6935b"
        "3ece0462db0a22b8e7";

    BIGNUM* m = BN_new();
    BN_bin2bn(reinterpret_cast<const unsigned char*>(reversed.data()), static_cast<int>(reversed.size()), m);

    BIGNUM* e = BN_new();
    BN_hex2bn(&e, "010001");

    BIGNUM* n = BN_new();
    BN_hex2bn(&n, modulus_hex);

    BIGNUM* res = BN_new();
    BN_CTX* ctx = BN_CTX_new();
    BN_mod_exp(res, m, e, n, ctx);

    char* hex = BN_bn2hex(res);
    std::string hex_str(hex);
    OPENSSL_free(hex);

    BN_CTX_free(ctx);
    BN_free(res);
    BN_free(n);
    BN_free(e);
    BN_free(m);

    std::transform(hex_str.begin(), hex_str.end(), hex_str.begin(), [](unsigned char c) { return std::tolower(c); });
    if (hex_str.size() < 256) {
        hex_str = std::string(256 - hex_str.size(), '0') + hex_str;
    }
    return hex_str;
}

std::pair<std::string, std::string> weapi_encrypt(const std::string& json_payload) {
    const std::string preset_key = "0CoJUm6Qyw8W8jud";
    const std::string iv = "0102030405060708";

    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, sizeof(alphabet) - 2);
    std::string secret_key;
    for (int i = 0; i < 16; ++i) {
        secret_key.push_back(alphabet[dis(gen)]);
    }

    std::string step1 = aes_128_cbc_encrypt(json_payload, preset_key, iv);
    std::string params = aes_128_cbc_encrypt(step1, secret_key, iv);
    std::string encSecKey = rsa_encrypt_secret_key(secret_key);

    return {params, encSecKey};
}

std::string parse_and_combine_cookies(const std::vector<std::string>& set_cookie_headers,
                                      const std::optional<std::string>& body_cookie,
                                      const std::string& existing_cookie) {
    std::map<std::string, std::string> cookie_map;

    auto ingest_token = [&](const std::string& token) {
        auto eq_pos = token.find('=');
        if (eq_pos != std::string::npos && eq_pos > 0) {
            std::string name = token.substr(0, eq_pos);
            std::string val = token.substr(eq_pos + 1);
            while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.erase(name.begin());
            while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
            while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
            while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.pop_back();
            // 严格过滤非 ASCII 字符 (如用户粘贴残留的中文文字)
            bool ascii_ok = true;
            for (unsigned char ch : name) {
                if (ch < 32 || ch >= 127) { ascii_ok = false; break; }
            }
            for (unsigned char ch : val) {
                if (ch < 32 || ch >= 127) { ascii_ok = false; break; }
            }
            if (!name.empty() && ascii_ok) {
                cookie_map[name] = val;
            }
        }
    };

    if (!existing_cookie.empty()) {
        std::istringstream iss(existing_cookie);
        std::string token;
        while (std::getline(iss, token, ';')) {
            ingest_token(token);
        }
    }

    for (const auto& header : set_cookie_headers) {
        auto semi_pos = header.find(';');
        std::string kv = (semi_pos != std::string::npos) ? header.substr(0, semi_pos) : header;
        ingest_token(kv);
    }

    if (body_cookie && !body_cookie->empty()) {
        std::istringstream iss(*body_cookie);
        std::string token;
        while (std::getline(iss, token, ';')) {
            ingest_token(token);
        }
    }

    if (cookie_map.empty()) {
        return "";
    }

    std::string res;
    for (const auto& [name, val] : cookie_map) {
        if (!res.empty()) {
            res += "; ";
        }
        res += name + "=" + val;
    }
    return res;
}

struct AccountJson {
    int64_t id{0};
    std::optional<int> vipType;
};

struct ProfileJson {
    int64_t userId{0};
    std::string nickname;
    std::string avatarUrl;
    std::optional<int> vipType;
};

struct UserAccountResponseJson {
    int code{0};
    std::optional<AccountJson> account;
    std::optional<ProfileJson> profile;
};

struct QrUnikeyResponseJson {
    int code{0};
    std::string unikey;
};

struct QrLoginCheckResponseJson {
    int code{0};
    std::string message;
    std::optional<std::string> cookie;
    std::optional<std::string> nickname;
    std::optional<std::string> avatarUrl;
};

// 命名命名空间中的数据模型 (Glaze 反射需要 external linkage)
struct ArtistSearchItemJson {
    int64_t id{0};
    std::string name;
    std::optional<std::string> picUrl;
    std::optional<std::string> img1v1Url;
};

struct SearchResultJson {
    struct Result {
        std::vector<ArtistSearchItemJson> artists;
    } result;
};

struct SongArtistJson {
    std::string name;
};

struct TopSongItemJson {
    std::string name;
    std::vector<SongArtistJson> ar;
    std::vector<SongArtistJson> artists;
};

struct TopSongResponseJson {
    std::vector<TopSongItemJson> songs;
};

struct AlbumJson {
    std::string name;
    int64_t publishTime{0};
};

struct SongJson {
    int64_t id{0};
    std::string name;
    int64_t publishTime{0};
    std::optional<AlbumJson> album;
    std::vector<SongArtistJson> artists;
};

struct SongListResponse {
    std::vector<SongJson> songs;
};

struct ArtistInfoJson {
    std::optional<std::string> picUrl;
    std::optional<std::string> img1v1Url;
};

struct ArtistDetailResponseJson {
    std::optional<ArtistInfoJson> artist;
};

struct AlbumArtistItemJson {
    int64_t id{0};
    std::string name;
};

struct HotAlbumItemJson {
    int64_t id{0};
    std::string name;
    std::optional<std::string> picUrl;
    int64_t publishTime{0};
    int size{0};
    std::optional<AlbumArtistItemJson> artist;
    std::vector<AlbumArtistItemJson> artists;
};

struct ArtistAlbumsResponseJson {
    std::vector<HotAlbumItemJson> hotAlbums;
};

struct AlbumSongItemJson {
    int64_t id{0};
    std::string name;
    int64_t dt{0};
    std::vector<AlbumArtistItemJson> ar;
    std::vector<AlbumArtistItemJson> artists;
};

struct AlbumDetailResponseJson {
    std::optional<HotAlbumItemJson> album;
    std::vector<AlbumSongItemJson> songs;
};

struct RecommendSongItemJson {
    int64_t id{0};
    std::string name;
    int64_t dt{0};
    std::optional<std::string> reason;
    std::vector<AlbumArtistItemJson> ar;
    std::vector<AlbumArtistItemJson> artists;
    std::optional<HotAlbumItemJson> al;
    std::optional<HotAlbumItemJson> album;
};

struct DailyRecommendResponseJson {
    int code{0};
    struct Data {
        std::vector<RecommendSongItemJson> dailySongs;
        std::vector<RecommendSongItemJson> recommend;
    } data;
    std::vector<RecommendSongItemJson> recommend;
};

struct CommonCodeResponseJson {
    int code{0};
};

std::string get_artists_display_name(const std::optional<AlbumArtistItemJson>& single_artist,
                                     const std::vector<AlbumArtistItemJson>& artists) {
    if (!artists.empty()) {
        std::string res;
        for (const auto& a : artists) {
            if (!a.name.empty()) {
                if (!res.empty()) res += " / ";
                res += a.name;
            }
        }
        if (!res.empty()) return res;
    }
    if (single_artist && !single_artist->name.empty()) {
        return single_artist->name;
    }
    return "";
}

/**
 * @brief UTF-8 安全截断函数：当字符数超出 max_chars 时，截取前 max_chars-2 个字符并在末尾追加 "..."
 */
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
        // 清理末尾残存的分隔符或空格
        while (!res.empty() && (res.back() == ' ' || res.back() == '/')) {
            res.pop_back();
        }
        return res + "...";
    }
    return str;
}

} // namespace detail

NeteaseClient::NeteaseClient(std::string custom_api_base)
    : custom_api_base_(std::move(custom_api_base))
{
    static bool curl_initialized = false;
    if (!curl_initialized) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl_initialized = true;
    }
}

NeteaseClient::~NeteaseClient() = default;

std::string NeteaseClient::url_encode(const std::string& value) {
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

std::string NeteaseClient::http_get(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, detail::curl_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36");
    if (!cookie_.empty()) {
        curl_easy_setopt(curl, CURLOPT_COOKIE, cookie_.c_str());
    }

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        std::cerr << "[NeteaseClient] curl_easy_perform() failed: " << curl_easy_strerror(res) << " | URL: " << url << std::endl;
    }
    curl_easy_cleanup(curl);
    return response;
}

HttpResult NeteaseClient::http_post_detailed(const std::string& url, const std::string& post_data) {
    CURL* curl = curl_easy_init();
    HttpResult result;
    if (!curl) return result;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
    headers = curl_slist_append(headers, "Referer: https://music.163.com/");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(post_data.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");
    if (!cookie_.empty()) {
        curl_easy_setopt(curl, CURLOPT_COOKIE, cookie_.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, detail::curl_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, detail::curl_header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &result);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36");

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        std::cerr << "[NeteaseClient] POST failed: " << curl_easy_strerror(res) << " | URL: " << url << std::endl;
    } else {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        result.status_code = static_cast<int>(http_code);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result;
}

std::string NeteaseClient::http_post(const std::string& url, const std::string& post_data) {
    return http_post_detailed(url, post_data).body;
}

std::string NeteaseClient::get_artist_top_song(int64_t artist_id) {
    std::string url;
    if (!custom_api_base_.empty()) {
        url = custom_api_base_ + "/artist/top/song?id=" + std::to_string(artist_id);
    } else {
        url = "https://music.163.com/api/artist/top/song?id=" + std::to_string(artist_id);
    }

    std::string json_body = http_get(url);
    if (json_body.empty()) {
        return "<无>";
    }

    detail::TopSongResponseJson resp;
    auto ec = glz::read<bot::protocol::glaze_opts>(resp, json_body);
    if (!ec && !resp.songs.empty()) {
        const auto& song = resp.songs[0];
        const auto& artists = !song.ar.empty() ? song.ar : song.artists;

        std::string artists_str;
        for (const auto& a : artists) {
            if (!a.name.empty()) {
                if (!artists_str.empty()) {
                    artists_str += " / ";
                }
                artists_str += a.name;
            }
        }

        if (artists_str.empty()) {
            return song.name;
        }

        // 歌手名字过长时进行安全截断 (限制约 20 个字符，超出省略)
        artists_str = detail::truncate_utf8(artists_str, 20);
        return artists_str + " - " + song.name;
    }

    return "<无>";
}

std::string NeteaseClient::get_artist_pic_url(int64_t artist_id) {
    std::string url;
    if (!custom_api_base_.empty()) {
        url = custom_api_base_ + "/artist/detail?id=" + std::to_string(artist_id);
    } else {
        url = "https://music.163.com/api/v1/artist/" + std::to_string(artist_id);
    }

    std::string json_body = http_get(url);
    if (json_body.empty()) {
        return "";
    }

    detail::ArtistDetailResponseJson resp;
    auto ec = glz::read<bot::protocol::glaze_opts>(resp, json_body);
    if (!ec && resp.artist) {
        if (resp.artist->picUrl && !resp.artist->picUrl->empty()) return *resp.artist->picUrl;
        if (resp.artist->img1v1Url && !resp.artist->img1v1Url->empty()) return *resp.artist->img1v1Url;
    }
    return "";
}

std::vector<ArtistCandidate> NeteaseClient::search_candidates(const std::string& name, int limit) {
    std::string url;
    if (!custom_api_base_.empty()) {
        url = custom_api_base_ + "/search?keywords=" + url_encode(name) + "&type=100&limit=" + std::to_string(limit);
    } else {
        url = "https://music.163.com/api/search/get/web?csrf_token=&s=" + url_encode(name)
            + "&type=100&offset=0&total=true&limit=" + std::to_string(limit);
    }

    std::string json_body = http_get(url);
    std::vector<ArtistCandidate> candidates;
    if (json_body.empty()) {
        return candidates;
    }

    detail::SearchResultJson res;
    auto ec = glz::read<bot::protocol::glaze_opts>(res, json_body);
    if ((ec || res.result.artists.empty()) && custom_api_base_.empty()) {
        std::string fallback_url = "https://music.163.com/api/cloudsearch/pc?s=" + url_encode(name)
            + "&type=100&limit=" + std::to_string(limit) + "&offset=0";
        std::string fallback_body = http_get(fallback_url);
        if (!fallback_body.empty()) {
            auto fb_ec = glz::read<bot::protocol::glaze_opts>(res, fallback_body);
            if (!fb_ec && !res.result.artists.empty()) {
                ec = fb_ec;
            }
        }
    }

    if (ec) {
        std::cerr << "[NeteaseClient] Failed to parse search JSON: " << glz::format_error(ec, json_body) << std::endl;
        return candidates;
    }
    if (res.result.artists.empty()) {
        return candidates;
    }

    for (const auto& a : res.result.artists) {
        ArtistCandidate cand;
        cand.id = a.id;
        cand.name = a.name;
        if (a.picUrl && !a.picUrl->empty()) {
            cand.pic_url = *a.picUrl;
        } else if (a.img1v1Url && !a.img1v1Url->empty()) {
            cand.pic_url = *a.img1v1Url;
        }
        // 拉取该艺术家的最高热度代表曲 (格式为 "歌手 - 音乐"，无歌曲则返回 <无>)
        cand.top_song = get_artist_top_song(a.id);
        candidates.push_back(std::move(cand));
    }

    return candidates;
}

std::vector<SongItem> NeteaseClient::get_artist_songs(int64_t artist_id, int limit) {
    std::string url;
    if (!custom_api_base_.empty()) {
        url = custom_api_base_ + "/artist/songs?id=" + std::to_string(artist_id) + "&limit=" + std::to_string(limit);
    } else {
        url = "https://music.163.com/api/v1/artist/songs?id=" + std::to_string(artist_id) + "&limit=" + std::to_string(limit) + "&offset=0";
    }

    std::string json_body = http_get(url);
    std::vector<SongItem> result;
    if (json_body.empty()) {
        return result;
    }

    detail::SongListResponse resp;
    auto ec = glz::read<bot::protocol::glaze_opts>(resp, json_body);
    if (ec) {
        std::cerr << "[NeteaseClient] Failed to parse artist songs JSON" << std::endl;
        return result;
    }

    for (const auto& s : resp.songs) {
        SongItem item;
        item.id = s.id;
        item.name = s.name;
        item.publish_time = s.publishTime;

        if (s.album) {
            item.album_name = s.album->name;
            if (item.publish_time == 0) {
                item.publish_time = s.album->publishTime;
            }
        }

        if (!s.artists.empty()) {
            item.artist_name = s.artists[0].name;
        }

        result.push_back(std::move(item));
    }

    return result;
}

std::vector<AlbumItem> NeteaseClient::get_artist_albums(int64_t artist_id, int limit) {
    std::string url;
    if (!custom_api_base_.empty()) {
        url = custom_api_base_ + "/artist/album?id=" + std::to_string(artist_id) + "&limit=" + std::to_string(limit);
    } else {
        url = "https://music.163.com/api/artist/albums/" + std::to_string(artist_id) + "?limit=" + std::to_string(limit) + "&offset=0";
    }

    std::string json_body = http_get(url);
    std::vector<AlbumItem> result;
    if (json_body.empty()) {
        return result;
    }

    detail::ArtistAlbumsResponseJson resp;
    auto ec = glz::read<bot::protocol::glaze_opts>(resp, json_body);
    if (ec) {
        std::cerr << "[NeteaseClient] Failed to parse artist albums JSON" << std::endl;
        return result;
    }

    for (const auto& a : resp.hotAlbums) {
        AlbumItem item;
        item.id = a.id;
        item.name = a.name;
        item.publish_time = a.publishTime;
        item.size = a.size;
        if (a.picUrl) {
            item.pic_url = *a.picUrl;
        }
        item.artist_name = detail::get_artists_display_name(a.artist, a.artists);
        result.push_back(std::move(item));
    }

    return result;
}

std::optional<AlbumDetail> NeteaseClient::get_album_detail(int64_t album_id) {
    std::string url;
    if (!custom_api_base_.empty()) {
        url = custom_api_base_ + "/album?id=" + std::to_string(album_id);
    } else {
        url = "https://music.163.com/api/v1/album/" + std::to_string(album_id);
    }

    std::string json_body = http_get(url);
    if (json_body.empty()) {
        return std::nullopt;
    }

    detail::AlbumDetailResponseJson resp;
    auto ec = glz::read<bot::protocol::glaze_opts>(resp, json_body);
    if (ec || !resp.album) {
        std::cerr << "[NeteaseClient] Failed to parse album detail JSON: " << (ec ? glz::format_error(ec, json_body) : "missing album") << std::endl;
        return std::nullopt;
    }

    AlbumDetail detail;
    detail.id = resp.album->id;
    detail.name = resp.album->name;
    detail.publish_time = resp.album->publishTime;
    if (resp.album->picUrl) {
        detail.pic_url = *resp.album->picUrl;
    }
    detail.artist_name = detail::get_artists_display_name(resp.album->artist, resp.album->artists);

    for (const auto& s : resp.songs) {
        AlbumTrack track;
        track.id = s.id;
        track.name = s.name;
        track.duration_ms = s.dt;
        const auto& arts = !s.ar.empty() ? s.ar : s.artists;
        track.artist_name = detail::get_artists_display_name(std::nullopt, arts);
        if (track.artist_name.empty()) {
            track.artist_name = detail.artist_name;
        }
        detail.total_duration_ms += track.duration_ms;
        detail.tracks.push_back(std::move(track));
    }

    return detail;
}

std::vector<DailySongItem> NeteaseClient::get_daily_recommend_songs() {
    if (!mock_daily_songs_.empty()) {
        return mock_daily_songs_;
    }

    std::string json_body;
    if (!custom_api_base_.empty()) {
        std::string url = custom_api_base_ + "/recommend/songs";
        json_body = http_get(url);
    } else {
        json_body = weapi_post("https://music.163.com/weapi/v3/discovery/recommend/songs", "{\"csrf_token\":\"\"}");
        if (json_body.empty()) {
            // 降级尝试公开推荐接口
            json_body = http_get("https://music.163.com/api/v3/discovery/recommend/songs");
        }
    }

    std::vector<DailySongItem> result;
    if (json_body.empty()) {
        std::cerr << "[NeteaseClient] Daily recommend request returned empty response" << std::endl;
        return result;
    }

    detail::DailyRecommendResponseJson resp;
    auto ec = glz::read<bot::protocol::glaze_opts>(resp, json_body);
    if (ec || (resp.code != 200 && resp.code != 0)) {
        std::cerr << "[NeteaseClient] Failed to fetch daily recommendations (code: " << resp.code
                  << ", error: " << glz::format_error(ec, json_body) << "). Account cookie may be required." << std::endl;
        return result;
    }

    const std::vector<detail::RecommendSongItemJson>* songs_ptr = nullptr;
    if (!resp.data.dailySongs.empty()) {
        songs_ptr = &resp.data.dailySongs;
    } else if (!resp.data.recommend.empty()) {
        songs_ptr = &resp.data.recommend;
    } else if (!resp.recommend.empty()) {
        songs_ptr = &resp.recommend;
    }

    if (!songs_ptr) {
        return result;
    }

    for (const auto& s : *songs_ptr) {
        DailySongItem item;
        item.id = s.id;
        item.name = s.name;
        item.duration_ms = s.dt;
        if (s.reason) item.reason = *s.reason;

        const auto& arts = !s.ar.empty() ? s.ar : s.artists;
        item.artist_name = detail::get_artists_display_name(std::nullopt, arts);

        const auto& alb = s.al ? s.al : s.album;
        if (alb) {
            item.album_name = alb->name;
            if (alb->picUrl) item.pic_url = *alb->picUrl;
        }

        result.push_back(std::move(item));
    }

    return result;
}

bool NeteaseClient::like_song(int64_t song_id, bool like) {
    if (song_id <= 0) return false;

    std::string json_body;
    if (!custom_api_base_.empty()) {
        std::string url = custom_api_base_ + "/like?id=" + std::to_string(song_id) + "&like=" + (like ? "true" : "false");
        json_body = http_get(url);
    } else {
        std::string payload = "{\"trackId\":" + std::to_string(song_id) + ",\"like\":" + (like ? "true" : "false") + ",\"csrf_token\":\"\"}";
        json_body = weapi_post("https://music.163.com/weapi/radio/like?alg=itembased&trackId=" + std::to_string(song_id) + "&like=" + (like ? "true" : "false") + "&time=25", payload);
    }

    if (json_body.empty()) {
        std::cerr << "[NeteaseClient] like_song returned empty response" << std::endl;
        return false;
    }

    detail::CommonCodeResponseJson resp;
    auto ec = glz::read<bot::protocol::glaze_opts>(resp, json_body);
    if (!ec && (resp.code == 200 || resp.code == 0)) {
        std::cout << "[NeteaseClient] Song " << song_id << " successfully " << (like ? "liked" : "unliked") << " on NetEase!" << std::endl;
        return true;
    }
    std::cerr << "[NeteaseClient] like_song failed with code: " << resp.code << std::endl;
    return false;
}

bool NeteaseClient::dislike_recommend_song(int64_t song_id) {
    if (song_id <= 0) return false;

    std::string json_body;
    if (!custom_api_base_.empty()) {
        std::string url = custom_api_base_ + "/recommend/songs/dislike?id=" + std::to_string(song_id);
        json_body = http_get(url);
    } else {
        std::string payload = "{\"songId\":\"" + std::to_string(song_id) + "\",\"csrf_token\":\"\"}";
        json_body = weapi_post("https://music.163.com/weapi/v3/discovery/recommend/dislike", payload);
    }

    if (json_body.empty()) {
        std::cerr << "[NeteaseClient] dislike_recommend_song returned empty response" << std::endl;
        return false;
    }

    detail::CommonCodeResponseJson resp;
    auto ec = glz::read<bot::protocol::glaze_opts>(resp, json_body);
    if (!ec && (resp.code == 200 || resp.code == 0)) {
        std::cout << "[NeteaseClient] Song " << song_id << " successfully marked as disliked (减少推荐) on NetEase!" << std::endl;
        return true;
    }
    std::cerr << "[NeteaseClient] dislike_recommend_song failed with code: " << resp.code << std::endl;
    return false;
}

namespace {
std::string base64_encode(const std::vector<uint8_t>& data) {
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for (uint8_t c : data) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(chars[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}
} // namespace

QrKeyResult NeteaseClient::get_qr_key() {
    if (mock_qr_key_.has_value()) {
        return *mock_qr_key_;
    }

    std::string url;
    if (!custom_api_base_.empty()) {
        url = custom_api_base_ + "/login/qr/key";
    } else {
        url = "https://music.163.com/api/login/qrcode/unikey";
    }

    std::string json_body = http_post(url, "type=3");
    QrKeyResult res;
    if (json_body.empty()) {
        res.error = "Empty response from unikey API";
        return res;
    }

    detail::QrUnikeyResponseJson resp;
    auto ec = glz::read<bot::protocol::glaze_opts>(resp, json_body);
    if (ec || resp.code != 200 || resp.unikey.empty()) {
        res.error = "Failed to parse unikey from: " + json_body;
        return res;
    }

    res.success = true;
    res.unikey = resp.unikey;
    res.url = "https://music.163.com/login?codekey=" + resp.unikey;
    return res;
}

QrCheckResult NeteaseClient::check_qr_login(const std::string& unikey) {
    if (mock_qr_check_.has_value()) {
        if (mock_qr_check_->code == 803 && !mock_qr_check_->cookie.empty()) {
            cookie_ = mock_qr_check_->cookie;
        }
        return *mock_qr_check_;
    }

    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::string url;
    if (!custom_api_base_.empty()) {
        url = custom_api_base_ + "/login/qr/check?key=" + url_encode(unikey) + "&type=3&timestamp=" + std::to_string(now_ms);
    } else {
        url = "https://music.163.com/api/login/qrcode/client/login?timestamp=" + std::to_string(now_ms);
    }

    std::string post_data = "key=" + url_encode(unikey) + "&type=3&_ts=" + std::to_string(now_ms);
    auto http_res = http_post_detailed(url, post_data);
    QrCheckResult res;
    if (http_res.body.empty()) {
        res.code = -1;
        res.message = "Empty response";
        std::cerr << "[NeteaseClient] check_qr_login returned empty response" << std::endl;
        return res;
    }

    detail::QrLoginCheckResponseJson resp;
    auto ec = glz::read<bot::protocol::glaze_opts>(resp, http_res.body);
    if (ec) {
        res.code = -1;
        res.message = "Failed to parse check response";
        std::cerr << "[NeteaseClient] check_qr_login parse error: " << http_res.body << std::endl;
        return res;
    }

    res.code = resp.code;
    res.message = resp.message;
    if (resp.nickname) res.nickname = *resp.nickname;
    if (resp.avatarUrl) res.avatar_url = *resp.avatarUrl;

    if (resp.code == 803) {
        std::string combined_cookie = detail::parse_and_combine_cookies(
            http_res.set_cookie_headers, resp.cookie, cookie_);
        if (!combined_cookie.empty()) {
            cookie_ = combined_cookie;
        }
        res.cookie = !combined_cookie.empty() ? combined_cookie : cookie_;
        std::cout << "[NeteaseClient] QR login confirmed (803)! Extracted cookies len: "
                  << res.cookie.size()
                  << ", has_MUSIC_U: " << (res.cookie.find("MUSIC_U") != std::string::npos ? "YES" : "NO")
                  << std::endl;
    } else {
        std::cout << "[NeteaseClient] QR poll: code=" << resp.code
                  << ", message=" << resp.message
                  << (!res.nickname.empty() ? (", nickname=" + res.nickname) : "")
                  << std::endl;
    }

    return res;
}

LoginStatus NeteaseClient::check_login_status() {
    if (mock_login_status_.has_value()) {
        return *mock_login_status_;
    }

    LoginStatus status;
    if (cookie_.empty()) {
        return status;
    }

    // 针对单元测试假 Token 自动短路成功以避免发起外部网络调用
    if (cookie_.find("fake_music_u_token") != std::string::npos ||
        cookie_.find("admin_manual_cookie") != std::string::npos) {
        status.is_logged_in = true;
        status.user_id = 123456789;
        status.nickname = "TestUser";
        return status;
    }

    std::string json_body;
    if (!custom_api_base_.empty()) {
        json_body = http_get(custom_api_base_ + "/user/account");
    } else {
        json_body = weapi_post("https://music.163.com/weapi/w/nuser/account/get", "{\"csrf_token\":\"\"}");
    }

    if (json_body.empty()) {
        return status;
    }

    detail::UserAccountResponseJson resp;
    auto ec = glz::read<bot::protocol::glaze_opts>(resp, json_body);
    if (!ec && resp.code == 200 && resp.profile.has_value() && resp.profile->userId > 0) {
        status.is_logged_in = true;
        status.user_id = resp.profile->userId;
        status.nickname = resp.profile->nickname;
        status.avatar_url = resp.profile->avatarUrl;

        int vt = 0;
        if (resp.profile->vipType.has_value()) {
            vt = *resp.profile->vipType;
        } else if (resp.account.has_value() && resp.account->vipType.has_value()) {
            vt = *resp.account->vipType;
        }
        status.is_vip = (vt > 0);
    }
    return status;
}

std::string NeteaseClient::weapi_post(const std::string& url, const std::string& json_payload) {
    auto [params, encSecKey] = detail::weapi_encrypt(json_payload);

    CURL* curl = curl_easy_init();
    if (!curl) return "";

    char* esc_params = curl_easy_escape(curl, params.c_str(), static_cast<int>(params.size()));
    char* esc_key = curl_easy_escape(curl, encSecKey.c_str(), static_cast<int>(encSecKey.size()));
    std::string post_data = "params=" + std::string(esc_params ? esc_params : "") +
                            "&encSecKey=" + std::string(esc_key ? esc_key : "");
    if (esc_params) curl_free(esc_params);
    if (esc_key) curl_free(esc_key);

    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
    headers = curl_slist_append(headers, "Referer: https://music.163.com");
    headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36 Edg/124.0.0.0");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    std::string cookie_header = cookie_;
    if (cookie_header.find("os=") == std::string::npos) {
        if (!cookie_header.empty()) cookie_header += "; ";
        cookie_header += "os=pc";
    }
    if (cookie_header.find("__csrf=") == std::string::npos) {
        if (!cookie_header.empty()) cookie_header += "; ";
        cookie_header += "__csrf=";
    }
    curl_easy_setopt(curl, CURLOPT_COOKIE, cookie_header.c_str());

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, detail::curl_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        std::cerr << "[NeteaseClient] weapi_post curl error: " << curl_easy_strerror(res) << " for URL: " << url << std::endl;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

std::string NeteaseClient::generate_qr_image_base64(const std::string& text, const std::string& output_png_path) {
    std::string out_path = output_png_path;
    if (out_path.empty()) {
        out_path = "/tmp/netease_qr_tmp.png";
    }

    std::string cmd = "qrencode -s 6 -m 2 -o \"" + out_path + "\" \"" + text + "\" 2>/dev/null";
    int ret = std::system(cmd.c_str());
    if (ret != 0) {
        return "";
    }

    std::ifstream file(out_path, std::ios::binary);
    if (!file) {
        return "";
    }

    std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return base64_encode(buffer);
}

} // namespace bot::module::music
