/**
 * @file database.cpp
 * @brief SQLite 数据库持久化存储层实现
 * @details 负责执行数据表初始化与增量列迁移，管理订阅、去重、日推、表情回应、配置与管理员列表。
 */

#include "bot/modules/music/database.hpp"

#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace bot::module::music {

MusicDatabase::MusicDatabase(const std::string& db_path)
    : db_path_(db_path)
{}

MusicDatabase::~MusicDatabase() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool MusicDatabase::init() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (sqlite3_open(db_path_.c_str(), &db_) != SQLITE_OK) {
        std::cerr << "[MusicDatabase] Failed to open SQLite DB: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    const char* schema = R"(
        CREATE TABLE IF NOT EXISTS artist_subscriptions (
            group_id INTEGER NOT NULL,
            artist_id INTEGER NOT NULL,
            artist_name TEXT NOT NULL,
            platform TEXT DEFAULT 'netease',
            pic_url TEXT DEFAULT '',
            cover_updated_at INTEGER DEFAULT 0,
            created_at INTEGER NOT NULL,
            PRIMARY KEY (group_id, artist_id, platform)
        );

        CREATE TABLE IF NOT EXISTS pushed_songs (
            song_id INTEGER PRIMARY KEY,
            artist_id INTEGER NOT NULL,
            song_name TEXT NOT NULL,
            publish_time INTEGER DEFAULT 0,
            pushed_at INTEGER NOT NULL
        );

        CREATE TABLE IF NOT EXISTS pushed_albums (
            album_id INTEGER NOT NULL,
            artist_id INTEGER NOT NULL,
            album_name TEXT NOT NULL,
            artist_name TEXT DEFAULT '',
            platform TEXT DEFAULT 'netease',
            publish_time INTEGER DEFAULT 0,
            pushed_at INTEGER NOT NULL,
            PRIMARY KEY (album_id, platform)
        );

        CREATE INDEX IF NOT EXISTS idx_artist_subscriptions_group ON artist_subscriptions(group_id);
        CREATE INDEX IF NOT EXISTS idx_artist_subscriptions_artist ON artist_subscriptions(artist_id);
        CREATE INDEX IF NOT EXISTS idx_pushed_albums_artist ON pushed_albums(artist_id);
        CREATE INDEX IF NOT EXISTS idx_pushed_albums_name_artist ON pushed_albums(album_name, artist_name);
        CREATE INDEX IF NOT EXISTS idx_pushed_albums_pushed_at ON pushed_albums(pushed_at);

        CREATE TABLE IF NOT EXISTS bot_configs (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL,
            updated_at INTEGER NOT NULL
        );

        CREATE TABLE IF NOT EXISTS daily_music_subscriptions (
            group_id INTEGER PRIMARY KEY,
            enabled INTEGER NOT NULL DEFAULT 1,
            created_at INTEGER NOT NULL
        );

        CREATE TABLE IF NOT EXISTS daily_pushed_records (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            task_name TEXT NOT NULL,
            group_id INTEGER NOT NULL,
            message_id INTEGER NOT NULL,
            item_id INTEGER NOT NULL,
            item_name TEXT NOT NULL,
            artist_name TEXT NOT NULL,
            platform TEXT NOT NULL,
            pushed_date TEXT NOT NULL,
            timestamp INTEGER NOT NULL,
            evaluated INTEGER NOT NULL DEFAULT 0,
            upvotes INTEGER NOT NULL DEFAULT 0,
            downvotes INTEGER NOT NULL DEFAULT 0
        );

        CREATE INDEX IF NOT EXISTS idx_daily_pushed_msg ON daily_pushed_records(message_id);
        CREATE INDEX IF NOT EXISTS idx_daily_pushed_group_task ON daily_pushed_records(group_id, task_name);
        CREATE INDEX IF NOT EXISTS idx_daily_pushed_unevaluated ON daily_pushed_records(evaluated, task_name);

        CREATE TABLE IF NOT EXISTS daily_reaction_records (
            message_id INTEGER NOT NULL,
            user_id INTEGER NOT NULL,
            reaction_type INTEGER NOT NULL,
            created_at INTEGER NOT NULL,
            PRIMARY KEY (message_id, user_id)
        );

        CREATE INDEX IF NOT EXISTS idx_daily_reactions_msg ON daily_reaction_records(message_id);

        CREATE TABLE IF NOT EXISTS bot_admins (
            user_id INTEGER PRIMARY KEY,
            created_at INTEGER NOT NULL
        );

        CREATE TABLE IF NOT EXISTS daily_eval_records (
            group_id INTEGER NOT NULL,
            eval_date TEXT NOT NULL,
            created_at INTEGER NOT NULL,
            PRIMARY KEY (group_id, eval_date)
        );

        CREATE TABLE IF NOT EXISTS group_netease_accounts (
            group_id INTEGER PRIMARY KEY,
            source_group_id INTEGER NOT NULL,
            cookie TEXT NOT NULL DEFAULT '',
            nickname TEXT DEFAULT '',
            account_uid INTEGER DEFAULT 0,
            is_vip INTEGER DEFAULT 0,
            recommend_slot INTEGER DEFAULT 0,
            updated_at INTEGER NOT NULL
        );
    )";

    char* err_msg = nullptr;
    if (sqlite3_exec(db_, schema, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "[MusicDatabase] Schema init error: " << (err_msg ? err_msg : "unknown") << std::endl;
        sqlite3_free(err_msg);
        return false;
    }

    // 增量迁移：为 group_netease_accounts 添加 recommend_slot 字段
    sqlite3_exec(db_, "ALTER TABLE group_netease_accounts ADD COLUMN recommend_slot INTEGER DEFAULT 0;", nullptr, nullptr, nullptr);

    // 针对现在已经登录的网易云账号，把它变成 1022206916 这个群的网易云音乐账号
    {
        bool has_1022206916 = false;
        sqlite3_stmt* acc_check_stmt = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT 1 FROM group_netease_accounts WHERE group_id = 1022206916;", -1, &acc_check_stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(acc_check_stmt) == SQLITE_ROW) {
                has_1022206916 = true;
            }
            sqlite3_finalize(acc_check_stmt);
        }

        if (!has_1022206916) {
            std::string cookie;
            sqlite3_stmt* cfg_stmt = nullptr;
            if (sqlite3_prepare_v2(db_, "SELECT value FROM bot_configs WHERE key = 'netease_cookie';", -1, &cfg_stmt, nullptr) == SQLITE_OK) {
                if (sqlite3_step(cfg_stmt) == SQLITE_ROW) {
                    const char* val = reinterpret_cast<const char*>(sqlite3_column_text(cfg_stmt, 0));
                    if (val) cookie = val;
                }
                sqlite3_finalize(cfg_stmt);
            }

            if (!cookie.empty() && cookie.find("MUSIC_U") != std::string::npos) {
                int64_t now_ts = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                sqlite3_stmt* ins_stmt = nullptr;
                const char* ins_sql = "INSERT INTO group_netease_accounts (group_id, source_group_id, cookie, updated_at) VALUES (1022206916, 1022206916, ?, ?);";
                if (sqlite3_prepare_v2(db_, ins_sql, -1, &ins_stmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(ins_stmt, 1, cookie.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(ins_stmt, 2, now_ts);
                    sqlite3_step(ins_stmt);
                    sqlite3_finalize(ins_stmt);
                    std::cout << "[MusicDatabase] Auto-migrated global netease_cookie to group 1022206916." << std::endl;
                }
            }
        }
    }

    // 确保代码中的硬性管理员写入数据库
    for (int64_t hardcoded_id : get_hardcoded_admins()) {
        std::string ins_sql = "INSERT OR IGNORE INTO bot_admins (user_id, created_at) VALUES (" +
                              std::to_string(hardcoded_id) + ", " +
                              std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::system_clock::now().time_since_epoch()).count()) + ");";
        sqlite3_exec(db_, ins_sql.c_str(), nullptr, nullptr, nullptr);
    }

    // 尝试为旧表热升级增加 platform, pic_url, cover_updated_at 字段 (若已存在则忽略错误)
    sqlite3_exec(db_, "ALTER TABLE artist_subscriptions ADD COLUMN platform TEXT DEFAULT 'netease';", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE artist_subscriptions ADD COLUMN pic_url TEXT DEFAULT '';", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE artist_subscriptions ADD COLUMN cover_updated_at INTEGER DEFAULT 0;", nullptr, nullptr, nullptr);

    // 检查旧表主键是否包含 platform，若未包含则迁移至联合主键 (group_id, artist_id, platform)
    int pk_count = 0;
    bool platform_in_pk = false;
    sqlite3_stmt* check_stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "PRAGMA table_info(artist_subscriptions);", -1, &check_stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(check_stmt) == SQLITE_ROW) {
            int is_pk = sqlite3_column_int(check_stmt, 5);
            const char* col_name = reinterpret_cast<const char*>(sqlite3_column_text(check_stmt, 1));
            if (is_pk > 0) {
                pk_count++;
                if (col_name && std::string(col_name) == "platform") {
                    platform_in_pk = true;
                }
            }
        }
        sqlite3_finalize(check_stmt);
    }

    if (pk_count > 0 && !platform_in_pk) {
        const char* migration = R"(
            BEGIN TRANSACTION;
            CREATE TABLE artist_subscriptions_new (
                group_id INTEGER NOT NULL,
                artist_id INTEGER NOT NULL,
                artist_name TEXT NOT NULL,
                platform TEXT DEFAULT 'netease',
                pic_url TEXT DEFAULT '',
                cover_updated_at INTEGER DEFAULT 0,
                created_at INTEGER NOT NULL,
                PRIMARY KEY (group_id, artist_id, platform)
            );
            INSERT OR IGNORE INTO artist_subscriptions_new
                SELECT group_id, artist_id, artist_name, COALESCE(platform, 'netease'), COALESCE(pic_url, ''), COALESCE(cover_updated_at, 0), created_at
                FROM artist_subscriptions;
            DROP TABLE artist_subscriptions;
            ALTER TABLE artist_subscriptions_new RENAME TO artist_subscriptions;
            CREATE INDEX IF NOT EXISTS idx_artist_subscriptions_group ON artist_subscriptions(group_id);
            CREATE INDEX IF NOT EXISTS idx_artist_subscriptions_artist ON artist_subscriptions(artist_id);
            COMMIT;
        )";
        sqlite3_exec(db_, migration, nullptr, nullptr, nullptr);
    }

    // 尝试为旧表热升级增加 platform 与 artist_name 字段
    sqlite3_exec(db_, "ALTER TABLE pushed_albums ADD COLUMN platform TEXT DEFAULT 'netease';", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE pushed_albums ADD COLUMN artist_name TEXT DEFAULT '';", nullptr, nullptr, nullptr);

    // 检查旧表主键是否包含 platform，若未包含则迁移至联合主键 (album_id, platform)
    int album_pk_count = 0;
    bool album_platform_in_pk = false;
    sqlite3_stmt* check_album_stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "PRAGMA table_info(pushed_albums);", -1, &check_album_stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(check_album_stmt) == SQLITE_ROW) {
            int is_pk = sqlite3_column_int(check_album_stmt, 5);
            const char* col_name = reinterpret_cast<const char*>(sqlite3_column_text(check_album_stmt, 1));
            if (is_pk > 0) {
                album_pk_count++;
                if (col_name && std::string(col_name) == "platform") {
                    album_platform_in_pk = true;
                }
            }
        }
        sqlite3_finalize(check_album_stmt);
    }

    if (album_pk_count > 0 && !album_platform_in_pk) {
        const char* migration = R"(
            BEGIN TRANSACTION;
            CREATE TABLE pushed_albums_new (
                album_id INTEGER NOT NULL,
                artist_id INTEGER NOT NULL,
                album_name TEXT NOT NULL,
                artist_name TEXT DEFAULT '',
                platform TEXT DEFAULT 'netease',
                publish_time INTEGER DEFAULT 0,
                pushed_at INTEGER NOT NULL,
                PRIMARY KEY (album_id, platform)
            );
            INSERT OR IGNORE INTO pushed_albums_new
                SELECT album_id, artist_id, album_name, COALESCE(artist_name, ''), COALESCE(platform, 'netease'), publish_time, pushed_at
                FROM pushed_albums;
            DROP TABLE pushed_albums;
            ALTER TABLE pushed_albums_new RENAME TO pushed_albums;
            CREATE INDEX IF NOT EXISTS idx_pushed_albums_artist ON pushed_albums(artist_id);
            CREATE INDEX IF NOT EXISTS idx_pushed_albums_name_artist ON pushed_albums(album_name, artist_name);
            CREATE INDEX IF NOT EXISTS idx_pushed_albums_pushed_at ON pushed_albums(pushed_at);
            COMMIT;
        )";
        sqlite3_exec(db_, migration, nullptr, nullptr, nullptr);
    }

    std::cout << "[MusicDatabase] SQLite database initialized successfully at " << db_path_ << std::endl;
    return true;
}

bool MusicDatabase::add_subscription(int64_t group_id, int64_t artist_id, const std::string& artist_name, const std::string& platform, const std::string& pic_url) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = R"(
        INSERT INTO artist_subscriptions (group_id, artist_id, artist_name, platform, pic_url, cover_updated_at, created_at)
        VALUES (?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(group_id, artist_id, platform) DO UPDATE SET
            artist_name = excluded.artist_name,
            pic_url = excluded.pic_url,
            cover_updated_at = excluded.cover_updated_at;
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    sqlite3_bind_int64(stmt, 1, group_id);
    sqlite3_bind_int64(stmt, 2, artist_id);
    sqlite3_bind_text(stmt, 3, artist_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, platform.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, pic_url.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 6, now);
    sqlite3_bind_int64(stmt, 7, now);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

bool MusicDatabase::update_artist_pic_url(int64_t artist_id, const std::string& pic_url, const std::string& platform, int64_t updated_at) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    if (updated_at <= 0) {
        updated_at = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }

    const char* sql = platform.empty()
        ? "UPDATE artist_subscriptions SET pic_url = ?, cover_updated_at = ? WHERE artist_id = ?;"
        : "UPDATE artist_subscriptions SET pic_url = ?, cover_updated_at = ? WHERE artist_id = ? AND platform = ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, pic_url.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, updated_at);
    sqlite3_bind_int64(stmt, 3, artist_id);
    if (!platform.empty()) {
        sqlite3_bind_text(stmt, 4, platform.c_str(), -1, SQLITE_STATIC);
    }

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return ok && (changes > 0);
}

bool MusicDatabase::remove_subscription(int64_t group_id, const std::string& artist_name, const std::string& platform) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = platform.empty()
        ? "DELETE FROM artist_subscriptions WHERE group_id = ? AND artist_name = ?;"
        : "DELETE FROM artist_subscriptions WHERE group_id = ? AND artist_name = ? AND platform = ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(stmt, 1, group_id);
    sqlite3_bind_text(stmt, 2, artist_name.c_str(), -1, SQLITE_STATIC);
    if (!platform.empty()) {
        sqlite3_bind_text(stmt, 3, platform.c_str(), -1, SQLITE_STATIC);
    }

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return ok && (changes > 0);
}

bool MusicDatabase::remove_subscription_by_id(int64_t group_id, int64_t artist_id, const std::string& platform) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = platform.empty()
        ? "DELETE FROM artist_subscriptions WHERE group_id = ? AND artist_id = ?;"
        : "DELETE FROM artist_subscriptions WHERE group_id = ? AND artist_id = ? AND platform = ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(stmt, 1, group_id);
    sqlite3_bind_int64(stmt, 2, artist_id);
    if (!platform.empty()) {
        sqlite3_bind_text(stmt, 3, platform.c_str(), -1, SQLITE_STATIC);
    }

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return ok && (changes > 0);
}

std::vector<ArtistSubscription> MusicDatabase::find_subscriptions(int64_t group_id, const std::string& artist_name, const std::string& platform) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<ArtistSubscription> result;
    if (!db_) return result;

    const char* sql = platform.empty()
        ? "SELECT group_id, artist_id, artist_name, COALESCE(platform, 'netease'), created_at, COALESCE(pic_url, ''), COALESCE(cover_updated_at, 0) FROM artist_subscriptions WHERE group_id = ? AND artist_name = ? ORDER BY created_at ASC;"
        : "SELECT group_id, artist_id, artist_name, COALESCE(platform, 'netease'), created_at, COALESCE(pic_url, ''), COALESCE(cover_updated_at, 0) FROM artist_subscriptions WHERE group_id = ? AND artist_name = ? AND platform = ? ORDER BY created_at ASC;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return result;
    }

    sqlite3_bind_int64(stmt, 1, group_id);
    sqlite3_bind_text(stmt, 2, artist_name.c_str(), -1, SQLITE_STATIC);
    if (!platform.empty()) {
        sqlite3_bind_text(stmt, 3, platform.c_str(), -1, SQLITE_STATIC);
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ArtistSubscription sub;
        sub.group_id = sqlite3_column_int64(stmt, 0);
        sub.artist_id = sqlite3_column_int64(stmt, 1);
        const char* name_txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        sub.artist_name = name_txt ? name_txt : "";
        const char* plat_txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        sub.platform = plat_txt ? plat_txt : "netease";
        sub.created_at = sqlite3_column_int64(stmt, 4);
        const char* pic_txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        sub.pic_url = pic_txt ? pic_txt : "";
        sub.cover_updated_at = sqlite3_column_int64(stmt, 6);
        result.push_back(std::move(sub));
    }

    sqlite3_finalize(stmt);
    return result;
}

std::vector<ArtistSubscription> MusicDatabase::get_group_subscriptions(int64_t group_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<ArtistSubscription> result;
    if (!db_) return result;

    const char* sql = "SELECT group_id, artist_id, artist_name, COALESCE(platform, 'netease'), created_at, COALESCE(pic_url, ''), COALESCE(cover_updated_at, 0) FROM artist_subscriptions WHERE group_id = ? ORDER BY created_at ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return result;
    }

    sqlite3_bind_int64(stmt, 1, group_id);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ArtistSubscription sub;
        sub.group_id = sqlite3_column_int64(stmt, 0);
        sub.artist_id = sqlite3_column_int64(stmt, 1);
        const char* name_txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        sub.artist_name = name_txt ? name_txt : "";
        const char* plat_txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        sub.platform = plat_txt ? plat_txt : "netease";
        sub.created_at = sqlite3_column_int64(stmt, 4);
        const char* pic_txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        sub.pic_url = pic_txt ? pic_txt : "";
        sub.cover_updated_at = sqlite3_column_int64(stmt, 6);
        result.push_back(std::move(sub));
    }

    sqlite3_finalize(stmt);
    return result;
}

std::vector<ArtistSubscription> MusicDatabase::get_all_unique_artists() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<ArtistSubscription> result;
    if (!db_) return result;

    const char* sql = "SELECT DISTINCT artist_id, artist_name, COALESCE(platform, 'netease'), COALESCE(pic_url, ''), COALESCE(cover_updated_at, 0) FROM artist_subscriptions;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return result;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ArtistSubscription sub;
        sub.artist_id = sqlite3_column_int64(stmt, 0);
        const char* name_txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        sub.artist_name = name_txt ? name_txt : "";
        const char* plat_txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        sub.platform = plat_txt ? plat_txt : "netease";
        const char* pic_txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        sub.pic_url = pic_txt ? pic_txt : "";
        sub.cover_updated_at = sqlite3_column_int64(stmt, 4);
        result.push_back(std::move(sub));
    }

    sqlite3_finalize(stmt);
    return result;
}

std::vector<int64_t> MusicDatabase::get_subscribers_for_artist(int64_t artist_id, const std::string& platform) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<int64_t> result;
    if (!db_) return result;

    const char* sql = platform.empty()
        ? "SELECT group_id FROM artist_subscriptions WHERE artist_id = ?;"
        : "SELECT group_id FROM artist_subscriptions WHERE artist_id = ? AND platform = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return result;
    }

    sqlite3_bind_int64(stmt, 1, artist_id);
    if (!platform.empty()) {
        sqlite3_bind_text(stmt, 2, platform.c_str(), -1, SQLITE_STATIC);
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        result.push_back(sqlite3_column_int64(stmt, 0));
    }

    sqlite3_finalize(stmt);
    return result;
}

std::vector<int64_t> MusicDatabase::get_subscribers_for_artist_name(const std::string& artist_name) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<int64_t> result;
    if (!db_) return result;

    const char* sql = "SELECT DISTINCT group_id FROM artist_subscriptions WHERE LOWER(TRIM(artist_name)) = LOWER(TRIM(?));";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return result;
    }

    sqlite3_bind_text(stmt, 1, artist_name.c_str(), -1, SQLITE_STATIC);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        result.push_back(sqlite3_column_int64(stmt, 0));
    }

    sqlite3_finalize(stmt);
    return result;
}

bool MusicDatabase::is_subscribed(int64_t group_id, int64_t artist_id, const std::string& platform) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = platform.empty()
        ? "SELECT 1 FROM artist_subscriptions WHERE group_id = ? AND artist_id = ? LIMIT 1;"
        : "SELECT 1 FROM artist_subscriptions WHERE group_id = ? AND artist_id = ? AND COALESCE(platform, 'netease') = ? LIMIT 1;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(stmt, 1, group_id);
    sqlite3_bind_int64(stmt, 2, artist_id);
    if (!platform.empty()) {
        sqlite3_bind_text(stmt, 3, platform.c_str(), -1, SQLITE_STATIC);
    }

    bool found = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return found;
}

bool MusicDatabase::is_song_pushed(int64_t song_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = "SELECT 1 FROM pushed_songs WHERE song_id = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(stmt, 1, song_id);
    bool exists = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return exists;
}

bool MusicDatabase::record_pushed_song(int64_t song_id, int64_t artist_id, const std::string& song_name, int64_t publish_time) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = R"(
        INSERT OR IGNORE INTO pushed_songs (song_id, artist_id, song_name, publish_time, pushed_at)
        VALUES (?, ?, ?, ?, ?);
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    sqlite3_bind_int64(stmt, 1, song_id);
    sqlite3_bind_int64(stmt, 2, artist_id);
    sqlite3_bind_text(stmt, 3, song_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 4, publish_time);
    sqlite3_bind_int64(stmt, 5, now);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

bool MusicDatabase::is_album_pushed(int64_t album_id, const std::string& platform) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = platform.empty()
        ? "SELECT 1 FROM pushed_albums WHERE album_id = ? LIMIT 1;"
        : "SELECT 1 FROM pushed_albums WHERE album_id = ? AND platform = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(stmt, 1, album_id);
    if (!platform.empty()) {
        sqlite3_bind_text(stmt, 2, platform.c_str(), -1, SQLITE_STATIC);
    }
    bool exists = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return exists;
}

bool MusicDatabase::record_pushed_album(int64_t album_id, int64_t artist_id, const std::string& album_name,
                                        int64_t publish_time, const std::string& platform, const std::string& artist_name) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = R"(
        INSERT OR IGNORE INTO pushed_albums (album_id, artist_id, album_name, artist_name, platform, publish_time, pushed_at)
        VALUES (?, ?, ?, ?, ?, ?, ?);
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    std::string plat = platform.empty() ? "netease" : platform;

    sqlite3_bind_int64(stmt, 1, album_id);
    sqlite3_bind_int64(stmt, 2, artist_id);
    sqlite3_bind_text(stmt, 3, album_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, artist_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, plat.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 6, publish_time);
    sqlite3_bind_int64(stmt, 7, now);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

bool MusicDatabase::is_album_recently_pushed_cross_platform(const std::string& artist_name,
                                                             const std::string& album_name,
                                                             int64_t within_seconds,
                                                             const std::string& current_platform) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = R"(
        SELECT 1 FROM pushed_albums
        WHERE LOWER(TRIM(album_name)) = LOWER(TRIM(?))
          AND (
              LOWER(TRIM(artist_name)) = LOWER(TRIM(?))
              OR (artist_name = '' AND EXISTS (
                  SELECT 1 FROM artist_subscriptions 
                  WHERE artist_subscriptions.artist_id = pushed_albums.artist_id 
                    AND LOWER(TRIM(artist_subscriptions.artist_name)) = LOWER(TRIM(?))
              ))
          )
          AND (? = '' OR platform != ?)
          AND (? - pushed_at) <= ?
        LIMIT 1;
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    sqlite3_bind_text(stmt, 1, album_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, artist_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, artist_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, current_platform.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, current_platform.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 6, now);
    sqlite3_bind_int64(stmt, 7, within_seconds);

    bool exists = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return exists;
}

bool MusicDatabase::has_artist_pushed_albums(int64_t artist_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = "SELECT 1 FROM pushed_albums WHERE artist_id = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(stmt, 1, artist_id);
    bool exists = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return exists;
}

bool MusicDatabase::set_config(const std::string& key, const std::string& value) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = "INSERT OR REPLACE INTO bot_configs (key, value, updated_at) VALUES (?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, now);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

std::string MusicDatabase::get_config(const std::string& key, const std::string& default_val) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return default_val;

    const char* sql = "SELECT value FROM bot_configs WHERE key = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return default_val;
    }

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_STATIC);

    std::string result = default_val;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (val) {
            result = val;
        }
    }

    sqlite3_finalize(stmt);
    return result;
}

bool MusicDatabase::set_daily_music_enabled(int64_t group_id, bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = "INSERT OR REPLACE INTO daily_music_subscriptions (group_id, enabled, created_at) VALUES (?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    sqlite3_bind_int64(stmt, 1, group_id);
    sqlite3_bind_int(stmt, 2, enabled ? 1 : 0);
    sqlite3_bind_int64(stmt, 3, now);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

bool MusicDatabase::is_daily_music_enabled(int64_t group_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = "SELECT enabled FROM daily_music_subscriptions WHERE group_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(stmt, 1, group_id);

    bool enabled = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        enabled = (sqlite3_column_int(stmt, 0) == 1);
    }

    sqlite3_finalize(stmt);
    return enabled;
}

std::vector<int64_t> MusicDatabase::get_daily_music_groups() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<int64_t> groups;
    if (!db_) return groups;

    const char* sql = "SELECT group_id FROM daily_music_subscriptions WHERE enabled = 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return groups;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        groups.push_back(sqlite3_column_int64(stmt, 0));
    }

    sqlite3_finalize(stmt);
    return groups;
}

int64_t MusicDatabase::record_daily_push(const std::string& task_name, int64_t group_id, int64_t message_id,
                                        int64_t item_id, const std::string& item_name, const std::string& artist_name,
                                        const std::string& platform, const std::string& date, int64_t timestamp) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return -1;

    const char* sql = R"(
        INSERT INTO daily_pushed_records
        (task_name, group_id, message_id, item_id, item_name, artist_name, platform, pushed_date, timestamp, evaluated, upvotes, downvotes)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 0, 0, 0);
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return -1;
    }

    int64_t ts = timestamp;
    if (ts == 0) {
        ts = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    sqlite3_bind_text(stmt, 1, task_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, group_id);
    sqlite3_bind_int64(stmt, 3, message_id);
    sqlite3_bind_int64(stmt, 4, item_id);
    sqlite3_bind_text(stmt, 5, item_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, artist_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 7, platform.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 8, date.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 9, ts);

    int64_t row_id = -1;
    if (sqlite3_step(stmt) == SQLITE_DONE) {
        row_id = sqlite3_last_insert_rowid(db_);
    }

    sqlite3_finalize(stmt);
    return row_id;
}

namespace {
DailyPushedRecord parse_daily_pushed_row(sqlite3_stmt* stmt) {
    DailyPushedRecord rec;
    rec.id = sqlite3_column_int64(stmt, 0);
    const char* t1 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    rec.task_name = t1 ? t1 : "";
    rec.group_id = sqlite3_column_int64(stmt, 2);
    rec.message_id = sqlite3_column_int64(stmt, 3);
    rec.item_id = sqlite3_column_int64(stmt, 4);
    const char* t5 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
    rec.item_name = t5 ? t5 : "";
    const char* t6 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
    rec.artist_name = t6 ? t6 : "";
    const char* t7 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
    rec.platform = t7 ? t7 : "";
    const char* t8 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
    rec.pushed_date = t8 ? t8 : "";
    rec.pushed_at = sqlite3_column_int64(stmt, 9);
    rec.evaluated = sqlite3_column_int(stmt, 10);
    rec.upvotes = sqlite3_column_int(stmt, 11);
    rec.downvotes = sqlite3_column_int(stmt, 12);
    return rec;
}
} // namespace

std::optional<DailyPushedRecord> MusicDatabase::get_daily_push_by_message_id(int64_t message_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return std::nullopt;

    const char* sql = R"(
        SELECT id, task_name, group_id, message_id, item_id, item_name, artist_name, platform, pushed_date, timestamp, evaluated, upvotes, downvotes
        FROM daily_pushed_records WHERE message_id = ? LIMIT 1;
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(stmt, 1, message_id);

    std::optional<DailyPushedRecord> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result = parse_daily_pushed_row(stmt);
    }

    sqlite3_finalize(stmt);
    return result;
}

std::optional<DailyPushedRecord> MusicDatabase::get_latest_daily_push(int64_t group_id, const std::string& task_name) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return std::nullopt;

    sqlite3_stmt* stmt = nullptr;
    if (task_name.empty()) {
        const char* sql = R"(
            SELECT id, task_name, group_id, message_id, item_id, item_name, artist_name, platform, pushed_date, timestamp, evaluated, upvotes, downvotes
            FROM daily_pushed_records WHERE group_id = ? ORDER BY timestamp DESC, id DESC LIMIT 1;
        )";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return std::nullopt;
        }
        sqlite3_bind_int64(stmt, 1, group_id);
    } else {
        const char* sql = R"(
            SELECT id, task_name, group_id, message_id, item_id, item_name, artist_name, platform, pushed_date, timestamp, evaluated, upvotes, downvotes
            FROM daily_pushed_records WHERE group_id = ? AND task_name = ? ORDER BY timestamp DESC, id DESC LIMIT 1;
        )";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return std::nullopt;
        }
        sqlite3_bind_int64(stmt, 1, group_id);
        sqlite3_bind_text(stmt, 2, task_name.c_str(), -1, SQLITE_STATIC);
    }

    std::optional<DailyPushedRecord> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result = parse_daily_pushed_row(stmt);
    }

    sqlite3_finalize(stmt);
    return result;
}

std::optional<DailyPushedRecord> MusicDatabase::get_today_daily_push(int64_t group_id, const std::string& date) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return std::nullopt;

    std::string d = date;
    if (d.empty()) {
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
        d = oss.str();
    }

    const char* sql = R"(
        SELECT id, task_name, group_id, message_id, item_id, item_name, artist_name, platform, pushed_date, timestamp, evaluated, upvotes, downvotes
        FROM daily_pushed_records WHERE group_id = ? AND pushed_date = ? ORDER BY id ASC LIMIT 1;
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(stmt, 1, group_id);
    sqlite3_bind_text(stmt, 2, d.c_str(), -1, SQLITE_STATIC);

    std::optional<DailyPushedRecord> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result = parse_daily_pushed_row(stmt);
    }

    sqlite3_finalize(stmt);
    return result;
}

std::vector<DailyPushedRecord> MusicDatabase::get_unevaluated_daily_pushes(const std::string& task_name) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<DailyPushedRecord> results;
    if (!db_) return results;

    sqlite3_stmt* stmt = nullptr;
    if (task_name.empty()) {
        const char* sql = R"(
            SELECT id, task_name, group_id, message_id, item_id, item_name, artist_name, platform, pushed_date, timestamp, evaluated, upvotes, downvotes
            FROM daily_pushed_records WHERE evaluated = 0 ORDER BY timestamp ASC;
        )";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return results;
        }
    } else {
        const char* sql = R"(
            SELECT id, task_name, group_id, message_id, item_id, item_name, artist_name, platform, pushed_date, timestamp, evaluated, upvotes, downvotes
            FROM daily_pushed_records WHERE evaluated = 0 AND task_name = ? ORDER BY timestamp ASC;
        )";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return results;
        }
        sqlite3_bind_text(stmt, 1, task_name.c_str(), -1, SQLITE_STATIC);
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.push_back(parse_daily_pushed_row(stmt));
    }

    sqlite3_finalize(stmt);
    return results;
}

bool MusicDatabase::mark_daily_push_evaluated(int64_t record_id, int upvotes, int downvotes) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = "UPDATE daily_pushed_records SET evaluated = 1, upvotes = ?, downvotes = ? WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int(stmt, 1, upvotes);
    sqlite3_bind_int(stmt, 2, downvotes);
    sqlite3_bind_int64(stmt, 3, record_id);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

bool MusicDatabase::record_daily_reaction(int64_t message_id, int64_t user_id, int reaction_type) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = "INSERT OR REPLACE INTO daily_reaction_records (message_id, user_id, reaction_type, created_at) VALUES (?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    sqlite3_bind_int64(stmt, 1, message_id);
    sqlite3_bind_int64(stmt, 2, user_id);
    sqlite3_bind_int(stmt, 3, reaction_type);
    sqlite3_bind_int64(stmt, 4, now);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

bool MusicDatabase::remove_daily_reaction(int64_t message_id, int64_t user_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = "DELETE FROM daily_reaction_records WHERE message_id = ? AND user_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(stmt, 1, message_id);
    sqlite3_bind_int64(stmt, 2, user_id);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

bool MusicDatabase::clear_daily_reactions(int64_t message_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    sqlite3_stmt* stmt = nullptr;
    if (message_id == 0) {
        const char* sql = "DELETE FROM daily_reaction_records;";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    } else {
        const char* sql = "DELETE FROM daily_reaction_records WHERE message_id = ?;";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_int64(stmt, 1, message_id);
    }

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

bool MusicDatabase::is_daily_evaluated(int64_t group_id, const std::string& date) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    std::string d = date;
    if (d.empty()) {
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
        d = oss.str();
    }

    const char* sql = "SELECT 1 FROM daily_eval_records WHERE group_id = ? AND eval_date = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, group_id);
        sqlite3_bind_text(stmt, 2, d.c_str(), -1, SQLITE_STATIC);
        bool exists = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);
        if (exists) return true;
    }

    const char* sql_push = "SELECT 1 FROM daily_pushed_records WHERE group_id = ? AND pushed_date = ? AND evaluated = 1 LIMIT 1;";
    if (sqlite3_prepare_v2(db_, sql_push, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, group_id);
        sqlite3_bind_text(stmt, 2, d.c_str(), -1, SQLITE_STATIC);
        bool exists = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);
        if (exists) return true;
    }

    return false;
}

bool MusicDatabase::set_daily_evaluated(int64_t group_id, const std::string& date) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    std::string d = date;
    if (d.empty()) {
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
        d = oss.str();
    }

    const char* sql = "INSERT OR REPLACE INTO daily_eval_records (group_id, eval_date, created_at) VALUES (?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    sqlite3_bind_int64(stmt, 1, group_id);
    sqlite3_bind_text(stmt, 2, d.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, now);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

bool MusicDatabase::clear_daily_push_records(int64_t group_id, const std::string& date) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    std::string sql = "DELETE FROM daily_pushed_records WHERE 1=1";
    if (group_id != 0) sql += " AND group_id = " + std::to_string(group_id);
    if (!date.empty()) sql += " AND pushed_date = '" + date + "'";
    sql += ";";

    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        if (err_msg) sqlite3_free(err_msg);
        return false;
    }
    return true;
}

bool MusicDatabase::clear_daily_evaluation(int64_t group_id, const std::string& date) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    std::string sql = "DELETE FROM daily_eval_records WHERE 1=1";
    if (group_id != 0) sql += " AND group_id = " + std::to_string(group_id);
    if (!date.empty()) sql += " AND eval_date = '" + date + "'";
    sql += ";";

    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        if (err_msg) sqlite3_free(err_msg);
        return false;
    }
    return true;
}

bool MusicDatabase::reset_daily_push_status(int64_t group_id, const std::string& date) {
    std::string d = date;
    if (d.empty()) {
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
        d = oss.str();
    }

    // 1. 获取该群当日推送的所有消息 ID，并清空对应的表情投票记录
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (db_) {
            const char* sql = "SELECT message_id FROM daily_pushed_records WHERE group_id = ? AND pushed_date = ?;";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(stmt, 1, group_id);
                sqlite3_bind_text(stmt, 2, d.c_str(), -1, SQLITE_STATIC);
                std::vector<int64_t> msg_ids;
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    msg_ids.push_back(sqlite3_column_int64(stmt, 0));
                }
                sqlite3_finalize(stmt);

                for (int64_t mid : msg_ids) {
                    const char* del_sql = "DELETE FROM daily_reaction_records WHERE message_id = ?;";
                    sqlite3_stmt* del_stmt = nullptr;
                    if (sqlite3_prepare_v2(db_, del_sql, -1, &del_stmt, nullptr) == SQLITE_OK) {
                        sqlite3_bind_int64(del_stmt, 1, mid);
                        sqlite3_step(del_stmt);
                        sqlite3_finalize(del_stmt);
                    }
                }
            }
        }
    }

    // 2. 清除当日推送记录
    clear_daily_push_records(group_id, d);

    // 3. 清除当日结算标记
    clear_daily_evaluation(group_id, d);

    return true;
}

DailyMusicTimeConfig MusicDatabase::get_daily_music_time_config() {
    DailyMusicTimeConfig cfg;
    cfg.push_hour = 9;
    cfg.push_minute = 0;
    cfg.eval_hour = 23;
    cfg.eval_minute = 40;
    cfg.update_hour = 6;
    cfg.update_minute = 0;

    auto push_str = get_config("daily_music_push_time");
    if (!push_str.empty()) {
        int h = 0, m = 0;
        if (std::sscanf(push_str.c_str(), "%d:%d", &h, &m) == 2 && h >= 0 && h <= 23 && m >= 0 && m <= 59) {
            cfg.push_hour = h;
            cfg.push_minute = m;
        }
    }

    auto eval_str = get_config("daily_music_eval_time");
    if (!eval_str.empty()) {
        int h = 0, m = 0;
        if (std::sscanf(eval_str.c_str(), "%d:%d", &h, &m) == 2 && h >= 0 && h <= 23 && m >= 0 && m <= 59) {
            cfg.eval_hour = h;
            cfg.eval_minute = m;
        }
    }

    auto update_str = get_config("daily_music_update_time");
    if (!update_str.empty()) {
        int h = 0, m = 0;
        if (std::sscanf(update_str.c_str(), "%d:%d", &h, &m) == 2 && h >= 0 && h <= 23 && m >= 0 && m <= 59) {
            cfg.update_hour = h;
            cfg.update_minute = m;
        }
    }

    return cfg;
}

bool MusicDatabase::set_daily_push_time(int hour, int minute) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", hour, minute);
    return set_config("daily_music_push_time", buf);
}

bool MusicDatabase::set_daily_eval_time(int hour, int minute) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", hour, minute);
    return set_config("daily_music_eval_time", buf);
}

bool MusicDatabase::set_daily_update_time(int hour, int minute) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", hour, minute);
    return set_config("daily_music_update_time", buf);
}

std::pair<int, int> MusicDatabase::get_daily_push_votes(int64_t message_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return {0, 0};

    // 若该条消息已结算，直接从 daily_pushed_records 获取保存的结算投票数据
    const char* sql_rec = "SELECT evaluated, upvotes, downvotes FROM daily_pushed_records WHERE message_id = ? LIMIT 1;";
    sqlite3_stmt* stmt_rec = nullptr;
    if (sqlite3_prepare_v2(db_, sql_rec, -1, &stmt_rec, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt_rec, 1, message_id);
        if (sqlite3_step(stmt_rec) == SQLITE_ROW) {
            int eval = sqlite3_column_int(stmt_rec, 0);
            if (eval == 1) {
                int up = sqlite3_column_int(stmt_rec, 1);
                int down = sqlite3_column_int(stmt_rec, 2);
                sqlite3_finalize(stmt_rec);
                return {up, down};
            }
        }
        sqlite3_finalize(stmt_rec);
    }

    // 尚未结算，从 daily_reaction_records 统计实时票数
    const char* sql = R"(
        SELECT 
            COALESCE(SUM(CASE WHEN reaction_type = 1 THEN 1 ELSE 0 END), 0),
            COALESCE(SUM(CASE WHEN reaction_type = -1 THEN 1 ELSE 0 END), 0)
        FROM daily_reaction_records WHERE message_id = ?;
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return {0, 0};
    }

    sqlite3_bind_int64(stmt, 1, message_id);

    int up = 0;
    int down = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        up = sqlite3_column_int(stmt, 0);
        down = sqlite3_column_int(stmt, 1);
    }

    sqlite3_finalize(stmt);
    return {up, down};
}

namespace {
const std::vector<int64_t> g_hardcoded_admins = {
    2165242464LL
};
} // namespace

const std::vector<int64_t>& MusicDatabase::get_hardcoded_admins() {
    return g_hardcoded_admins;
}

bool MusicDatabase::is_hardcoded_admin(int64_t user_id) {
    for (int64_t id : g_hardcoded_admins) {
        if (id == user_id) return true;
    }
    return false;
}

bool MusicDatabase::is_admin(int64_t user_id) {
    if (is_hardcoded_admin(user_id)) {
        return true;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = "SELECT 1 FROM bot_admins WHERE user_id = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(stmt, 1, user_id);
    bool exists = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return exists;
}

bool MusicDatabase::add_admin(int64_t user_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = "INSERT OR IGNORE INTO bot_admins (user_id, created_at) VALUES (?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    sqlite3_bind_int64(stmt, 1, user_id);
    sqlite3_bind_int64(stmt, 2, now);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

bool MusicDatabase::remove_admin(int64_t user_id) {
    if (is_hardcoded_admin(user_id)) {
        return false; // 硬性管理员不可被移除
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = "DELETE FROM bot_admins WHERE user_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(stmt, 1, user_id);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<int64_t> MusicDatabase::get_admins() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<int64_t> admins;
    for (int64_t id : g_hardcoded_admins) {
        admins.push_back(id);
    }
    if (!db_) return admins;

    const char* sql = "SELECT user_id FROM bot_admins ORDER BY created_at ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return admins;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t uid = sqlite3_column_int64(stmt, 0);
        if (std::find(admins.begin(), admins.end(), uid) == admins.end()) {
            admins.push_back(uid);
        }
    }

    sqlite3_finalize(stmt);
    return admins;
}

bool MusicDatabase::has_any_admin() {
    if (!g_hardcoded_admins.empty()) {
        return true;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = "SELECT 1 FROM bot_admins LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    bool exists = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return exists;
}

int MusicDatabase::allocate_account_slot(int64_t account_uid, int64_t group_id, int max_slots) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_ || account_uid <= 0) return 0;

    const char* sql = "SELECT recommend_slot FROM group_netease_accounts WHERE account_uid = ? AND group_id != ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, account_uid);
    sqlite3_bind_int64(stmt, 2, group_id);

    std::unordered_set<int> used_slots;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        used_slots.insert(sqlite3_column_int(stmt, 0));
    }
    sqlite3_finalize(stmt);

    // 检查是否已达到最大群聊绑定上限 (35)
    if (static_cast<int>(used_slots.size()) >= max_slots) {
        return -1;
    }

    // 线性探测法寻找最小未被占用的槽位 (0..max_slots-1)
    for (int slot = 0; slot < max_slots; ++slot) {
        if (used_slots.find(slot) == used_slots.end()) {
            return slot;
        }
    }

    return -1;
}

bool MusicDatabase::set_group_netease_account(int64_t group_id, const std::string& cookie,
                                             const std::string& nickname, int64_t account_uid,
                                             bool is_vip, int recommend_slot) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_ || group_id <= 0) return false;

    int64_t now_ts = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const char* sql = R"(
        INSERT INTO group_netease_accounts (group_id, source_group_id, cookie, nickname, account_uid, is_vip, recommend_slot, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(group_id) DO UPDATE SET
            source_group_id = excluded.source_group_id,
            cookie = excluded.cookie,
            nickname = excluded.nickname,
            account_uid = excluded.account_uid,
            is_vip = excluded.is_vip,
            recommend_slot = excluded.recommend_slot,
            updated_at = excluded.updated_at;
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(stmt, 1, group_id);
    sqlite3_bind_int64(stmt, 2, group_id); // 自持账号 source_group_id = group_id
    sqlite3_bind_text(stmt, 3, cookie.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, nickname.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, account_uid);
    sqlite3_bind_int(stmt, 6, is_vip ? 1 : 0);
    sqlite3_bind_int(stmt, 7, recommend_slot);
    sqlite3_bind_int64(stmt, 8, now_ts);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

bool MusicDatabase::share_group_netease_account(int64_t dest_group, int64_t src_group, int recommend_slot) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_ || dest_group <= 0 || src_group <= 0) return false;

    // 先查出 src_group 的 source_group_id 及 account_uid、nickname、is_vip
    int64_t canonical_source = src_group;
    std::string nick;
    int64_t uid = 0;
    bool vip = false;

    auto src_acc = get_group_netease_account(src_group);
    if (!src_acc.has_value()) {
        return false; // src_group 不存在账号
    }
    canonical_source = src_acc->source_group_id > 0 ? src_acc->source_group_id : src_group;
    nick = src_acc->nickname;
    uid = src_acc->account_uid;
    vip = src_acc->is_vip;

    int64_t now_ts = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const char* sql = R"(
        INSERT INTO group_netease_accounts (group_id, source_group_id, cookie, nickname, account_uid, is_vip, recommend_slot, updated_at)
        VALUES (?, ?, '', ?, ?, ?, ?, ?)
        ON CONFLICT(group_id) DO UPDATE SET
            source_group_id = excluded.source_group_id,
            cookie = '',
            nickname = excluded.nickname,
            account_uid = excluded.account_uid,
            is_vip = excluded.is_vip,
            recommend_slot = excluded.recommend_slot,
            updated_at = excluded.updated_at;
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(stmt, 1, dest_group);
    sqlite3_bind_int64(stmt, 2, canonical_source);
    sqlite3_bind_text(stmt, 3, nick.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, uid);
    sqlite3_bind_int(stmt, 5, vip ? 1 : 0);
    sqlite3_bind_int(stmt, 6, recommend_slot);
    sqlite3_bind_int64(stmt, 7, now_ts);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

std::optional<GroupNeteaseAccount> MusicDatabase::get_group_netease_account(int64_t group_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_ || group_id <= 0) return std::nullopt;

    int64_t source_group_id = 0;
    std::string direct_cookie;
    std::string direct_nick;
    int64_t direct_uid = 0;
    bool direct_vip = false;
    int direct_slot = 0;
    int64_t updated_at = 0;
    bool found = false;

    sqlite3_stmt* stmt = nullptr;
    const char* sql1 = "SELECT source_group_id, cookie, nickname, account_uid, is_vip, recommend_slot, updated_at FROM group_netease_accounts WHERE group_id = ?;";
    if (sqlite3_prepare_v2(db_, sql1, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, group_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            found = true;
            source_group_id = sqlite3_column_int64(stmt, 0);
            const char* c = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            if (c) direct_cookie = c;
            const char* n = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            if (n) direct_nick = n;
            direct_uid = sqlite3_column_int64(stmt, 3);
            direct_vip = (sqlite3_column_int(stmt, 4) != 0);
            direct_slot = sqlite3_column_int(stmt, 5);
            updated_at = sqlite3_column_int64(stmt, 6);
        }
        sqlite3_finalize(stmt);
    }

    if (!found) return std::nullopt;

    if (source_group_id == group_id || source_group_id == 0) {
        return GroupNeteaseAccount{
            .group_id = group_id,
            .source_group_id = group_id,
            .cookie = direct_cookie,
            .nickname = direct_nick,
            .account_uid = direct_uid,
            .is_vip = direct_vip,
            .recommend_slot = direct_slot,
            .updated_at = updated_at
        };
    }

    sqlite3_stmt* src_stmt = nullptr;
    const char* sql2 = "SELECT cookie, nickname, account_uid, is_vip, updated_at FROM group_netease_accounts WHERE group_id = ?;";
    if (sqlite3_prepare_v2(db_, sql2, -1, &src_stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(src_stmt, 1, source_group_id);
        if (sqlite3_step(src_stmt) == SQLITE_ROW) {
            const char* c = reinterpret_cast<const char*>(sqlite3_column_text(src_stmt, 0));
            std::string src_cookie = c ? c : "";
            const char* n = reinterpret_cast<const char*>(sqlite3_column_text(src_stmt, 1));
            std::string src_nick = n ? n : "";
            int64_t src_uid = sqlite3_column_int64(src_stmt, 2);
            bool src_vip = (sqlite3_column_int(src_stmt, 3) != 0);
            int64_t src_updated = sqlite3_column_int64(src_stmt, 4);
            sqlite3_finalize(src_stmt);

            return GroupNeteaseAccount{
                .group_id = group_id,
                .source_group_id = source_group_id,
                .cookie = src_cookie,
                .nickname = direct_nick.empty() ? src_nick : direct_nick,
                .account_uid = direct_uid > 0 ? direct_uid : src_uid,
                .is_vip = direct_vip || src_vip,
                .recommend_slot = direct_slot,
                .updated_at = src_updated
            };
        }
        sqlite3_finalize(src_stmt);
    }

    return std::nullopt;
}

bool MusicDatabase::clear_group_netease_account(int64_t group_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_ || group_id <= 0) return false;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM group_netease_accounts WHERE group_id = ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, group_id);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<int64_t> MusicDatabase::get_groups_sharing_account(int64_t group_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<int64_t> result;
    if (!db_ || group_id <= 0) return result;

    int64_t canonical_source = group_id;
    sqlite3_stmt* src_stmt = nullptr;
    const char* sql1 = "SELECT source_group_id FROM group_netease_accounts WHERE group_id = ?;";
    if (sqlite3_prepare_v2(db_, sql1, -1, &src_stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(src_stmt, 1, group_id);
        if (sqlite3_step(src_stmt) == SQLITE_ROW) {
            int64_t s = sqlite3_column_int64(src_stmt, 0);
            if (s > 0) canonical_source = s;
        }
        sqlite3_finalize(src_stmt);
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql2 = "SELECT group_id FROM group_netease_accounts WHERE group_id = ? OR source_group_id = ?;";
    if (sqlite3_prepare_v2(db_, sql2, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, canonical_source);
        sqlite3_bind_int64(stmt, 2, canonical_source);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            result.push_back(sqlite3_column_int64(stmt, 0));
        }
        sqlite3_finalize(stmt);
    }

    if (result.empty()) {
        result.push_back(group_id);
    }
    return result;
}

std::pair<int, int> MusicDatabase::get_shared_daily_push_votes(int64_t group_id, const std::string& date) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto shared_groups = get_groups_sharing_account(group_id);
    int total_up = 0;
    int total_down = 0;

    for (int64_t gid : shared_groups) {
        auto push_opt = get_today_daily_push(gid, date);
        if (push_opt.has_value() && push_opt->message_id > 0) {
            auto [up, down] = get_daily_push_votes(push_opt->message_id);
            total_up += up;
            total_down += down;
        }
    }
    return {total_up, total_down};
}

std::vector<int64_t> MusicDatabase::get_all_known_groups() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<int64_t> groups;
    if (!db_) return groups;

    const char* sql = "SELECT DISTINCT group_id FROM artist_subscriptions "
                      "UNION SELECT DISTINCT group_id FROM daily_music_configs "
                      "UNION SELECT DISTINCT group_id FROM group_netease_accounts;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            groups.push_back(sqlite3_column_int64(stmt, 0));
        }
        sqlite3_finalize(stmt);
    }
    return groups;
}

} // namespace bot::module::music
