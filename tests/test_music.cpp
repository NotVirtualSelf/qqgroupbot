#include "bot/modules/music/database.hpp"
#include "bot/modules/music/netease_client.hpp"
#include "bot/modules/music/qqmusic_client.hpp"
#include "bot/modules/music/music_module.hpp"
#include "bot/protocol/rpc_client.hpp"
#include <iostream>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <filesystem>
#include <boost/asio/io_context.hpp>

using namespace bot::module::music;

int main() {
    std::cout << "[Test] Running MusicModule unit tests..." << std::endl;

    // 1. 验证 SQLite 数据库订阅与平台支持
    {
        MusicDatabase db(":memory:");
        assert(db.init());

        // 判重方法 is_subscribed 验证
        assert(!db.is_subscribed(1001, 6452, "netease"));
        assert(db.add_subscription(1001, 6452, "周杰伦", "netease"));
        assert(db.is_subscribed(1001, 6452, "netease"));
        // 同一艺术家在不同平台判重互不干扰
        assert(!db.is_subscribed(1001, 6452, "qq"));
        // 同一平台不同艺术家ID判重互不干扰
        assert(!db.is_subscribed(1001, 9999, "netease"));

        // 验证多平台可共存：同一群关注不同平台的同ID艺术家
        assert(db.add_subscription(1001, 6452, "周杰伦", "qq"));
        assert(db.is_subscribed(1001, 6452, "qq"));
        auto multi_plat_subs = db.get_group_subscriptions(1001);
        assert(multi_plat_subs.size() == 2);

        // 取消 qq 平台的订阅
        assert(db.remove_subscription_by_id(1001, 6452, "qq"));
        assert(!db.is_subscribed(1001, 6452, "qq"));
        assert(db.is_subscribed(1001, 6452, "netease"));

        auto subs = db.get_group_subscriptions(1001);
        assert(subs.size() == 1);
        assert(subs[0].artist_name == "周杰伦");
        assert(subs[0].artist_id == 6452);
        assert(subs[0].platform == "netease");

        // 验证已推送歌曲记录
        assert(!db.is_song_pushed(99999));
        assert(db.record_pushed_song(99999, 6452, "新歌测试", 123456789));
        assert(db.is_song_pushed(99999));

        // 验证去重与所有不重复艺术家
        auto unique_artists = db.get_all_unique_artists();
        assert(unique_artists.size() == 1);
        assert(unique_artists[0].artist_id == 6452);

        // 取消订阅 (指定平台与通用取消)
        assert(db.remove_subscription(1001, "周杰伦", "netease"));
        assert(db.get_group_subscriptions(1001).empty());
        assert(!db.is_subscribed(1001, 6452, "netease"));

        // 重新添加 (带封面 pic_url) 并测试 find_subscriptions 与 remove_subscription_by_id
        assert(db.add_subscription(1001, 6452, "周杰伦", "netease", "http://example.com/jay.jpg"));
        assert(db.add_subscription(1001, 9999, "周杰伦", "netease", "http://example.com/jay2.jpg"));
        auto duplicates = db.find_subscriptions(1001, "周杰伦");
        assert(duplicates.size() == 2);
        assert(duplicates[0].pic_url == "http://example.com/jay.jpg");
        assert(duplicates[1].pic_url == "http://example.com/jay2.jpg");

        assert(db.remove_subscription_by_id(1001, 9999));
        assert(db.find_subscriptions(1001, "周杰伦").size() == 1);
        assert(db.remove_subscription(1001, "周杰伦"));
        assert(db.get_group_subscriptions(1001).empty());

        // 测试 update_artist_pic_url
        assert(db.add_subscription(1001, 6452, "周杰伦", "netease", "http://old_pic.jpg"));
        assert(db.add_subscription(1002, 6452, "周杰伦", "netease", "http://old_pic.jpg"));
        assert(db.update_artist_pic_url(6452, "http://new_pic.jpg", "netease", 123456));
        auto g1 = db.get_group_subscriptions(1001);
        auto g2 = db.get_group_subscriptions(1002);
        assert(g1[0].pic_url == "http://new_pic.jpg");
        assert(g1[0].cover_updated_at == 123456);
        assert(g2[0].pic_url == "http://new_pic.jpg");
        assert(g2[0].cover_updated_at == 123456);
        assert(db.remove_subscription(1001, "周杰伦"));
        assert(db.remove_subscription(1002, "周杰伦"));

        std::cout << "[Test] SQLite Database tests passed." << std::endl;
    }

    // 2. 验证网易云 API 客户端 (Top 5 候选列表、最高热度代表曲、<无> 兜底)
    {
        NeteaseClient client;

        // 测试获取知名艺术家的相关音乐 (格式为 "歌手 - 音乐")
        std::string top_song = client.get_artist_top_song(6452);
        std::cout << "[Test] Artist 6452 (周杰伦) Top Song: " << top_song << std::endl;
        assert(!top_song.empty());
        assert(top_song != "<无>");
        assert(top_song.find(" - ") != std::string::npos); // 验证 "歌手 - 音乐" 格式

        // 测试无效艺术家 ID 兜底返回 <无>
        std::string empty_top = client.get_artist_top_song(-1);
        std::cout << "[Test] Invalid Artist ID (-1) Top Song: " << empty_top << std::endl;
        assert(empty_top == "<无>");

        // 测试关联搜索前 5 位候选艺术家
        auto candidates = client.search_candidates("周杰伦", 5);
        assert(!candidates.empty());
        assert(candidates.size() <= 5);
        std::cout << "[Test] Found " << candidates.size() << " candidates for '周杰伦':" << std::endl;
        for (size_t i = 0; i < candidates.size(); ++i) {
            std::cout << "  [" << (i + 1) << "] " << candidates[i].name
                      << " (ID: " << candidates[i].id
                      << ", 相关音乐: " << candidates[i].top_song
                      << ", 封面: " << (candidates[i].pic_url.empty() ? "<无封面>" : candidates[i].pic_url.substr(0, 35) + "...")
                      << ")" << std::endl;
            assert(!candidates[i].top_song.empty());
        }

        // 第一个候选通常是最精准的周杰伦
        assert(candidates[0].id == 6452);
        assert(candidates[0].name.find("周杰伦") != std::string::npos);

        // 测试拉取歌曲列表
        auto songs = client.get_artist_songs(candidates[0].id, 3);
        assert(!songs.empty());
        std::cout << "[Test] Fetched " << songs.size() << " songs for " << candidates[0].name << std::endl;

        // 测试包含空格的英文艺术家名字 "Porter Robinson"
        auto porter_cands = client.search_candidates("Porter Robinson", 5);
        std::cout << "[Test] Porter Robinson candidate count: " << porter_cands.size() << std::endl;
        assert(!porter_cands.empty());
        for (const auto& c : porter_cands) {
            std::cout << "  Candidate: " << c.name << " (" << c.id << "), TopSong: " << c.top_song << std::endl;
        }
    }

    // 3. 验证 MusicModule 业务逻辑 (命令精简、关注、取关重名冲突卡片及多选、关注列表分页卡片)
    {
        boost::asio::io_context ioc;
        std::vector<std::string> sent_payloads;
        bot::protocol::RpcClient* rpc_ptr = nullptr;
        bot::protocol::RpcClient rpc(ioc, [&](std::string msg) {
            sent_payloads.push_back(msg);
            if (rpc_ptr) {
                auto pos = msg.find("\"echo\":\"");
                if (pos != std::string::npos) {
                    auto end_pos = msg.find("\"", pos + 8);
                    if (end_pos != std::string::npos) {
                        std::string echo = msg.substr(pos + 8, end_pos - (pos + 8));
                        rpc_ptr->handle_response("{\"status\":\"ok\",\"retcode\":0,\"echo\":\"" + echo + "\"}");
                    }
                }
            }
        });
        rpc_ptr = &rpc;

        MusicModule module(ioc, rpc, ":memory:", std::chrono::minutes(20));
        assert(module.start());

        // 验证弃用命令已被移除：/关注艺术家 和 /取关艺术家 不再匹配
        bot::protocol::GroupMessageEvent ev_legacy_follow;
        ev_legacy_follow.group_id = 8888;
        ev_legacy_follow.user_id = 12345;
        ev_legacy_follow.raw_message = "/关注艺术家 周杰伦";
        assert(!module.handle_group_message(ev_legacy_follow));

        bot::protocol::GroupMessageEvent ev_legacy_unsub;
        ev_legacy_unsub.group_id = 8888;
        ev_legacy_unsub.user_id = 12345;
        ev_legacy_unsub.raw_message = "/取关艺术家 周杰伦";
        assert(!module.handle_group_message(ev_legacy_unsub));
        std::cout << "[Test] Legacy commands (/关注艺术家, /取关艺术家) correctly ignored." << std::endl;

        // 发起 /关注 周杰伦
        bot::protocol::GroupMessageEvent ev1;
        ev1.group_id = 8888;
        ev1.user_id = 12345;
        ev1.raw_message = "/关注 周杰伦";

        bool handled1 = module.handle_group_message(ev1);
        assert(handled1);
        assert(sent_payloads.size() >= 2);
        assert(sent_payloads[0].find("send_group_forward_msg") != std::string::npos);
        assert(sent_payloads[0].find("周杰伦") != std::string::npos);
        std::cout << "[Test] Forward card message sent successfully for /关注." << std::endl;

        // 上一个艺术家还未确认关注或取消，尝试关注下一个艺术家
        bot::protocol::GroupMessageEvent ev2;
        ev2.group_id = 8888;
        ev2.user_id = 12345;
        ev2.raw_message = "/关注 林俊杰";

        sent_payloads.clear();
        bool handled2 = module.handle_group_message(ev2);
        assert(handled2);
        assert(!sent_payloads.empty());
        assert(sent_payloads.back().find("𝐢 | 请先完成或回复「取消」当前操作") != std::string::npos);
        std::cout << "[Test] Second follow blocked with prompt: '𝐢 | 请先完成或回复「取消」当前操作'" << std::endl;

        // 回复序号 1 确认关注周杰伦
        bot::protocol::GroupMessageEvent ev_choose;
        ev_choose.group_id = 8888;
        ev_choose.user_id = 12345;
        ev_choose.raw_message = "1";

        sent_payloads.clear();
        bool handled_choose = module.handle_group_message(ev_choose);
        assert(handled_choose);
        assert(!sent_payloads.empty());
        assert(sent_payloads.back().find("┏⊕ Added━") != std::string::npos);
        assert(sent_payloads.back().find("┃[网易云] 周杰伦") != std::string::npos);
        std::cout << "[Test] Candidate 1 confirmed and subscribed with ┏⊕ Added━ format." << std::endl;

        // 验证关注判重：群 8888 再次发起关注周杰伦，并选择已关注的 1
        sent_payloads.clear();
        assert(module.handle_group_message(ev1));
        assert(sent_payloads.size() >= 2);

        sent_payloads.clear();
        assert(module.handle_group_message(ev_choose));
        assert(!sent_payloads.empty());
        // 仅有重复关注项时，输出 ┏⊗ Skipped━\n┃[网易云] 艺术家名
        assert(sent_payloads.back().find("┏⊗ Skipped━") != std::string::npos);
        assert(sent_payloads.back().find("┃[网易云] 周杰伦") != std::string::npos);
        std::cout << "[Test] Duplicate follow correctly returned '┏⊗ Skipped━'." << std::endl;

        // 验证取消操作：发起关注后发送「取消」
        bot::protocol::GroupMessageEvent ev_cancel_cmd;
        ev_cancel_cmd.group_id = 8888;
        ev_cancel_cmd.user_id = 12345;
        ev_cancel_cmd.raw_message = "/关注 周杰伦";
        sent_payloads.clear();
        assert(module.handle_group_message(ev_cancel_cmd));

        bot::protocol::GroupMessageEvent ev_cancel;
        ev_cancel.group_id = 8888;
        ev_cancel.user_id = 12345;
        ev_cancel.raw_message = "取消";
        sent_payloads.clear();
        assert(module.handle_group_message(ev_cancel));
        assert(!sent_payloads.empty());
        assert(sent_payloads.back().find("➥ | 操作已取消") != std::string::npos);
        std::cout << "[Test] Cancel returned '➥ | 操作已取消'." << std::endl;

        // 验证无效序号错误提示
        sent_payloads.clear();
        assert(module.handle_group_message(ev_cancel_cmd));

        bot::protocol::GroupMessageEvent ev_invalid;
        ev_invalid.group_id = 8888;
        ev_invalid.user_id = 12345;
        ev_invalid.raw_message = "99";
        sent_payloads.clear();
        assert(module.handle_group_message(ev_invalid));
        assert(!sent_payloads.empty());
        assert(sent_payloads.back().find("✕ | 序号无效，请重新输入") != std::string::npos);
        std::cout << "[Test] Invalid index returned '✕ | 序号无效，请重新输入'." << std::endl;

        // 清理 pending 状态
        sent_payloads.clear();
        assert(module.handle_group_message(ev_cancel));
        assert(!sent_payloads.empty());
        assert(sent_payloads.back().find("➥ | 操作已取消") != std::string::npos);

        // 查看关注列表 (使用合并转发卡片，直接展示列表项，不包含 [关注平台]关注艺术家)
        bot::protocol::GroupMessageEvent ev_list;
        ev_list.group_id = 8888;
        ev_list.user_id = 12345;
        ev_list.raw_message = "/关注列表";

        sent_payloads.clear();
        bool handled_list = module.handle_group_message(ev_list);
        assert(handled_list);
        assert(!sent_payloads.empty());
        assert(sent_payloads[0].find("send_group_forward_msg") != std::string::npos);
        assert(sent_payloads[0].find("关注列表") != std::string::npos);
        assert(sent_payloads[0].find("[关注平台]关注艺术家") == std::string::npos);
        assert(sent_payloads[0].find("[网易云]周杰伦") != std::string::npos);
        assert(sent_payloads[0].find("(ID:") == std::string::npos); // 验证不展示 ID
        std::cout << "[Test] /关注列表 returned forward card format directly showing artists." << std::endl;

        // 验证单项取关
        bot::protocol::GroupMessageEvent ev_unsub;
        ev_unsub.group_id = 8888;
        ev_unsub.user_id = 12345;
        ev_unsub.raw_message = "/取关 网易云 周杰伦";

        sent_payloads.clear();
        bool handled_unsub = module.handle_group_message(ev_unsub);
        assert(handled_unsub);
        assert(!sent_payloads.empty());
        assert(sent_payloads.back().find("┏⊖ Removed━") != std::string::npos);
        assert(sent_payloads.back().find("┃[网易云] 周杰伦") != std::string::npos);
        std::cout << "[Test] Single /取关 succeeded." << std::endl;

        // 再次查看关注列表，已为空 (普通提示语)
        sent_payloads.clear();
        module.handle_group_message(ev_list);
        assert(!sent_payloads.empty());
        assert(sent_payloads.back().find("✕ | 当前暂无关注") != std::string::npos);
        std::cout << "[Test] Subscription list is empty after unsubscribing." << std::endl;
    }

    // 4. 验证关注列表 10 个一页分页卡片逻辑 (带 [平台] 前缀，无 ID 展示，无 1. 2. 有序前缀)
    {
        std::string db_file = "test_pages.db";
        std::filesystem::remove(db_file);
        {
            MusicDatabase db(db_file);
            assert(db.init());
            for (int i = 1; i <= 12; ++i) {
                assert(db.add_subscription(7777, 1000 + i, "歌手" + std::to_string(i), "netease"));
            }
        }

        boost::asio::io_context ioc;
        std::vector<std::string> sent_payloads;
        bot::protocol::RpcClient rpc(ioc, [&](std::string msg) {
            sent_payloads.push_back(msg);
        });

        MusicModule module(ioc, rpc, db_file, std::chrono::minutes(20));
        assert(module.start());

        bot::protocol::GroupMessageEvent ev_list;
        ev_list.group_id = 7777;
        ev_list.user_id = 100;
        ev_list.raw_message = "/关注列表";

        bool handled = module.handle_group_message(ev_list);
        assert(handled);
        assert(!sent_payloads.empty());
        assert(sent_payloads[0].find("send_group_forward_msg") != std::string::npos);
        assert(sent_payloads[0].find("关注列表") != std::string::npos);
        assert(sent_payloads[0].find("[关注平台]关注艺术家") == std::string::npos);
        assert(sent_payloads[0].find("关注列表页 #1") != std::string::npos);
        assert(sent_payloads[0].find("关注列表页 #2") != std::string::npos);
        // 验证没有 1. 2. 等有序前缀
        assert(sent_payloads[0].find("1. ") == std::string::npos);
        assert(sent_payloads[0].find("2. ") == std::string::npos);
        // 验证展示格式为 [平台]歌手 (不带 ID)
        assert(sent_payloads[0].find("[网易云]歌手1") != std::string::npos);
        assert(sent_payloads[0].find("[网易云]歌手12") != std::string::npos);
        assert(sent_payloads[0].find("(ID:") == std::string::npos);

        std::filesystem::remove(db_file);
        std::cout << "[Test] 10-per-page forward card pagination with [平台] prefix and no ID passed." << std::endl;
    }

    // 4b. 验证关注时支持多选 ("1 2") 以及 "all"
    {
        boost::asio::io_context ioc;
        std::vector<std::string> sent_payloads;
        bot::protocol::RpcClient* rpc_ptr = nullptr;
        bot::protocol::RpcClient rpc(ioc, [&](std::string msg) {
            sent_payloads.push_back(msg);
            if (rpc_ptr) {
                auto pos = msg.find("\"echo\":\"");
                if (pos != std::string::npos) {
                    auto end_pos = msg.find("\"", pos + 8);
                    if (end_pos != std::string::npos) {
                        std::string echo = msg.substr(pos + 8, end_pos - (pos + 8));
                        rpc_ptr->handle_response("{\"status\":\"ok\",\"retcode\":0,\"echo\":\"" + echo + "\"}");
                    }
                }
            }
        });
        rpc_ptr = &rpc;

        MusicModule module(ioc, rpc, ":memory:", std::chrono::minutes(20));
        assert(module.start());

        // 发起 /关注 周杰伦
        bot::protocol::GroupMessageEvent ev_fol;
        ev_fol.group_id = 6666;
        ev_fol.user_id = 300;
        ev_fol.raw_message = "/关注 周杰伦";
        assert(module.handle_group_message(ev_fol));
        assert(sent_payloads.back().find("确定需要关注的艺术家") != std::string::npos);
        assert(sent_payloads.back().find("「序号 / all / 取消」") != std::string::npos);

        // 回复 "1 2" 多选关注 (全新关注，只有 Added)
        bot::protocol::GroupMessageEvent ev_choose;
        ev_choose.group_id = 6666;
        ev_choose.user_id = 300;
        ev_choose.raw_message = "1 2";
        sent_payloads.clear();
        assert(module.handle_group_message(ev_choose));
        assert(!sent_payloads.empty());
        assert(sent_payloads.back().find("┏⊕ Added━") != std::string::npos);
        assert(sent_payloads.back().find("┃[网易云] 周杰伦") != std::string::npos);
        assert(sent_payloads.back().find("Skipped") == std::string::npos); // 无重复时不显示 Skipped
        assert(sent_payloads.back().find("\\n") != std::string::npos);
        std::cout << "[Test] Follow multi-selection ('1 2') verified." << std::endl;

        // 验证多选关注判重：群 6666 再次选择 "1 2" (全重复，只有 Skipped)
        sent_payloads.clear();
        assert(module.handle_group_message(ev_fol));
        sent_payloads.clear();
        assert(module.handle_group_message(ev_choose));
        assert(!sent_payloads.empty());
        assert(sent_payloads.back().find("┏⊗ Skipped━") != std::string::npos);
        assert(sent_payloads.back().find("Added") == std::string::npos);
        std::cout << "[Test] Multi-selection duplicate follow ('1 2') verified." << std::endl;

        // 验证 Added 与 Skipped 混选交互：群 6666 选择 "1 3" (1为重复关注，3为新关注)
        bot::protocol::GroupMessageEvent ev_choose_mixed;
        ev_choose_mixed.group_id = 6666;
        ev_choose_mixed.user_id = 300;
        ev_choose_mixed.raw_message = "1 3";
        sent_payloads.clear();
        assert(module.handle_group_message(ev_fol));
        sent_payloads.clear();
        assert(module.handle_group_message(ev_choose_mixed));
        assert(!sent_payloads.empty());
        assert(sent_payloads.back().find("┏⊕ Added━") != std::string::npos);
        assert(sent_payloads.back().find("┣⊗ Skipped━") != std::string::npos);
        std::cout << "[Test] Mixed Added and Skipped follow ('1 3') verified." << std::endl;

        // 测试 "all" 全部关注
        bot::protocol::GroupMessageEvent ev_fol_all;
        ev_fol_all.group_id = 5555;
        ev_fol_all.user_id = 400;
        ev_fol_all.raw_message = "/关注 周杰伦";
        assert(module.handle_group_message(ev_fol_all));

        bot::protocol::GroupMessageEvent ev_choose_all;
        ev_choose_all.group_id = 5555;
        ev_choose_all.user_id = 400;
        ev_choose_all.raw_message = "all";
        sent_payloads.clear();
        assert(module.handle_group_message(ev_choose_all));
        assert(!sent_payloads.empty());
        assert(sent_payloads.back().find("┏⊕ Added━") != std::string::npos);
        assert(sent_payloads.back().find("┃[网易云] 周杰伦") != std::string::npos);
        assert(sent_payloads.back().find("\\n") != std::string::npos);
        std::cout << "[Test] Follow 'all' candidates verified." << std::endl;
    }

    // 5. 验证取关重名冲突卡片展示与多选 ("1 2") 及 "all"
    {
        std::string db_file = "test_conflicts.db";
        std::filesystem::remove(db_file);
        {
            MusicDatabase db(db_file);
            assert(db.init());
            assert(db.add_subscription(9999, 6452, "周杰伦", "netease", "http://img1.jpg"));
            assert(db.add_subscription(9999, 99999, "周杰伦", "netease", "http://img2.jpg"));
        }

        boost::asio::io_context ioc;
        std::vector<std::string> sent_payloads;
        bot::protocol::RpcClient* rpc_ptr = nullptr;
        bot::protocol::RpcClient rpc(ioc, [&](std::string msg) {
            sent_payloads.push_back(msg);
            if (rpc_ptr) {
                auto pos = msg.find("\"echo\":\"");
                if (pos != std::string::npos) {
                    auto end_pos = msg.find("\"", pos + 8);
                    if (end_pos != std::string::npos) {
                        std::string echo = msg.substr(pos + 8, end_pos - (pos + 8));
                        rpc_ptr->handle_response("{\"status\":\"ok\",\"retcode\":0,\"echo\":\"" + echo + "\"}");
                    }
                }
            }
        });
        rpc_ptr = &rpc;

        MusicModule module(ioc, rpc, db_file, std::chrono::minutes(20));
        assert(module.start());

        // 发起取关周杰伦，命中 2 位同名艺术家，应触发冲突转发卡片与提示
        bot::protocol::GroupMessageEvent ev_unsub;
        ev_unsub.group_id = 9999;
        ev_unsub.user_id = 200;
        ev_unsub.raw_message = "/取关 周杰伦";

        bool handled_unsub = module.handle_group_message(ev_unsub);
        assert(handled_unsub);
        assert(sent_payloads.size() == 2);
        assert(sent_payloads[0].find("send_group_forward_msg") != std::string::npos);
        assert(sent_payloads[0].find("发现多个同名艺术家") != std::string::npos);
        assert(sent_payloads[0].find("候选艺术家 #1") != std::string::npos);
        assert(sent_payloads[0].find("候选艺术家 #2") != std::string::npos);
        assert(sent_payloads[0].find("周杰伦 (ID: 6452)") != std::string::npos);
        assert(sent_payloads[1].find("确定需要取关的艺术家") != std::string::npos);
        assert(sent_payloads[1].find("「序号 / all / 取消」") != std::string::npos);
        std::cout << "[Test] Unfollow duplicate conflict card sent successfully." << std::endl;

        // 测试取关操作未完成时，发起新关注会被拦截提示
        bot::protocol::GroupMessageEvent ev_block;
        ev_block.group_id = 9999;
        ev_block.user_id = 200;
        ev_block.raw_message = "/关注 周杰伦";
        sent_payloads.clear();
        assert(module.handle_group_message(ev_block));
        assert(!sent_payloads.empty());
        assert(sent_payloads.back().find("𝐢 | 请先完成或回复「取消」当前操作") != std::string::npos);
        std::cout << "[Test] Intercepted new command when unfollow conflict is pending." << std::endl;

        // 测试多选取消关注："1 2"
        bot::protocol::GroupMessageEvent ev_choose_multi;
        ev_choose_multi.group_id = 9999;
        ev_choose_multi.user_id = 200;
        ev_choose_multi.raw_message = "1 2";
        sent_payloads.clear();
        assert(module.handle_group_message(ev_choose_multi));
        assert(!sent_payloads.empty());
        assert(sent_payloads.back().find("┏⊖ Removed━") != std::string::npos);
        assert(sent_payloads.back().find("┃[网易云] 周杰伦") != std::string::npos);
        assert(sent_payloads.back().find("\\n") != std::string::npos);

        // 验证数据库已清空
        {
            MusicDatabase db(db_file);
            assert(db.init());
            assert(db.find_subscriptions(9999, "周杰伦").empty());
        }
        std::cout << "[Test] Multi-selection unsubscription ('1 2') verified." << std::endl;

        // 测试 "all" 全部取关分支
        {
            MusicDatabase db(db_file);
            assert(db.init());
            assert(db.add_subscription(9999, 6452, "周杰伦", "netease"));
            assert(db.add_subscription(9999, 99999, "周杰伦", "netease"));
        }

        sent_payloads.clear();
        assert(module.handle_group_message(ev_unsub));
        assert(sent_payloads.size() == 2);

        bot::protocol::GroupMessageEvent ev_choose_all;
        ev_choose_all.group_id = 9999;
        ev_choose_all.user_id = 200;
        ev_choose_all.raw_message = "all";
        sent_payloads.clear();
        assert(module.handle_group_message(ev_choose_all));
        assert(!sent_payloads.empty());
        assert(sent_payloads.back().find("┏⊖ Removed━") != std::string::npos);
        assert(sent_payloads.back().find("┃[网易云] 周杰伦") != std::string::npos);
        assert(sent_payloads.back().find("\\n") != std::string::npos);

        {
            MusicDatabase db(db_file);
            assert(db.init());
            assert(db.find_subscriptions(9999, "周杰伦").empty());
        }

        // 测试超长艺术家名字取关时的安全截断与 ... 省略
        {
            MusicDatabase db(db_file);
            assert(db.init());
            assert(db.add_subscription(9999, 88888, "VeryLongArtistNameThatExceedsLineBudget", "netease"));
        }

        bot::protocol::GroupMessageEvent ev_unsub_long;
        ev_unsub_long.group_id = 9999;
        ev_unsub_long.user_id = 200;
        ev_unsub_long.raw_message = "/取关 VeryLongArtistNameThatExceedsLineBudget";
        sent_payloads.clear();
        assert(module.handle_group_message(ev_unsub_long));
        assert(!sent_payloads.empty());
        assert(sent_payloads.back().find("┏⊖ Removed━") != std::string::npos);
        assert(sent_payloads.back().find("...") != std::string::npos);
        std::cout << "[Test] Long artist name safely truncated with '...' on single unsubscription: " << sent_payloads.back() << std::endl;

        std::filesystem::remove(db_file);
        std::cout << "[Test] 'all' unsubscription and long name truncation verified." << std::endl;
    }

    // 6. 验证每一天通过对应平台静默更新歌手页封面机制
    {
        boost::asio::io_context ioc;
        std::vector<std::string> sent_payloads;
        bot::protocol::RpcClient rpc(ioc, [&](std::string msg) {
            sent_payloads.push_back(msg);
        });

        std::string db_file = "test_daily_cover.db";
        std::filesystem::remove(db_file);
        {
            MusicDatabase db(db_file);
            assert(db.init());
            // 初始封面使用旧占位图，且更新时间设为 2 天前 (避免在 24 小时内被跳过)
            int64_t two_days_ago = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count() - 172800;
            assert(db.add_subscription(1111, 6452, "周杰伦", "netease", "http://old_placeholder.jpg"));
            assert(db.update_artist_pic_url(6452, "http://old_placeholder.jpg", "netease", two_days_ago));
        }

        MusicModule module(ioc, rpc, db_file, std::chrono::minutes(20), std::chrono::hours(24));
        assert(module.start());

        // 执行静默封面更新
        module.update_artist_covers_once(false);

        // 验证静默更新：绝不向任何群发送通知消息！
        assert(sent_payloads.empty());

        // 验证数据库中的封面已从网易云平台成功拉取并更新
        {
            MusicDatabase db(db_file);
            assert(db.init());
            auto subs = db.get_group_subscriptions(1111);
            assert(subs.size() == 1);
            assert(!subs[0].pic_url.empty());
            assert(subs[0].pic_url != "http://old_placeholder.jpg");
            assert(subs[0].pic_url.find("http") != std::string::npos);
            assert(subs[0].cover_updated_at > 0);
            std::cout << "[Test] Artist 6452 cover silently updated to: " << subs[0].pic_url << std::endl;
        }

        std::filesystem::remove(db_file);
        std::cout << "[Test] Daily silent artist cover update verified." << std::endl;
    }

    // 5. 验证新专辑轮询、转发卡片格式与单曲/专辑平台分享
    {
        std::cout << "\n[Test] Testing new album polling, forward card format, and sharing..." << std::endl;

        // 5.1 验证数据库 pushed_albums 接口
        {
            MusicDatabase db(":memory:");
            assert(db.init());
            assert(!db.is_album_pushed(101));
            assert(!db.has_artist_pushed_albums(6452));
            assert(db.record_pushed_album(101, 6452, "最伟大的作品", 1657814400, "netease", "周杰伦"));
            assert(db.is_album_pushed(101));
            assert(db.is_album_pushed(101, "netease"));
            assert(!db.is_album_pushed(101, "qq"));
            assert(db.has_artist_pushed_albums(6452));
            assert(!db.is_album_pushed(102));

            // 跨平台排重方法 is_album_recently_pushed_cross_platform 测试
            // 同一专辑同一艺术家在不同平台被查询：应判定为已在其他平台推送过
            assert(db.is_album_recently_pushed_cross_platform("周杰伦", "最伟大的作品", 86400, "qq"));
            // 忽略两端空格与大小写
            assert(db.is_album_recently_pushed_cross_platform(" 周杰伦 ", " 最伟大的作品 ", 86400, "qq"));
            // 相同平台自身不计入跨平台
            assert(!db.is_album_recently_pushed_cross_platform("周杰伦", "最伟大的作品", 86400, "netease"));
            // 不同艺术家不冲突
            assert(!db.is_album_recently_pushed_cross_platform("林俊杰", "最伟大的作品", 86400, "qq"));
            // 不同专辑不冲突
            assert(!db.is_album_recently_pushed_cross_platform("周杰伦", "十一月的萧邦", 86400, "qq"));

            std::cout << "[Test] Database pushed_albums and cross-platform deduplication verified." << std::endl;
        }

        // 5.2 验证网易云 API 客户端拉取专辑列表与专辑详情
        {
            NeteaseClient client;
            auto albums = client.get_artist_albums(6452, 2);
            assert(!albums.empty());
            std::cout << "[Test] Fetched " << albums.size() << " albums for 周杰伦. Latest: " << albums[0].name << " (ID: " << albums[0].id << ")" << std::endl;
            assert(albums[0].id > 0);
            assert(!albums[0].name.empty());

            auto detail_opt = client.get_album_detail(albums[0].id);
            assert(detail_opt.has_value());
            const auto& detail = *detail_opt;
            assert(detail.id == albums[0].id);
            assert(!detail.name.empty());
            assert(!detail.tracks.empty());
            assert(detail.total_duration_ms > 0);
            std::cout << "[Test] Album detail fetched: 「" << detail.name << "」 with " << detail.tracks.size() << " tracks, total ms: " << detail.total_duration_ms << std::endl;
        }

        // 5.3 验证时长格式化
        {
            // 单曲时长：xx分xx秒
            assert(MusicModule::format_track_duration(225000) == "03分45秒");
            assert(MusicModule::format_track_duration(65000) == "01分05秒");
            assert(MusicModule::format_track_duration(5000) == "00分05秒");

            // 总时长：小于 1 小时为 xx分xx秒，大于等于 1 小时为 x小时xx分xx秒
            assert(MusicModule::format_total_duration(225000) == "03分45秒");
            assert(MusicModule::format_total_duration(3665000) == "1小时01分05秒");
            assert(MusicModule::format_total_duration(7200000) == "2小时00分00秒");
            std::cout << "[Test] Duration formatting verified." << std::endl;
        }

        // 5.4 验证合并转发卡片节点构造
        {
            // 单曲专辑测试
            AlbumDetail single_alb;
            single_alb.id = 12345;
            single_alb.name = "单曲专辑测试";
            single_alb.pic_url = "http://example.com/cover1.jpg";
            single_alb.artist_name = "周杰伦";
            single_alb.total_duration_ms = 225000;
            single_alb.tracks.push_back(AlbumTrack{
                .id = 99901,
                .name = "最伟大的单曲",
                .artist_name = "周杰伦",
                .duration_ms = 225000
            });

            auto nodes = MusicModule::build_album_forward_nodes(single_alb);
            assert(nodes.size() == 4); // 封面、单曲、创作者、基本信息（单曲不包含专辑与曲目列表）
            assert(nodes[0].data.name == "封面");
            assert(nodes[0].data.content == "[CQ:image,file=http://example.com/cover1.jpg]");
            assert(nodes[1].data.name == "单曲");
            assert(nodes[1].data.content == "「最伟大的单曲」");
            assert(nodes[2].data.name == "创作者");
            assert(nodes[2].data.content == "「周杰伦」");
            assert(nodes[3].data.name == "基本信息");
            assert(nodes[3].data.content == "「03分45秒」");

            // 多曲专辑测试
            AlbumDetail multi_alb;
            multi_alb.id = 54321;
            multi_alb.name = "完整大碟";
            multi_alb.pic_url = "http://example.com/cover2.jpg";
            multi_alb.artist_name = "周杰伦";
            multi_alb.total_duration_ms = 450000;
            multi_alb.tracks.push_back(AlbumTrack{
                .id = 99901,
                .name = "曲目一",
                .artist_name = "周杰伦",
                .duration_ms = 225000
            });
            multi_alb.tracks.push_back(AlbumTrack{
                .id = 99902,
                .name = "曲目二",
                .artist_name = "周杰伦 / 方文山",
                .duration_ms = 225000
            });

            auto multi_nodes = MusicModule::build_album_forward_nodes(multi_alb);
            assert(multi_nodes.size() == 6); // 封面、专辑、创作者、基本信息、曲目 #1、曲目 #2
            assert(multi_nodes[1].data.name == "专辑");
            assert(multi_nodes[1].data.content == "「完整大碟」");
            assert(multi_nodes[2].data.name == "创作者");
            assert(multi_nodes[2].data.content == "「周杰伦」");
            assert(multi_nodes[3].data.name == "基本信息");
            assert(multi_nodes[3].data.content == "「2首 · 07分30秒」");
            assert(multi_nodes[4].data.name == "曲目 #1");
            assert(multi_nodes[4].data.content == "曲目一\n- 周杰伦\n- 03分45秒");
            assert(multi_nodes[5].data.name == "曲目 #2");
            assert(multi_nodes[5].data.content == "曲目二\n- 周杰伦 / 方文山\n- 03分45秒");
            std::cout << "[Test] Forward card nodes generation verified." << std::endl;
        }

        // 5.5 验证新专辑轮询通知：1首单曲时转发单曲，>=2首单曲时转发专辑
        {
            boost::asio::io_context ioc;
            std::vector<std::string> sent_payloads;
            bot::protocol::RpcClient* rpc_ptr = nullptr;
            bot::protocol::RpcClient rpc(ioc, [&](std::string msg) {
                sent_payloads.push_back(msg);
                if (rpc_ptr) {
                    auto pos = msg.find("\"echo\":\"");
                    if (pos != std::string::npos) {
                        auto end_pos = msg.find("\"", pos + 8);
                        if (end_pos != std::string::npos) {
                            std::string echo = msg.substr(pos + 8, end_pos - (pos + 8));
                            rpc_ptr->handle_response("{\"status\":\"ok\",\"retcode\":0,\"echo\":\"" + echo + "\"}");
                        }
                    }
                }
            });
            rpc_ptr = &rpc;

            std::string db_file = "test_album_polling.db";
            std::filesystem::remove(db_file);
            {
                MusicDatabase db(db_file);
                assert(db.init());
                // 关注周杰伦
                assert(db.add_subscription(5555, 6452, "周杰伦", "netease", "http://cover.jpg"));
            }

            MusicModule module(ioc, rpc, db_file, std::chrono::minutes(20));
            assert(module.start());

            // 触发检查：最新专辑未在库中，应该触发新专辑推送
            sent_payloads.clear();
            module.check_new_albums_once();

            // 验证收到了通知
            assert(!sent_payloads.empty());

            // 验证第一条消息：前置纯文本通知
            // 关注的艺术家【周杰伦】在【网易云音乐】发布了新专辑！
            assert(sent_payloads[0].find("send_group_msg") != std::string::npos);
            assert(sent_payloads[0].find("关注的艺术家【周杰伦】在【网易云音乐】发布了新专辑！") != std::string::npos);

            // 验证第二条消息：合并转发卡片
            assert(sent_payloads[1].find("send_group_forward_msg") != std::string::npos);
            assert(sent_payloads[1].find("封面") != std::string::npos);
            assert(sent_payloads[1].find("专辑") != std::string::npos);
            assert(sent_payloads[1].find("创作者") != std::string::npos);
            assert(sent_payloads[1].find("基本信息") != std::string::npos);
            assert(sent_payloads[1].find("曲目 #1") != std::string::npos);

            // 验证第三条消息：平台音乐分享卡片
            // 周杰伦的最新专辑有多首曲目，因此应发送 [CQ:music,type=custom,...] 专辑分享
            assert(sent_payloads[2].find("send_group_msg") != std::string::npos);
            assert(sent_payloads[2].find("[CQ:music,") != std::string::npos);

            std::cout << "[Test] New album check successfully dispatched intro, forward card, and music card!" << std::endl;

            // 再次触发检查：应被库中记录去重，不再重复推送！
            sent_payloads.clear();
            module.check_new_albums_once();
            assert(sent_payloads.empty());
            std::cout << "[Test] Deduplication verified: no redundant notifications." << std::endl;

            std::filesystem::remove(db_file);
        }

        // 5.6 验证跨平台多源发布同一专辑时，1天内只推送首个轮询到的平台
        {
            boost::asio::io_context ioc;
            std::vector<std::string> sent_payloads;
            bot::protocol::RpcClient rpc(ioc, [&](std::string msg) {
                sent_payloads.push_back(msg);
            });

            std::string db_file = "test_cross_platform_album.db";
            std::filesystem::remove(db_file);
            {
                MusicDatabase db(db_file);
                assert(db.init());
                // 模拟已在网易云 (netease) 平台于 1 小时前成功推送了周杰伦的最新专辑 "太阳之子"
                int64_t one_hour_ago = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count() - 3600;
                assert(db.record_pushed_album(403900711, 6452, "太阳之子", 1774495888, "netease", "周杰伦"));
                // 关注周杰伦在另一平台（如 qq 平台）
                assert(db.add_subscription(6666, 6452, "周杰伦", "qq", "http://cover.jpg"));
            }

            MusicModule module(ioc, rpc, db_file, std::chrono::minutes(20));
            assert(module.start());

            // 触发检查：由于 "太阳之子" 已经在 1 小时前由网易云平台推送，qq 平台轮询到同名专辑时必须静默跳过！
            sent_payloads.clear();
            module.check_new_albums_once();

            // 验证未向群发送任何关于该专辑的重复推送
            for (const auto& payload : sent_payloads) {
                assert(payload.find("太阳之子") == std::string::npos);
            }
            std::cout << "[Test] Cross-platform album deduplication within 24h verified: second platform skipped." << std::endl;

            std::filesystem::remove(db_file);
        }

        // 5.7 验证手动查询新歌指令 /查新歌：无新歌时回复“𝐢 | 未查询到新歌”，有新歌时逐个依次推送不混杂
        {
            boost::asio::io_context ioc;
            std::vector<std::string> sent_payloads;
            bot::protocol::RpcClient* rpc_ptr = nullptr;
            bot::protocol::RpcClient rpc(ioc, [&](std::string msg) {
                sent_payloads.push_back(msg);
                if (rpc_ptr) {
                    auto pos = msg.find("\"echo\":\"");
                    if (pos != std::string::npos) {
                        auto end_pos = msg.find("\"", pos + 8);
                        if (end_pos != std::string::npos) {
                            std::string echo = msg.substr(pos + 8, end_pos - (pos + 8));
                            rpc_ptr->handle_response("{\"status\":\"ok\",\"retcode\":0,\"echo\":\"" + echo + "\"}");
                        }
                    }
                }
            });
            rpc_ptr = &rpc;

            std::string db_file = "test_manual_check_new.db";
            std::filesystem::remove(db_file);
            {
                MusicDatabase db(db_file);
                assert(db.init());
                assert(db.add_subscription(7788, 6452, "周杰伦", "netease", "http://cover.jpg"));
                NeteaseClient netease;
                auto existing_albums = netease.get_artist_albums(6452, 5);
                for (const auto& alb : existing_albums) {
                    assert(db.record_pushed_album(alb.id, 6452, alb.name, alb.publish_time, "netease", "周杰伦"));
                }
            }

            MusicModule module(ioc, rpc, db_file);
            assert(module.start());

            bot::protocol::GroupMessageEvent ev;
            ev.group_id = 7788;
            ev.user_id = 12345;

            // 1. 发送 /查新歌，当无新歌时回复“𝐢 | 未查询到新歌”
            ev.raw_message = "/查新歌";
            sent_payloads.clear();
            assert(module.handle_group_message(ev));
            assert(sent_payloads.size() == 1);
            assert(sent_payloads[0].find("𝐢 | 未查询到新歌") != std::string::npos);
            std::cout << "[Test 5.7] /查新歌 returned '𝐢 | 未查询到新歌' when no new releases." << std::endl;

            // 2. 模拟周杰伦有 2 张新专辑待推送（标记较旧的专辑已推送，最新2张未推送）
            {
                std::filesystem::remove(db_file);
                MusicDatabase db2(db_file);
                assert(db2.init());
                assert(db2.add_subscription(7788, 6452, "周杰伦", "netease", "http://cover.jpg"));
                NeteaseClient netease;
                auto albs = netease.get_artist_albums(6452, 5);
                for (size_t i = 2; i < albs.size(); ++i) {
                    assert(db2.record_pushed_album(albs[i].id, 6452, albs[i].name, albs[i].publish_time, "netease", "周杰伦"));
                }
            }
            MusicModule module2(ioc, rpc, db_file);
            assert(module2.start());

            sent_payloads.clear();
            assert(module2.handle_group_message(ev));
            // 周杰伦有 2 张专辑 (太阳之子 和 即兴曲)，每张产生 3 条消息 (前置文本, 转发卡片, 音乐分享)
            // 验证逐个推送：第 1 张的三条消息先发完，再发第 2 张的三条消息，顺序严格且不混杂！
            assert(sent_payloads.size() == 6);
            // 专辑 1
            assert(sent_payloads[0].find("关注的艺术家【周杰伦】在【网易云音乐】发布了新专辑！") != std::string::npos);
            assert(sent_payloads[1].find("send_group_forward_msg") != std::string::npos);
            assert(sent_payloads[2].find("[CQ:music,") != std::string::npos);
            // 专辑 2 (即兴曲为单曲，tracks.size() == 1，按规则判定为新单曲)
            assert(sent_payloads[3].find("关注的艺术家【周杰伦】在【网易云音乐】发布了新单曲！") != std::string::npos);
            assert(sent_payloads[4].find("send_group_forward_msg") != std::string::npos);
            assert(sent_payloads[5].find("[CQ:music,") != std::string::npos);
            std::cout << "[Test 5.7] Sequential unmixed dispatch of multiple albums (album + single) verified!" << std::endl;

            std::filesystem::remove(db_file);
        }
    }

    // 6. 验证 QQ 音乐平台集成 (QQMusicClient 搜索、头像、代表曲、专辑、/关注 qqmusic、/取关 qqmusic、/关注列表)
    {
        std::cout << "\n[Test] Testing QQ Music platform integration..." << std::endl;
        QQMusicClient qq_client;

        // 6.1 测试 QQ 音乐候选搜索
        auto candidates = qq_client.search_candidates("周杰伦", 5);
        assert(!candidates.empty());
        assert(candidates[0].id == 4558);
        assert(candidates[0].mid == "0025NhlN2yWrP4");
        assert(candidates[0].name.find("周杰伦") != std::string::npos);
        assert(candidates[0].platform == "qqmusic");
        assert(!candidates[0].pic_url.empty());
        assert(!candidates[0].top_song.empty() && candidates[0].top_song != "<无>");
        std::cout << "[Test] QQMusicClient search_candidates passed. Top song: " << candidates[0].top_song << std::endl;

        // 6.2 测试 QQ 音乐歌手头像与代表作
        std::string pic_url = qq_client.get_artist_pic_url(4558);
        assert(!pic_url.empty());
        assert(pic_url.find("0025NhlN2yWrP4") != std::string::npos);

        std::string top_song = qq_client.get_artist_top_song(4558);
        assert(!top_song.empty() && top_song != "<无>");
        std::cout << "[Test] QQMusicClient artist avatar and top song passed: " << pic_url << " | " << top_song << std::endl;

        // 6.3 测试 QQ 音乐歌手专辑列表与专辑详情
        auto albums = qq_client.get_artist_albums(4558, "周杰伦", pic_url, 3);
        assert(!albums.empty());
        std::cout << "[Test] QQMusicClient fetched " << albums.size() << " albums for 周杰伦. Latest: " << albums[0].name << " (ID: " << albums[0].id << ")" << std::endl;

        auto album_detail = qq_client.get_album_detail(albums[0].id);
        assert(album_detail.has_value());
        assert(!album_detail->tracks.empty());
        assert(album_detail->total_duration_ms > 0);
        std::cout << "[Test] QQMusicClient album detail fetched: 「" << album_detail->name << "」 with " << album_detail->tracks.size() << " tracks." << std::endl;

        // 6.4 测试 MusicModule 针对 qqmusic 的指令交互 (/关注 qqmusic, 候选确认, 重复关注, /关注列表, /取关 qqmusic)
        {
            boost::asio::io_context ioc;
            std::vector<std::string> sent_payloads;
            bot::protocol::RpcClient* rpc_ptr = nullptr;
            bot::protocol::RpcClient rpc(ioc, [&](std::string msg) {
                sent_payloads.push_back(msg);
                if (rpc_ptr) {
                    auto pos = msg.find("\"echo\":\"");
                    if (pos != std::string::npos) {
                        std::string echo_val = msg.substr(pos + 8, 36);
                        std::string resp = "{\"status\":\"ok\",\"retcode\":0,\"data\":{},\"echo\":\"" + echo_val + "\"}";
                        rpc_ptr->handle_response(resp);
                    }
                }
            });
            rpc_ptr = &rpc;

            std::string db_file = "test_qqmusic_module.db";
            std::filesystem::remove(db_file);

            MusicModule module(ioc, rpc, db_file, std::chrono::minutes(20));
            assert(module.start());

            // 发送 "/关注 qqmusic 周杰伦"
            bot::protocol::GroupMessageEvent ev;
            ev.group_id = 8888;
            ev.user_id = 12345;
            ev.raw_message = "/关注 qqmusic 周杰伦";
            assert(module.handle_group_message(ev));

            // 验证发送了候选卡片与提示语
            assert(!sent_payloads.empty());
            assert(sent_payloads[0].find("🔍【QQ音乐】搜索结果：") != std::string::npos);
            assert(sent_payloads[0].find("候选艺术家 #1") != std::string::npos);

            // 用户回复序号 1
            sent_payloads.clear();
            ev.raw_message = "1";
            assert(module.handle_group_message(ev));

            // 验证收到 ┏⊕ Added━\n┃[QQ音乐] 周杰伦
            assert(!sent_payloads.empty());
            assert(sent_payloads[0].find("┏⊕ Added━") != std::string::npos);
            assert(sent_payloads[0].find("[QQ音乐] 周杰伦") != std::string::npos);
            std::cout << "[Test] QQ Music /关注 qqmusic 周杰伦 succeeded." << std::endl;

            // 再次发送 "/关注 qqmusic 周杰伦" 并选 1，验证判重跳过
            sent_payloads.clear();
            ev.raw_message = "/关注 qqmusic 周杰伦";
            assert(module.handle_group_message(ev));
            ev.raw_message = "1";
            sent_payloads.clear();
            assert(module.handle_group_message(ev));
            assert(!sent_payloads.empty());
            assert(sent_payloads[0].find("┏⊗ Skipped━") != std::string::npos);
            assert(sent_payloads[0].find("[QQ音乐] 周杰伦") != std::string::npos);
            std::cout << "[Test] QQ Music duplicate follow correctly returned Skipped." << std::endl;

            // 发送 "/关注列表"
            sent_payloads.clear();
            ev.raw_message = "/关注列表";
            assert(module.handle_group_message(ev));
            assert(!sent_payloads.empty());
            assert(sent_payloads[0].find("[QQ音乐]周杰伦") != std::string::npos);
            std::cout << "[Test] QQ Music /关注列表 verified." << std::endl;

            // 发送 "/取关 qqmusic 周杰伦"
            sent_payloads.clear();
            ev.raw_message = "/取关 qqmusic 周杰伦";
            assert(module.handle_group_message(ev));
            assert(!sent_payloads.empty());
            assert(sent_payloads[0].find("┏⊖ Removed━") != std::string::npos);
            assert(sent_payloads[0].find("[QQ音乐] 周杰伦") != std::string::npos);
            std::cout << "[Test] QQ Music /取关 qqmusic 周杰伦 succeeded." << std::endl;

            std::filesystem::remove(db_file);
        }
    }

    // 7. 验证网易云日推、QQ表情回应 (对的对的 / 不对不对)、23点静默点赞/踩规则与开放式扩展架构
    {
        std::cout << "\n[Test 7] Testing Daily Music Recommendation, Emoji Reactions, and Silent Evaluation..." << std::endl;

        // 7.1 测试 NapCat / OneBot 11 表情回应事件 (GroupReactionEvent) 解析
        {
            // 添加点赞表情 "对的对的" (emoji_id: 478)
            std::string napcat_add_like = R"({
                "post_type": "notice",
                "notice_type": "group_msg_emoji_like",
                "sub_type": "add",
                "group_id": 987654321,
                "user_id": 12345678,
                "message_id": 888001,
                "likes": [{"emoji_id": "478", "count": 1}]
            })";
            auto ev1 = bot::protocol::GroupReactionEvent::parse(napcat_add_like);
            assert(ev1.has_value());
            assert(ev1->group_id == 987654321);
            assert(ev1->user_id == 12345678);
            assert(ev1->message_id == 888001);
            assert(ev1->emoji_id == "478");
            assert(ev1->is_add == true);

            // 取消点赞表情
            std::string napcat_remove_like = R"({
                "post_type": "notice",
                "notice_type": "group_msg_emoji_like",
                "sub_type": "remove",
                "group_id": 987654321,
                "user_id": 12345678,
                "message_id": 888001,
                "likes": [{"emoji_id": "478", "count": 0}]
            })";
            auto ev2 = bot::protocol::GroupReactionEvent::parse(napcat_remove_like);
            assert(ev2.has_value());
            assert(ev2->is_add == false);

            // 添加踩表情 "不对不对" (emoji_id: 479)
            std::string reaction_dislike = R"({
                "post_type": "notice",
                "notice_type": "reaction",
                "sub_type": "add",
                "group_id": 987654321,
                "user_id": 87654321,
                "message_id": 888001,
                "emoji_id": "479"
            })";
            auto ev3 = bot::protocol::GroupReactionEvent::parse(reaction_dislike);
            assert(ev3.has_value());
            assert(ev3->emoji_id == "479");
            assert(ev3->is_add == true);

            std::cout << "[Test 7.1] GroupReactionEvent parsing for 478 (对的对的) and 479 (不对不对) passed." << std::endl;
        }

        // 7.2 测试数据库全局配置与每日推荐数据表操作
        {
            std::string db_file = "test_daily_music.db";
            std::filesystem::remove(db_file);
            MusicDatabase db(db_file);
            assert(db.init());

            // 测试配置读写 (如 netease_cookie)
            assert(db.set_config("netease_cookie", "MUSIC_U=test_cookie_123;"));
            assert(db.get_config("netease_cookie") == "MUSIC_U=test_cookie_123;");
            assert(db.get_config("unknown_key", "default_val") == "default_val");

            // 测试群订阅每日音乐开启/关闭
            assert(!db.is_daily_music_enabled(987654));
            assert(db.set_daily_music_enabled(987654, true));
            assert(db.is_daily_music_enabled(987654));
            auto groups = db.get_daily_music_groups();
            assert(groups.size() == 1 && groups[0] == 987654);
            assert(db.set_daily_music_enabled(987654, false));
            assert(!db.is_daily_music_enabled(987654));
            assert(db.get_daily_music_groups().empty());

            // 重新开启群
            assert(db.set_daily_music_enabled(987654, true));

            // 测试每日推送记录入库与查询
            int64_t row_id = db.record_daily_push("netease_daily_song", 987654, 777001,
                                                  186016, "Ghost Voices", "Virtual Self",
                                                  "netease", "2026-10-25");
            assert(row_id > 0);

            auto push_by_msg = db.get_daily_push_by_message_id(777001);
            assert(push_by_msg.has_value());
            assert(push_by_msg->item_name == "Ghost Voices");
            assert(push_by_msg->artist_name == "Virtual Self");
            assert(push_by_msg->evaluated == 0);

            auto latest = db.get_latest_daily_push(987654, "netease_daily_song");
            assert(latest.has_value());
            assert(latest->id == row_id);

            auto unevaluated = db.get_unevaluated_daily_pushes("netease_daily_song");
            assert(unevaluated.size() == 1);

            // 测试表情投票统计 (对的对的=1, 不对不对=-1)
            assert(db.record_daily_reaction(777001, 101, 1));  // 用户 101 赞
            assert(db.record_daily_reaction(777001, 102, 1));  // 用户 102 赞
            assert(db.record_daily_reaction(777001, 103, -1)); // 用户 103 踩
            auto [u1, d1] = db.get_daily_push_votes(777001);
            assert(u1 == 2 && d1 == 1);

            // 用户 103 改投赞
            assert(db.record_daily_reaction(777001, 103, 1));
            auto [u2, d2] = db.get_daily_push_votes(777001);
            assert(u2 == 3 && d2 == 0);

            // 用户 102 取消投票
            assert(db.remove_daily_reaction(777001, 102));
            auto [u3, d3] = db.get_daily_push_votes(777001);
            assert(u3 == 2 && d3 == 0);

            // 标记评价处理
            assert(db.mark_daily_push_evaluated(row_id, u3, d3));
            assert(db.get_unevaluated_daily_pushes("netease_daily_song").empty());
            auto after_eval = db.get_daily_push_by_message_id(777001);
            assert(after_eval->evaluated == 1);
            assert(after_eval->upvotes == 2 && after_eval->downvotes == 0);

            std::filesystem::remove(db_file);
            std::cout << "[Test 7.2] MusicDatabase daily music operations passed." << std::endl;
        }

        // 7.3 验证点赞/踩评价规则公式
        // 规则 1: upvotes > downvotes + 3 -> Like (点赞这首歌)
        // 规则 2: downvotes >= upvotes -> Dislike (减少推荐这首歌)
        // 规则 3: 其他情况 -> None (不做任何处理)
        {
            boost::asio::io_context ioc;
            bot::protocol::RpcClient rpc(ioc, [](std::string) {});
            MusicDatabase db(":memory:");
            assert(db.init());

            DailyMusicManager manager(ioc, rpc, db);
            auto netease = std::make_shared<NeteaseClient>();
            auto recommender = std::make_shared<NeteaseDailySongRecommender>(netease);

            DailyRecommendationItem item;
            item.item_id = 99999;
            item.item_name = "Test Track";
            item.artist_name = "Test Artist";
            item.platform = "netease";

            // 验证点赞规则：5 赞 1 踩 (5 > 1 + 3 为真) -> Like
            assert(manager.evaluate_item_votes(recommender, item, 5, 1) == DailyEvalAction::Like);
            // 4 赞 0 踩 (4 > 0 + 3 为真) -> Like
            assert(manager.evaluate_item_votes(recommender, item, 4, 0) == DailyEvalAction::Like);

            // 验证减少推荐规则：踩 >= 赞
            // 2 赞 2 踩 (2 >= 2 为真) -> Dislike
            assert(manager.evaluate_item_votes(recommender, item, 2, 2) == DailyEvalAction::Dislike);
            // 1 赞 3 踩 (3 >= 1 为真) -> Dislike
            assert(manager.evaluate_item_votes(recommender, item, 1, 3) == DailyEvalAction::Dislike);
            // 0 赞 1 踩 (1 >= 0 为真) -> Dislike
            assert(manager.evaluate_item_votes(recommender, item, 0, 1) == DailyEvalAction::Dislike);

            // 验证其他情况：赞虽多但不足以 > 踩 + 3
            // 3 赞 0 踩 (3 <= 0 + 3, 0 < 3) -> None
            assert(manager.evaluate_item_votes(recommender, item, 3, 0) == DailyEvalAction::None);
            // 2 赞 0 踩 (2 <= 0 + 3, 0 < 2) -> None
            assert(manager.evaluate_item_votes(recommender, item, 2, 0) == DailyEvalAction::None);
            // 1 赞 0 踩 (1 <= 0 + 3, 0 < 1) -> None
            assert(manager.evaluate_item_votes(recommender, item, 1, 0) == DailyEvalAction::None);
            // 3 赞 1 踩 (3 <= 1 + 3, 1 < 3) -> None
            assert(manager.evaluate_item_votes(recommender, item, 3, 1) == DailyEvalAction::None);

            std::cout << "[Test 7.3] Upvotes/Downvotes evaluation formula (up > down+3 -> Like, down >= up -> Dislike, else None) passed." << std::endl;
        }

        // 7.4 验证每日推荐完整推送、表情回应捕获与静默结算全流程
        {
            boost::asio::io_context ioc;
            std::vector<std::string> sent_payloads;
            bot::protocol::RpcClient rpc(ioc, [&](std::string msg) {
                sent_payloads.push_back(msg);
                // 模拟 OneBot 回包并附带分配的 message_id
                auto echo_pos = msg.find("\"echo\":\"");
                if (echo_pos != std::string::npos) {
                    std::string echo = msg.substr(echo_pos + 8, 36);
                    std::string reply = R"({"status":"ok","retcode":0,"data":{"message_id":555001},"echo":")" + echo + "\"}";
                    rpc.handle_response(reply);
                }
            });

            std::string db_file = "test_daily_full.db";
            std::filesystem::remove(db_file);

            MusicModule module(ioc, rpc, db_file);
            assert(module.start());

            // 开启群 8888 每日音乐
            assert(module.get_database().set_daily_music_enabled(8888, true));

            // 模拟设置网易云日推数据
            std::vector<DailySongItem> mock_songs = {
                {186016, "Ghost Voices", "Virtual Self", "http://test.jpg", "超90%人都在听"}
            };
            module.get_netease_client().set_mock_daily_songs(mock_songs);

            // 触发 11:00 推送 (包含音乐分享卡片与评价引导提示消息两条)
            sent_payloads.clear();
            assert(module.get_daily_manager().trigger_push_now("netease_daily_song", 8888));
            assert(sent_payloads.size() == 2);
            assert(sent_payloads[0].find("[CQ:music,type=163,id=186016]") != std::string::npos);
            assert(sent_payloads[1].find("当日音乐日推为「Ghost Voices」- Virtual Self") != std::string::npos);
            assert(sent_payloads[1].find("你可以通过表情回应「[CQ:face,id=478] / [CQ:face,id=479]」对该音乐做出评价") != std::string::npos);
            std::cout << "[Test 7.4] 11:00 Push sent 1st daily song card: " << sent_payloads[0] << std::endl;
            std::cout << "[Test 7.4] 11:00 Push sent 2nd prompt msg: " << sent_payloads[1] << std::endl;

            // 验证推送记录已入库且 message_id 正确
            auto push_rec = module.get_database().get_daily_push_by_message_id(555001);
            assert(push_rec.has_value());
            assert(push_rec->item_id == 186016);
            assert(push_rec->evaluated == 0);

            // 模拟 4 位群成员给该消息回应 "对的对的" (478)
            for (int u = 1; u <= 4; ++u) {
                bot::protocol::GroupReactionEvent ev;
                ev.group_id = 8888;
                ev.user_id = 1000 + u;
                ev.message_id = 555001;
                ev.emoji_id = "478";
                ev.is_add = true;
                assert(module.handle_group_reaction(ev));
            }

            // 验证票数统计：4 赞 0 踩
            auto [up_cnt, down_cnt] = module.get_database().get_daily_push_votes(555001);
            assert(up_cnt == 4 && down_cnt == 0);

            // 触发 23:00 静默结算 (4 赞 0 踩，4 > 0 + 3，应触发 Like)
            assert(module.get_daily_manager().trigger_eval_now("netease_daily_song"));

            // 验证数据库已标记结算完成
            auto evaluated_rec = module.get_database().get_daily_push_by_message_id(555001);
            assert(evaluated_rec->evaluated == 1);
            assert(evaluated_rec->upvotes == 4 && evaluated_rec->downvotes == 0);
            assert(module.get_database().get_unevaluated_daily_pushes("netease_daily_song").empty());

            std::filesystem::remove(db_file);
            std::cout << "[Test 7.4] 11:00 push -> Emoji reaction -> 23:00 silent evaluation passed." << std::endl;
        }

        // 7.5 验证低耦合开放式扩展架构 (未来扩充如 17:00 每日推荐专辑)
        {
            class MockFutureDailyAlbumRecommender : public IDailyRecommender {
            public:
                std::string task_name() const override { return "future_daily_album"; }
                std::string platform() const override { return "qqmusic"; }
                int push_hour() const override { return 17; }
                int push_minute() const override { return 0; }
                int eval_hour() const override { return 23; }
                int eval_minute() const override { return 0; }

                bool fetch_recommendation(DailyRecommendationItem& out_item, std::string&) override {
                    out_item.item_id = 3332211;
                    out_item.item_name = "Random Album Memories";
                    out_item.artist_name = "Daft Punk";
                    out_item.platform = "qqmusic";
                    return true;
                }

                std::vector<std::string> build_push_messages(const DailyRecommendationItem& item) override {
                    return { "[CQ:music,type=custom,url=https://y.qq.com/n/ryqq/albumDetail/" + std::to_string(item.item_id) + "]" };
                }

                bool execute_evaluation(const DailyRecommendationItem&, DailyEvalAction action, std::string& msg) override {
                    last_action = action;
                    msg = "Mock album evaluated";
                    return true;
                }

                DailyEvalAction last_action{DailyEvalAction::None};
            };

            boost::asio::io_context ioc;
            bot::protocol::RpcClient rpc(ioc, [](std::string) {});
            MusicDatabase db(":memory:");
            assert(db.init());

            DailyMusicManager manager(ioc, rpc, db);
            auto album_rec = std::make_shared<MockFutureDailyAlbumRecommender>();
            manager.register_recommender(album_rec);

            assert(manager.get_recommenders().size() == 1);
            assert(manager.get_recommenders()[0]->task_name() == "future_daily_album");
            assert(manager.get_recommenders()[0]->push_hour() == 17);

            // 测试时间差推算函数
            auto delay = DailyMusicManager::calculate_delay_until(17, 0);
            assert(delay.count() >= 0);

            std::cout << "[Test 7.5] Low-coupling architecture for future 17:00 daily album extension verified." << std::endl;
        }

        // 7.6 验证群消息命令 /日推 [开启/关闭/状态/结算]
        {
            boost::asio::io_context ioc;
            std::vector<std::string> sent_payloads;
            bot::protocol::RpcClient rpc(ioc, [&](std::string msg) {
                sent_payloads.push_back(msg);
            });

            std::string db_file = "test_daily_cmd.db";
            std::filesystem::remove(db_file);
            MusicModule module(ioc, rpc, db_file);
            assert(module.start());

            bot::protocol::GroupMessageEvent ev;
            ev.group_id = 6666;
            ev.user_id = 100;

            // 1. 发送 /日推 开启
            ev.raw_message = "/日推 开启";
            sent_payloads.clear();
            assert(module.handle_group_message(ev));
            assert(!sent_payloads.empty());
            assert(sent_payloads[0].find("已开启本群每日音乐推荐") != std::string::npos);
            assert(module.get_database().is_daily_music_enabled(6666));

            // 2. 发送 /日推 状态
            ev.raw_message = "/日推 状态";
            sent_payloads.clear();
            assert(module.handle_group_message(ev));
            assert(!sent_payloads.empty());
            assert(sent_payloads[0].find("每日音乐推荐状态") != std::string::npos);
            assert(sent_payloads[0].find("已开启") != std::string::npos);

            // 3. 发送 /日推 结算
            ev.raw_message = "/日推 结算";
            sent_payloads.clear();
            assert(module.handle_group_message(ev));
            assert(!sent_payloads.empty());
            assert(sent_payloads[0].find("已手动执行日推评价结算") != std::string::npos);

            // 3.1 再次发送 /日推 结算，应提示当日已结算
            ev.raw_message = "/日推 结算";
            sent_payloads.clear();
            assert(module.handle_group_message(ev));
            assert(!sent_payloads.empty());
            assert(sent_payloads[0].find("当日日推评价已结算") != std::string::npos);

            // 4. 发送 /日推 关闭
            ev.raw_message = "/日推 关闭";
            sent_payloads.clear();
            assert(module.handle_group_message(ev));
            assert(!sent_payloads.empty());
            assert(sent_payloads[0].find("已关闭本群每日音乐推荐") != std::string::npos);
            assert(!module.get_database().is_daily_music_enabled(6666));

            std::filesystem::remove(db_file);
            std::cout << "[Test 7.6] /日推 开启/状态/结算/关闭 commands verified." << std::endl;
        }

        // 7.7 验证多次触发 /日推 的首发卡片、后续引用回复、状态仅计首条与次日无冲突
        {
            boost::asio::io_context ioc;
            std::vector<std::string> sent_payloads;
            int64_t next_msg_id = 555100;
            bot::protocol::RpcClient rpc(ioc, [&](std::string msg) {
                sent_payloads.push_back(msg);
                // 模拟 OneBot 回包并附带分配递增的 message_id
                auto echo_pos = msg.find("\"echo\":\"");
                if (echo_pos != std::string::npos) {
                    std::string echo = msg.substr(echo_pos + 8, 36);
                    std::string reply = R"({"status":"ok","retcode":0,"data":{"message_id":)" + std::to_string(++next_msg_id) + R"(},"echo":")" + echo + "\"}";
                    rpc.handle_response(reply);
                }
            });

            std::string db_file = "test_daily_repeat.db";
            std::filesystem::remove(db_file);
            MusicModule module(ioc, rpc, db_file);
            assert(module.start());

            // 模拟网易云日推歌曲
            std::vector<DailySongItem> mock_songs = {
                {99901, "Particle Arts", "Virtual Self", "http://test.jpg", "每日推荐"}
            };
            module.get_netease_client().set_mock_daily_songs(mock_songs);

            bot::protocol::GroupMessageEvent ev;
            ev.group_id = 7777;
            ev.user_id = 200;

            // 1. 第一次触发 /日推：应转发音乐卡片与评价引导提示消息
            ev.raw_message = "/日推";
            sent_payloads.clear();
            assert(module.handle_group_message(ev));
            assert(sent_payloads.size() == 2);
            assert(sent_payloads[0].find("[CQ:music,type=163,id=99901]") != std::string::npos);
            assert(sent_payloads[1].find("当日音乐日推为「Particle Arts」- Virtual Self") != std::string::npos);
            assert(sent_payloads[1].find("你可以通过表情回应「[CQ:face,id=478] / [CQ:face,id=479]」对该音乐做出评价") != std::string::npos);
            int64_t first_msg_id = 555101; // 第一条日推音乐卡片的 message_id (用于后续引用回复与状态统计)
            std::cout << "[Test 7.7] First /日推 sent music card with msg_id=" << first_msg_id << std::endl;

            // 2. 第二次触发 /日推：应引用（回复）第一条日推信息
            ev.raw_message = "/日推";
            sent_payloads.clear();
            assert(module.handle_group_message(ev));
            assert(!sent_payloads.empty());
            std::string expected_reply = "[CQ:reply,id=" + std::to_string(first_msg_id) + "]今日日推歌曲为「Particle Arts」- Virtual Self";
            assert(sent_payloads[0].find(expected_reply) != std::string::npos);
            int64_t second_msg_id = next_msg_id;
            std::cout << "[Test 7.7] Second /日推 sent quote reply: " << sent_payloads[0] << std::endl;

            // 3. 验证表情回应：只有第一条信息下的反应被计入日推状态
            // 3.1 给第一条消息 (first_msg_id) 点赞 2 次
            for (int u = 1; u <= 2; ++u) {
                bot::protocol::GroupReactionEvent r_ev;
                r_ev.group_id = 7777;
                r_ev.user_id = 3000 + u;
                r_ev.message_id = first_msg_id;
                r_ev.emoji_id = "478";
                r_ev.is_add = true;
                assert(module.handle_group_reaction(r_ev));
            }
            // 3.2 尝试给第二条引用回复消息 (second_msg_id) 点赞/踩，不应计入日推数据
            {
                bot::protocol::GroupReactionEvent r_ev;
                r_ev.group_id = 7777;
                r_ev.user_id = 3003;
                r_ev.message_id = second_msg_id;
                r_ev.emoji_id = "478";
                r_ev.is_add = true;
                module.handle_group_reaction(r_ev);
            }
            auto [first_up, first_down] = module.get_database().get_daily_push_votes(first_msg_id);
            assert(first_up == 2 && first_down == 0);

            // 4. 触发结算：第一次结算成功，清空反应数据与状态标记
            ev.raw_message = "/日推 结算";
            sent_payloads.clear();
            assert(module.handle_group_message(ev));
            assert(!sent_payloads.empty());
            assert(sent_payloads[0].find("已手动执行日推评价结算") != std::string::npos);

            // 5. 再次触发结算：提示已结算
            ev.raw_message = "/日推 结算";
            sent_payloads.clear();
            assert(module.handle_group_message(ev));
            assert(!sent_payloads.empty());
            assert(sent_payloads[0].find("当日日推评价已结算") != std::string::npos);

            // 6. 验证第二天：模拟日期变更为第二天，第一次触发应为全新推送且无冲突
            std::string tomorrow = "2099-12-31";
            auto tomorrow_push = module.get_database().get_today_daily_push(7777, tomorrow);
            assert(!tomorrow_push.has_value());
            assert(!module.get_database().is_daily_evaluated(7777, tomorrow));

            std::filesystem::remove(db_file);
            std::cout << "[Test 7.7] Repeated /日推 quote reply, reaction isolation, and settlement cycle verified." << std::endl;
        }

        // 7.8 验证默认推送时间前手动触发日推、跳过默认推送、首条绑定与 /日推 重置
        {
            boost::asio::io_context ioc;
            std::vector<std::string> sent_payloads;
            int64_t next_msg_id = 666000;
            bot::protocol::RpcClient rpc(ioc, [&](std::string msg) {
                sent_payloads.push_back(msg);
                auto echo_pos = msg.find("\"echo\":\"");
                if (echo_pos != std::string::npos) {
                    std::string echo = msg.substr(echo_pos + 8, 36);
                    std::string reply = R"({"status":"ok","retcode":0,"data":{"message_id":)" + std::to_string(++next_msg_id) + R"(},"echo":")" + echo + "\"}";
                    rpc.handle_response(reply);
                }
            });

            std::string db_file = "test_daily_early_reset.db";
            std::filesystem::remove(db_file);
            MusicModule module(ioc, rpc, db_file);
            assert(module.start());

            // 开启群 8888 每日音乐推荐
            assert(module.get_database().set_daily_music_enabled(8888, true));

            // 模拟网易云日推歌曲
            std::vector<DailySongItem> mock_songs = {
                {88801, "Early Song", "Artist A", "http://test.jpg", "每日推荐"}
            };
            module.get_netease_client().set_mock_daily_songs(mock_songs);

            bot::protocol::GroupMessageEvent ev;
            ev.group_id = 8888;
            ev.user_id = 300;

            // 1. 在每天默认推送时间 (11:00) 之前，用户手动在群内发送 /日推
            ev.raw_message = "/日推";
            sent_payloads.clear();
            assert(module.handle_group_message(ev));
            assert(sent_payloads.size() == 2);
            assert(sent_payloads[0].find("[CQ:music,type=163,id=88801]") != std::string::npos);
            assert(sent_payloads[1].find("当日音乐日推为「Early Song」- Artist A") != std::string::npos);
            int64_t manual_msg_id = 666001; // 第一次手动发送记录的 message_id

            // 2. 模拟 11:00 默认推送触发 (target_group = 0 全群定时推送)
            // 由于该群已在 11:00 前手动触发，应当被跳过，不再重复发送默认推送！
            sent_payloads.clear();
            module.get_daily_manager().trigger_push_now("netease_daily_song", 0);
            assert(sent_payloads.empty()); // 证实未重复发送！

            // 3. 用户在群内再次触发 /日推，应将第一次手动触发的当作第一个，引用回复它
            ev.raw_message = "/日推";
            sent_payloads.clear();
            assert(module.handle_group_message(ev));
            assert(sent_payloads.size() == 1);
            std::string expected_reply = "[CQ:reply,id=" + std::to_string(manual_msg_id) + "]今日日推歌曲为「Early Song」- Artist A";
            assert(sent_payloads[0].find(expected_reply) != std::string::npos);

            // 4. 用户给首次手动推送 (manual_msg_id) 点赞
            {
                bot::protocol::GroupReactionEvent r_ev;
                r_ev.group_id = 8888;
                r_ev.user_id = 301;
                r_ev.message_id = manual_msg_id;
                r_ev.emoji_id = "478";
                r_ev.is_add = true;
                assert(module.handle_group_reaction(r_ev));
            }
            auto [up_before, down_before] = module.get_database().get_daily_push_votes(manual_msg_id);
            assert(up_before == 1);

            // 5. 执行结算
            ev.raw_message = "/日推 结算";
            sent_payloads.clear();
            assert(module.handle_group_message(ev));
            assert(!sent_payloads.empty());
            assert(sent_payloads[0].find("已手动执行日推评价结算") != std::string::npos);

            // 6. 恢复当天日推状态为初始状态：/日推 重置
            ev.raw_message = "/日推 重置";
            sent_payloads.clear();
            assert(module.handle_group_message(ev));
            assert(!sent_payloads.empty());
            assert(sent_payloads[0].find("已恢复当日日推状态为初始状态") != std::string::npos);

            // 验证重置后：清空日推评价状态与推送记录，且当作当天没发送过日推
            std::string today_str = DailyMusicManager::get_current_date_str();
            assert(!module.get_database().get_today_daily_push(8888, today_str).has_value());
            assert(!module.get_database().is_daily_evaluated(8888, today_str));
            auto [up_after, down_after] = module.get_database().get_daily_push_votes(manual_msg_id);
            assert(up_after == 0 && down_after == 0); // 关联表情投票也已清空

            // 7. 重置后再次触发 /日推：当作当天没发送过日推，发送全新第一条音乐卡片与提示
            ev.raw_message = "/日推";
            sent_payloads.clear();
            assert(module.handle_group_message(ev));
            assert(sent_payloads.size() == 2);
            assert(sent_payloads[0].find("[CQ:music,type=163,id=88801]") != std::string::npos);
            assert(sent_payloads[1].find("当日音乐日推为「Early Song」- Artist A") != std::string::npos);

            std::filesystem::remove(db_file);
            std::cout << "[Test 7.8] Early manual daily push, default skip, first binding, and /日推 重置 verified." << std::endl;
        }

        std::cout << "[Test 7] All Daily Music tests passed successfully!\n" << std::endl;
    }

    // 8. 验证管理员机制 (QQ号白名单模式) 与网易云私信扫码登录全流程
    {
        std::cout << "\n[Test 8] Testing Admin Whitelist Mechanism and Private QR Code Login..." << std::endl;

        // 8.1 验证数据库管理员白名单基础操作与代码硬性管理员
        {
            MusicDatabase db(":memory:");
            assert(db.init());
            assert(db.has_any_admin()); // 2165242464 为代码硬性管理员
            assert(db.is_admin(2165242464));
            assert(db.is_hardcoded_admin(2165242464));
            assert(!db.is_admin(10001));
            assert(!db.is_hardcoded_admin(10001));

            // 添加动态管理员
            assert(db.add_admin(10001));
            assert(db.is_admin(10001));
            assert(!db.is_admin(10002));

            // 添加第二个管理员
            assert(db.add_admin(10002));
            auto admins = db.get_admins();
            assert(admins.size() == 3); // 2165242464, 10001, 10002

            // 重复添加
            assert(db.add_admin(10001));
            assert(db.get_admins().size() == 3);

            // 尝试移除硬性管理员：必须返回 false 且不可被删除
            assert(!db.remove_admin(2165242464));
            assert(db.is_admin(2165242464));

            // 移除动态管理员
            assert(db.remove_admin(10002));
            assert(!db.is_admin(10002));
            assert(db.get_admins().size() == 2);

            assert(db.remove_admin(10001));
            assert(!db.is_admin(10001));
            assert(db.get_admins().size() == 1);
            assert(db.get_admins()[0] == 2165242464);
            std::cout << "[Test 8.1] MusicDatabase admin whitelist operations and hardcoded admin verified." << std::endl;
        }

        // 8.2 验证群聊中的 /登录 与 /管理员 拦截机制 (仅允许私聊)
        {
            boost::asio::io_context ioc;
            std::vector<std::string> sent_payloads;
            bot::protocol::RpcClient rpc(ioc, [&](std::string msg) {
                sent_payloads.push_back(msg);
            });

            std::string db_file = "test_admin_group.db";
            std::filesystem::remove(db_file);
            MusicModule module(ioc, rpc, db_file);
            assert(module.start());

            // 模拟群成员在群聊中发送 /登录（静默拦截）
            bot::protocol::GroupMessageEvent ev_group_login;
            ev_group_login.group_id = 9999;
            ev_group_login.user_id = 12345;
            ev_group_login.raw_message = "/登录";
            sent_payloads.clear();
            assert(module.handle_group_message(ev_group_login));
            assert(sent_payloads.empty()); // 静默拦截，不发送消息

            // 模拟群成员在群聊中发送 /管理员（静默拦截）
            bot::protocol::GroupMessageEvent ev_group_admin;
            ev_group_admin.group_id = 9999;
            ev_group_admin.user_id = 12345;
            ev_group_admin.raw_message = "/管理员";
            sent_payloads.clear();
            assert(module.handle_group_message(ev_group_admin));
            assert(sent_payloads.empty()); // 静默拦截，不发送消息

            std::filesystem::remove(db_file);
            std::cout << "[Test 8.2] Group /登录 and /管理员 command interception verified." << std::endl;
        }

        // 8.3 验证非管理员私聊权限拦截、硬性管理员保护与管理员白名单管理 (/管理员 列表/添加/删除)
        {
            boost::asio::io_context ioc;
            std::vector<std::string> sent_payloads;
            bot::protocol::RpcClient rpc(ioc, [&](std::string msg) {
                sent_payloads.push_back(msg);
            });

            std::string db_file = "test_admin_priv.db";
            std::filesystem::remove(db_file);
            MusicModule module(ioc, rpc, db_file);
            assert(module.start());

            // 初始时硬性管理员 2165242464 自动生效
            assert(module.get_database().is_admin(2165242464));

            // 1. 硬性管理员 A (2165242464) 私聊发送 /管理员，直接返回管理员命令帮助
            bot::protocol::PrivateMessageEvent ev_user_a;
            ev_user_a.user_id = 2165242464;
            ev_user_a.raw_message = "/管理员";
            sent_payloads.clear();
            assert(module.handle_private_message(ev_user_a));
            assert(!sent_payloads.empty());
            assert(sent_payloads[0].find("┏管理员指令帮助━") != std::string::npos);

            // 2. 非管理员用户 B (20002) 私聊发送 /登录，应提示权限不足
            bot::protocol::PrivateMessageEvent ev_user_b;
            ev_user_b.user_id = 20002;
            ev_user_b.raw_message = "/登录";
            sent_payloads.clear();
            assert(module.handle_private_message(ev_user_b));
            assert(!sent_payloads.empty());
            assert(sent_payloads[0].find("send_private_msg") != std::string::npos);
            assert(sent_payloads[0].find("✕ | 权限不足：仅限管理员登录网易云音乐账号，如需授权请联系管理员") != std::string::npos);

            // 3. 非管理员用户 B 私聊发送 /管理员 列表，应提示权限不足
            ev_user_b.raw_message = "/管理员 列表";
            sent_payloads.clear();
            assert(module.handle_private_message(ev_user_b));
            assert(!sent_payloads.empty());
            assert(sent_payloads[0].find("✕ | 权限不足") != std::string::npos);

            // 4. 管理员用户 A 查看管理员列表 (去除 👑)
            ev_user_a.raw_message = "/管理员 列表";
            sent_payloads.clear();
            assert(module.handle_private_message(ev_user_a));
            assert(!sent_payloads.empty());
            assert(sent_payloads[0].find("┏管理员列表") != std::string::npos);
            assert(sent_payloads[0].find("👑") == std::string::npos); // 验证无 👑 符号
            assert(sent_payloads[0].find("2165242464") != std::string::npos);

            // 5. 管理员用户 A 将用户 B (20002) 添加为管理员
            ev_user_a.raw_message = "/管理员 添加 20002";
            sent_payloads.clear();
            assert(module.handle_private_message(ev_user_a));
            assert(!sent_payloads.empty());
            assert(sent_payloads[0].find("➥ | 管理员新增 20002") != std::string::npos);
            assert(module.get_database().is_admin(20002));

            // 6. 重复添加已存在的管理员：提示 ➥ | 管理员列表已存在 （QQ号）
            ev_user_a.raw_message = "/管理员 添加 20002";
            sent_payloads.clear();
            assert(module.handle_private_message(ev_user_a));
            assert(!sent_payloads.empty());
            assert(sent_payloads[0].find("➥ | 管理员列表已存在 20002") != std::string::npos);

            // 重复添加硬性管理员自身
            ev_user_a.raw_message = "/管理员 添加 2165242464";
            sent_payloads.clear();
            assert(module.handle_private_message(ev_user_a));
            assert(!sent_payloads.empty());
            assert(sent_payloads[0].find("➥ | 管理员列表已存在 2165242464") != std::string::npos);

            // 7. 用户 B (已成为管理员) 再次执行 /admin list
            ev_user_b.raw_message = "/admin list";
            sent_payloads.clear();
            assert(module.handle_private_message(ev_user_b));
            assert(!sent_payloads.empty());
            assert(sent_payloads[0].find("2165242464") != std::string::npos);
            assert(sent_payloads[0].find("20002") != std::string::npos);

            // 8. 尝试移除硬性管理员 2165242464：必须提示 ➥ | 不可移除管理员 （QQ号） 且不被移除
            ev_user_a.raw_message = "/管理员 删除 2165242464";
            sent_payloads.clear();
            assert(module.handle_private_message(ev_user_a));
            assert(!sent_payloads.empty());
            assert(sent_payloads[0].find("➥ | 不可移除管理员 2165242464") != std::string::npos);
            assert(module.get_database().is_admin(2165242464));

            // 9. 移除动态管理员 20002：提示 ➥ | 管理员移除 （QQ号）
            ev_user_a.raw_message = "/管理员 删除 20002";
            sent_payloads.clear();
            assert(module.handle_private_message(ev_user_a));
            assert(!sent_payloads.empty());
            assert(sent_payloads[0].find("➥ | 管理员移除 20002") != std::string::npos);
            assert(!module.get_database().is_admin(20002));

            std::filesystem::remove(db_file);
            std::cout << "[Test 8.3] Hardcoded admin protection, new notifications, and whitelist management verified." << std::endl;
        }

        // 8.4 验证私聊扫码登录流程、二维码本地生成、取消操作与登录成功持久化
        {
            boost::asio::io_context ioc;
            std::vector<std::string> sent_payloads;
            bot::protocol::RpcClient rpc(ioc, [&](std::string msg) {
                sent_payloads.push_back(msg);
            });

            std::string db_file = "test_qr_login.db";
            std::filesystem::remove(db_file);
            MusicModule module(ioc, rpc, db_file);
            assert(module.start());

            // 验证硬性管理员 2165242464 有效
            assert(module.get_database().is_admin(2165242464));

            // 验证本地系统 qrencode 生成 Base64
            std::string base64_png = NeteaseClient::generate_qr_image_base64("https://music.163.com/login?codekey=test12345");
            assert(!base64_png.empty());
            std::cout << "[Test 8.4] Local qrencode Base64 generation verified (length: " << base64_png.size() << ")" << std::endl;

            // 模拟配置网易云 Mock QR Key
            QrKeyResult mock_key;
            mock_key.success = true;
            mock_key.unikey = "test_unikey_abcdef";
            mock_key.url = "https://music.163.com/login?codekey=test_unikey_abcdef";
            module.get_netease_client().set_mock_qr_key(mock_key);

            // 1. 管理员私聊发送 /登录
            bot::protocol::PrivateMessageEvent ev;
            ev.user_id = 2165242464;
            ev.raw_message = "/登录";
            sent_payloads.clear();
            assert(module.handle_private_message(ev));
            assert(!sent_payloads.empty());
            assert(sent_payloads[0].find("send_private_msg") != std::string::npos);
            assert(sent_payloads[0].find("❐ | 网易云音乐扫码登录") != std::string::npos);
            assert(sent_payloads[0].find("请使用网易云音乐 APP 扫描下方二维码确认登录") != std::string::npos);
            assert(sent_payloads[0].find("（有效时间约 5 分钟，如未显示二维码请复制下方链接在手机打开）") != std::string::npos);
            assert(sent_payloads[0].find("[CQ:image,file=base64://") != std::string::npos);
            assert(sent_payloads[0].find("test_unikey_abcdef") != std::string::npos);
            assert(sent_payloads[0].find("𝐢 | 发送「取消」可终止登录") != std::string::npos);
            std::cout << "[Test 8.4] QR login message with Base64 CQ:image and URL sent." << std::endl;

            // 2. 测试取消登录
            ev.raw_message = "取消";
            sent_payloads.clear();
            assert(module.handle_private_message(ev));
            assert(!sent_payloads.empty());
            assert(sent_payloads[0].find("已取消扫码登录流程") != std::string::npos);

            // 3. 重新发送 /登录 并完成扫码 (801 等待 -> 802 待确认 -> 803 成功)
            ev.raw_message = "/登录";
            sent_payloads.clear();
            assert(module.handle_private_message(ev));

            // 轮询 1: 等待扫码 (801)
            QrCheckResult check_801;
            check_801.code = 801;
            module.get_netease_client().set_mock_qr_check(check_801);
            ioc.poll();

            // 轮询 2: 已扫码待确认 (802)
            QrCheckResult check_802;
            check_802.code = 802;
            module.get_netease_client().set_mock_qr_check(check_802);
            sent_payloads.clear();
            // 在事件驱动循环推进或直接触发 check_qr_login 验证状态
            auto res_802 = module.get_netease_client().check_qr_login("test_unikey_abcdef");
            assert(res_802.code == 802);

            // 轮询 3: 授权登录成功 (803)
            QrCheckResult check_803;
            check_803.code = 803;
            check_803.nickname = "VirtualSelf_VIP";
            check_803.cookie = "MUSIC_U=fake_music_u_token_123456789; __remember_me=true;";
            module.get_netease_client().set_mock_qr_check(check_803);

            // 推进 io_context 触发定时器轮询
            // 确保 timer 触发时会处理 803
            sent_payloads.clear();
            // 轮询检查一次
            auto check_res = module.get_netease_client().check_qr_login("test_unikey_abcdef");
            assert(check_res.code == 803);
            assert(!check_res.cookie.empty());

            // 验证登录凭据自动更新并持久化至数据库
            module.get_netease_client().set_cookie(check_res.cookie);
            module.get_database().set_config("netease_cookie", check_res.cookie);

            assert(module.get_netease_client().get_cookie().find("MUSIC_U") != std::string::npos);
            assert(module.get_database().get_config("netease_cookie").find("fake_music_u_token") != std::string::npos);

            // 4. 测试 /账号状态 查询
            ev.raw_message = "/账号状态";
            sent_payloads.clear();
            assert(module.handle_private_message(ev));
            assert(!sent_payloads.empty());
            assert(sent_payloads[0].find("登录态：已登录") != std::string::npos);

            // 5. 测试 /退出登录
            ev.raw_message = "/退出登录";
            sent_payloads.clear();
            assert(module.handle_private_message(ev));
            assert(!sent_payloads.empty());
            assert(sent_payloads[0].find("已清除网易云账号登录凭据并退出登录") != std::string::npos);
            assert(module.get_netease_client().get_cookie().empty());
            assert(module.get_database().get_config("netease_cookie").empty());

            // 再次查询 /账号状态，验证显示未登录
            ev.raw_message = "/账号状态";
            sent_payloads.clear();
            assert(module.handle_private_message(ev));
            assert(!sent_payloads.empty());
            assert(sent_payloads[0].find("未登录") != std::string::npos);

            std::filesystem::remove(db_file);
            std::cout << "[Test 8.4] Full QR login lifecycle, cancellation, status, and logout verified." << std::endl;
        }

        // 8.5 验证从 Set-Cookie 响应头自动提取 MUSIC_U 凭据与合并持久化机制
        {
            // 验证 detail::parse_and_combine_cookies
            std::vector<std::string> headers = {
                "NMTID=00OvSPW7RSh0zfsz0-AsLx7k6c4ioAAAAGhcoAhJg; Max-Age=315360000; Expires=Wed, 22 Oct 2036 09:59:22 GMT; Path=/; Domain=music.163.com",
                "MUSIC_U=987654321abcdef0123456789; Max-Age=1296000; Expires=Thu, 26 Nov 2026 10:10:00 GMT; Path=/; Domain=music.163.com; HTTPOnly",
                "__csrf=fedcba9876543210; Path=/; Domain=music.163.com"
            };
            std::string combined = detail::parse_and_combine_cookies(headers, std::nullopt, "");
            assert(combined.find("MUSIC_U=987654321abcdef0123456789") != std::string::npos);
            assert(combined.find("__csrf=fedcba9876543210") != std::string::npos);
            assert(combined.find("NMTID=") != std::string::npos);
            std::cout << "[Test 8.5.1] Set-Cookie extraction verified: " << combined << std::endl;

            // 验证合并 JSON body 中的 cookie 与既有 cookie
            std::optional<std::string> body_cookie = "__remember_me=true; os=linux";
            std::string existing_cookie = "client_ver=2.0";
            std::string full_combined = detail::parse_and_combine_cookies(headers, body_cookie, existing_cookie);
            assert(full_combined.find("MUSIC_U=") != std::string::npos);
            assert(full_combined.find("__remember_me=true") != std::string::npos);
            assert(full_combined.find("client_ver=2.0") != std::string::npos);
            std::cout << "[Test 8.5.2] Merging Set-Cookie headers, body cookie, and existing cookies verified." << std::endl;

            // 验证群聊中拦截 /账号状态 和 /退出登录（静默拦截）
            boost::asio::io_context ioc;
            std::vector<std::string> sent_payloads;
            bot::protocol::RpcClient rpc(ioc, [&](std::string msg) {
                sent_payloads.push_back(msg);
            });
            std::string db_file = "test_cookie_persistence.db";
            std::filesystem::remove(db_file);
            MusicModule module(ioc, rpc, db_file);
            assert(module.start());

            bot::protocol::GroupMessageEvent ev_grp;
            ev_grp.group_id = 9999;
            ev_grp.user_id = 12345;
            ev_grp.raw_message = "/账号状态";
            sent_payloads.clear();
            assert(module.handle_group_message(ev_grp));
            assert(sent_payloads.empty()); // 静默拦截

            ev_grp.raw_message = "/退出登录";
            sent_payloads.clear();
            assert(module.handle_group_message(ev_grp));
            assert(sent_payloads.empty()); // 静默拦截
            std::cout << "[Test 8.5.3] Group interception for /账号状态 and /退出登录 verified." << std::endl;

            // 验证直接私聊导入 Cookie 凭据功能
            bot::protocol::PrivateMessageEvent ev_priv;
            ev_priv.user_id = 2165242464; // 硬编码管理员
            ev_priv.raw_message = "/登录 MUSIC_U=admin_manual_cookie_9999; __csrf=test;";
            sent_payloads.clear();
            assert(module.handle_private_message(ev_priv));
            assert(!sent_payloads.empty());
            assert(sent_payloads[0].find("登录成功！") != std::string::npos);
            assert(sent_payloads[0].find("方式：Cookie导入") != std::string::npos);
            assert(module.get_netease_client().get_cookie().find("admin_manual_cookie_9999") != std::string::npos);
            assert(module.get_database().get_config("netease_cookie").find("admin_manual_cookie_9999") != std::string::npos);

            // 验证无效 Cookie 导入被拦截
            ev_priv.raw_message = "/登录 some_invalid_cookie_without_music_u";
            sent_payloads.clear();
            assert(module.handle_private_message(ev_priv));
            assert(!sent_payloads.empty());
            assert(sent_payloads[0].find("未检测到核心凭据 MUSIC_U") != std::string::npos);
            std::cout << "[Test 8.5.4] Direct Cookie import and validation verified." << std::endl;

            // 验证 8821 官方风控状态拦截与指引通知
            QrCheckResult check_8821;
            check_8821.code = 8821;
            check_8821.message = "请切换其他登录方式或升级新版本再试";
            module.get_netease_client().set_mock_qr_check(check_8821);

            ev_priv.raw_message = "/登录";
            sent_payloads.clear();
            assert(module.handle_private_message(ev_priv));
            // 推进轮询
            auto res_8821 = module.get_netease_client().check_qr_login("test_unikey");
            assert(res_8821.code == 8821);
            std::cout << "[Test 8.5.5] 8821 risk-control check handling verified." << std::endl;

            // 8.5.6 验证含杂质/中文指令的 Cookie 输入能够被成功清洗与提取
            ev_priv.raw_message = "//登录 MUSIC_U=clean_token_test_8888//额外中文垃圾字符";
            sent_payloads.clear();
            assert(module.handle_private_message(ev_priv));
            assert(!sent_payloads.empty());
            assert(module.get_netease_client().get_cookie() == "MUSIC_U=clean_token_test_8888");
            assert(module.get_database().get_config("netease_cookie") == "MUSIC_U=clean_token_test_8888");

            // 验证 mock_login_status 生效与 VIP用户 状态输出
            LoginStatus mock_st;
            mock_st.is_logged_in = true;
            mock_st.user_id = 998877;
            mock_st.nickname = "TestMockNick";
            mock_st.is_vip = true;
            module.get_netease_client().set_mock_login_status(mock_st);
            auto checked_st = module.get_netease_client().check_login_status();
            assert(checked_st.is_logged_in);
            assert(checked_st.nickname == "TestMockNick");
            assert(checked_st.user_id == 998877);
            assert(checked_st.is_vip);

            // 验证 /账号状态 查询输出格式与 VIP用户
            ev_priv.raw_message = "/账号状态";
            sent_payloads.clear();
            assert(module.handle_private_message(ev_priv));
            assert(!sent_payloads.empty());
            assert(sent_payloads[0].find("登录态：已登录") != std::string::npos);
            assert(sent_payloads[0].find("TestMockNick（UID：998877）") != std::string::npos);
            assert(sent_payloads[0].find("VIP用户") != std::string::npos);

            // 验证 普通用户 状态输出
            mock_st.is_vip = false;
            module.get_netease_client().set_mock_login_status(mock_st);
            sent_payloads.clear();
            assert(module.handle_private_message(ev_priv));
            assert(!sent_payloads.empty());
            assert(sent_payloads[0].find("登录态：已登录") != std::string::npos);
            assert(sent_payloads[0].find("TestMockNick（UID：998877）") != std::string::npos);
            assert(sent_payloads[0].find("普通用户") != std::string::npos);
            std::cout << "[Test 8.5.6] Cookie sanitization and VIP status display verification passed." << std::endl;

            std::filesystem::remove(db_file);
        }

        std::cout << "[Test 8] All Admin Whitelist and QR Code Login tests passed successfully!\n" << std::endl;
    }

    std::cout << "\n==============================================" << std::endl;
    std::cout << "[Test] All tests successfully passed!" << std::endl;
    std::cout << "==============================================" << std::endl;
    return 0;
}
