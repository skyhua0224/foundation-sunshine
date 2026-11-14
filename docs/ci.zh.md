# Foundation Sunshine CI 重设计

状态:设计稿 + 部分已实现(分支 `ci/redesign`),2026-11-15。所有"实测"数字来自 fork `skyhua0224/foundation-sunshine` 上的 GitHub 托管 `windows-latest`(4 vCPU / 16 GB,公开仓库免费)实验,实验工作流在 `ci/lab*` 分支,不属于产品流水线。同一配置在不同虚拟机上的耗时有 ±20–50% 的差异(见 F15),凡需要比较的地方都标明"同一虚拟机交错重复"或"单次";单次比较只作线索。"推算"数字注明推导依据。来源编号 `[S#]` 见文末来源索引,每条均已读取原文并记录版本/日期。

## 1 目标

1. 快:PR 反馈(代码改动,热缓存)端到端 < 15 分钟,其中构建步骤热缓存下 < 5 分钟;文档/文案类改动约 1 分钟。
2. 准:任何真实失败都让流水线失败,不静默通过;没有随机噪声(隔离的不稳定测试有负责人和到期日)。
3. 全:发布产物保持现有名称与规则(§9),增加测试报告、供应链安全、夜间全量与可选的 Linux 可移植部分检查。
4. 每个场景单独优化,不是一条流水线通吃(§4)。

## 2 现状与实测

### 2.1 参考运行 43598583189(Windows 作业 30 min 48 s)

| 步骤 | 秒 | 备注 |
|---|---|---|
| Checkout(`submodules: recursive`) | 185–192 | 3.9 GB 工作区,串行拉取约 50 个子模块,含 Linux/Android/文档专用树 |
| setup-msys2 | 27 | |
| `pacman -Syu` | 85–93 | 191 个包 |
| Web UI(npm ci、lint、test、build) | 99–111 | 在 Windows 上串行,位于关键路径 |
| Configure | 64–94 | 含 Boost 1.92 下载+解压(FetchContent) |
| Build | 1205–1500 | 1278 个编译动作;ccache 命中 94%,但 579 次是预处理模式命中(F4) |
| ctest(86 个,串行) | 56–120 | |
| Package | 48(失败) | 脚本里有孤立的 `fi`,该步骤永远失败(F12) |
| Post Checkout | 20–25 | |

### 2.2 已确认的事实(实测,fork 实验)

| # | 发现 | 数据 | 处置 |
|---|---|---|---|
| F1 | 定向子模块(浅克隆 + blobless + build-deps/AMF 稀疏)代替递归检出 | Checkout 192 + post 22 s、3.9 GB → 9 + 74–99 + 12 s、686 MB;`submodule.fetchJobs` 对 `actions/checkout` 递归阶段无收益 | **已实现** `scripts/ci/init-submodules.sh` |
| F2 | Defender 实时扫描拖慢小文件密集的步骤,对长编译影响小 | 48 单元子集冷编译 368→213 s、404→277 s(−31%…−42%);`pacman` 93→65、configure 59→47(单次);**完整冷构建几乎无影响:3443 s(开)对 3502 s(关)** | **已实现**(composite action),噪声 ±30% |
| F3 | `nvhttp.h` 里 `constexpr SUNSHINE_VERSION = PROJECT_NAME " " PROJECT_VER`,非 master 构建里 `PROJECT_VER` 含提交号,30+ 个编译单元每次新提交都失效 | 48 个样本中 8 个未命中,其余 39 个直接命中 | **已修** `00efef82` |
| F4 | 命令行与缓存来源不一致时直接模式落到预处理器路径 | 同一热缓存、同一代码:一致 **85 s**(994 直接命中/13 未命中/1012 编译);每个单元多一个 `-D` **567 s**(1005 预处理命中)= 6.7 倍 | **已修** `200cbb11`(适配器 `-D/-I` 只给 3 个读取它的文件);ccache 命中成本:直接 66 ms、depend 66 ms、预处理 451 ms(中等单元),编译 4.4 s,进程创建 29 ms |
| F5 | lld 对本项目无加速 | 5 个测试可执行文件链接:GNU ld 6.8 s,lld 7.6 s;94 个测试链接合计仅 29 CPU 秒;ninja 日志里最长的编译在 2%–55% 处开始,最后 15% 时间里空闲 0–23 s,没有"尾部长任务" | 否决 lld 提速、否决 Ninja 临界路径调度(§6.3) |
| F6 | Authenticode 与追加载荷 | 签名后证书表落在文件末尾,页脚不再在最后 1024 字节;签名后改 overlay 一个字节 → `HashMismatch`;签名后再追加 → `NotSigned` | 设计:装配后最后签名,外壳在证书表之前找页脚(§9.3) |
| F7 | master 的 Web UI 步骤从未真正通过 | ① 兄弟目录 `foundation-ui` 用 `npm ci --omit=dev --omit=peer` 安装,缺 `vue`(它既是 peer 又是 dev),导致 3 个测试文件加载失败;完整 `npm ci` 后 **770 个测试全过**(106 s)。② `foundation-ui` 以浮动 HEAD 克隆。③ `npm run i18n:validate` 对任何翻译滞后都返回非零(53 个缺失键等),不能当门禁 | **已实现**:固定提交 `scripts/ci/foundation-ui.sha` + 完整安装;i18n 改为带数字的警告 |
| F8 | PR #1254 的 `build-setup-ui.ps1` 在 `$PSNativeCommandUseErrorActionPreference = $true` 下调用 `robocopy`,成功时退出码 1 被当成失败 | lab 复现 | 需告知 #1254 作者 |
| F9 | 稳定版工作流重新编译,工具链与 CI 不同(gcc 15.1.0-5 固定、pacman 的 nodejs、无 ccache) | 读取工作流 | §8.4、§9.3 |
| F10 | action 按浮动标签引用,无顶层最小权限 | 读取工作流 | **已实现** 全部 SHA 固定 + `permissions: {}` + Dependabot |
| F11 | 组织为 GitHub Free:没有 larger runners;合并队列对"组织拥有的公开仓库"可用;规则集 `FOR_CI` 未启用,没有必需检查 | `gh api` | §6.3、§8 |
| F12 | `main.yml` 的 "Package Windows" 步骤末尾有孤立的 `echo/exit 1/fi`,bash 语法错误 → 该步骤从来不可能成功;actionlint 不检查 `msys2 {0}` 步骤 | `bash -n` 报 `syntax error near unexpected token 'fi'` | **已实现** `check-workflow-shell.py` 对所有 run 块做 `bash -n`;**已修** `a4d590c0`(与 #1254 的删除相同) |
| F13 | 构建隐含依赖:`python` 原本只经 `gdb` 间接安装,CMake 打包配置需要它;`diffutils` 同理 | 最小包列表 configure 失败 | 显式包列表;**但包数量本身不是收益**:61–87 s 对 65–99 s(镜像自带 MSYS2 已有 182 个包) |
| F14 | **测试可执行文件重复编译主机源码** | 编译 CPU 的 38.3% 是测试再编译主机源码。改为一个 OBJECT 库后(同一 GCC,同一虚拟机类型):**冷构建 3449 → 2050 s(−41%)**;**热构建(全部目标文件删除、提交变化、热 ccache)171 → 85–92 s(−46%)**;编译边 1277 → 1012;**86/86 测试通过**;`ctest` 57 → 51 s | **已实现** `777a7c4b`(Windows 且带测试时启用;`-DSUNSHINE_SHARE_HOST_OBJECTS=OFF` 恢复旧布局) |
| F15 | Clang 与 GCC 的速度 | 同一 1250 个共同目标文件:一次运行 clang 比 gcc 少 31% CPU(墙钟 2375 对 3443 s);**另一次运行 clang 反而是 gcc 的 1.05 倍(每条边都慢 1.51 倍)**,而 gcc 两次一致 | **结论未定**:等待 lab-14 同一虚拟机交错重复。Clang 暴露 3 个真实的标准符合性问题(已修,见 F16) |
| F16 | Clang 发现的真实缺陷 | ① `VdfNode` 含 `std::vector<std::pair<std::string, VdfNode>>`(不完整类型,GCC 放过);② `color_profile.cpp` 的 `constexpr` 转换到枚举范围外的值;③ `tests/tools/ds5_native_usbip_smoke.cpp` 的 `int`→`uint8_t` 花括号窄化 | **已修** 三个独立提交 `04f488ba`、`e025bdc8`、`f62f4070`,各自可上游,与 CI 无关 |
| F17 | 编译时间去向(Clang `-ftime-trace`,1011 个编译单元,无缓存,ClangBuildAnalyzer) | 前端解析 9012 s、后端代码生成 2842 s(**解析占 76%**);解析总耗时最高的头文件见 §6.2 | §6.2 |
| F18 | 包数量、-O1/-O2 的单次冷构建 | -O1 4123 s、-O2 3932 s,比 -O3 的 3466 s 还慢 → 纯虚拟机噪声(±20%) | 单次结果不用于结论,交错重复见 lab-13 |

(F1–F18 之外的详细实验记录见 §6、§7。)
## 3 总体架构(已实现于 `.github/workflows/ci.yml`)

一个入口工作流,触发器:`pull_request`、`merge_group`、`push`(master)、`workflow_dispatch`、`schedule`。开头的 `changes` 作业读改动文件并给后面的作业分流;最后一个 `ci-gate` 作业 `if: always()`,检查所有 `needs` 的结果,**它是唯一的必需检查**。不使用 `on.paths`:GitHub 文档说明工作流被路径过滤跳过时相关检查一直 Pending 会卡住合并,而被条件跳过的**作业**报告为成功;依赖失败的作业被跳过时需要 `always()` 才不会放行[S1]。`merge_group` 触发器必须有,否则合并队列拿不到必需检查[S1][S20]。

```
changes (ubuntu-slim, 5 s)
 ├─ docs-check        (相对链接/图片,约 20 s)                              [改动含文档]
 ├─ workflow-lint     (actionlint + 所有 run 块 bash -n + 脚本单测)         [改动含工作流/脚本]
 ├─ webui             (Linux:固定 foundation-ui,lint,769 个测试,构建)       [改动含 Web UI]
 ├─ win-build         (Windows:定向子模块→工具链→configure→build→ctest)     [改动含 C++/CMake/pin]  ← 关键路径
 ├─ cache-prune       (ubuntu-slim,只保留最新的一份编译缓存)                [master/夜间/手动]
 └─ ci-gate           (ubuntu-slim,always,唯一必需检查)
```

打包、安装器 UI、候选构建与晋升、签名作业按 §8、§9 接入(依赖 #1254 的脚本与合并队列开启,不在本轮草案里启用)。

### 3.1 改动分类(`scripts/ci/classify-changes.py`,有单测)

文件列表来自 REST API(PR:`pulls/N/files`;push:`compare/before...after`),不检出历史、不用第三方 action(LLVM 同一做法[S21],实测 0.6–0.8 s;第三方 changed-files 动作有过供应链事件[S22])。输出 `cpp`、`webui`、`installer`、`workflow`、`docs`、`docs_touched`、`any_code`。未知路径一律算 `cpp`(宁可多构建,不漏构建);合并队列、夜间、手动与超过 3000 个文件的改动一律全跑。`ci.yml`、`.github/actions`、`scripts/ci` 的改动被视为"流水线自身",触发完整运行。
## 4 目标预算:按场景

"关键路径"= 从触发到 `ci-gate` 通过的串行最长链。**当前**为参考运行实测;**目标**中的数字由已实测的分项推算,依据写在括号里,带 ⏳ 的等实验或 fork 上完整运行补齐。

| 场景 | 现状 | 流水线 | 关键路径(目标) |
|---|---|---|---|
| (a) PR,C++ 改动,热缓存 | ≈ 31–40 分钟 | `changes` + `webui`(Linux)‖ `win-build`;不打包;fork PR 无密钥,适配器关闭 | **≈ 8.5 分钟 + 未命中编译**:启动 0.1 + 检出 0.2 + 子模块 1.7(实测 99 s)+ 工具链 2.4(实测 143 s)+ configure 1.1(68 s)+ 构建 1.5(热缓存 85–92 s,F14)+ ctest 0.85(串行 51 s;`-j4` ⏳)+ 报告/上传 0.2。改动一个常用头文件时未命中增加:每个未命中单元约 5–40 CPU 秒 |
| (a') PR,仅 Web UI | ≈ 31 分钟 | 只跑 `webui`(Linux)+ `ci-gate` | **≈ 2.5 分钟**:检出 0.1 + Node 0.1 + foundation-ui 0.1 + `npm ci` 0.05 + lint 0.05 + 测试 1.3(78 s,F7)+ 构建 ⏳ |
| (b) 功能分支 push | 与 PR 相同 | 无 PR 时只跑 `changes` + 轻量检查;C++ 构建由 PR、手动 `workflow_dispatch` 触发 | 轻量约 3 分钟;手动全量同 (a) |
| (c) 合并队列 / push 到 master(预发布) | ≈ 40 分钟 + 打包(现在打包永远失败) | 方案 A(§8):队列里做候选构建(带密钥,适配器 ON,打包,来源证明);master `push` 只"晋升" | 队列:同 (a) + 打包 ⏳ ≈ 12–16 分钟;**master 晋升:2–3 分钟**(推算,§8.2);master 同时跑一次不阻塞的缓存刷新构建 |
| (d) 发布/稳定版 | 重新编译 + 签名 ≈ 35–45 分钟 | 下载同一提交的候选产物,验证来源证明,三次 SignPath 请求,重新打包(§9.3) | ≈ 8–12 分钟(SignPath 延迟未测) |
| (e) 仅文档/图片/README/locale 文案 | ≈ 31–40 分钟 | `docs-check`(相对链接/图片)+ `ci-gate`;locale 文件改动时 `webui` 一并跑 | **≈ 1 分钟**(实测 `changes` 5 s;文档检查约 20 s;fork PR 上该作业已被正确地跳过) |
| (f) 仅工作流/CI 脚本 | ≈ 31–40 分钟 | `workflow-lint`(actionlint + 对所有 `run` 块的 `bash -n` + 脚本单测)+ `ci-gate`;**`ci.yml`/脚本自身变化会触发完整流水线**(必须被真正执行一次) | 仅其它工作流文件:**≈ 1.5 分钟**(实测 7 s 级);ci.yml/脚本:同 (a) |
| (g) 夜间(`schedule`)+ 手动 | 无 | 全量冷构建 GCC 与 Clang 两条腿(刷新默认分支 ccache)、完整 ctest、可选 Linux 可移植部分、依赖漂移金丝雀 | 冷构建 2050 s(GCC,共享对象)+ 工具链 4.5 分钟 + 测试 ≈ **40 分钟**,不影响任何人 |

## 5 逐步骤表(每一步都要更快)

"现状"取自运行 43598583189 / 43605340508;"实测"来自 fork 实验。实测无收益的做法也记录为"否决"。

| # | 步骤 | 现状 s | 目标 s | 技术(来源) | 状态 |
|---|---|---|---|---|---|
| 1 | Set up job | 2 | 2 | — | — |
| 2 | Checkout | 185–192 | 10 | `persist-credentials: false`,不递归[S8] | 实测 10 s |
| 3 | 子模块 | (含在上) | 75–99 | 浅克隆 + blobless + 稀疏,不递归 Linux/macOS/Android 树[S8][S11] | 实测 74–99 s;`init-submodules.sh` 的 pyrowave→Granite 链仍串行,并行化可到约 50–60 s(待做) |
| 4 | Post Checkout | 20–25 | 8–12 | 子模块少、无持久凭据 | 实测 11–12 s |
| 5 | MSYS2 + 依赖 | 27 + 85–93 | 约 100–145 | Defender 排除;显式包列表;`release: false` 用镜像自带 MSYS2[S9] | 实测整个 composite 143 s;pacman 65–117 s 的噪声 ±30%;**包数量缩减无收益**(F13) |
| 6 | Node(Windows) | 10 | 0 | Web UI 移到 Linux | 实测 Linux 4–6 s |
| 7 | foundation-ui | 20 | 3–6 | 固定提交 + 完整安装 | 实测 Linux 6 s |
| 8 | Web UI(lint/测试/构建) | 99–111(Windows,关键路径) | 0(出关键路径) | Linux 并行作业[S14] | 实测:lint 3 s,测试 78 s(769 个全过),`npm ci` 3 s |
| 9 | 安装器 UI | 223 | 0–2 | Panel 发布固定资产 + sha256 + 来源证明(§9.2) | 实测:冷 236 s,cargo 缓存 186 s(仅省 50 s,否决缓存路线),下载校验 2 s |
| 10 | 恢复编译缓存 | 5 | 5–20 | `actions/cache/restore`;只有 master/夜间保存 | 设计;保存的体积 0.2–0.5 GB |
| 11 | Configure | 64–94 | 47–68 | 定向子模块;Defender 排除 | 实测 47–68 s;**Boost 源码缓存无意义**:108 MB 压缩包下载 1 s,整个 configure 仅 50 s(lab-8) |
| 12 | **Build(热缓存)** | 1205–1500 | **85–92** | 共享对象库(F14)+ 命令行一致(F3/F4) | **实测 85–92 s**(GCC,全部目标文件删除) |
| 12' | **Build(冷缓存)** | 3500(58 分钟) | 2050 | 共享对象库 | 实测 2050 s(−41%),Clang/PCH 等继续压缩见 §6 |
| 13 | native tests | 56–120 | 20–50 | `ctest -j4`、`--no-tests=error`、JUnit→注解与摘要、允许跳过清单 | 串行实测 51 s;并行 ⏳ |
| 14 | 打包 + 装配 | ≈ 240(现在失败) | PR 不跑;候选/发布 60–150 | Inno `Compression` 档位、`LZMANumBlockThreads`[S15] | ⏳(lab-5 打包基准未跑到) |
| 15 | 上传工件 | ? | 少 | `compression-level`、`retention-days`[S26] | 测试结果 7 天、压缩 6 |
| 16 | 整体(PR 热缓存) | 1848 | **≈ 510(8.5 分钟)+ 未命中** | 并行 + 以上 | §4 |

### 5.1 Defender 排除项

实测:48 个编译单元的子集冷编译 368→213 s(lab-2)、404→277 s(lab-9);Dev Drive 加排除项 215 s,不加 414 s(≈ D: 不加的 404 s)。机制:Windows Defender 对编译器写出的大量小文件逐个扫描。GitHub 托管 Windows runner 以管理员运行,`Add-MpPreference -ExclusionPath/-ExclusionProcess` 可用。完整冷构建几乎无差别(3443 对 3502 s):长编译被 CPU 主导。风险:runner 是一次性虚拟机,排除范围只限构建目录与编译工具进程;微软为同一目的提供 Dev Drive 性能模式[S18],取舍相同。**Dev Drive(ReFS VHDX)没有额外收益**(215 对 213 s 噪声内;configure 反而慢:104 对 62 s)→ 否决。

## 6 编译专项

### 6.1 冷构建的成本构成(Clang,冷 ccache,`.ninja_log` 逐边求和,13791 CPU 秒)

| 类别 | CPU 秒 | 占比 | 边数 |
|---|---|---|---|
| `sunshine` 产品的主机源码 | 4779 | 34.6% | 253 |
| 测试目标**重新编译**主机源码:`test_sunshine` | 3442 | 25.0% | 254 |
| 测试目标重新编译主机源码:其余测试可执行文件 | 1834 | 13.3% | 237 |
| 测试自身源码:其余测试可执行文件 | 1340 | 9.7% | 135 |
| 第三方(Boost、PyroWave/Granite) | 1215 | 8.8% | 291 |
| 测试自身源码:`test_sunshine` | 1182 | 8.6% | 80 |

38% 是测试把主机源码再编一遍 → 共享对象库(F14)。链接几乎免费:94 个测试链接合计 29 CPU 秒。

### 6.2 `-ftime-trace` 诊断(1011 个编译单元,无缓存)

前端解析 9012 s,后端 2842 s;**解析占 76%**,所以减少解析(PCH、头文件瘦身)是第一杠杆,优化级别(影响后端)是次要杠杆。

解析总耗时最高的 10 个头文件(跨所有包含它的单元累加,嵌套有重叠):

| # | 头文件 | 累计 | 包含次数 | 每次 | 处置 |
|---|---|---|---|---|---|
| 1 | `src/logging.h` | 1098 s | 219 | 5.0 s | 拆分(见 2) |
| 2 | `windows.h` | 882 s | 463 | 1.9 s | PCH |
| 3 | `<chrono>`(C++23 下带出 `<format>`/`<sstream>`) | 795 s | 541 | 1.5 s | PCH |
| 4 | `boost/log/sinks.hpp` | 748 s | 220 | 3.4 s | 只让 `logging.cpp` 包含(**已改** `b7fc88c9`) |
| 5 | `<ostream>` | 628 s | 800 | 0.8 s | PCH |
| 6 | `<istream>` | 610 s | 735 | 0.8 s | PCH |
| 7 | `<sstream>` | 606 s | 706 | 0.9 s | PCH |
| 8 | `nlohmann/json.hpp` | 580 s | 319 | 1.8 s | PCH;`json_fwd.hpp` 用于只声明的头文件 |
| 9 | `src/platform/common.h` | 542 s | 136 | 4.0 s | 拆分/前置声明 |
| 10 | `immintrin.h` | 536 s | 478 | 1.1 s | PCH |

模板实例化:`std::vformat_to`(`<chrono>` 的格式化支持,而项目源码没有任何 `std::format`)累计 708 s(1363 次);`nlohmann::basic_json::parse` 316 s(447 次)、解析器 226 + 194 s;`std::unique_ptr` 253 s(10143 次)。

### 6.3 方案表与结论

| # | 做法 | 来源 | 实测/预期 | 结论 |
|---|---|---|---|---|
| 1 | **一次编译多处使用(OBJECT 库)** | CMake `add_library(OBJECT)`、`$<TARGET_OBJECTS>`[S13] | 冷 −41%,热 −46%,86/86 测试通过(F14) | **采用,已实现** |
| 2 | 命令行一致(版本常量移出 `nvhttp.h`、适配器 `-D/-I` 按文件设置) | ccache 手册"The direct mode"[S12] | 一致 85 s 对不一致 567 s(F4) | **采用,已实现** |
| 3 | 头文件瘦身(`logging.h` 去掉 sinks) | `-ftime-trace` | 估计 ≤ 5%(sinks.hpp 748 s / 前端 9012 s ≈ 8%,减去仍需的 `common.hpp`) | **采用,已实现** `b7fc88c9`,fork 构建验证中 |
| 4 | 预编译头(PCH),GCC 与 Clang;Clang 加 `-fpch-instantiate-templates` | CMake `target_precompile_headers`[S13];ccache 手册:需 `sloppiness=pch_defines,time_macros`,GCC 加 `-fpch-preprocess`[S12] | ⏳ lab-14 `pch`(同一虚拟机交错 3 次);`-fpch-codegen/-fpch-debuginfo` 要求单独生成 PCH 对象,CMake 不管理,不在本轮 | ⏳ |
| 5 | Asio/Beast 分离编译(`BOOST_ASIO_SEPARATE_COMPILATION`、`boost/asio/impl/src.hpp` 放进一个源文件)[S28] + 对最重的实例化用 `extern template` | Boost.Asio 文档 | ⏳ lab-14 `sep`;注意 `src/http/server.cpp` 的 GCC 编译 140 s,其中后端 93 s(Clang 追踪),分离编译主要减少前端 | ⏳ |
| 6 | Clang(UCRT64,libstdc++)+ lld | MSYS2 环境页[S10];lld[S25] | 速度**未定**(0.69 对 1.05,F15);lld 无收益;**CLANG64(libc++)不可行**:预编译 `libx265.a` 引用 libstdc++ 符号;全部 86 测试在 Clang 上的结果待修复 3 个缺陷后复测 | ⏳ lab-14 `gccclang` |
| 7 | 编译标志:`-pipe`、`--param ggc-min-*`、`-ftrack-macro-expansion=0`、-O1/-O2(仅 PR/测试构建) | GCC 手册 | ⏳ lab-13(10 个变体,同一虚拟机交错 3 次);当前无 `-g`(Release),所以 `-g0`/`-fno-var-tracking` 不适用 | ⏳ |
| 8 | 更快的编译器二进制:官方 LLVM 发布版、llvm-mingw、WinLibs GCC | LLVM 文档(PGO/LTO 构建) | ⏳ lab-13 尝试官方 LLVM(`--gcc-install-dir` 指向 MSYS2);失败则记录原因 | ⏳ |
| 9 | 进程创建开销 | 实测 | `g++ --version` 29 ms,ccache 命中 66 ms,depend 模式 66 ms = 直接模式,预处理模式 451 ms | **否决**(无杠杆) |
| 10 | 预构建第三方(Boost 作为已安装包,按版本 + 工具链哈希缓存) | CMake `FetchContent`[S13] | Boost 编译 758 CPU 秒(冷构建的 5.5%),PyroWave/Granite 另 155;热构建里它们本来就是缓存命中 | **否决**(只对夜间冷构建有 5% 收益,换来额外的缓存键与安装步骤) |
| 11 | Ninja ≥ 1.12 临界路径调度 | Ninja 手册(未记载此功能) | MSYS2 的 ninja 是 1.13.2;日志显示最长的编译在构建开始处(2%–55%)启动,最后 15% 时间里仅 0–23 s 低于 4 路并行 | **否决**(无尾部问题) |
| 12 | Unity 构建 | CMake `UNITY_BUILD`[S13] | 全局启用:Boost 内 `WinSock.h` 重复包含、`archive_serializer_map.ipp` 重定义,构建失败 | **否决**(要逐目录选择并排除冲突文件,收益与 PCH 重叠) |
| 13 | 拆成并行 Windows 作业(产品 / 测试) | GitHub 作业并行 | 共享对象库后测试半边仍要主机对象,关键路径=测试半边,拆分不降低它,只重复 4–5 分钟的检出与工具链 | **否决**,冷构建用 `schedule` 矩阵并行 GCC/Clang 两条腿 |
| 14 | 远程编译缓存放 Cloudflare R2(在**免费额度内**) | R2 定价页:每月 10 GB-月存储、100 万次 Class A、1000 万次 Class B 免费;出口免费;Worker 免费版 100,000 请求/天[S29] | 适合:一次构建约 1300 次 GET + ≤ 950 次 PUT,存储 0.2–0.5 GB。**但无明确提速**:见下 | **否决**(见下) |
| 15 | 托管远程构建执行(BuildBuddy/EngFlow 式) | — | 所有者已排除 | **不做** |
| 16 | C++20 模块 / `import std` | CMake 4.5 手册:GCC 15+、Clang 18+、Ninja ≥ 1.11 支持[S13];GCC 16 发布说明仍称模块"实验性"[S30] | 技术上可行(CMake 4.5、GCC 16.2、Ninja 1.13.2 都满足),但标准头文件之外的重头(Boost、nlohmann、Windows 头)不是模块,且 GCC 16 的支持是实验性的;预期收益 ≤ 标准头文件那一部分解析 | **不迁移**,仅记录可行性 |
| 17 | 运行时 PGO / ThinLTO(发布的 `sunshine.exe`) | Clang/GCC 手册 | 改变产品性能而不是 CI 速度;需要有代表性的训练负载;LTO 增加发布构建时间 | **推迟**,列入后续路线,本轮不实现 |

**R2 远程缓存(第 14 项)为什么否决:** 免费额度够用(上面的估算),但它要解决的问题已经被更简单的做法解决——(a) 命令行一致 + 共享对象库后,热缓存构建只要 85–92 s,远程缓存最多再省几十秒;(b) 默认分支缓存变旧(采用晋升后 master 不再构建)用"master `push` 时并行跑一次不阻塞的缓存刷新构建 + 夜间全量"解决,不需要新服务;(c) R2 需要 Worker 代理(ccache 的 HTTP 后端不会 SigV4)和令牌管理,fork PR 只读要靠公共读取;多出的基础设施与攻击面换不来可见提速。如果将来 PR 常常落在过期缓存上、实测未命中成本变成主要部分,再重新评估。

## 7 并行与关键路径

| # | 并行化 | 离开关键路径的时间 | 重复成本 | 依据/状态 |
|---|---|---|---|---|
| 1 | Web UI(lint+测试+i18n+vite build)作为独立 Linux 作业 | 99–111 s | 无 | **已实现**。实测整个作业约 2–3 分钟,在 Windows 构建期间早已跑完,失败更早 |
| 2 | 安装器 UI 与固定资产 | 223 s 或固定资产 2 s | 若从子模块构建:一个 Windows 作业 | §9.2;等 #1254 合并后接入 |
| 3 | C++ 拆成"产品+打包"与"测试"两个并行 Windows 作业 | 无净收益 | 4–5 分钟 | 共享对象库后测试半边仍要主机对象(§6.3 第 13 项)→ **不拆** |
| 4 | `vdd_smoke`、`mat_smoke` | 已并行 | — | 保持,按路径条件运行 |
| 5 | `setup_release` | 已并行(19 s) | — | 只在会发布的场景运行 |
| 6 | ctest 并行 | 51 s → ⏳ | — | `-j4` |
| 7 | 缓存修剪 | 出关键路径 | — | **已实现**(独立 ubuntu-slim 作业) |
| 8 | 打包与 ZIP 并行 | ⏳ | 内存(`lzma2/ultra64` 编译字典约 742 MB[S15]) | 待测 |

### 7.1 各场景关键路径(分钟)

| 场景 | 链 | 合计 |
|---|---|---|
| (a) PR,C++,热缓存 | 启动 0.1 → 检出 0.2 → 子模块 1.7 → 工具链 2.4 → configure 1.1 → 构建 1.5(+未命中)→ ctest 0.85 → 上传 0.2 | **≈ 8–9**(+ 未命中) |
| (a') Web UI | `changes` 0.2 → Linux `webui` 2.3 | ≈ 2.5 |
| (c) master 晋升 | 启动 0.2 + 查找 + 下载 0.6 + 校验 0.3 + 发布 1 | 2–3(推算) |
| (d) 稳定版 | 下载+校验 1 + 三次 SignPath 各 1–3 + 打包与校验 2 | 8–12 |
| (e) 文档 | `changes` 0.2 + 链接检查 0.4 + gate 0.2 | ≈ 1 |
| (f) 工作流 | `changes` 0.2 + lint 约 1 + gate 0.2 | ≈ 1.5 |
| (g) 夜间冷全量 | 工具链 4.5 + 构建 34(GCC,共享对象)+ ctest 1 + 保存缓存 | ≈ 40 |
## 8 "晋升,而不是重建"(promote, don't rebuild)

问题:一个 PR 通过之后,master 能否直接复用该结果,而不是再全量构建一次。

### 8.1 先决事实(均已核对原文或实测)

| 事实 | 结论 | 来源 |
|---|---|---|
| 合并队列对"组织拥有的公开仓库"可用(组织为 Free 也可);队列在临时分支 `gh-readonly-queue/{base}/pr-N-…` 上构建,该提交的 SHA 与 PR 的提交不同;Actions 用 `merge_group` 事件(`checks_requested`),`GITHUB_SHA` = 合并组的 SHA | 队列构建的是"将要落地的那个提交",并且运行在基础仓库上下文,可使用密钥(RTX/DLSS 适配器可以构建) | [S20] [S1] 事件表 |
| 官方文档只说"临时分支会被合并进目标分支",没有明说落地提交的 SHA 是否等于被测试的 SHA;社区实测报告 squash 队列下 main 快进到被测提交,但这不是官方保证;fork 是用户仓库,**无法在 fork 上验证合并队列** | 设计不依赖这一点:master 上先验证 `GITHUB_SHA == 队列产物里记录的提交`(并附带树哈希),不相等就回退到完整构建。上线前需要在 AlkaidLab 组织下一个沙盒仓库验证一次 | [S20] + 社区报告(二手) |
| 发布 tag/版本由 `LizardByte/setup-release-action` 从 push 事件的 `commits[0].timestamp` 计算(`年.月日.时分秒`);实测该时间戳等于提交的**committer date**(不是 author date) | tag 是提交的纯函数,队列运行中可以用 `git log -1 --format=%cI` 算出与落地后完全一致的版本,因此版本字符串可以在构建时编译进去 | action 源码 `action/main.py:256-270`;lab-6 实测 |
| 构建标识(`cmake/prep/build_info.cmake`)编译进 `build_info_defs.h`:提交、分支、dirty、渠道(`beta`/`dev`)、构建时间(`string(TIMESTAMP)`,遵守 `SOURCE_DATE_EPOCH`)、变体、包类型、builder、CI 运行 URL;另有 `PROJECT_VER`(master 用 `BUILD_VERSION`,其他用基础版本加提交号)和 Windows VERSIONINFO | 队列运行把 `BRANCH=master`、渠道 `beta`、时间取提交时间、CI 运行 URL 取队列运行,就与 master 构建产物的标识一致(运行 URL 指向真正构建它的那次运行,本就应该如此) | 源码阅读 |
| `merge_group` 触发的运行**不能**写默认分支的 ccache 作用域(只有 push、workflow_dispatch、repository_dispatch、delete、registry_package、page_build、schedule 可以);PR 创建的缓存只属于 `refs/pull/N/merge`,其他 PR 和 master 读不到 | 采用晋升后 master 不再构建,默认分支的 ccache 会变旧,队列构建靠它提速——需要一个每夜的 `schedule` 全量构建保鲜(同时跑 GCC/clang 矩阵、完整测试) | [S3] |
| RTX/DLSS 适配器 DLL 的 SHA-256 被编译进 sunshine(信任头文件),所以"没有密钥的 PR 构建"与"有密钥的发布构建"的二进制本质不同 | fork PR 的产物永远不能当 master 产物晋升;只有队列(基础仓库上下文)的产物可以 | `GenerateRtxVideoTrustHeader.cmake`、sign-and-repackage 注释 |

### 8.2 方案 A(推荐):合并队列构建,master 只发布

流程:
1. PR(含 fork PR)照常跑 §6 的 PR 流水线(无密钥,适配器关闭,快速)。
2. 点"加入合并队列":`merge_group` 事件触发同一个工作流的"候选构建"路径,在基础仓库上下文构建将要落地的提交:渠道 `beta`,`BRANCH=master`,`BUILD_VERSION` 由 `git log -1 --format=%cI` 按 action 的同一公式计算,`SOURCE_DATE_EPOCH` = 提交时间,RTX/DLSS 适配器 ON。产出未签名的发布产物(安装器、便携包、校验和)和一个 `candidate.json`(提交 SHA、树哈希、版本、渠道、产物 SHA-256),全部用 `actions/attest` 做构建来源证明[S19],上传为工件,保留 14 天(足够排队与重试;发布后真正的资产在 GitHub Release 里)。
3. 队列通过后快进 master,`push` 触发"晋升"作业(ubuntu,不装工具链):
   - `gh api` 查找同一 SHA 的成功 `merge_group` 运行;下载 `candidate.json` 与产物(`actions/download-artifact` 的 `run-id` + `github-token` 跨运行下载[S26]);
   - 校验:`candidate.json.commit == GITHUB_SHA`、`git rev-parse $SHA^{tree}` 与记录一致、`gh attestation verify <文件> --repo <仓库> --signer-workflow <工作流>`[S19]、SHA-256 与 `SHA256SUMS` 一致;
   - 用晋升作业创建/更新预发布(`prerelease: true`,tag 与现在一致)、上传同名资产(§9 的命名与 legacy 更新器规则不变)、发布说明;
   - 任一校验失败或找不到候选 → 自动回退到完整构建路径(即现有行为),并在作业摘要写明原因。
4. 稳定版仍由 `release: published` 触发签名(§9),使用同一份候选产物(下面 8.4)。

master 上仍必须运行的:发布(release 创建、资产上传)、发布说明、预发布 tag、校验和与来源证明校验、`ci-gate`;不需要编译、不需要 MSYS2、不需要 ccache。

预期 master 耗时:晋升作业 ≈ 作业启动 10 s + 查找 3 s + 下载约 200 MB 产物 20–40 s + 校验 10 s + 创建 release/上传 30–60 s ≈ **2–3 分钟**(推算,依据:artifact 下载/上传按 runner 带宽,lab-4 的 `fetch-installer-inputs` 实测 2 s/几十 MB;发布步骤现状 < 1 分钟)。回退路径(无候选)= 完整构建,与现状同。

代价:合并队列要求维护者开启并把 `merge_group` 触发加入必需检查;队列里每个 PR 多一次完整候选构建(PR 阶段 + 候选阶段),但候选构建用热缓存与 PR 同样快,而 master 不再构建,**总机器分钟数下降**。

### 8.3 方案 B:不用合并队列,按"树哈希 + 工具链/输入哈希"复用

要求:构建标识必须在打包时注入,而不是编进目标文件。需要搬动的内容:`build_info_defs.h`(整体,仅 `build_info.cpp` 包含)改成链接末尾才生成的单个小目标文件(提交、分支、渠道、时间、CI URL),`PROJECT_VER`(含提交/tag)同理,Windows VERSIONINFO 在链接后用 `windres` 重生成资源再最终链接。这样除"标识目标文件 + 最终链接"外的目标文件只依赖树内容,可以按树哈希缓存。

但有两个硬障碍:
1. PR 缓存/工件的信任:fork PR 的构建没有密钥,适配器关闭,二进制不同(§8.1 最后一行),所以 B 只适用于"同仓库、有密钥"的分支;对 fork PR 完全不适用,等于大多数外部贡献仍要在 master 重建。
2. PR 的 `refs/pull/N/merge` 树 = 基线(合并时刻)+ PR;squash 落地时基线可能已前进,树哈希不同,复用失效。

预期 master 耗时:命中时 = 重新链接 + 打包 ≈ 4–6 分钟(推算:链接 140 个目标中只需 `sunshine` 及工具,实测单个测试可执行文件 0.1–2 s;打包见 §4);未命中 = 完整构建。工作量大、可复用面窄,**不推荐**;作为方案 A 不可用时的备选。

### 8.4 稳定版:签名什么

目标:被签名的就是被测试、被晋升的那份构建(F9 指出现状是重新编译,工具链与 CI 不同)。`sign-and-repackage.yml` 改为:下载同一提交的候选产物 → 验证来源证明与提交 → 按 §9.3 签名 → 重新打包。预计从约 35–45 分钟(重新编译 + 签名)降到约 8–12 分钟(其中 SignPath 排队/处理时间不可控)。SignPath 的 GitHub 连接器要求被签名工件是**本次运行**上传的 GitHub 工件[S16],所以"下载后在本次运行里重新上传再提交签名",并保留来源证明校验;该做法与仓库里已有的 `test-signpath.yml`(下载另一次运行的工件后签名)一致。

## 9 发布管线(保持不变的部分 + 端到端设计)

### 9.1 必须保持不变

| 项 | 规则 | 现状位置 |
|---|---|---|
| 资产名 | `Sunshine.<tag>.WindowsInstaller.exe`、`Sunshine.<tag>.Portable-x64.zip`、`SHA256SUMS.txt`、`checksums.json`;tag 形如 `vYYYY.DATE`,DATE 补零到 4 位 | main.yml "Rename release assets" |
| legacy 更新器 | 旧控制面板把名字里含 `windows` 的资产当安装包,所以只有安装器可以含该词;构建中有守卫(不符合就失败) | main.yml 同上 |
| 预发布 | master 发布的是预发布(`prerelease: true`),tag/名称 `<tag>.杂鱼`,`allowUpdates: true`;稳定版由人把预发布发布为正式版,`release: published`(非预发布)触发签名 | main.yml、sign-and-repackage.yml |
| 签名 | SignPath 项目 `Sunshine-Foundation`,策略 `release-signing`,配置 `windows-portable` / `windows-installer`;被哈希绑定的文件(RTX 适配器 DLL、mat-capture DLL)签名后还原为未签名原件 | sign-and-repackage.yml |
| 适配器构建要求 | 受保护的 master 构建必须产出 RTX 适配器,否则失败 | main.yml "Package Windows" |

### 9.2 安装器输入与"安装器 UI"

PR #1254 在 CI 里构建安装器 UI(npm + cargo/MSVC)并按 sha256 固定拉取 GUI 与 WebView2 引导程序。实测(lab-4,windows-latest):

| 做法 | 秒 | 备注 |
|---|---|---|
| 冷构建安装器 UI | 236 | |
| 恢复 cargo 注册表 + `target` 缓存后构建 | 186(恢复缓存 18 s) | 只省 50 s:crate 本身与 tauri 链接仍要编译 |
| 下载固定的、哈希校验的 Panel 发布资产 | **2** | `fetch-installer-inputs.ps1` 对 GUI+WebView2 的实测值 |

建议:安装器 UI 由 Panel 仓库自己的 CI 构建(它自带缓存),作为 Panel 发布资产发布并附来源证明;本仓库的 `scripts/installer-inputs.json` 增加 `setupUi` 条目(tag、commit、sha256),CI 只下载并校验(sha256 + `gh attestation verify --repo AlkaidLab/foundation-panel`)。保留 `workflow_dispatch` 输入 `build_setup_ui_from_submodule` 以便 Panel 变更时从子模块构建。需要 Panel 仓库配合(跨仓库改动,由所有者决定)。如果暂不改 Panel 仓库,退而求其次:安装器 UI 单独成作业、与主构建并行,用 cargo 缓存,只在安装器相关路径或发布时运行(PR 普通代码改动完全不构建它)。

**F8:** PR #1254 的 `build-setup-ui.ps1` 在 `$PSNativeCommandUseErrorActionPreference = $true` 下运行 `robocopy`,而 robocopy 成功时返回 1——在 CI 的 pwsh 7 下该步骤失败(lab-4 复现)。需要在调用 robocopy 的前后关闭/恢复该偏好,或在调用处不依赖它。

### 9.3 稳定版签名(安装器是"外壳 + UI + WebView2 + 引擎 + 页脚")

事实:
- Authenticode 哈希覆盖证书表之前的所有字节,包括节之后的追加数据(overlay)[S17 第 14 步]。lab 实测:签名后证书表紧接在页脚之后;签名后改一个 overlay 字节 → `HashMismatch`;签名后再追加数据 → `NotSigned`。
- 所以**最终装配好的文件必须最后签名**,先签外壳再追加会使签名失效。
- 外壳现在从文件**最后 1024 字节**找页脚(`read_payload_footer`、`read_named_payload`),签名后页脚被证书表挤出这个窗口,安装器会找不到载荷。现有的发布流程签的是"引擎即安装器"时没有这个问题,新结构会触发。

设计:
1. 构建一次(§8 的候选构建),得到未签名的便携目录、`sunshine-installer-shell.exe`(外壳,不单独签)、安装器 UI、WebView2 引导程序(微软已签名)。
2. SignPath 请求 1(`windows-portable`):便携目录里的全部 PE + 安装器 UI;哈希绑定的文件排除并签后还原(现有逻辑)。
3. 用签名文件重新打包便携 ZIP;重新生成 Inno 暂存目录并 ISCC 得到 `Sunshine-engine.exe`。
4. SignPath 请求 2(`windows-installer`):签名 `Sunshine-engine.exe`(引擎由提升权限的 worker 运行,保持现状"被运行的安装包是签名的")。可选:若评估认为不必要则省略这一次,默认保留。
5. `append_installer_payload.py` 装配 `[外壳][ui][wv2][引擎][页脚]`。
6. SignPath 请求 3(`windows-installer`):签名装配后的整个文件。页脚里各节的 sha256 与外层签名共同保护载荷。
7. **外壳必须能在证书表之前找到页脚**:读取自身 PE 头的 `IMAGE_DIRECTORY_ENTRY_SECURITY`(数据目录第 5 项)得到证书表偏移,页脚窗口取 `[certOffset−1024, certOffset)`;无证书表时仍取文件末尾。一个共享的小函数同时服务 `read_payload_footer` 与 `read_named_payload`,并增加一个 `--verify-payload`(只校验页脚和各节哈希,不安装)供 CI 冒烟。需要用自签名证书的"签名后夹具"做单元测试(本实验的脚本可以直接改成测试夹具)。
8. 验证:`Get-AuthenticodeSignature` 对全部产物为 `Valid`;对签名后的安装器运行 `--verify-payload`。
9. 时间:三次 SignPath 请求串行,每次含排队与处理,现配置的超时是 600 s;预计 4–9 分钟(SignPath 延迟未测)。
10. SignPath 侧:构建策略建议开启 `runners.require_github_hosted: true`;是否 `disallow_reruns` 由所有者决定[S16]。

### 9.4 供应链(发布相关)

- `permissions:` 顶层 `{}`,各作业按需声明;只有 `release`/`promote`/`sign` 作业有 `contents: write`、`id-token: write`、`attestations: write`[S2]。
- 全部 action 固定到完整提交 SHA 并保留 `# vX.Y.Z` 注释,Dependabot 用 `github-actions` 生态更新这些固定[S2](该文档明确说明 Dependabot 会按同一行注释更新)。本次已解析的当前 SHA:`actions/checkout@v7` = `3d3c42e5…`(LLVM premerge 也固定了同一个提交,可交叉印证[S21]),`actions/cache@v6` = `55cc8345…`,`actions/upload-artifact@v7` = `cf430e03…`,`msys2/setup-msys2@v2` = `ec48f7c5…`,`signpath/github-action-submit-signing-request@v2` = `c92b9587…` 等,完整表在 `.github/dependabot.yml` 与工作流中。
- 变更文件检测不用第三方 action:`tj-actions/changed-files`(CVE-2025-30066)曾被篡改标签泄露密钥[S22];LLVM 的做法是 `gh api repos/…/pulls/N/files`(无检出、无第三方)[S21],lab-4 实测该调用 0.6–0.8 s。
- 上游 `foundation-ui` 以固定提交(同 `installer-inputs.json` 的做法)引用,不再 `git clone --depth 1` 浮动 HEAD(F7)。
- `actions/attest`(公开仓库免费)为发布产物生成来源证明,晋升和签名作业用 `gh attestation verify` 校验[S19]。

## 10 实施状态、验证与风险

### 10.1 已在 `ci/redesign` 上的提交(可审查的小提交)

| 提交 | 内容 | 验证 |
|---|---|---|
| `scripts/ci/init-submodules.sh` | 定向子模块 | fork 上检出 + 配置 + 完整构建通过 |
| `scripts/ci/report-junit.py` | JUnit→注解/摘要;无测试、测试数骤降、未登记的跳过都失败 | 样例单测;fork PR 上报告 769 个 Web UI 测试 |
| `00efef82` | 版本常量移出 `nvhttp.h` | fork 构建 |
| `04f488ba`、`e025bdc8`、`f62f4070` | 三个 Clang 暴露的标准符合性缺陷(各自可上游) | Clang 编译 |
| `777a7c4b` | 共享主机对象库 | 冷 −41%、热 −46%、86/86 测试 |
| `200cbb11` | 适配器 `-D/-I` 只给 3 个文件 | 命令行对比;RTX 适配器分支需要密钥,**fork 上无法验证**,由 #(c) 的候选构建首次验证 |
| `b7fc88c9` | `logging.h` 去掉 sinks | fork PR 构建 |
| `7d76c34b`…`ee57762a`…`b05688e4` | 脚本、`ci.yml`、composite action、Dependabot;`main.yml` 不再构建 PR | fork PR:`changes`、`docs-check`(被正确跳过)、`workflow-lint`、`webui`、`win-build` |
| `a4d590c0` | 删除 `main.yml` 里孤立的 `fi` | `bash -n` |
| TEMP 提交 | 从 #1254 拷来的测试修复,让完整套件能链接;**#1254 合并后丢弃** | — |

### 10.2 接入顺序

1. #1254 合并后 rebase,丢弃 TEMP 提交;接入安装器 UI 与固定资产(§9.2)。
2. 开启合并队列、把 `ci-gate` 设为必需检查(规则集目前 disabled)、在 AlkaidLab 组织下的沙盒仓库验证"队列落地的提交 SHA 等于被测试的 SHA"(§8.1),再启用晋升。
3. 选定默认编译器:Clang 需 lab-14 交错重复显示稳定更快,并且全部测试在 Clang 上通过(3 个缺陷已修,需复测)。
4. 签名三步与外壳页脚定位修改(§9.3)作为独立 PR,用自签名夹具测试;SignPath 真机验证只能在下一次发布时做。

### 10.3 风险

| 风险 | 缓解 |
|---|---|
| 单次计时受虚拟机差异影响(±20–50%) | 同一虚拟机交错重复;文档中标明单次与重复 |
| 共享对象库改变了测试看到的二进制 | 只共享与 `SUNSHINE_TESTS`/`SUNSHINE_SHADERS_DIR` 无关的文件(9 个文件仍各编一次);头文件里的 `#ifdef SUNSHINE_TESTS` 只增加声明;86/86 测试通过;`-DSUNSHINE_SHARE_HOST_OBJECTS=OFF` 一键回退 |
| 合并队列行为未经官方文档保证 | 晋升前校验 SHA 与树哈希,不相等回退到完整构建 |
| Defender 排除项 | 范围限于构建目录与编译进程;runner 一次性 |
| 误判为文档改动而漏测 | 未知路径一律算 C++;分类器有 12 个单测 |
| 夜间依赖漂移 | 锁定版本 + 夜间金丝雀对最新 MSYS2(设计,待实现) |

## 11 来源索引(均已读取原文;抓取日期 2026-11-14)

| 编号 | 来源 | 版本/日期 | 用于 |
|---|---|---|---|
| S1 | GitHub Docs "Troubleshooting required status checks" | 在线版 | 路径过滤跳过 = Pending;条件跳过的作业 = 成功;`always()`;`merge_group` |
| S2 | GitHub Docs "Secure use reference" | 在线版 | SHA 固定、最小权限、Dependabot 按注释更新、`pull_request_target` |
| S3 | GitHub Docs "Dependency caching reference" | 在线版 | 缓存作用域:PR 缓存只属于 `refs/pull/N/merge`;`merge_group` 不能写默认分支缓存 |
| S4 | GitHub Docs "Workflow commands" | 在线版 | 注解、`$GITHUB_STEP_SUMMARY` |
| S5 | GitHub Docs "Workflow syntax" | 在线版 | 并发、容器作业只支持 Linux runner |
| S6 | GitHub Docs "GitHub-hosted runners" / "Larger runners" / "Actions runner pricing" | 在线版 | 公开仓库标准 Windows 4 vCPU/16 GB 免费;larger runners 仅 Team/Enterprise;Windows 8 核 $0.042/分钟 |
| S7 | actions/runner-images `Windows2025-Readme.md` | 镜像 20261111.331.1 | 预装 Inno Setup 6.7.1、Ninja 1.13.2、CMake 3.31.6、MSYS2 未入 PATH |
| S8 | actions/checkout README | v7 | `persist-credentials`、`sparse-checkout`、`filter`、`submodules` |
| S9 | msys2/setup-msys2 README | v2 | `release: false`、`update: true` |
| S10 | MSYS2 文档 "Environments"、"Using MSYS2 in CI" | 在线版 | MINGW 环境里的 Clang 仍用 GNU 链接器与 GNU C++ 库;CLANG64 用 libc++/lld |
| S11 | git 文档 `git-submodule`、`git-clone`(2.56.1) | 2.56.1 | 浅克隆、`--filter`、稀疏检出 |
| S12 | ccache 手册 | 4.14.3 | 直接模式/预处理模式/manifest、`remote_storage`、PCH 前提、depend 模式、压缩 |
| S13 | CMake 文档 | 4.5.0 | `ctest --output-junit`/`--no-tests`/`--repeat`、`FetchContent`、`target_precompile_headers`、`UNITY_BUILD`、OBJECT 库、`import std` |
| S14 | Node.js 测试运行器文档 | v26.13.1 | `--test-reporter=junit`、`--test-concurrency` |
| S15 | Inno Setup 帮助 `Compression`、`LZMANumBlockThreads` | 在线版 | 压缩档位与内存 |
| S16 | SignPath 文档 "Trusted build systems: GitHub" | 在线版 | 工件必须在本次运行上传;`require_github_hosted` 策略 |
| S17 | 微软 "Windows Authenticode Portable Executable Signature Format" | docx | 第 14 步:overlay 被哈希;证书表位置 |
| S18 | 微软 Dev Drive 文档;社区测评(pip 维护者博客) | 在线版 | Dev Drive/性能模式;标准 runner 自带 D: 盘 |
| S19 | GitHub Docs "Using artifact attestations";`gh attestation verify` | 在线版 | `actions/attest@v4`、验证 |
| S20 | GitHub Docs "Managing a merge queue"、"Events that trigger workflows" | 在线版 | 公开组织仓库可用;`merge_group`;临时分支 SHA 不同于 PR |
| S21 | LLVM `premerge.yaml`、OBS Studio、mpv、LizardByte Sunshine 的工作流文件 | master 当前 | `gh api pulls/N/files`、SHA 固定、`paths-ignore`、顶层 `permissions` |
| S22 | tj-actions/changed-files 事件(CVE-2025-30066)报道 | 2025-03 | 避免第三方 changed-files |
| S23 | npm `npm ci` 文档 | 11.21.2 | 干净安装 |
| S24 | sccache GHA 后端文档 | main | 每目标文件一次缓存往返 |
| S25 | LLVM lld 文档 | main | lld 与 GNU ld 对比 |
| S26 | `actions/upload-artifact`、`actions/download-artifact` README | v7/v8 | `compression-level`、`retention-days`、跨运行下载 |
| S27 | Cargo 文档 "Cargo Home";Swatinem/rust-cache | 在线版 | 缓存 `registry`、`target` |
| S28 | Boost.Asio 文档 "Using Boost.Asio"(separate compilation) | latest | `boost/asio/impl/src.hpp` + `BOOST_ASIO_SEPARATE_COMPILATION` |
| S29 | Cloudflare R2 定价页;Workers 限制页 | 在线版 | 免费额度:10 GB-月、100 万 Class A、1000 万 Class B;Worker 免费版 100,000 请求/天 |
| S30 | GCC 15、16 发布说明 | gcc.gnu.org | 模块与 `import std` 状态("实验性") |
| S31 | Ninja 手册 | 在线版 | 未记载临界路径调度 |
| S32 | actionlint | 1.7.12 | 工作流静态检查,SHA-256 `8aca8db9…` |

实验环境:`windows-latest`(Windows Server 2025,镜像 20261111.331.1),MSYS2 UCRT64:GCC 16.2.0-7、CMake 4.5.0-1、Ninja 1.13.2-1、ccache 4.14.2-1、binutils 2.47-4、Clang/LLD 23.1.5。
