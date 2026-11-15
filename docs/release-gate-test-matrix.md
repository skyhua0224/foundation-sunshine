# 发布前验收矩阵

发布前在 fork 上对同一份构建逐项验收。本文列出每一项做什么、怎么算通过、由谁来做，以及对应的脚本。

约定：

- 构建：fork 上 `workflow_dispatch` 跑 `Build and Release`（`publish_release=false`，不设 `AUTO_PRERELEASE`），被测提交与上游 `master` 相同。
- 证据：每项的输出放进一个运行目录，结果文件是 JSON（`id`、`status`、`detail`）。报告里写证据路径，不贴原始日志。
- 判定：`pass` 通过，`fail` 红，`skip` 本次没跑（写原因），`info` 只记录事实。

## 方式说明

| 标记 | 含义 |
|---|---|
| **自动·Linux** | 在构建机上无人值守完成 |
| **自动·Win** | 在 Windows 测试机上无人值守完成；只用隔离目录和独立端口，不动已有安装、服务和用户会话 |
| **自动·净机** | 脚本已备好，必须在没有安装过本软件的干净 Windows 环境（快照还原或新机）上跑；检测到已有安装会拒绝执行 |
| **作者** | 需要在自己的环境里操作并确认 |
| **实体** | 需要实体设备：手柄、麦克风、手机或 Mac 客户端、真实显示器 |

安装器的 `AppId` 固定，并且默认装进上一次的安装目录，同时注册 `SunshineService`、防火墙规则和驱动，所以无法在已有安装的机器上换目录试装。涉及真实安装、升级、卸载的项目一律归入「自动·净机」。

## 一、构建与产物

| ID | 项目 | 通过标准 | 方式 | 脚本 |
|---|---|---|---|---|
| B1 | fork 构建（与上游 `master` 同一提交）完成 | `Build and Release` 全部作业成功 | 自动·Linux | `gh run view` |
| B2 | 原生测试 | 全部通过，数量记入报告 | 自动·Linux | 同上，读 `test-results` |
| B3 | 产物齐全 | 安装包、便携包、校验和都在；校验和一致；各组件所需文件都在便携包里；符号包存在 | 自动·Linux | `scripts/release-gate/check-release-artifacts.py` |

## 二、安装、升级、卸载

| ID | 项目 | 通过标准 | 方式 | 脚本 |
|---|---|---|---|---|
| I1 | 全新安装（推荐、完整、精简三种类型） | 退出码 0 或 3010；日志无异常；各组件文件齐全；服务已注册 | 自动·净机 | `windows-installer-gate.ps1`（`fresh-*`） |
| I2 | 从 v2026.1031 升级 | 同上；升级前放在配置目录里的标记文件仍在 | 自动·净机 | 同上（`upgrade-from-v2026.1031`） |
| I3 | 从 v2026.1114.173055 升级 | 同 I2 | 自动·净机 | 同上（`upgrade-from-v2026.1114.173055`） |
| I4 | 逐个组件：vdd、firewall、autostart、gamepad、usbip、vmouse、tools | 只选 `application,assets,gui` 加该组件时，该组件的脚本落盘、其余可选组件不出现 | 自动·净机 | 同上（`component-*`）；文件清单的静态部分由 B3 覆盖 |
| I5 | USB/IP 保护 | 同版本 usbip-win2 已装时不重装（重装会让已附加的设备断开）；升级时迁移 ALKAIDUSB 根设备；卸载只清自己登记的根设备；运行期不下发共享驱动（#1168） | 自动·净机；设备断开与否需要**实体** | 同上（`fresh-full.usbip-guard-reinstall`） |
| I6 | 需要重启的提示 | 驱动类组件返回 3010 时安装器给出重启提示，不强制重启 | 自动·净机 | 安装器日志与退出码 |
| I7 | 卸载 | 服务、注册表项、防火墙规则、安装目录（配置目录除外）都已清掉 | 自动·净机 | `Check-Uninstalled` |
| I8 | 界面语言 | 中文系统装出中文界面，英文系统装出英文界面 | **作者** | 手动 |

## 三、服务与主机启动

| ID | 项目 | 通过标准 | 方式 | 脚本 |
|---|---|---|---|---|
| S1 | 服务包装器干净退出（#1270） | 在服务管理器之外启动 `sunshinesvc.exe`，退出码 0，崩溃目录里没有新报告；旧版本的报告是 `sunshinesvc.exe` 的 `terminate`，调用链经过 `std::terminate` 和静态线程对象的析构 | 自动·Win | `windows-host-smoke.ps1`（`svc.clean-exit`） |
| S2 | 服务真实启动与停止三次 | 每次停止后崩溃目录为空 | 自动·净机 | 手动 `sc start/stop`，再跑 `scan-host-logs.ps1` |
| S3 | 主机启动 | 隔离目录、独立端口启动，Web UI 端口监听，`--version` 有输出 | 自动·Win | `windows-host-smoke.ps1`（`smoke.*`） |
| S4 | 日志特征 | 日志里没有 `terminate called` 之类的行；崩溃目录里没有 `sunshinesvc.exe` 的 `terminate` 报告；日志里没有对上报的 HTTP 400（主机不一定记这一行，以 S5 的服务端结果为准） | 自动·Win；对现有安装只读扫描 | `scan-host-logs.ps1` |
| S5 | 主机运行状态上报 | 主机发出的信封被服务端接受；空版本的信封被拒绝（负向对照） | 自动·Win + 自动·Linux | `report-envelope-probe.py`；主机本身的上报指向本机回环的服务端实例 |

## 四、配对与串流

| ID | 项目 | 通过标准 | 方式 | 脚本 |
|---|---|---|---|---|
| P1 | 配对端点 | 未配对时 `/serverinfo` 返回 `PairStatus 0`，`/pin` 页面可打开 | 自动·Win | `windows-host-smoke.ps1`（`smoke.serverinfo`、`web.page.pin`） |
| P2 | 用 PIN 完成配对 | 客户端出现主机，输入 PIN 后配对成功，重启主机后仍是已配对 | **作者**（客户端：Mac、手机、Windows 各一次） | 手动 |
| V1 | 编码能力 | `/serverinfo` 的 `ServerCodecModeSupport` 位：H.264、HEVC、HEVC 10 bit、AV1、AV1 10 bit | 自动·Win（无桌面会话时探测可能失败，记 `skip` 并写原因） | `windows-host-smoke.ps1` |
| V2 | 串流：H.264、HEVC、AV1 各一次；SDR 与 HDR；60 与 120 fps（共 12 组，AV1 与 HDR 视显卡能力） | 画面连续，无黑屏；码率、帧率与设置一致；退出后主机回到空闲 | **作者** + **实体**（120 fps 需要 120 Hz 的显示器或虚拟显示器） | 手动，记录表见下 |
| V3 | 镜头转动稳定（#1267） | 满码率、快速转动视角 60 秒：发送延迟不持续上涨，没有一次丢 32 帧的清空；模型测试 `video_pacing_unit_tests` 通过 | 模型部分自动·Linux（CI）；实机**作者** | CI 测试结果；实机记延迟曲线 |
| V4 | 串流中的主机日志 | 串流前后同样跑 S4 | 作者串流时自动·Win（只读扫描） | `scan-host-logs.ps1` |

当前没有可在 Linux 或回环上驱动完整串流的客户端：仓库里的 `moonlight-common-c` 只是库，`tests/tools` 里只有 USB/IP 与 VDD 的探针。V2、V3 因此需要真实客户端。要把它们自动化，需要先做一个最小的拉流客户端，这是单独的工作。

## 五、输入设备与音频

| ID | 项目 | 通过标准 | 方式 | 脚本 |
|---|---|---|---|---|
| D1 | DualSense 原生（USB/IP 路径） | 主机侧出现虚拟 DualSense；按键、摇杆、触摸板、陀螺仪、震动、自适应扳机可用 | **实体**（真实 DualSense + 客户端） | 手动 |
| D2 | 音频 | 串流有声音；静音与恢复；断开后主机默认输出恢复 | **作者** | 手动 |
| D3 | 麦克风 | 客户端说话，主机侧应用能录到 | **实体** | 手动 |
| D4 | 剪贴板 | 文字双向同步；大文本与非文本内容不卡死 | **作者** | 手动 |

## 六、Panel 与 Web UI

| ID | 项目 | 通过标准 | 方式 | 脚本 |
|---|---|---|---|---|
| U1 | Panel 概览磁贴各尺寸 | 360 至 1920 px 宽、深浅主题、空闲与串流中、智能与手动 s/m/l/xl：没有被截断、重叠、被遮盖、缩得过小的文字或图标 | 自动·Linux（真实长度的数据；测的是 Panel 源码树的该提交） | 内部巡检机器人的 `tile-fit`；报告里写被测的 Panel 提交 |
| U2 | Panel 在目标机器上能启动、能连到主机 | 窗口出现，概览有数据 | **作者** | 手动 |
| W1 | Web UI 页面 | `/`、`/config`、`/apps`、`/password`、`/pin`、`/troubleshooting`、`/dualsense`、`/downloads` 都返回 200 | 自动·Win | `windows-host-smoke.ps1`（`web.page.*`） |
| W2 | Web UI 单元与文档测试 | CI 的 Web UI 分片全部通过 | 自动·Linux（CI） | `gh run view` |
| W3 | 配置保存 | 在 `/config` 改一项、保存、重启后仍在 | **作者** | 手动 |

## 七、Sparxie

| ID | 项目 | 通过标准 | 方式 | 脚本 |
|---|---|---|---|---|
| X1 | 从应用列表启动 Sparxie 应用 | 游戏启动，参数与所选档位一致 | **作者** | 手动 |
| X2 | 崩坏：星穹铁道的帧率设置 | 在 Sparxie 里选 60/120 后，游戏内帧率与之一致，退出后游戏自己的设置被还原 | **作者** | 手动 |

## 八、问题上报

| ID | 项目 | 通过标准 | 方式 | 脚本 |
|---|---|---|---|---|
| R1 | 上报到达服务端 | 主机、崩溃报告、用户反馈三类都能到达服务端；信封校验通过 | 自动·Win + 自动·Linux（服务端用本机回环的实例，不碰线上） | `windows-host-smoke.ps1` + `report-envelope-probe.py` |
| R2 | 反馈生成的 issue 显示中文 | 标题、字段名、说明都是中文，无乱码，附件链接可打开 | 服务端渲染部分自动·Linux（服务端测试）；线上 issue 的实际显示**作者** | 服务端测试；手动 |
| R3 | 隐私开关 | 关闭「错误报告」「使用统计」后不再发送；报告里没有路径、主机名、用户名 | 自动·Win（对本机回环的端点抓包） | 在配置里关掉对应开关后重跑 S5 |

## 九、运行方式

```
# 1. Linux：取构建产物并做静态检查
gh run download <run-id> -R <fork> -D artifacts/
python3 scripts/release-gate/check-release-artifacts.py artifacts/ --out evidence/artifacts.json

# 2. Windows 测试机：隔离目录里的主机冒烟（不动已有安装）
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\release-gate\windows-host-smoke.ps1 `
  -PortableZip <便携包> -WorkDir <工作目录> -ReportEndpoint http://127.0.0.1:<端口> -EvidenceDir <证据目录>

# 3. 对现有安装只读扫描日志
powershell -NoProfile -File scripts\release-gate\scan-host-logs.ps1 -LogPath <sunshine.log> -CrashDir <crashes\pending>

# 4. 上报信封探针（本机回环的服务端）
python3 scripts/release-gate/report-envelope-probe.py --endpoint http://127.0.0.1:<端口> --version <被测版本号>

# 5. 干净 Windows 环境：安装器全套
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\release-gate\windows-installer-gate.ps1 `
  -NewInstaller <新版安装包> -OldInstallers @{ 'v2026.1031'='<旧包>'; 'v2026.1114.173055'='<旧包>' } `
  -EvidenceDir <证据目录> -CleanMachineConfirmed
```

脚本的保护：

- `windows-host-smoke.ps1` 只在 `-WorkDir` 下写文件，拒绝 `Program Files` 下的目录，端口被占用时直接中止，上报端点必须是回环地址；它只结束自己启动的进程。
- `windows-installer-gate.ps1` 在发现 `SunshineService`、安装器的卸载项或默认安装目录时拒绝开始。
- `report-envelope-probe.py` 只接受回环地址。
- 没有任何脚本会启动、停止或还原虚拟机。

## 十、串流记录表（V2）

每组填：通过 / 红 / 未测，以及异常现象。

| 编码 | 动态范围 | 60 fps | 120 fps |
|---|---|---|---|
| H.264 | SDR | | |
| H.264 | HDR | 不适用 | 不适用 |
| HEVC | SDR | | |
| HEVC | HDR（10 bit） | | |
| AV1 | SDR | | |
| AV1 | HDR（10 bit） | | |

## 十一、不在本矩阵内

- 多显示器与多 GPU 组合、笔记本混合显卡。
- 公网与弱网下的串流质量；那是网络传输自己的测试。
- macOS、Android、Linux 客户端的功能测试。
