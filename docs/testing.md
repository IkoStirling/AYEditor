# Editor 测试运行与报告

27 个主测试源文件独立编译，不再通过 main.cpp 文本包含；CMake 核验源码归属。
主运行器通过 AYTest 元数据发现套件，保留逐套件子进程隔离，不依赖手写套件名数组。
每个子进程限时 120 秒，结束时清理所属进程；完整 CTest 限时 600 秒。

```text
AYEditor_UnitTests --list
AYEditor_UnitTests --suite AYEditor_AnimationExtension --case NAME
AYEditor_UnitTests --report-json editor-report.json
ctest --test-dir out/build/windows-debug -L "^editor-fast$" --output-on-failure
ctest --test-dir out/build/windows-debug -L "^editor-full$" --output-on-failure
```

`--list`、`--help` 不注册 Entity 组件、初始化运行时或创建临时目录。
筛选与 AYTest 公共入口一致；位置参数 suite 仍可用。实际执行即使只选一个套件也
使用子进程隔离。报告目录为本次独占 `testTmpDir()/reports`，每个套件有 schema 1
JSON；主进程核验子报告身份与数量后合并，结尾输出统一用例统计和问题表。
子进程失败不阻止其他套件执行；缺报告、清理超时与退出码不一致均视为失败。
异常/崩溃的精确位置未知时明确标注；用例声明位置只作辅助定位。

`editor-fast` 覆盖 P0 核心、Gizmo、资源瓦片展示和工作区框架。`editor-full` 覆盖
完整主入口及独立模块装配测试。UIFlow/GameFlow/Recovery 与 2D 专项门禁保留，
但不另加 full 标签，避免完整层重复执行。当前部分夹具仍访问固定平台资源，故
Editor 分区保留串行；该迁移不代表所有 Editor 夹具均已可安全并行。

```powershell
pwsh -NoProfile -File scripts/tests/verify-module-test-inventory.ps1 -Module AYEditor
```

核验比较源码身份与完整层实际注册，重复、遗漏和筛选拼错都会失败。
CTest 注册项数量不是用例数；主报告使用用例通过/失败/跳过，断言数单独显示。
2026-09-28 Windows Debug 迁移基线：主入口 28 个套件、281 个用例，模块装配
另 1 个用例，完整层共 282 个；主入口实际断言 4106 个，全部通过、无跳过。

2026-09-29 动画作者加固后：主入口 292 + 装配 1 = full 293 用例，全部通过。
新增覆盖跨轨操作、会话 Clipboard/只读、时间变换、历史选择/100 次拖动合并、
3.2 万键快照复用与正式 Hermite/Quaternion 插入；其他 suite 不重复纳入 full。
独立 `AYAnimationEditorCore` 另有 45 用例：fast 13 / integration 31 / stress 1，
不与 Editor 的 UI 适配测试混计。作者核心快速门禁用 `animation-editor-fast`，
full 用 `animation-editor-full`；源码/运行时审计必须检查完整层，无遗漏、无重复。

2026-09-29 人形控制器接入回归：作者核心 full 53（fast 21 / integration 31 /
stress 1），Editor 最终主入口 301 + 装配 1 = 302，UI full 1229，动画运行时 296。
比首轮 301 增加的 1 用例来自并行精灵编辑器任务；不计作新增控制器用例。
控制器专项覆盖四肢/Pole/端点/均匀 scale、双向切换、拖动/旋转取消、姿势键、
原子重排及选择撤销、保存重开、metadata 失败回滚、坏 metadata 防静默覆盖、
清除重绑定历史和烘焙撤销。Editor source ABI 28，旧二进制需重建。
精确报告与未交付的 Recovery/真实资产验收见
[控制器实施记录](../../../AYDocs/control-rig-implementation.md)。
