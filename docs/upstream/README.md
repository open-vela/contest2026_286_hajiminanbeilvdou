# 作品代码部署说明（补丁 + 源文件）

本作品运行在黄山派 SF32LB52（`sf32lb52_lchspi_ulp`）上，
**全部改动都在 openvela 公共仓（packages/ai_agent、vendor/sifli、apps）之内**，
因此以「补丁 + 新增源文件」的形式提交；本仓 `src/` 下同时保留了可读的源文件副本。

补丁**已在本机实测过可干净应用**（`git apply --check` 通过），
由开发时的真实工作树直接生成，与 `src/` 下的源文件**逐字节一致**，不存在两份分叉。

---

## 一、补丁清单

| 补丁 | 目标仓库 | 规模 | 内容 |
|---|---|---|---|
| `patches/01-ai_agent.patch` | `packages/ai_agent` | 26 个文件 | LVGL 聊天 UI（含 PTT）、麦克风上行 `mic_stream`、`cmd_mic` 调试命令、GB2312 中文字体、WS 服务器（二进制帧/发送锁/广播）、消息总线镜像上屏、LLM 看门狗、技能描述解析修复、工具裁剪、时钟窗口与偏移、TLS 单次写上限 |
| `patches/02-vendor_sifli.patch` | `vendor/sifli` | 8 个文件 | **AUDCODEC ADC 录音驱动**（`sf32lb_audcodec.c/.h` + Kconfig/CMakeLists 挂载）、板级 defconfig（`configs/agent/`）、GCC14 缺原型修复 |
| `patches/03-apps-pppd.patch` | `apps` | 6 个文件 | pppd 直连（空 modem 脚本、`/dev/console`、`local_ip` 字段）、拨号时代自毁逻辑修复 |

## 二、部署步骤（在一个干净的同版本工作树上）

```bash
# 1. 应用三个补丁
cd <工作树>/packages/ai_agent && git apply <本仓>/docs/upstream/patches/01-ai_agent.patch
cd <工作树>/vendor/sifli     && git apply <本仓>/docs/upstream/patches/02-vendor_sifli.patch
cd <工作树>/apps             && git apply <本仓>/docs/upstream/patches/03-apps-pppd.patch
```

补丁已包含全部新增源文件（含 3.8 MB 中文字体源码与板级 defconfig），无需再手工拷贝。

> 如果你的工具链来自 Gitee 镜像且行尾设置不同，应用补丁时加 `--whitespace=nowarn`。

## 三、编译与烧录

```bash
cd <工作树>
./build.sh vendor/sifli/boards/sf32lb52/sf32lb52_lchspi_ulp/configs/agent \
  -e "-Wno-return-mismatch -Wno-implicit-function-declaration" --cmake -j8
# 产物: cmake_out/sf32lb52_lchspi_ulp_agent/nuttx.bin
```

⚠️ 改了 `defconfig` 后必须 `rm -rf cmake_out/sf32lb52_lchspi_ulp_agent` 再编译，
否则 CMake 不会重跑配置阶段，宏静默不生效。

烧录：Windows 侧 `sftool`，板载 CH340 串口。注意 `--before soft_reset` 不会驱动 RTS，
需先用 RTS 脉冲复位（ROM bootloader 只监听约 2 秒的 ATSF32 窗口），再以 `--before no_reset` 连接。

## 四、`src/` 与 `docs/upstream/` 的关系

```
src/ai_agent/            自研新增源文件（mic_stream、cmd_mic、字体、UI）
src/ai_agent/docs/sidecar/   01 补丁触及的既有文件的"修改后"版本（可直接阅读）
src/vendor_sifli/        自研 AUDCODEC 驱动 + 头文件
src/vendor_sifli/docs/sidecar/   02 补丁触及的既有文件
src/apps_pppd/           pppd 修改后的源文件
src/apps_pppd/docs/sidecar/      03 补丁触及的既有文件
board/sf32lb52_agent/defconfig   板级配置（与 02 补丁内的一份相同）
```

sidecar 是为了评审方便：不用打补丁就能直接读改动后的完整文件。

## 五、字体来源与再生成

`lv_font_misans_18_cjk.c` 由 MiSans 字体（小米开源字体）经 LVGL 字体转换生成，
覆盖 GB2312 汉字 + ASCII，编入固件约 +535 KB。
生成脚本见 `tools/host/` 中的字体生成脚本（`gen_cjk_font.sh`），可复现。

## 六、与上游 PR 的关系

本目录下的补丁同时也是提交到 openvela 公共仓的 PR 素材。
除此之外，开发过程中还发现并整理了若干**上游平台缺陷**（A1–A23 与 B1–B5），
其中一部分已包含在上述补丁里（如 TLS 单次写上限、技能描述解析、pppd 自毁逻辑），
另一部分（AUDCODEC HAL 层、Kconfig 缺少 select 等）属于纯上游修复，未包含在本作品补丁中，
将按赛事要求单独 PR 至各公共仓的 `dev-ai-contest-2026` 分支。
