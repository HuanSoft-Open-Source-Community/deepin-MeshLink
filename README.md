# 深度织讯（deepin-MeshLink）

面向 deepin V25 / UOS V25 的局域网去中心化即时通讯客户端。每个节点既是客户端也是服务端：不依赖中心服务器即可完成节点发现、聊天室组建、消息广播与文件按需分发。所有节点间数据传输均经过 TLS 1.3 加密通道。

- 开发主体：[HuanSoft 开源社区](https://huansoft.org.cn)
- 代码仓库：<https://github.com/HuanSoft-Open-Source-Community/deepin-MeshLink>
- 版本：`26.4.0.0` alpha（Debian 包为 `26.4.0.0~alpha1`）
- 技术底座：DTK6 + Qt 6（C++17）+ OpenSSL 3
- 许可：GPL-3.0-or-later（见 [LICENSE](LICENSE)）

## 版本方案

版本号采用 `<yy>.<q>.<x>.<y>`：

- `yy`：两位年份；`q`：季度（1–4），因此前两位合起来表示“年份 + 季度”。
- `x.y`：该季度内的版本序号；alpha 阶段从 `0.0` 起，当前首个 alpha 为 **26.4.0.0**（2026 年第 4 季度）。
- 程序内显示 `26.4.0.0 alpha`；Debian 包版本按预发布语义写为 `26.4.0.0~alpha1`，以便 APT 在正式版（`26.4.0.1` 及以后）到来时正确升级。

## 功能与需求用例对照

| 用例 | 能力 | 代码入口 |
|---|---|---|
| UC-01 | 首次启动向导（昵称 + **局域网网段选择**）、本地账号/密钥对生成、32 位指纹、LAN 地址关联、已完成账号的自动加载 | `src/core/account.cpp`、`src/ui/firststartwizard.cpp` |
| UC-02 | 销毁账号：先向所在聊天室尽力广播离开/解散，再擦除私钥、指纹与（可选）会话缓存 | `src/core/engine.cpp`、`src/core/account.cpp` |
| UC-03 | 昵称（2–32 字符）与头像编辑；头像以内容哈希随心跳广播、按需拉取 | `src/core/account.cpp` |
| UC-04 | 带 Ed25519 签名的 UDP 心跳、在线列表与离线淘汰、告别包、伪造包告警、手动单播引导；心跳只发往并只接受所选网段 | `src/core/discovery.cpp`、`src/core/lansegment.cpp` |
| UC-05 | 创建聊天室（名称/简介/加入策略/容量），生成 **32 位房间指纹 id** 与唯一的 **6 位数字邀请码**，随心跳广播可发现摘要 | `src/core/roommanager.cpp` |
| UC-06 | 发现并加入聊天室（按房间 id 或 **邀请码**）：自由加入 / 需批准；容量已满、黑名单、托管者离线、身份校验失败均有明确结果 | `src/core/roommanager.cpp`、`src/ui/discoverview.cpp` |
| UC-07 | 托管者—管理员—普通成员三级权限；设/撤管理员、禁言（限时/直至解除）、全员禁言、踢出、黑名单 | `src/core/roommanager.cpp`、`src/ui/roomsview.cpp` |
| UC-08 | 文本/图片消息经托管节点中转，托管者分配递增序号；图片二进制不进入消息体，按哈希按需拉取 | `src/core/chatmanager.cpp` |
| UC-09 | 文件公告：流式 SHA-256、仅元信息经消息流分发，不触发文件字节传输 | `src/core/filemanager.cpp` |
| UC-10 | 按需下载：向托管者查询供给节点、选择来源后经 TLS 请求；“不请求不传输” | `src/core/filemanager.cpp`、`src/ui/chatview.cpp` |
| UC-11 | 多线程分片下载、分片进度持久化、暂停/恢复、断点续传、供给节点切换 | `src/core/downloader.cpp` |
| UC-12 | 合并后整文件 SHA-256 校验；按分片哈希定位损坏分片并定向重传，5 轮后失败 | `src/core/downloader.cpp` |
| UC-13 | 退出聊天室；托管者退出即解散并广播关闭 | `src/core/roommanager.cpp`、`src/ui/roomsview.cpp` |

## 安全模型

- **身份**：Ed25519 密钥对，账号指纹 = `SHA-256(Ed25519 公钥 DER)` 的前 32 个十六进制字符。
- **传输**：所有 TCP 连接强制 TLS 1.3（`QSslSocket`），禁止明文 TCP。
- **认证**：自签 ECDSA P-256 证书 CN 绑定账号指纹；TLS 握手后再以带会话随机数的 Ed25519 签名互相证明身份；证书指纹按账号指纹做 TOFU 固定，不一致即中止并告警。
- **心跳**：UDP 心跳附带 Ed25519 签名，接收方用包头公钥验签并核对派生指纹，伪造包静默丢弃并记录。
- **落盘**：私钥/证书私钥经 AES-256-GCM 加密后写入 SQLite，密钥加密密钥由本机因子与随机盐派生。
- **成员/管理帧**：成员侧的加入响应、房间快照、成员事件、解散通知，托管侧的离开/状态同步，均绑定 TLS 通道的已认证对端身份，拒绝载荷内伪造指纹。
- **文件完整性**：TLS 负责传输机密性/完整性，文件级 SHA-256 负责端到端内容正确性；未通过校验的临时文件绝不按正式文件名交付。

## 目录结构

```
src/core/    协议、加密、账号、存储、网段、发现、TLS 通道、房间、消息、文件与下载
src/ui/      DTK6 桌面界面（会话、发现、传输、个人资料、设置、首次启动向导）
src/app/     应用入口
tests/       纯逻辑单测 + 多进程集成场景（UC-01…UC-13、网段切换）
resources/   desktop 文件与图标
debian/      Debian 打包
```

## 构建

依赖：

- CMake ≥ 3.16、C++17 编译器
- Qt 6：Core、Gui、Widgets、Network、Sql、Concurrent
- DTK6：Core、Gui、Widget
- OpenSSL ≥ 1.1.1（TLS 1.3）

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
sudo cmake --install build      # 安装到 /usr
```

## 运行

```bash
deepin-meshlink                 # 图形界面
deepin-meshlink --bootstrap 192.168.1.10:2425   # 广播被防火墙阻断时补一个单播发现目标
deepin-meshlink --headless      # 仅运行核心引擎（排障用）
```

首次启动会弹出账号向导：昵称可留空（自动生成“深度织讯用户_XXXX”），并选择本机所在的**局域网网段**（多网卡/有线+无线/虚拟网卡并存时，心跳只在该网段广播并只接受该网段的心跳）；创建失败时不会进入主界面。数据默认位于 `~/.local/share/HuanSoft/deepin-meshlink`，配置位于 `~/.config/HuanSoft/deepin-meshlink`。

运行中可在“设置 → 局域网网段”随时切换网段（无需重启）：旧网段的在线列表会立即清空，新网段的心跳会在下一个周期重新发现节点；TCP/TLS 监听与已建立的传输不受影响。`MESHLINK_IP` 环境变量可强制固定网段（测试/无头场景），此时界面选择不生效。手动添加的单播 bootstrap 目标只改变心跳的发送方向，不会绕过网段过滤——目标本身也需位于所选网段内。

聊天室创建后会显示 **6 位数字邀请码** 与 **32 位房间指纹 id**；同一网段的成员可在“发现”页输入邀请码加入（局域网内无中心服务器，邀请码通过心跳摘要解析）。

## 默认参数（NFR-06）

| 参数 | 默认值 | 可调位置 |
|---|---|---|
| 局域网网段 | 自动选择（优先非回环 IPv4） | 首次向导 / 设置页 / `MESHLINK_IP` |
| UDP 发现端口 | 2425 | 设置页 / `MESHLINK_DISCOVERY_PORT` |
| TLS 服务端口 | 2426 | 设置页 / `MESHLINK_TCP_PORT` |
| 心跳周期 | 3000 ms | 设置页 / `MESHLINK_HEARTBEAT_MS` |
| 离线判定 | 3 个心跳周期 | 设置页 / `MESHLINK_OFFLINE_PERIODS` |
| 分片大小 | 256 KiB（64–1024 KiB） | 设置页 / `MESHLINK_CHUNK_KB` |
| 下载并行数 | 4（1–16） | 设置页 / `MESHLINK_THREADS` |
| 房间快照消息上限 | 200 | 设置页 |

网段与心跳周期即时生效；端口类参数需要重启应用。

## 测试

```bash
# 纯逻辑：帧格式、序列化、分片数学、加密原语、网段计算、房间 id 迁移
build/tests/tst_logic

# 多进程集成：UC-01…UC-13、按邀请码加入、运行中切换网段（约 3–6 分钟，使用 127.0.0.1 与固定端口）
build/tests/tst_integration
```

集成测试以父进程方式拉起若干节点，通过 stdin/stdout 驱动。**在混合工具链环境**（例如沙箱内使用宿主的 Qt/DTK 库运行自编译二进制）中，需要让父进程用指定加载器启动子节点：

```bash
MESHLINK_LAUNCH_LOADER="/path/to/ld-linux-x86-64.so.2 --library-path /path/to/qt/libs" \
  build/tests/tst_integration
```

普通 deepin/UOS 环境无需该变量。测试相关环境变量：`MESHLINK_DATA_HOME`、`MESHLINK_CONFIG_HOME`、`MESHLINK_IP`、`MESHLINK_LAN_PREFIX`、`MESHLINK_DISCOVERY_PORT`、`MESHLINK_TCP_PORT`、`MESHLINK_BOOTSTRAP`；`MESHLINK_DEBUG=1` 输出协议诊断日志；`MESHLINK_EVIL_CHUNKS` 仅供测试注入损坏分片；`MESHLINK_CHUNK_DELAY_MS` 仅供集成测试放慢分片落盘，便于观察暂停/崩溃等中间状态。以上测试钩子在生产环境均不会启用。

## Debian 打包

```bash
dpkg-buildpackage -b -us -uc
sudo dpkg -i ../deepin-meshlink_26.4.0.0~alpha1_*.deb
```

## 已知限制

- 不实现托管迁移：托管者离线则该聊天室不可用，托管者退出即解散。
- 不提供“仅下载缩略图/预览”（UC-10 A2）与空文件分片下载。
- 私钥加密使用本机因子 + 随机盐派生 KEK，暂未接入内核 keyring / TPM。
- 不使用外部 CA、不在线吊销证书（TOFU 模型）。
- 仅面向 Linux 桌面（deepin/UOS），未做跨平台适配。
- alpha 阶段的线协议为 v2（心跳格式 + 房间快照携带邀请码），不同构建版本之间不保证互通；升级后首次启动会把 1.0.0 的 16 位房间 id 自动重键为 32 位并补邀请码。
- 6 位数字邀请码只在同一托管者内保证唯一；跨托管者碰撞时，“按邀请码加入”会列出当前网段内所有命中的聊天室供选择，不做中心化全局注册。

## 联系

HuanSoft 开源社区 · <https://huansoft.org.cn>
源码与问题跟踪：<https://github.com/HuanSoft-Open-Source-Community/deepin-MeshLink>
