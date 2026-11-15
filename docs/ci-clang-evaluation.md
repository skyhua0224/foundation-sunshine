# Windows 构建改用 Clang 的评估:未采用

2026-11-15。托管 `windows-latest`(4 vCPU / 16 GB)上实测,结论:**端到端没有可靠收益,不切换,GCC 保持默认。**
完整改动留在分支 `ci/clang-final`(单个提交,基于当时的 `ci/batch-2`),需要时可以单独挑选其中与编译器无关的部分。

## 端到端:同一提交、同一时刻并发(验证分支)

| 运行 | 编译步骤 | 整个 Windows 作业 | 测试 |
|---|---|---|---|
| Clang(预编译头 + ccache 依赖模式)1 | 1963 s | 2899 s | 86/86 通过 |
| Clang 2 | 2036 s | 2997 s | **失败 1 项**(见下) |
| Clang 3 | 2004 s | 2965 s | 86/86 通过 |
| GCC(同样的依赖模式)1 | 2099 s | 2916 s | 86/86 通过 |
| GCC 2 | 1988 s | 2743 s | 86/86 通过 |
| GCC 3 | 2815 s | 未完成时取数 | — |

两个编译器的编译步骤中位数相差不到 5%,远低于 10% 的门槛。单个构建的编译 CPU 总量也一样:Clang 12454 s,GCC 12729 s;
最重的 `src/http/server.cpp` Clang 138 s、GCC 120 s(Clang 更慢)。

## 为什么早先的数字看起来好很多

不同时刻、不同虚拟机的冷构建:Clang(无预编译头)8 次 1612–2499 s,中位数 1672 s;GCC 5 次 2197–3232 s,中位数 2257 s,
看起来快 26%。但**同一配置在不同虚拟机上的编译步骤相差 1.6 倍**(GCC 1988–3232 s),这个差距比编译器之间的差距大,
不同时刻的对比只反映抽到的虚拟机。带预编译头的两次 889 s 和 1316 s 同样落在这种波动里,不能作为证据。

只有同一虚拟机内交错重复的测量可信(24 个编译单元,`-j4`,3 次取中位数):

| 项 | 结果 |
|---|---|
| Clang 对 GCC | 0.70–0.88×(三台虚拟机),但落到整个作业里被 Boost、链接、打包、最重的单个文件和工具链安装稀释 |
| 预编译头(标准库 + nlohmann + Boost.Log + property_tree) | −10%;再加 `windows.h` −13%,但有几个源文件因 `NTSTATUS` 不能用,未采用 |
| ccache 未命中路径:直接模式 / 依赖模式 / 不用缓存 | 139.9 s / 127.9 s / 124.8 s;依赖模式比直接模式快 8.6%,命中耗时不变(三台虚拟机一致) |
| 预编译头 + ccache | 必须 `sloppiness=pch_defines,time_macros`,否则用到预编译头的单元完全不被缓存(命中 0) |
| `-O1`(仅编译) | −11%,但测试和打包的不再是发布配置,未采用 |
| 调试信息 | 发布构建本来就没有 `-g`;加 `-g` 慢 20%,`-gline-tables-only` 慢 3% |
| ninja 并行度 | `-j4`、`-j5`、`-j6`、`-j8` 无差别,空闲内存始终超过 10 GB |
| Defender 排除项 | runner 的实时保护本来就是关的,无可测量的影响 |
| lld 对 GNU ld | 无收益(此前实测) |

## Clang 构建本身的质量

- 运行时:同一虚拟机上交替运行,用各自编译器编译的 FEC 编码器,Clang 编码快 12%、解码快 14%。
  `sunshine.exe` 85.1 MB(GCC 101.0 MB),符号表仍在(14.3 万个,GCC 22.4 万个),崩溃符号化仍可用。
- 警告:把全部 Clang 警告清零(缺 `override`、未使用的成员和常量、布尔上的 `|`、C++ 里的变长数组、多余的 lambda 捕获、
  `dllexport` 重复声明、仅 GCC 认识的 `-Wno-` 选项、Boost 头文件按系统头处理),没有用笼统的 `-Wno`。
- `HttpSafety.SingleHttpsThreadRemainsResponsiveAndDisconnectCancelsAllStreams` 失败的原因是测试辅助函数
  `wait_until` 在条件成立后又求值一次,而条件 `submit_stream` 每成功一次就占一个许可,第二次可能找不到空位。
  与编译器无关(Linux 上两种编译器的复现程序约 50% 失败),GCC 只是时序上躲过。已改为每轮求值一次,并先等全部许可归还。
  修复后所有运行都通过,另外 `ctest --repeat until-fail:10` 也通过。

## 这次运行遇到的失败

Clang 第 2 次运行的 `test_sunshine` 在 `Poster` 测试里以 `0xc0000409` 退出:
`filesystem error: cannot remove all ... source.png ... being used by another process`。
`src/nvhttp/poster.cpp` 起了一个分离的后台线程生成预览,测试在预览出现后马上 `remove_all` 临时目录,
后台线程可能还开着 `source.png`,Windows 上就是共享冲突。这是测试与后台线程之间的竞态,不是编译器的代码生成问题,
但它在 Clang 的完整运行里出现了 2 次(17 次中),在 GCC 的完整运行里 0 次(8 次中),时序相关。
没有修(切换已不再推进);如果以后切换,要先让测试等后台线程结束或对 `remove_all` 做重试。

## 与编译器无关、值得单独合入的部分

1. `HttpSafety` 测试辅助函数的修复(真实竞态)。
2. `CCACHE_DEPEND=1`:GCC 也受益(未命中路径快约 9%),命中不变。
3. Clang 暴露的标准符合性问题(`VdfNode` 的不完整类型、枚举范围外的 `constexpr` 转换)。
