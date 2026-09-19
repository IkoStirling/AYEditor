# UI Flow Editor 操作指南

UI Flow 用来编排“什么时候显示哪套 UI”，布局编辑器则负责“一个界面内部有哪些控件、如何排版”。
`.uiflow.json` 是项目级流程资产，不属于某个 Scene，也不应把完整 Widget 树直接写进 Flow。

## 1. 打开与接入项目

可通过以下任一入口打开：

1. 在 Assets 面板中新建 UI Flow，或双击已有的 `*.uiflow.json`。
2. 使用主编辑器的 `Tools -> UI Flow Editor...`。如果项目描述符声明了 UI Flow，会优先打开该文件。
3. 在 Flow Editor 顶部点击“打开”，选择其他 `*.uiflow.json`。

项目描述符的最小接入形式如下。路径相对于项目 `Assets` 目录：

```json
{
  "ui": {
    "flow": "ui/game.uiflow.json",
    "entry": "Boot"
  },
  "worlds": [
    {
      "id": "village",
      "scene": "worlds/village.ayscene",
      "uiContext": "Gameplay"
    }
  ]
}
```

`ui.entry` 可省略，此时使用 Flow 文档的默认入口。`worlds[].uiContext` 也可省略；复杂项目可以由
Scene 信号、GameFlow 或并行 Region 自由控制 Context。

## 2. 界面分区

- 左侧“流程模型”：创建对象并浏览完整 Outline。
- 中间左侧画布：显示 Region、State、Transition，选中 Graph 时显示节点和连线。
- 中间右侧预览：加载 Screen 引用的真实 `.ui.json`，播放进出场动画，并接受真实控件事件。
- 下方“已挂载界面”：显示当前 Layer、Slot 与 Screen 的实际挂载结果。
- 下方“运行时跟踪”：显示信号、状态迁移、动作图节点和错误。
- 右侧检查器：编辑选中对象；修改后必须点击“应用属性”。
- 右下“诊断”：同时显示 Flow 模型错误、Graph 错误和布局/动画资产闭包错误。

顶部的“重新启动预览”会从所选 Entry 重新创建生产 `UIFlowRuntime`。保存文件不会自动替代这一步。

## 3. 核心对象

建议按下列顺序建立 Flow：

1. **Layer（层）**：定义绘制和输入顺序，例如 `application`、`hud`、`modal`。`Order` 越大越靠上。
2. **Slot（插槽）**：Layer 内的展示位置。Context 通过 Slot 显示或隐藏 Screen。
3. **Screen（界面）**：引用一个 `.ui.json`，并声明 Layer、Slot、Scope 和可选进出场动画。
4. **Context（上下文）**：一组 Slot 操作，例如主菜单、Gameplay HUD、暂停模态框。
5. **Entry（入口）**：预览或运行时启动时首先激活的 Context 集合。
6. **Signal（信号）**：触发状态转换或被控件事件发出。
7. **Region（区域）与 State（状态）**：状态机。多个 Region 并行运行，例如主流程和暂停层互不替代。
8. **Transition（转换）**：指定 Region、起始状态、目标状态和触发 Signal。
9. **Action（动作）与 Graph（动作图）**：连接宿主能力、信号、界面和世界切换。

ID、资产路径、Scope、输入策略、Signal 名称等是序列化标识，即使界面显示中文，也应继续使用稳定的
ASCII 技术名称，例如 `gameplay`、`pause.open`、`ui/hud.ui.json`。

## 4. 从零制作 Menu → Gameplay → Pause

### 4.1 Screen 与 Context

创建三个 Screen：

- `menu`：Layout Asset 为 `ui/menu.ui.json`，放入 `application.main`。
- `hud`：Layout Asset 为 `ui/hud.ui.json`，Scope 为 `world`，放入 `hud.main`。
- `pause`：Layout Asset 为 `ui/pause.ui.json`，放入 `modal.main`。

创建 Context，并在“插槽分配”中填写：

```text
application.main=menu; !hud.main
```

这表示显示菜单并隐藏 HUD。Gameplay Context 可写：

```text
application.main=game; hud.main=hud
```

Pause Context 只需要：

```text
modal.main=pause
```

多个操作用分号分隔；`slot=screen` 表示显示，`!slot` 表示隐藏。

### 4.2 Region、State 与 Transition

建立两个并行 Region：

- `application`：`menu`、`gameplay` 两个 State。
- `modal`：`playing`、`paused` 两个 State。

在 State 的“上下文”中填写逗号分隔的 Context ID。随后声明 Signal：

```text
game.start
pause.open
pause.close
```

再创建转换：

- `application/menu -> gameplay`，触发信号 `game.start`。
- `modal/playing -> paused`，触发信号 `pause.open`。
- `modal/paused -> playing`，触发信号 `pause.close`。

在中间“信号”列表选择信号并点击“触发”，即可观察活动 State、真实预览和已挂载界面是否同步变化。

## 5. 让布局中的按钮驱动 Flow

先在 UI Layout Designer 中给按钮绑定声明式 handler，例如 `startGame`。然后选中对应 Screen，在
“控件事件”字段填写：

```text
startGame=game.start; openPause=pause.open
```

格式是 `handler=signal`，多项以分号分隔。点击顶部“打开布局”可跳转到选中 Screen 的布局；布局编辑器的
Workflow 功能也能扫描 handler，并自动补齐 Flow Signal 与映射。

真实预览中的按钮点击会走生产 Screen Host 和 Signal 队列，不是编辑器伪造点击。若诊断提示 handler
不存在，应回到布局检查控件事件绑定，而不是只在 Flow 中新增同名字符串。

## 6. Graph 与调试

1. 在左侧创建并选中 Graph。
2. 在“图”工具行选择节点类型，点击“+ 节点”。
3. 选择输出端和类型兼容的输入端，点击“连接”。输入端列表会自动过滤不兼容类型和同节点连接。
4. 在 Entry、State 或 Transition 的相应属性中填写 Graph ID。
5. 在调试行选择节点，可设置/清除断点；“下一步暂停”会在下一个节点副作用发生前停止。
6. “单步”执行当前节点并停在下一个可运行节点；“继续”恢复到下一个断点或完成。

琥珀色粗边框表示当前暂停节点。暂停状态会显示已经合并默认值和连线值之后的最终输入；节点执行与输出
同时进入“运行时跟踪”。编辑器中的 Host/World 操作是 Mock，正式游戏仍需由应用组合根注册执行器。

## 7. 诊断与常见问题

- **预览为空**：确认 Entry 激活了 Context，Context 又把 Screen 分配到有效 Slot。
- **触发信号没有变化**：检查 Transition 的 Region、起始 State 和 Signal ID；转换只在起始 State 活动时生效。
- **布局无法加载**：Screen 的 Layout Asset 必须相对项目 Assets 根目录，不能使用绝对路径或 `..` 越界。
- **按钮点击无效**：检查 Layout handler 与 Screen 的 `handler=signal` 映射，并确认 Signal 已声明。
- **无法删除对象**：编辑器禁止删除仍被 Slot、Context、State、Transition 或 Graph 引用的对象；先移除引用。
- **暂停界面没有挡住下层输入**：将模态 Layer 的输入策略设为 `blockLower`，并提高其 Order。
- **修改后预览仍是旧状态**：点击“应用属性”，然后“重新启动预览”。
- **技术诊断仍显示英文**：JSON schema、运行时和插件返回的原始错误保留其源语言；固定界面、对象类型、
  属性名、状态和诊断类别会跟随 AYEditor 语言切换。

## 8. 保存与验证

- “保存”写回当前文件；“另存为”创建新文件。
- 撤销/重做使用文档级历史，并与 dirty 状态一致。
- 保存前确保右侧没有错误级诊断；警告应逐项确认。
- 项目打包前运行 Full Client 内容验证，确保 Flow、Layout、动画、Scene 和 GameFlow 构成完整资产闭包。

界面语言由 AYEditor 的 `Editor.Appearance.Language` 决定；`system` 会选择系统支持的语言。切换语言时，已打开
的 UI Flow 窗口会原位更新，不会重建文档，也不会丢失选择和预览状态。
