# steamac：运行正版 EAC 游戏，并提供 DXMT 图形路径

[English](README.md) · [Русский](README.ru.md) · **简体中文**

> 本文是英文 README 的机器翻译，以英文版（[README.md](README.md)）为准。

**[steamac](https://github.com/fxgl/steamac) 的分支，面向 Apple Silicon Mac。可运行带有正版 Easy Anti-Cheat 的
x86-64 Windows 游戏（已在游戏中验证：VRChat；Mini Royale 也能通过 EAC），并提供两条图形路径可选。**

- **Vulkan（经典）**：Direct3D 经 DXVK 转为 SteamOS 内的 Vulkan（Venus），再经 KosmicKrisp 或 MoltenVK 在 Mac 上运行。
  这是上游 steamac 的路径。
- **DXMT（精简）**：Direct3D 11 经 [DXMT](https://github.com/3Shain/dxmt) 处理。它的 Metal 调用通过专用的 virtio-gpu
  通道在 Mac 上原生执行，转换层更少。

EAC 客户端是 Proton EasyAntiCheat Runtime 自带的原版。没有任何东西伪造、重放或绕过它的结果。与 FX GAMES、Valve、
Epic Games、VRChat 及 DXMT 作者均无关联。

**发现了错误？** 请在这里报告（提交 issue，或附带测试的 pull request），而不要只在你自己的分支里修复，这样每个人都能
得到修复。本分支自身不会向 libkrun、Mesa、FEX 或 Proton 提交任何内容；steamac 的改动直接与其维护者讨论。如果你审阅过
这里的某个改动并认为它足以进入上述某个项目，欢迎你自行提交：保留它的测试，遵循该项目的流程，并注明来源。FEX 代码改动
请以 pull request 的形式提交到 [StonkGuy/FEX](https://github.com/StonkGuy/FEX) 当前的 `eac-<release>` 分支，并在其
`eac-tests/` 中附带测试。

状态：仅在一台 Mac（M2、16 GB）上验证过。VRChat 在两条图形路径上都通过 EAC 并进入世界。默认使用 Vulkan 路径；DXMT
需在设置中开启。在这台 Mac 上实测：VRChat 在 Vulkan 上约 29-44 fps，在 DXMT 上约 76-78 fps（单次测试，见
[docs/eac-performance.md](docs/eac-performance.md)）。

## 要求

| 需要 | 说明 |
|---|---|
| Apple Silicon Mac | 已在 M2 MacBook Air（16 GB、macOS 27.0）上验证；其他 Mac 未测试 |
| macOS 15 或更新 | KosmicKrisp（更快的 Vulkan 驱动）需要 macOS 26。DXMT 路径的主机端库按 macOS 15.5 或更新构建 |
| 可用磁盘空间 | 建议 Mac 上约有 20 GB 可用空间，用于 SteamOS 磁盘及其下载缓存，另加游戏本身所需的空间（这是我们的估计，实际可能不同）。实测：创建磁盘期间其所在卷约需 14 GB，之后约 9 GB。home 大小在创建时选择，之后可用“扩展磁盘…”扩大 |
| 内存 | 至少 16 GB，更少不受支持。虚拟机内存自动取 Mac 内存的一半（4-16 GiB） |
| Steam 账号 | 游戏在 SteamOS 内安装和运行 |

## 安装与首次启动

1. 从本分支的 `main` 分支或某个[发布](https://github.com/StonkGuy/steamac/releases)标签构建应用：`./build.sh`（[要求与构建](docs/upstream/requirements-and-building.md)；DXMT 路径还需在 layer 步骤之前运行 `./build.sh dxmt`）。也可以下载[发布](https://github.com/StonkGuy/steamac/releases)附带的 DMG（自 v1.9-eac.1 起，由 GitHub Actions 构建，ad-hoc 签名）。把 `work/out/`（或 DMG）中的 **FX Steam Launcher.app** 复制到“应用程序”并打开（见下文*首次打开*）。
2. 首次启动时选择**新建磁盘…**。应用会下载 Valve 的 SteamOS 映像，校验并完成设置。此过程较久，但只需一次。
3. 在窗口中登录 Steam。保持 **16:10** 的窗口大小（设置 > 显示 > 默认窗口大小；默认的 1280×800 即 16:10）。16:9 的游戏
   在 16:10 窗口中会出现黑边。
4. macOS 询问时允许**麦克风**权限。没有它，客户机的音频采集会反复失败，游戏可能卡顿。
5. 在 Steam 中安装 VRChat，把它的兼容工具设为 **Proton Experimental**，并安装 **Proton EasyAntiCheat Runtime**。完整步骤见
   [SETUP.md](SETUP.md)。

### 首次打开

本分支的构建为 ad-hoc 签名，未经公证，因此 macOS Gatekeeper 会拦截首次打开。只需操作一次：

- 在“应用程序”中右键点按（或按住 Control 点按）该应用，选择**打开**，再点一次**打开**；或
- 如果 macOS 15 及更新版本没有提供该选项，打开系统设置 > 隐私与安全性，滚动到“已阻止使用 FX Steam Launcher”的提示，点按**仍要打开**并确认；或
- 在“终端”中运行：`xattr -dr com.apple.quarantine "/Applications/FX Steam Launcher.app"`。

## 选择图形路径

设置 > 高级 > **允许 DXMT（通过 Mac 的 Metal 运行 D3D11）**：开启为 DXMT 路径，关闭为 Vulkan 路径；下次启动时生效。
无需为每个游戏单独设置：Steam 启动时，每个已安装的 Proton 都会获得 DXMT，并由 Proton 放入每个游戏的前缀（还可在 设置 > 游戏
中为游戏指定配置：选择 DXMT 的配置会把没有自己兼容性工具的游戏改用 Proton 10.0，因为 DXMT 需要 x86-64 的 Proton）。
关闭后会原样恢复 Proton 自带的文件。
若 Proton 的原始文件缺失（例如在同一磁盘上切换两个应用构建之后），DXMT 的文件会保留，启动日志会给出提示；请在 Steam 中修复
Proton：该 Proton 工具 > 属性 > 已安装文件 > **验证工具文件的完整性**。DXMT 覆盖 Direct3D 11（及 10）；Direct3D 12 游戏请用
*Vulkan*（DXMT 的 Direct3D 12 受限）。DXMT 下的 EAC 已用 VRChat 验证；若其他游戏 EAC 失败，请切换到 *Vulkan*。

| 路径 | 适用情况 |
|---|---|
| **DXMT** | 在其上渲染正确的 Direct3D 11/10 游戏。在我们能比较的每款游戏上实测都更快（见 [docs/eac-performance.md](docs/eac-performance.md)） |
| **Vulkan** | Direct3D 12 游戏；使用 D3D11 流输出（几何着色器或顶点着色器的流输出并进行光栅化，例如 Unigine Superposition，在 DXMT 上会出现黑色阴影区域）的游戏；在 DXMT 上无法运行或显示异常的任何游戏 |

**Metal 性能 HUD** 在两条路径上都能显示帧率：设置 > 显示、“显示”菜单或 Ctrl+Cmd+P。游戏运行时可随时开关。

## Easy Anti-Cheat

EAC 在 SteamOS 内由 Valve 的 FEX 模拟器运行。本分支打过补丁的 FEX 可通过 VRChat 的 EAC 检查，Mini Royale 也能通过（EAC `301`；未越过其账号登录提示继续游玩）。结果因游戏而异：每款游戏都有自己的 EAC 版本和检查，未测试的游戏可能可以运行，也可能不行。目前 VRChat 需要两个手动
步骤，均计划自动化：

1. 安装打过补丁的 FEX：启用 SSH（设置 > 高级），然后在客户机中运行 `build-fex.sh` 和 `install-fex-tool.sh`
   （[fex-eac/README.md](fex-eac/README.md)）。
2. 设置 VRChat 的 Steam 启动选项（`EAC_LAUNCHERDIR`、`PROTON_EAC_RUNTIME`、`WINE_CPU_TOPOLOGY` 等），完全照
   [fex-eac/README.md](fex-eac/README.md) 与 [SETUP.md](SETUP.md) 所给。

- **预期结果：**EAC 启动器输出 `Launcher finished with: 301, 'Easy Anti-Cheat successfully loaded in-game'`，游戏日志出现
  `AntiCheat Session Begin: Success`。
- **预期无法工作：**拒绝虚拟机的反作弊，以及在 EAC 的 Linux 模块之外还带有自身反调试检查的游戏。
- **Steam 更新之后：**若 Steam 更新其 FEX 工具，会覆盖打过补丁的版本。请再次运行 `sh install-fex-tool.sh`。

## 故障排除

| 现象 | 处理 |
|---|---|
| DXMT 设置似乎没有生效 | 设置在下次启动虚拟机时生效：更改后请重启虚拟机。若应用缺少 DXMT 主机库，设置中会提示，并改用 Vulkan（启动日志：`gpu: graphics path Vulkan`） |
| DXMT 下 EAC 失败 | 切换到 *Vulkan* 并重启启动器 |
| 用过 DXMT 后，Direct3D 11 游戏在 *Vulkan* 上无法运行 | Proton 中仍是 DXMT 的文件：在 Steam 中，该 Proton 工具 > 属性 > 已安装文件 > 验证工具文件的完整性 |
| EAC 无法完成，或在 Steam 更新后失败 | 打过补丁的 FEX 缺失或被 Valve 的版本覆盖；运行 `sh install-fex-tool.sh status` |
| 进入世界前停顿约一分钟 | 检查启动选项中是否有 `WINE_CPU_TOPOLOGY` |
| 卡顿并伴有爆音 | 授予麦克风权限（系统设置 > 隐私与安全性 > 麦克风） |
| 首次启动很慢 | 着色器对每个游戏和每条路径只编译一次；之后的启动会更快 |

更多：[docs/eac-troubleshooting.md](docs/eac-troubleshooting.md)。

## 内存

虚拟机内存在 设置 > 高级 > **内存** 中设置（自动：Mac 内存的一半，4–16 GB）。Mac 的 GPU 显存来自同一块内存，因此虚拟机越大，留给 macOS 和游戏图形的就越少。客户机不再使用的内存会归还给 Mac：

| 机制 | 作用 | 默认 | 实测（M2、16 GB、虚拟机 8 GiB） |
|---|---|---|---|
| 空闲页报告（libkrun 补丁 0026） | 客户机释放的页面会立即从 Mac 上虚拟机的内存占用中移除（原版 libkrun 在 macOS 上仍计入这些页面） | 开启 | 启动后空闲时低 0.7–0.8 GiB |
| 启动后页缓存清理（`steamac-mem-trim`） | 启动 150 秒后，客户机丢弃一次干净的页缓存，使其也被报告为空闲；每次启动只运行一次，游戏期间不做任何事 | 开启；内核命令行中的 `steamac.mem_trim=0` 可关闭 | 虚拟机空闲内存占用 8.92 -> 7.39 GiB |
| 游戏结束后清理（`steamac-mem-trim-games`） | 最后一个运行中的游戏结束（或被终止）时，客户机丢弃干净的页缓存并整理空闲内存，使游戏用过的内存以完整的 2 MiB 块被报告为空闲；每 10 秒检查一次，游戏期间不做任何事 | 开启；`steamac.mem_trim=0` 可关闭 | VRChat 因内存不足被终止后，整理内存使虚拟机内存占用从 13 降到 8.5 GiB |
| DXMT 内存池清理（`wmtr/`，libkrun 补丁 0028） | DXMT 路径：加载高峰后，超出 128 MiB 保留量且空闲 1 秒的共享缓冲区会归还给 Mac | 开启；`WMTR_ARENA_KEEP_MB`、`WMTR_ARENA_TRIM_MS`，`-1` 表示全部保留 | 离线测试：高峰后 504 -> 203 MiB；实机验证待进行 |
| 已结束游戏的 DXMT 对象（`wmtr/`） | 游戏退出或被终止时，它仍持有的 Metal 对象（纹理、缓冲区、管线）会立即释放；此前它们会留在虚拟机中直到关机 | 开启 | VRChat 因内存不足被终止后约 1.1 GiB 图形内存残留；离线测试，实机验证待进行 |

客户机内的交换空间：与虚拟机内存一样大的 zram，使用 zstd 压缩（SteamOS 自带的是内存的一半，lzo-rle），其后是 /home 上大小为虚拟机内存一半的交换文件，仅在 zram 用满后才使用（`steamac.swap_fallback=0` 关闭并删除该文件）。只用 SteamOS 自带的 zram 时，在 8 GiB 虚拟机中切换到大型 VRChat 世界会占满内存和 zram，客户机内核随即终止游戏。

适用范围：通用（此技术栈上的任何游戏）。这些机制降低空闲和加载后的内存占用；若游戏的峰值需求超过虚拟机和 Mac 的内存，它们无济于事（见*限制*）。

## 限制

- 仅验证了一台机器（M2、16 GB、macOS 27.0）。VRChat 已在两条路径上于游戏中验证；Mini Royale 的 EAC 启动器可达到 `301`（未继续游玩）。其他游戏未测试，结果因游戏而异。
- 16 GB 的 Mac：重型游戏可能超出可用内存。Unigine Superposition（720p，Low）在两条路径上、虚拟机 8 GiB 时，应用内存都超过了 14 GB。
- VR（通过 WiVRn 连接 Meta Quest 3）为实验性功能，默认关闭：见 [docs/vr.md](docs/vr.md)。
- DXMT：Direct3D 11/10。其 Direct3D 12 受限（功能级别 11_1，仅 DXBC，无 DXIL，无光线追踪；ExecuteIndirect 会使 GPU 停顿）；Direct3D 12 游戏请用 *Vulkan*。
- DXMT：上游未实现带光栅化或来自几何着色器的 Direct3D 11 流输出；此类游戏在 DXMT 上渲染异常（Unigine Superposition：黑色阴影区域）。请用 *Vulkan* 运行它们。
- 不包含 D3DMetal。
- DXMT 设置对所有游戏生效；此时游戏的 Metal 层覆盖整个虚拟机视图（窗口化游戏或 alt-tab 会显示游戏层）。
- 内存需求超过虚拟机所得（设置 > 高级）的游戏会使用交换。
- 切换图形路径会在下次启动时更改所有已安装的 Proton；若某个 Proton 中留下了 DXMT 的文件，修复方法见*选择图形路径*。
- 构建中包含异步管线编译，但默认关闭且不推荐（实测更差）。
- 麦克风测试很少；麦克风启动后卡死的可能性尚未完全排除。重新构建或重新签名的应用会再次请求麦克风权限。

## 文档

| 页面 | 内容 |
|---|---|
| [SETUP.md](SETUP.md) | 完整的设置指南：安装、图形路径、Easy Anti-Cheat、故障排除（英文） |
| [docs/fork.md](docs/fork.md) | 本分支的改动及其状态（英文） |
| [docs/eac-how-it-works.md](docs/eac-how-it-works.md) | EAC 链路，以及为何要替换 FEX（英文） |
| [docs/eac-performance.md](docs/eac-performance.md) | 测量与限制（英文） |
| [fex-eac/README.md](fex-eac/README.md) | 打过补丁的 FEX 的安装与启动选项 |
| [docs/upstream/README.zh.md](docs/upstream/README.zh.md) | 上游 steamac 的文档（Apache-2.0，© FX GAMES FZ LLC） |

## 许可与归属

| 组件 | 许可 |
|---|---|
| steamac（本分支） | Apache-2.0，© FX GAMES FZ LLC 及贡献者（`LICENSE`、`NOTICE`） |
| 打过补丁的 FEX（[StonkGuy/FEX](https://github.com/StonkGuy/FEX/tree/eac-2610)，由 `fex-eac/build-fex.sh` 构建） | MIT，与 FEX-Emu 相同 |
| DXMT 及本分支的补丁（`host/dxmt/patches/`） | LGPL-2.1 |
| wmtr（DXMT 远程调用层，`wmtr/`） | BSD-3-Clause |
| libkrun 及其补丁 | Apache-2.0 |
| virglrenderer、Mesa / KosmicKrisp | MIT |
| MoltenVK | Apache-2.0 |
| DXVK（zlib）、Proton / Wine（LGPL-2.1 等） | 遵循各上游；由 Steam 安装，不随应用分发 |
| Linux 内核衍生物 | GPL-2.0-only；每个发布版的完整源码随该版本提供 |

**不包含 D3DMetal。**应用不含游戏文件和 EAC 二进制文件。EAC 来自 Valve 的 Proton EasyAntiCheat Runtime，由 Steam 安装。
