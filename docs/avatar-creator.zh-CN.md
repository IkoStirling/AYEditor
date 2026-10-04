# Avatar Creator 编辑器工具

**Version:** 0.1.0
**Date:** 2026-10-04
**Status:** Active
**Owner:** AYEditor

启用 `AY_ENABLE_AVATAR=ON` 并链接 Avatar Engine/UI 时，编辑器内置注册
`ayeditor.avatar` 资源文档和 `ayeditor.tool.avatar` 单例工具。默认未启用 Avatar 的
编辑器仍可构建；核心捏人会话和独立 Studio 不依赖 AYEditor。

## 打开与操作

选择 **Tools → Avatar Creator…**，点击 **Open character** 打开 `.avb` 素体包或
`.avp` 预设。也可从资源浏览器双击这两种资源，在中心 Dock 中打开文档。
可将资源浏览器中的 `.avb` 或 `.avp` 拖入已打开的 Avatar Creator 面板直接加载。
拖拽在释放鼠标后的更新阶段执行；当前角色有未保存参数时拒绝替换并提示先保存。
加载失败保留当前会话。这里接收已转换的 Avatar 资源，FBX 仍需先通过导入工具转换。
工具使用现有文档宿主的保存、另存为、重载、脏状态、关闭提示和恢复副本接口。

左侧参数按素体 schema 显示，支持滑条、数值输入、重置和撤销重做。
参数数量由资产定义；真实青年女头部诊断包当前只有五项近似坐标形变参数。
已有 DCC 形态键仍需先通过 Avatar 导入工具转换，不直接编辑 FBX。

右侧为独立、有深度的角色渲染视口，复用 `EditorSceneCamera` / `EditorFreecam`：

| 输入 | 行为 |
| --- | --- |
| 按住鼠标右键并拖动 | 调整观察方向 |
| 按住右键，同时 W/A/S/D | 前后左右移动 |
| 按住右键，同时 Q/E | 向下/向上移动 |
| 同时按 Shift | 加速移动 |
| 视口内滚轮 | 朝指针射线方向缩放/移动 |
| 视口获得焦点后 F | 聚焦整个角色 |
| 正视 / 侧视 / 俯视按钮 | 回正并按角色边界适配正交视图，分别从 +Z / +X / +Y 观察 |
| 标准视图内滚轮 | 保持轴向并缩放正交画面 |
| Esc、失去焦点、停用面板 | 结束相机控制 |

没有移动画布的按钮。右键捕获允许在视口外释放；焦点离开时清除保持的导航键。
标准视图中右键观察会返回透视导航，F 也恢复自由透视并聚焦；俯视的画面上方为 -Z。
参数输入框使用文字编辑快捷键，视口导航不修改参数、装备、撤销历史或文档脏状态。
数值输入按 Enter 或失焦提交；保存命令先提交当前输入，非法输入阻止本次保存。

## 保存与文件关联

打开 `hero.avb` 时优先加载同目录 `hero.avp`；没有该文件则使用包中默认预设。
**Save parameters** 写 `hero.avp`，不会改写 `.avb`、源 FBX 或源形态键。
**Save As** 选择新的 `.avp` 并改变文档路径。原始 bundle 保持只读。
文档的可写路径始终指向参数 `.avp`；从 bundle 打开也遵循该规则，
因此编辑器崩溃恢复只会还原参数文件，不能用参数副本覆盖原始素体包。

每次保存同时写一个编辑器关联文件，例如 `hero.avp.editor.json`：

```json
{
  "format": "AYEditorAvatarBinding",
  "version": 1,
  "bundle": "hero.avb"
}
```

关联优先使用相对预设目录的路径。重新打开预设需要该文件，或同名 `.avb`；
它只属于作者工具，不是运行时预设格式。迁移预设时请带上关联文件并保留 bundle 路径。
关联 JSON 最多 64 KiB，严格检查版本、字段、重复键与深度。
两个文件分别原子替换，不组成跨文件事务；保存失败报告错误并保留未保存状态，可重试。
恢复副本也带关联，但不改变当前文档路径、保存基线或历史。
重载只读取参数文件，不替换 UI 所借用的 Creator；重新打开角色才会加载新的素体包。
有未保存编辑时，工具内打开另一角色会要求先保存，避免静默丢失参数。

## 渲染与生命周期

每个文档有独立 Catalog/Creator，视口持有独立 Presenter、材质和深度渲染目标。
参数/装备/材质或 LOD 改变才更新求值与 GPU 网格；保存和相机导航复用已上传网格。
预览不更改主场景、Play 场景或主视口相机。关闭面板先释放输入、外部参数子树及 GPU 资源，
再由 Dock 宿主销毁根控件；UI/Renderer 必须覆盖该销毁顺序。

当前预览使用 clay 材质，不提供完整纹理作者流程、全身自动绑定、布料模拟或美术质量保证。
真实头部中的五项参数为诊断原型，独立眼睛、表情、发型和服装仍需要相应资产。

## 验证入口

```powershell
cmake --build out/build/windows-debug-vs2026-insider --target AYEditorShell_Demo AYEditor_AvatarTests AYEditor_AvatarPreviewRenderTest
ctest --test-dir out/build/windows-debug-vs2026-insider -R '^AYEditor_AvatarTests$' --output-on-failure
out/build/windows-debug-vs2026-insider/AYRuntime/AYEditor/unittest/AYEditor_AvatarPreviewRenderTest.exe out/avatar-head-validation/probe/neutral-v2.avb out/avatar-head-validation/editor-preview
```

单元回归通过 DeviceInputBridge 检查捕获、视角、W 移动、滚轮、F、焦点丢失、参数不变、
pending text 保存、失败保留、Unicode 预设和重开。完整 EditorSession 检查资源浏览器打开、
主相机隔离、Ctrl+S；实际 RecoveryStore 轮换与恢复验证源 bundle 字节不变。
真实输入桥还检查中文/emoji 文件从资源瓦片拖入、保存后拖入预设、脏编辑拒绝替换，
以及三视图精确轴向、正交滚轮和相机操作不改变参数/历史。
显式 D3D11 回归加载真实头部并比较视口像素，保存中性、改形、右键转向、滚轮四张 TGA，
以及正视、侧视、俯视三张 TGA，随后关闭完整 UI/Renderer 生命周期。
