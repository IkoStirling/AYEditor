# Editor 测试运行与报告

## 来源内混合确认（2026-10-22）

`timing-ack-before-ack.json`在原Editor库下1例28断言中5失败：两次错误清除后的Console/状态不一致，
Updated+Unconfirmed和NotResident+Unconfirmed未保留待确认提示，且来源名丢失。
测试走真实glTF converter、队列、recipe和Session更新，只有消费者确认状态由fixture提供，
不把它当作GPU像素证明。`timing-ack-baseline-valid-proof.json`核对加入测试前后两个实现库hash均未变。
同一来源先Retained，再混合确认两种组合，最后两变体Updated，覆盖提示保留、扫描后仍在及最终解除。
修复后定向1例28断言通过，`timing-ack-first-ack.json`。
最终172步消费方构建及`render-final-*`冻结回归通过：主入口471例12538断言，
独立默认模块装配1例13断言，无失败/跳过；清单472项/两分区无遗漏重复。
Renderer九组CPU1020例18581断言、两后端完整Gpu各58例45072断言，
两后端Entity实际静态/蒙皮及六项实际Editor流程通过，冻结CPP/H摘要未变。
Workshop两后端未保存Scene差异均为0、预览变化136457像素，保存并显式重载后Scene变化20995像素；
实际重导入仍覆盖CPU合法/GPU超限保留、提示持续、修复重试与Scene保存重开。
总日志`render-final-regression-run.log`，源码`render-final-frozen-source-hashes.json`；
SDK128/53/102/576零错零警告，`sdk-render-final.json`。
本例没有新增GPU设备功能或改变文件导入成功状态。

## 最终资源观察与停止采集（2026-10-22）

Renderer文本修复联动的Editor完整主入口470例12510断言，资源观察专项5例130断言，均无失败/跳过。
真实watch前置事件有效时，旧Session在同一World关闭后仍产生日志，安全复现1个失败断言；
`residency-shutdown-watch-before.json`。关闭早期显式停用诊断后`residency-shutdown-fixed.json`通过。
先前误用作者显式报告的夹具未写watch日志，导致2个前置断言失败，保留该报告但不计为生产缺陷复现。

最终RendererCPU1018例18562断言、两后端Gpu各54例44665断言，以及
`ui-paragraph-native-final-{MaterialWorkshop,AssetReimport,MaterialPreview}-{d3d11,d3d12}`六项通过。
实际AssetReimport面板已在两后端逐图检查，控件/滚动/换行均正常，旧Scene/材质预览保持、
超设备尺寸拒绝、来源提示、修复/稳定句柄及保存重开均通过。
源码和完整串行结束记录为`ui-paragraph-frozen-source-hashes.json`、`ui-paragraph-regression-final-run.log`；
SDK128/53/102/576零错误零警告。下文较早冻结阶段不验证后补退出修复，不能重复累计为新增用例。

## 渲染资源观察与真实浮动布局（2026-10-22）

新增资源观察4例115断言、完整Editor469例12495断言通过，无失败/跳过。
原控件测试仅证明Widget存在；新增真实宽度/顺序/滚动可见检查先失败4项。
局部调用panel.performLayout后通过，但实机截图仍不可读；改用UIManager完整布局再次失败4项，
暴露root Box浮动child和unchanged-client快速路径没有布局新卡的问题。
修复VBox自动尺寸、详情实际尺寸和创建/重开时布局后，完整Host定向测试通过。
报告`residency-layout-before.json`、`residency-host-layout-before.json`、`residency-floating-fixed.json`均保留。

`residency-floating-native-{d3d11,d3d12}`通过，直接读取actualSession控件几何并滚动到摘要；
两后端截图均已查看，记录列表及逻辑存储说明可见。它们仍使用旧文本后端，会显示LF控制字形；
后续Renderer段落修复另行编译/验收，不能将该已知问题写成已修复。
原重导入、CPU超设备尺寸/GPU拒绝、旧Scene与预览、恢复/保存重开和独立草稿保护继续通过。
冻结Renderer两后端完整Gpu各53例44644断言和Entity实际场景通过，
`residency-native-final-{MaterialWorkshop,MaterialPreview}-{d3d11,d3d12}`四项关联回归亦通过。
所有证据位于根`out/render-sampling-20261021/`，当前冻结源码见`residency-frozen-source-hashes.json`。

## 分层刷新结果与来源隔离（2026-10-22）

ABI45完整`refresh-editor-source-final.json`465例12380断言，导入定向
`refresh-editor-source-isolation-first.json`28例380断言，均无失败/跳过。
覆盖文件导入成功与消费者Retained/Failed分别显示、NR/未知不误报、扫描不覆盖提示、
其他来源成功不清除未解决项、同来源未知保留待确认、确认后清除；提示包含来源文件名。

`refresh-source-isolation-native-{d3d11,d3d12}`两项实际Editor验收通过：
16385×1合法CPU纹理已发布，GPU两个变体拒绝保留，实际Scene和材质预览均与旧参考一致；
状态跨扫描保留；修复DDS后稳定id下更新、Scene保存重开及预览恢复，独立未保存草稿保持。
对应`gpu-refresh-preview-{before,retained,repaired}.png`及`gpu-refresh-scene-reopened.png`。
`refresh-source-final-{MaterialWorkshop,MaterialPreview}-{d3d11,d3d12}`四项回归也通过。
全Renderer/Resource/Entity与ASan阶段报告见根刷新设计；不同冻结阶段不重复累计为用例总数。

## 纹理采样作者流程（2026-10-22）

Source ABI44 的完整主入口463例/12308断言通过，无跳过；
报告在根仓库 `out/render-sampling-20261021/editor-reviewed-full.json`。
首轮只因版本检查仍要求43而失败，修正检查后复验；保留首轮报告，不计为最终通过。
`AYEditor_Importer` 检查采样表单候选值、raw输出拒绝、各向异性固定线性放大、Apply后入队与配方恢复。

`AYEditor_AssetReimportRenderTest <新目录> d3d11|d3d12` 的当前两后端验收包括：
三层DDS最近邻导入；打开Scene和预览；在实际表单选择三线性；真实Cancel指针保持原画面；
重开仍为最近邻；真实Apply指针保存/排队并改变Scene和预览；typed纹理保留三层与新设置；
Scene保存重开；坏源保留旧图；修复重试；无关可写材质草稿始终保留。
证据目录 `native-current-AssetReimport-{d3d11,d3d12}` 中Scene变化89349像素、预览67058像素。

同轮 MaterialWorkshop 与 MaterialPreview 实际两后端回归均通过；Workshop未保存编辑的Scene变化为0，
保存并显式重载后变化20995像素。这里只验收显式Scene Host夹具与真实Session/UI路由，
没有新增完整默认GameLoop或原生Computer Use验收。

启用 Avatar Engine/UI 后，聚合库存包括 `Test_EditorAvatar.cpp`；专项
`AYEditor_AvatarTests` 覆盖资源注册、只读 bundle、预设保存/重开/历史、失败保留和
真实输入桥的视口导航及 pending 数值保存。未启用 Avatar 时该源不注册套件。
专项也覆盖资源瓦片中文/emoji 路径拖入与脏编辑保护、正交三视图及返回自由导航。
AssetBrowser 回归检查中文工程/目录/文件的扫描、索引重开、旧索引迁移和原生文件通知。
显式 `AYEditor_AvatarPreviewRenderTest` 用真实头部做 D3D11 像素回归，不加入 Noop 全量门禁；
见 [操作与复现指南](avatar-creator.zh-CN.md)。

Recovery 独立门禁覆盖 Windows 短暂占用后成功轮换，以及持续占用导致预算耗尽时
保留旧会话、释放占用后再次恢复；使用真实无 delete-share 文件句柄复现目录占用。

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
