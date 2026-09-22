面向 coc.nvim 后台 watcher 的测试覆盖审计，2026-09-23

本报告已按 coc.nvim 的用途缩减，只保留影响文件事件正确性、持续监听、正常订阅生命周期及故障处理的补测建议。原审计阶段只修改报告；后续补测实施记录如下。

## 补测实施记录（2026-09-23）

按“不增加测试钩子、只用真实文件变化”的约束补充了以下自动化覆盖：

- 初始化扫描期间连续移动子树，订阅完成后继续验证最终路径的修改事件；该用例复现并修复了 macOS 扫描期间路径消失导致订阅失败、旧 FSEvents 延迟批次污染最终索引的问题。
- 初始化完成后立即退订、已有事件可能在投递时退订；均用同根新订阅的真实事件作为完成边界，确认旧 callback 不再收到后续事件。
- 每个原生 callback 批次独立检查 `renameId`：同一 ID 必须在该批内恰好包含一个 delete 和一个 create，不再只对展平事件断言。
- 快速创建后删除、删除后重建通过最终事件状态检查，避免调用方留下与磁盘相反的路径状态。
- Windows 目录连续改名后替换、目标先 `REMOVED` 再收到 rename pair 的通知顺序继续由真实文件操作覆盖；实现保留当前批次的替换证据。
- Linux pending move 不再固定等待 200ms；移动一个额外目录到根外并等待其 delete，确认宽限期确实到期后再检查路径复用和后续监听。
- 父根与子根同时订阅，退订父根后由子根的真实事件确认独立性。
- 订阅后创建指向根外目录的 symlink / Windows junction，修改外部目标并用根内 marker 建立交付边界，确认不报告链接下的根外后代。
- macOS / Windows 目录仅改大小写后修改子文件，确认后代路径使用新大小写。

没有自动制造 `IN_Q_OVERFLOW`、Windows 缓冲区溢出、macOS `MustScanSubDirs` 或真实 ENOSPC。无注入时这些场景需要依赖系统限额制造大量操作，资源开销高且结果不稳定。本轮改为静态审核：Linux 初始化安装 watch 失败会回滚已安装 descriptors；运行期安装失败会使该订阅失效、清理残留并向 callback 报错，以允许后续重新订阅重建。各平台真正的溢出通知仍需独立、受控的系统级测试环境验证。

核查基准：native-watcher `e04d27c` 当前工作区、coc.nvim `54c3dd31d`。当前 coc 仍接 Watchman，尚无 native-watcher 接入实现；下文据现有消费契约判断原生组件的必要测试，不把未来接入方案当成已实现事实。

| 已核实的消费方式 | 测试范围 |
| --- | --- |
| 单一 Node 环境，按根复用 client；各 glob watcher 在 JS 层分发 | 验证单环境中的原生异步启动、投递、退订交错 |
| LSP 消费 create / update / delete，扩展 API 另有 `onDidRename` | 文件变化和 rename 基本正确性都要保证；不要求每一步文件操作均被记录 |
| 根使用 URI 的绝对 `fsPath`，父子根可以同时存在 | 验证绝对路径及多根独立性 |
| 工作区移除会 dispose client；创建中的 client 用 generation 防止过期发布 | 原生保证启动、退订可靠；是否重试、是否发布由 coc 接入层验证 |

依据：[client 管理](/Users/chemzqm/vim-dev/coc.nvim/src/core/fileSystemWatcher.ts:80)、[根移除与销毁](/Users/chemzqm/vim-dev/coc.nvim/src/core/fileSystemWatcher.ts:48)、[RelativePattern 根](/Users/chemzqm/vim-dev/coc.nvim/src/core/fileSystemWatcher.ts:137)、[LSP 消费](/Users/chemzqm/vim-dev/coc.nvim/src/language-client/fileSystemWatcher.ts:115)、[公开 rename API](/Users/chemzqm/vim-dev/coc.nvim/typings/index.d.ts:10087)。文件监听路径没有使用 `worker_threads`；测试运行器使用 Worker 不等于生产监听采用该架构。

保留以下 6 组补测。P1 优先补齐；P2 排在主链路之后。

1. **P1：丢事件后的通知与恢复（原 T02）。**

   批量文件操作不能让 coc 的文件状态永久停留在旧状态。现有测试未专门触发 Linux `IN_Q_OVERFLOW`、Windows 缓冲区溢出或 macOS `MustScanSubDirs`。对应入口：[Linux](/Users/chemzqm/vim-dev/native-watcher/src/linux/InotifyBackend.cc:235)、[Windows](/Users/chemzqm/vim-dev/native-watcher/src/windows/WindowsBackend.cc:204)、[macOS](/Users/chemzqm/vim-dev/native-watcher/src/macos/FSEventsBackend.cc:368)。

   最小验收：Linux / Windows 注入代表性的丢事件通知，确认错误交付、旧订阅可清理、重新订阅后继续收到真实事件；macOS 注入重扫标志，确认增删改、rename 不变量、ignore 过滤及后续监听正确。自动重订阅和恢复时向 LSP 补发变化属于 coc 接入层。

2. **P1：初始化与正常退订；保留根失效的已知回归（原 T01、T03、T08 的生命周期部分）。**

   打开工作区时文件可能正在变化，离开工作区时初始化或事件投递可能尚未结束。[现有初始树用例](/Users/chemzqm/vim-dev/native-watcher/test/watcher.test.js:81) 先建树、等订阅成功才操作，没有固定扫描交错。macOS 的 [stream / 扫描 / pendingEvents 协调](/Users/chemzqm/vim-dev/native-watcher/src/macos/FSEventsBackend.cc:633) 应有一个受控用例：扫描到确定节点时改变子树，成功返回后核对后续文件事件和路径。

   另保留初始化完成后立即退订、投递期间退订两种正常交错：退订可等待，之后新写入不触发旧 callback，其他根继续工作。Windows 补强 [现有循环用例](/Users/chemzqm/vim-dev/native-watcher/test/watcher.test.js:478)，控制 pending read / cancellation 的完成顺序即可。coc 已有创建中移除工作区、dispose manager 的测试，接入时沿用这些生命周期约束，不在 addon 重做 manager 测试。

   首次审计在 macOS 复现：收到根 delete 后保留旧句柄，重建同路径并再次订阅，新订阅成功却在 2 秒内无事件；另一根正常，退订旧句柄后再订阅可恢复。保留一条该原生回归，见 [根 stream 清理](/Users/chemzqm/vim-dev/native-watcher/src/macos/FSEventsBackend.cc:495)、[Backend 复用](/Users/chemzqm/vim-dev/native-watcher/src/Backend.cc:142)。coc 当前按根复用 client，不能声称正常接入必然触发旧句柄并存；根失效后的重建策略由 coc 接入层确定。

3. **P1：事件合并与 rename 批次断言（原 T04）。**

   [rename 辅助函数](/Users/chemzqm/vim-dev/native-watcher/test/rename.test.js:33) 和 EventCollector 会展平批次。首次审计已确认：把同 ID 的 delete / create 分到两个回调，现有断言仍通过。这是辅助函数缺口，不是后端错误配对的实机证据。

   最小验收：保留 callback 批次，每个出现的 `renameId` 在该批内恰好对应一个 delete 和一个 create。对共用 [EventList](/Users/chemzqm/vim-dev/native-watcher/src/Event.hh:54) 用少量表驱动用例覆盖短命文件的创建后删除、同路径删除后重建、连续改名、合并后只剩 rename 单边；对应临时文件、替换保存和连续操作。各平台沿用已有实机用例，不把每种合并顺序再复制三套，也不固定允许合并的中间事件顺序。

4. **P1：Linux 部分安装失败的清理与隔离（收窄原 T06）。**

   Linux 每个目录都要安装 inotify watch；大工作区安装到一半失败会造成漏报或资源残留。现有 FD 耗尽只覆盖 `inotify_init1()`，未验证 [子目录安装](/Users/chemzqm/vim-dev/native-watcher/src/linux/InotifyBackend.cc:111) 部分完成后的清理。

   最小验收：子进程内使安装过程返回一次 ENOSPC，确认失败可观察、已安装部分能清理、另一个有效根继续投递，解除注入后能重新订阅。运行中移入目录时安装失败，也应可观察，不能永久静默漏报。

5. **P2：补强已有目录移动回归（原 T05、T07，保留 T09 的普通大小写路径场景）。**

   [Windows 连续改名用例](/Users/chemzqm/vim-dev/native-watcher/test/rename.test.js:388) 的两次 `await fs.rename()` 之间允许后端消费，未确定进入“处理中间路径时它已不存在”的分支。控制消费或回放明确通知，检查后代索引以及随后修改、替换的路径和 rename 信息。Windows / macOS 另保留一个目录仅改大小写后修改子文件的场景，验证后代路径更新。

   Linux 已用 SIGSTOP 固定排队，并检查路径复用没有旧 delete。只补强 [固定等待 200ms](/Users/chemzqm/vim-dev/native-watcher/test/fixtures/linux-chained-directory-renames.js:90)：确认 pending move 实际到期后再断言。保留一例逐阶段确认处理完成的“可见 → 忽略期间替换 → 再次可见”，验证恢复后仍能监听。

6. **P2：多根独立性与目录链接边界（收窄原 T07、T08）。**

   coc 只对相同根去重，父子工作区根可以并存；Linux 可能共享底层 descriptor。补一个父根与子根同时监听的用例，退订其中一个后另一个仍能收到后代变化。见 [coc 根去重](/Users/chemzqm/vim-dev/coc.nvim/src/core/fileSystemWatcher.ts:131)、[Linux descriptor 释放](/Users/chemzqm/vim-dev/native-watcher/src/linux/InotifyBackend.cc:505)。

   [现有 symlink 用例](/Users/chemzqm/vim-dev/native-watcher/test/watcher.test.js:307) 只创建、删除根内文件链接；junction 用例只验证启动能完成。补一个订阅后新增的根外目录链接，修改目标后确认不以链接下的路径报告根外后代，真实根仍正常投递；Windows 用 junction 覆盖运行期分支。已有循环链接启动用例继续承担扫描终止的检查。

现有基本文件事件、递归和移入目录、ignore 及不同配置订阅、替换歧义、硬链接、case-only 文件改名、中文路径及已修复问题的回归继续承担基础验证。coc 当前 `fileSystemWatch.ignoredFolders` 用于决定是否创建根 client，尚不能当成已传入 addon 的目录排除参数，见 [根过滤入口](/Users/chemzqm/vim-dev/coc.nvim/src/core/fileSystemWatcher.ts:80)。

发行验证沿用现有 7 个 OS / 架构 / libc 目标。交付的同一 `.node` 产物只需另在 coc 确认的最低 Node 版本做加载、订阅、事件、退订的最小检查，不按 addon 的 `>=18` 声明扩展通用兼容矩阵。当前 coc [package.json](/Users/chemzqm/vim-dev/coc.nvim/package.json:9) 写 `>=24.12.0`，[Vim 启动检查](/Users/chemzqm/vim-dev/coc.nvim/autoload/coc/client.vim:119) 仍放行 22.15 及以上，接入时应先确认实际支持下限。

首次审计证据：macOS x64 / Node v26.5.0，`npm run build && npm run test` 退出码 0，47 项中 35 通过、12 跳过；Linux、Windows 仅静态核对，未核实远程 CI 成功状态。本次仅重审调用路径并修改报告，未重跑测试，不将首次结果当作后来未提交改动的验证。
