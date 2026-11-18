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
| UC-02 | 销毁账号：先向所在聊天室尽力广播离开/解散，再擦除私钥、指纹与（可选）会话缓存；销毁最后一个账号后应用内返回首次启动向导创建新身份（不退出应用），仍有其他账号时自动切换到剩余账号并恢复网络 | `src/core/engine.cpp`、`src/core/account.cpp`、`src/ui/profilepage.cpp`、`src/ui/mainwindow.cpp` |
| UC-03 | 昵称（2–32 字符）与头像编辑；头像以内容哈希随心跳广播、按需拉取 | `src/core/account.cpp`、`src/ui/profilepage.cpp` |
| UC-04 | 带 Ed25519 签名的 UDP 心跳、在线列表与离线淘汰、告别包、伪造包告警、手动单播引导；心跳只发往并只接受所选网段 | `src/core/discovery.cpp`、`src/core/lansegment.cpp` |
| UC-05 | 创建聊天室（名称/简介/加入策略/容量；名称留空时自动生成随机名称，源语言规则为 `deepin-MeshLink room_XXXX`，中文界面为“深度织讯聊天室_XXXX”，与默认昵称同风格；创建成功对话框会显示实际房间名），生成 **32 位房间指纹 id** 与唯一的 **6 位数字邀请码**，随心跳广播可发现摘要 | `src/core/roommanager.cpp`、`src/ui/roomsview.cpp` |
| UC-06 | 发现并加入聊天室（按房间 id 或 **邀请码**）：自由加入 / 需批准；容量已满、黑名单、托管者离线、身份校验失败均有明确结果 | `src/core/roommanager.cpp`、`src/ui/discoverview.cpp` |
| UC-07 | 托管者—管理员—普通成员三级权限；设/撤管理员、禁言（限时/直至解除）、全员禁言、踢出、黑名单；管理层可撤回任意消息。会话页与“管理聊天室”的成员页均可右键成员（头像/昵称/成员行）打开同一成员菜单：@提及、设/撤管理员（仅托管者可操作）、禁言（10 分钟/1 小时/直至解除）、解除禁言、移出聊天室；管理页保留双击管理入口 | `src/core/roommanager.cpp`、`src/ui/roomsview.cpp`、`src/ui/membermenu.cpp`、`src/core/chatmanager.cpp` |
| UC-08 | 文本/图片消息经托管节点中转，托管者分配递增序号；图片二进制不进入消息体，按哈希按需拉取；支持引用与 2 分钟撤回墓碑 | `src/core/chatmanager.cpp`、`src/ui/chatview.cpp` |
| UC-09 | 文件公告：流式 SHA-256、仅元信息经消息流分发，不触发文件字节传输 | `src/core/filemanager.cpp` |
| UC-10 | 按需下载：向托管者查询供给节点、选择来源后经 TLS 请求；“不请求不传输” | `src/core/filemanager.cpp`、`src/ui/chatview.cpp` |
| UC-11 | 多线程分片下载、分片进度持久化、暂停/恢复、断点续传、供给节点切换 | `src/core/downloader.cpp` |
| UC-12 | 合并后整文件 SHA-256 校验；按分片哈希定位损坏分片并定向重传，5 轮后失败 | `src/core/downloader.cpp` |
| UC-13 | 退出聊天室；托管者退出即解散并广播关闭 | `src/core/roommanager.cpp`、`src/ui/roomsview.cpp` |
| UC-14 | 消息气泡右键菜单：回复（引用）、复制文本、按权限撤回；消息操作只在气泡（含气泡内文本、引用、图片、文件）右键时弹出，头像与昵称右键打开成员菜单，整行空白处不弹菜单；移除每条消息上的独立操作按钮 | `src/ui/chatview.cpp` |
| UC-15 | @提及与通知：输入区“@”按钮与实时成员补全、消息内提及高亮（含 @all/@所有人/@全体成员）；被 @ 触发桌面通知且可突破房间免打扰（以 critical urgency 提示）；XDG 桌面通知（`org.freedesktop.Notifications`）；每房间免打扰（本地持久化） | `src/core/notifypolicy.h`、`src/ui/chatview.cpp`、`src/ui/notificationcenter.cpp`、`src/ui/xdgnotifications.cpp` |
| UC-16 | XDG 托盘常驻（`org.kde.StatusNotifierItem` + 标准 DBusMenu；Wayland 下 QSystemTrayIcon 不可用，故直接自实现 SNI）：以多尺寸 `IconPixmap`（ARGB32 网络字节序）导出内嵌产品图标，未安装图标主题也能显示；完整导出 `GetLayout` 等 DBusMenu 接口，deepin V25（dde-shell/dde-tray-loader）右键菜单可用；托盘宿主晚启动或被替换后自动重新注册；Wayland/X11 均适用；可在设置开关（关闭后立即注销托盘对象路径；受 SNI watcher 无注销接口限制，任务栏条目可能保留到应用退出或宿主刷新）；关闭时行为提示（每次询问/最小化到托盘/退出，可记住选择） | `src/ui/xdgtrayicon.cpp`、`src/ui/mainwindow.cpp` |
| UC-17 | 多账号管理（添加/切换/销毁，聊天室按本地账号隔离）与随机头像按钮（图标主题缺失时以内嵌绘制的刷新图形回退，按钮始终可见）；切换失败时回滚到原身份，若目标身份无法解密则提示并可只销毁该账户；个人资料页指纹始终以可选文本显示并支持一键复制 | `src/core/account.cpp`、`src/core/engine.cpp`、`src/ui/profilepage.cpp` |

## 界面与国际化

- 界面使用 DTK6 原生控件（`DMainWindow`/`DListView`/`DDialog`/`DLineEdit` 等）与 `DPalette`/`DPaletteHelper`，不使用 QSS 硬编码颜色，亮/暗主题自动跟随系统。
- 设置面板为 DTK `DSettingsDialog`（JSON 驱动；网段/心跳/离线判定即时生效，端口修改重启后生效）；入口为标题栏菜单 → 设置、`Ctrl+,` 或侧栏底部齿轮。设置仍写入 `~/.config/HuanSoft/deepin-meshlink/meshlink.ini`，环境变量覆盖、网段即时切换与端口重启生效语义保持不变。
- 文案源语言为英文；简体中文语言包位于 `translations/deepin-meshlink_zh_CN.ts`，安装为 `/usr/share/deepin-meshlink/translations/deepin-meshlink_zh_CN.qm`。界面语言跟随系统 locale（`LANG`），切换语言后需重启应用。
- 开发者可用 `cmake --build build --target update_translations` 更新 `.ts`；普通构建只运行 `lrelease`，不修改源文件。
- 新增语言时复制 `.ts` 为 `deepin-meshlink_<locale>.ts`，补全翻译并加入 `CMakeLists.txt` 的 `MESHLINK_TS_FILES`；当前随包提供 `zh_CN`。
- 左栏为窄方形按钮栏（图标在上、文字在下），可在“设置 → 界面 → 侧栏”隐藏文字；隐藏后保留 tooltip 和选中/悬停高亮。会话列表行高约 52px，三栏用 `DVerticalLine`/`DHorizontalLine` 分隔。
- 会话输入区支持消息操作（回复、复制文本、按权限撤回）：操作只收进消息气泡及其内部文本/引用/图片/文件的右键菜单，不再显示每条消息上的操作按钮；右键消息头像或昵称则打开成员菜单（@提及/设撤管理员/禁言/移出），整行空白处不弹菜单。引用条与引用块保留，消息头像从 blob/本地文件/指纹 identicon 三级回退，不再显示字母占位。
- 成员管理：会话页右键成员头像/昵称，或“管理聊天室 → 成员”页右键成员行/头像，都会打开同一个成员菜单——@提及（自动填入输入框）、设为/撤销管理员（仅托管者可操作，托管者自身无该入口）、禁言（10 分钟/1 小时/直至手动解除，已禁言时只显示解除禁言）、移出聊天室（需确认）；无权限的条目置灰，并以 tooltip/状态栏说明原因。管理页仍保留双击成员的旧管理菜单。
- 输入区提供“@”提及按钮：点击或紧随“@”继续输入时弹出成员补全（按已输入片段不区分大小写过滤、排除自己、最多 12 项，提及 token 结束即关闭）；正文中的 @昵称 与 @all/@所有人/@全体成员 以主题高亮色渲染。
- 房间头部提供免打扰按钮：仅静音该房间的消息通知，写入本地 `room_prefs`（SQLite，不上线）；被 @（含 @all）仍会通知并以 critical urgency 提示。会话列表为被提及房间显示 “@N” 徽标，托盘标题同步显示未读 @ 数，选中该房间或显示主窗口后清零。
- 应用注册 XDG 托盘常驻图标（`org.kde.StatusNotifierItem` + 标准 DBusMenu，菜单：显示主窗口/设置/退出，左键切换显示/隐藏）：图标以 16–64px 多尺寸 `IconPixmap`（ARGB32 网络字节序）直接随 SNI 导出，不依赖 `IconName` 对应的图标主题，未安装的本机构建也能显示；DBusMenu 的 `GetLayout`/`GetGroupProperties`/`GetProperty`/`AboutToShow`/`Event` 完整可用，deepin V25 的 dde-shell/dde-tray-loader 下右键菜单正常弹出；若托盘宿主晚于应用启动、重启或被替换，会自动重新注册。Wayland 与 X11 会话均可使用。“设置 → 行为 → 窗口与托盘”可开关托盘图标（关闭会立即注销对象路径，但受 SNI watcher 无注销接口限制，任务栏条目可能保留到应用退出或宿主刷新），并选择关闭窗口时每次询问（默认）/最小化到托盘/退出，询问时可勾选记住选择；托盘被关闭或无托盘宿主时关闭窗口即退出。
- “设置 → 通知”提供桌面通知总开关与消息预览开关；通知经 `org.freedesktop.Notifications` 发送，同一房间合并气泡，点击消息通知（正文或“打开”按钮）跳转到对应房间；托管侧的入群审批请求、被移出房间与房间解散也会通知。
- 个人资料页新增账号卡片：下拉切换本地身份（昵称 · 指纹缩写，多于一个账号时可选）、“添加账号…”以首次启动向导追加身份、销毁当前账号后自动切换到剩余账号；销毁最后一个账号后回到首次启动向导（不会自动退出应用，只有在向导中显式取消/退出才结束应用），创建新身份后继续运行。切换身份是原子的：身份加载或网络启动失败都会恢复原身份、原网络状态与个人资料页字段（下拉框回到原账户）；若失败原因是目标账户的本地凭据库无法在本机解密，会弹出“身份无法解锁”说明（指出昵称与本地凭据库无法解密的原因），提供默认的“取消”与“销毁账户”，可勾选同时删除其聊天记录/文件/下载缓存，销毁只移除该无法解锁的账户并保留当前身份与共享密钥。头像区新增随机头像按钮（生成随机种子 identicon，保存后生效），其图标在系统图标主题缺失时以内嵌绘制的刷新图形回退，按钮始终可见，并带 tooltip 与无障碍名称。账户指纹始终以占满整行的可选文本显示（等宽字体按可用宽度自适应、完整 32 字符，tooltip 携带完整值，并有无障碍名称），旁边提供一键复制按钮并提示已复制；只有无账户的瞬时状态显示占位符，不会出现空白指纹。
- “设置 → 界面 → 会话 → 发送快捷键”可选 **Enter**（默认，Shift+Enter 换行）或 Ctrl+Enter；“设置 → 文件 → 下载与缓存”提供分片大小、并行线程数和快照消息上限。

## 安全模型

- **身份**：Ed25519 密钥对，账号指纹 = `SHA-256(Ed25519 公钥 DER)` 的前 32 个十六进制字符。
- **传输**：所有 TCP 连接强制 TLS 1.3（`QSslSocket`），禁止明文 TCP。
- **认证**：自签 ECDSA P-256 证书 CN 绑定账号指纹；TLS 握手后再以带会话随机数的 Ed25519 签名互相证明身份；证书指纹按账号指纹做 TOFU 固定，不一致即中止并告警。
- **心跳**：UDP 心跳附带 Ed25519 签名，接收方用包头公钥验签并核对派生指纹，伪造包静默丢弃并记录。
- **落盘**：私钥/证书私钥经 AES-256-GCM 加密后写入 SQLite；密钥加密密钥（KEK）由每次安装随机生成的 32 字节设备密钥（`<data>/keystore/device.key`，0600，仅首次创建、绝不覆盖）与 16 字节随机盐（`<data>/keystore/salt`）按 HKDF 式 `SHA-256(SHA-256(deviceKey‖salt)‖salt)` 派生，**不绑定 machine-id、主机名、MAC 或 IP**，换网、改主机名或迁移数据目录后仍可解密。旧版以 machine-id/主机名 + uid 派生的密文在加载时自动迁移：先以 0600 保留原密文副本（`<data>/keystore/legacy/<id>.blob`），仅在设备密钥归属可证明（本库中已有设备密钥密文，或设备密钥与标签均为首次创建的升级场景）时才用新 KEK 重写；无法证明时保留原密文不变（当前会话仍可解密使用）。销毁账号时只清理该账号的副本；盐、设备密钥与诊断标签在最后一个账号销毁且剩余数量核实为 0 后才删除，计数失败时一律保留。切换账户时若目标身份在本机无法解密，会回滚到原身份并提示，可选择只销毁该无法解锁的账户，不影响其余账户与共享密钥。
- **成员/管理帧**：成员侧的加入响应、房间快照、成员事件、解散通知，托管侧的离开/状态同步，均绑定 TLS 通道的已认证对端身份，拒绝载荷内伪造指纹。
- **文件完整性**：TLS 负责传输机密性/完整性，文件级 SHA-256 负责端到端内容正确性；未通过校验的临时文件绝不按正式文件名交付。
- **撤回授权**：托管者/管理员可撤回任意消息；普通成员仅能撤回自己在 2 分钟窗口内的消息。2 分钟以托管节点记录的 `recv_ts` 为准，防止发送方伪造时间；成员请求经托管节点按当前成员角色二次校验后广播。

## 目录结构

```
src/core/    协议、加密、账号、存储、网段、发现、TLS 通道、房间、消息、文件与下载、通知策略
src/ui/      DTK6 桌面界面（会话、发现、传输、个人资料、设置面板、首次启动向导、通知中心、XDG 桌面通知与托盘）
src/app/     应用入口
tests/       纯逻辑单测 + 多进程集成场景 + UI 冒烟/翻译完整性检查
resources/   desktop 文件、图标与 DTK 设置面板 JSON
translations/ i18n 源文件（英文源 + 简体中文语言包）
debian/      Debian 打包
```

## 构建

依赖：

- CMake ≥ 3.16、C++17 编译器
- Qt 6：Core、Gui、Widgets、Network、Sql、Concurrent、DBus、LinguistTools
- DTK6：Core、Gui、Widget
- OpenSSL ≥ 1.1.1（TLS 1.3）
- Debian 构建还依赖 `qt6-tools-dev`（Qt6LinguistTools CMake 配置）与 `qt6-l10n-tools`（lrelease/lupdate）

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
sudo cmake --install build      # 安装到 /usr
```

在宿主/沙箱混合工具链中，可把 `MESHLINK_LRELEASE_EXECUTABLE` / `MESHLINK_RCC_EXECUTABLE`
指向包装脚本，让 Qt 主机工具通过宿主加载器运行；普通 deepin/UOS 构建无需设置。

## 运行

```bash
deepin-meshlink                 # 图形界面
deepin-meshlink --bootstrap 192.168.1.10:2425   # 广播被防火墙阻断时补一个单播发现目标
deepin-meshlink --headless      # 仅运行核心引擎（排障用）
```

首次启动会弹出账号向导：昵称可留空（自动生成“深度织讯用户_XXXX”），并选择本机所在的**局域网网段**（多网卡/有线+无线/虚拟网卡并存时，心跳只在该网段广播并只接受该网段的心跳）；创建失败时不会进入主界面。数据默认位于 `~/.local/share/HuanSoft/deepin-meshlink`，配置位于 `~/.config/HuanSoft/deepin-meshlink`。

运行中可在“设置”（标题栏菜单 → 设置，或 `Ctrl+,`，或侧栏底部齿轮）→“局域网网段”随时切换网段（无需重启）：旧网段的在线列表会立即清空，新网段的心跳会在下一个周期重新发现节点；TCP/TLS 监听与已建立的传输不受影响。`MESHLINK_IP` 环境变量可强制固定网段（测试/无头场景），此时界面选择不生效。手动添加的单播 bootstrap 目标只改变心跳的发送方向，不会绕过网段过滤——目标本身也需位于所选网段内。

聊天室创建后会显示 **6 位数字邀请码** 与 **32 位房间指纹 id**；创建时名称可留空，应用会自动生成随机名称（源语言规则为 `deepin-MeshLink room_XXXX`，中文界面为“深度织讯聊天室_XXXX”，与默认昵称同风格），创建成功对话框会显示实际房间名；显式名称仍需 2–64 字符。同一网段的成员可在“发现”页输入邀请码加入（局域网内无中心服务器，邀请码通过心跳摘要解析）。

消息发送默认按 Enter；可在“设置 → 界面 → 会话”改为 Ctrl+Enter。消息操作（回复、复制文本、撤回）仅在消息气泡的右键菜单中，右键头像/昵称则打开成员菜单；普通成员可在托管节点收到消息后的 **2 分钟**内撤回自己的消息，托管者与管理员可撤回任意时间的任意消息（服务端二次校验，不信任客户端时间）。撤回保留墓碑并清空正文/文件元信息，在线成员即时同步，离线成员在下次会话快照中恢复撤回状态。

本轮 UI/通知/托盘/多账号特性未新增命令行参数（仍只有 `--bootstrap` 与 `--headless`），全部在界面与设置中配置：消息操作在气泡右键菜单、成员管理（@提及/设撤管理员/禁言/移出）在头像或昵称右键菜单，@ 补全在会话页；每房间免打扰在房间头部；“设置 → 行为 → 窗口与托盘”决定托盘显示与关闭窗口的行为；“设置 → 通知”控制桌面通知；账号的添加/切换/销毁在“个人资料”页，切换身份会以新身份重建网络连接并只载入该账号的聊天室；销毁最后一个账号会回到首次启动向导，而不是自动退出应用（除非在向导中显式退出）。

## 默认参数（NFR-06）

| 参数 | 默认值 | 可调位置 |
|---|---|---|
| 局域网网段 | 自动选择（优先非回环 IPv4） | 首次向导 / 设置（标题栏菜单或 `Ctrl+,`） / `MESHLINK_IP` |
| UDP 发现端口 | 2425 | 设置（标题栏菜单或 `Ctrl+,`） / `MESHLINK_DISCOVERY_PORT` |
| TLS 服务端口 | 2426 | 设置（标题栏菜单或 `Ctrl+,`） / `MESHLINK_TCP_PORT` |
| 心跳周期 | 3000 ms | 设置（标题栏菜单或 `Ctrl+,`） / `MESHLINK_HEARTBEAT_MS` |
| 离线判定 | 3 个心跳周期 | 设置（标题栏菜单或 `Ctrl+,`） / `MESHLINK_OFFLINE_PERIODS` |
| 分片大小 | 256 KiB（64–1024 KiB） | 设置（标题栏菜单或 `Ctrl+,`） / `MESHLINK_CHUNK_KB` |
| 下载并行数 | 4（1–16） | 设置（标题栏菜单或 `Ctrl+,`） / `MESHLINK_THREADS` |
| 房间快照消息上限 | 200 | 设置（标题栏菜单或 `Ctrl+,`） → 文件 → 下载与缓存 |
| 发送快捷键 | Enter（Shift+Enter 换行） | 设置 → 界面 → 会话 |
| 左栏导航文字 | 显示 | 设置 → 界面 → 侧栏 |
| 桌面通知 | 开启（含消息预览） | 设置 → 通知 |
| 托盘图标 | 显示 | 设置 → 行为 → 窗口与托盘 |
| 关闭窗口时 | 每次询问（可记住选择） | 设置 → 行为 → 窗口与托盘 |
| 每房间免打扰 | 关闭（按房间单独切换） | 会话页房间头部免打扰按钮 |

网段、心跳、导航文字、发送快捷键、通知开关、托盘开关与关闭窗口行为即时生效；每房间免打扰即时生效并本地持久化；端口类参数需要重启应用。

## 测试

```bash
# 纯逻辑：帧格式、序列化、分片数学、加密原语、网段计算、房间 id 迁移、
# @提及边界匹配/mention-all 与免打扰通知决策、凭据库（设备密钥/盐/旧密文安全迁移/
# 销毁账号时的共享密钥生命周期）
build/tests/tst_logic

# 凭据库加固回归：错误/轮换的设备密钥不得改写旧密文，共享盐/设备密钥在剩余账号
# 未核实为 0 时不得删除
build/tests/tst_keystore_guard

# 多进程集成：UC-01…UC-13、按邀请码加入、运行中切换网段、引用/2 分钟撤回/管理层撤回
#（约 3–6 分钟，使用 127.0.0.1 与固定端口）
build/tests/tst_integration

# UI 冒烟：方形导航/隐藏文字、紧凑会话列表、头像、设置文件页、Enter 发送、
# 引用/撤回、按钮几何检查、DTK 设置面板与对话框、右键消息菜单（无每条消息按钮）、
# @ 补全与免打扰按钮、账号切换下拉与随机头像、亮/暗截图（offscreen）
QT_QPA_PLATFORM=offscreen build/tests/tst_ui_smoke
# 可选：MESHLINK_UI_SNAPSHOT_DIR=/tmp/meshlink-ui 时输出 light-*/dark-*.png

# 消息/成员右键菜单回归：气泡（文本/引用）右键只出消息操作、头像与昵称右键只出
# 成员菜单、角色权限与禁言时长/移出链路、管理页成员菜单与 @提及
QT_QPA_PLATFORM=offscreen build/tests/tst_chat_menus

# XDG 托盘 SNI 线格式回归（自启私有会话总线 + mock 托盘宿主）：IconPixmap、
# DBusMenu GetLayout/Event、宿主重启后重新注册
QT_QPA_PLATFORM=offscreen build/tests/tst_tray_sni

# 账户切换/销毁回归：切换失败回滚（身份/设置/网络）、按 id 销毁无法解锁账户、
# 最后账户计数失败保留共享密钥、指纹始终可读渲染、空名称创建随机聊天室
QT_QPA_PLATFORM=offscreen build/tests/tst_switch_guard

# 翻译完整性：.ts 中不允许存在 unfinished/vanished 条目
ctest --test-dir build -R translations --output-on-failure
```

集成测试以父进程方式拉起若干节点，通过 stdin/stdout 驱动。**在混合工具链环境**（例如沙箱内使用宿主的 Qt/DTK 库运行自编译二进制）中，需要让父进程用指定加载器启动子节点：

```bash
MESHLINK_LAUNCH_LOADER="/path/to/ld-linux-x86-64.so.2 --library-path /path/to/qt/libs" \
  build/tests/tst_integration
```

普通 deepin/UOS 环境无需该变量。测试相关环境变量：`MESHLINK_DATA_HOME`、`MESHLINK_CONFIG_HOME`、`MESHLINK_IP`、`MESHLINK_LAN_PREFIX`、`MESHLINK_DISCOVERY_PORT`、`MESHLINK_TCP_PORT`、`MESHLINK_BOOTSTRAP`；`MESHLINK_DEBUG=1` 输出协议诊断日志；`MESHLINK_EVIL_CHUNKS` 仅供测试注入损坏分片；`MESHLINK_CHUNK_DELAY_MS` 仅供集成测试放慢分片落盘，便于观察暂停/崩溃等中间状态；`MESHLINK_RECALL_WINDOW_MS` 仅供集成测试缩短 2 分钟撤回窗口；`MESHLINK_UI_SNAPSHOT_DIR` 仅供 UI 冒烟测试输出截图。以上测试钩子在生产环境均不会启用。

## Debian 打包

```bash
dpkg-buildpackage -b -us -uc
sudo dpkg -i ../deepin-meshlink_26.4.0.0~alpha1_*.deb
```

## 已知限制

- 不实现托管迁移：托管者离线则该聊天室不可用，托管者退出即解散。
- 不提供“仅下载缩略图/预览”（UC-10 A2）与空文件分片下载。
- 私钥加密使用本机随机设备密钥 + 随机盐派生 KEK（设备密钥文件权限 0600），暂未接入内核 keyring / TPM；删除数据目录中的 `keystore/`（或恢复到缺少设备密钥/盐的旧备份）会导致已存账户无法解密，应用会给出明确错误且不会覆盖现有密文。
- 不使用外部 CA、不在线吊销证书（TOFU 模型）。
- 仅面向 Linux 桌面（deepin/UOS），未做跨平台适配。
- alpha 阶段的线协议为 v3（v2 的 32 位房间 id/邀请码 + v3 的引用/撤回墓碑字段；心跳格式仍为 v2），不同构建版本之间不保证互通；升级后首次启动会把 1.0.0 的 16 位房间 id 自动重键为 32 位并补邀请码。
- 界面语言在启动时按系统 locale 加载，切换语言后需重启应用。
- 引用/撤回使用线协议 v3；v2 客户端（旧 alpha 构建）不兼容，升级需双方同步。引用只保存目标 `msgId`，目标超出本地近期快照或已撤回时显示对应占位；托管节点离线时成员撤回请求会失败，不做本地伪装。
- 离线成员重连只从最近的房间快照恢复撤回状态；早于快照窗口的旧消息撤回不会重新推送。
- 6 位数字邀请码只在同一托管者内保证唯一；跨托管者碰撞时，“按邀请码加入”会列出当前网段内所有命中的聊天室供选择，不做中心化全局注册。
- @提及识别为本地行为：基于当前房间成员昵称的文本匹配，不进入线协议。通知判定大小写不敏感且要求标识符边界（“@张三”不会命中“张三丰”，多词昵称需整体 @）；消息内高亮按“@”到空白之间的 token 与成员昵称精确比对，含空格的昵称不会被高亮（通知判定不受影响）；同名昵称的成员无法相互区分。
- 托盘常驻依赖会话总线上的 `org.kde.StatusNotifierWatcher` 宿主（deepin V25 由 dde-shell 等托盘面板提供）：无宿主时不注册图标；托盘被关闭或无宿主时，关闭窗口即退出。托盘开关会立即注销对象路径，但 `org.kde.StatusNotifierWatcher` 没有 `UnregisterStatusNotifierItem`，运行中关闭开关后任务栏条目可能保留到应用退出或宿主刷新。
- 桌面通知依赖会话总线与实现 `org.freedesktop.Notifications` 的通知守护进程（如 deepin 通知中心）：二者缺失时通知静默降级（不弹窗、不报错），不影响聊天功能；点击跳转仅对消息通知生效（入群审批等系统通知不携带房间映射）。

## 联系

HuanSoft 开源社区 · <https://huansoft.org.cn>
源码与问题跟踪：<https://github.com/HuanSoft-Open-Source-Community/deepin-MeshLink>
