# 项目级开发规范与技术栈约束

## 1. 技术栈约束

- **开发语言**：现代 C++ (C++23, 标准宏 `202302L`)
- **编译器**：Clang 23+ (`clang` / `clang++`)
- **包管理器**：Conan 2.x
  - `boost/1.86.0`（Asio 网络通信、Beast HTTP/WebSocket、UUID）
  - `openssl/3.3.2`（WSS / HTTPS 安全连接）
  - `glaze/8.4.0`（高性能编译期反射 JSON 库）
  - `sqlite3/3.47.2`（Conan 官方托管的 SQLite 关系型数据库）
  - `libcurl`（网络 HTTP 客户端）
- **构建系统**：CMake 3.20+ 与 CMakePresets (`conan-release`)
- **协议端**：NapCatQQ（OneBot 11 协议标准，通过 Docker 容器化运行）

---

## 2. 工程目录结构规范

```text
qqbot-cpp/
├── .antigravity/
│   └── rules.md             # 项目级开发规范与技术栈约束（极关键）
├── CMakeLists.txt           # 构建入口与依赖链接
├── CMakeUserPresets.json   # Conan 自动生成的预设 (git 忽略)
├── conanfile.txt            # Conan 依赖配置
├── docker-compose.yml       # 协议端 (NapCatQQ) 编排与端口映射
├── data/                    # 本地持久化数据存储目录 (SQLite、缓存)
├── include/
│   └── bot/
│       ├── core/            # WebSocket Session、RPC 异步调度器、Echo 追踪
│       ├── protocol/        # OneBot V11 事件结构体与反序列化 (JSON)
│       └── modules/         # 业务模块插件头文件
│           └── music/       # 网易云音乐订阅与新歌监控
├── src/
│   ├── core/                # WebSocket 服务端与底层网络实现
│   ├── protocol/            # RPC 客户端与 Echo 唤醒实现
│   ├── modules/             # 各业务模块源文件实现
│   │   └── music/           # 音乐模块实现 (SQLite / NetEase API)
│   └── main.cpp             # 机器人入口、信号注册与事件消费流水线
├── tests/                   # 单元测试与接口集成验证测试
└── napcat/                  # NapCat 运行数据与 QQ 登录凭据 (git 忽略)
```

---

## 3. 命名空间命名约定

- **核心基础设施**：`namespace bot::core`
- **协议与反序列化**：`namespace bot::protocol`
- **业务功能模块**：`namespace bot::module::<module_name>`
  - 音乐业务模块：`namespace bot::module::music`

---

## 4. 核心开发守则与避坑指南

1. **队头阻塞与 RPC 前置拦截**：
   - 所有的 OneBot 11 API 请求（带有 `echo` 字段）在收到回包时，必须通过 `WsServer::set_message_interceptor` 在底层第一时间由 `RpcClient::handle_response` 拦截并唤醒挂起的协程。
   - 严禁将回包与普通业务事件混入同一个阻塞等待通道，否则会导致协程死锁并超时。
2. **事件处理并发化**：
   - 主消费循环从 `dispatch_channel` 取出群消息后，业务分发必须通过 `net::co_spawn(executor, ..., net::detached)` 启动独立协程，确保接收循环永远非阻塞。
3. **Glaze 编译期反射规则**：
   - 必须配置 `glz::opts{.error_on_unknown_keys = false}`，以宽容忽略 OneBot 11 的多余元字段。
   - 被 Glaze 反射的数据模型结构体**绝对不能**放在匿名命名空间（`namespace { ... }`）中，必须具备 External Linkage（例如放在 `namespace bot::module::music::detail` 中）。
4. **数据库与持久化**：
   - 所有数据库文件必须存放在 `data/` 目录下（如 `data/bot_music.db`）。
   - 必须使用参数化预编译绑定（`sqlite3_prepare_v2` / `sqlite3_bind_*`），杜绝拼接 SQL。

## 5. 代码规范与生命周期要求
1. 内存管理: 严禁裸指针，严格使用智能指针与 RAII。
2. 异常与错误: 网络断开和 JSON 解析失败必须捕获，不可导致进程崩溃。
3. RPC 契约: 调用 NapCat API 时必须分配唯一 `echo` 字符串，通过异步 Promise / 挂起句柄跟踪响应结果。
4. 业务隔离: 新功能以独立 Module 类形式实现，在 main 中通过路由注册。
5. 代码规范：使用现代C++语法规范，且尽可能使用新标准C++的语法糖
6. 尽可能地去考虑使用算法来进行性能上的优化