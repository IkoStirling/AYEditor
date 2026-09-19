# GameFlow 可视化编排指南

GameFlow Editor 面向流程作者提供可视化状态机编排，不要求手写 DSL。运行时仍读取稳定的 `.gameflow.json`，编辑器只通过类型化文档模型修改该资产。

## 核心工作流

- 从 State 右侧绿色端口拖到另一个 State 左侧蓝色端口，可创建 Transition。新 Transition 默认使用当前文档的第一个 Intent，随后可在 Inspector 中通过下拉框修改。
- State、Intent、Transition、Guard、失败与取消目标均使用文档内引用选择器；不再要求手工输入对象 ID。
- Action 参数根据注册表元数据生成编辑器：Boolean 使用复选框，数值使用类型校验输入，已知枚举使用下拉框，World/UIFlow Entry/Context/Signal 使用项目引用选择器。
- Action Palette 是可拖动列表。把 Action 放到 Transition 节点上即可添加；也可以选中 Transition 后使用 Add Action。
- Subflow 是独立的作者入口和紫色节点。编辑器内部生成规范 `flow.enter` 调用，但作者不需要手工编写 `subflowId` 参数。
- 右键画布打开带前缀搜索能力的快速创建菜单，可创建 State、Intent、Transition、Subflow、Action，应用模板或执行自动布局。

## 选择与布局

- 左键拖动空白区域：框选 State。
- Ctrl+左键：增减 State 多选。
- 左键拖动 State：调整当前会话中的节点位置。
- 中键拖动：平移画布；滚轮：缩放。
- `Ctrl+A`：选择全部 State；`Ctrl+C` / `Ctrl+V`：类型化复制与粘贴；`Delete`：批量删除。
- Auto Layout 会清除手工位置并恢复确定性的层级布局。

批量删除会在同一撤销记录中移除被选 State 及其关联 Transition；复制粘贴会自动重映射 State、Intent 与 Transition ID，避免产生悬空引用。

## 模板

内置的首个生产模板为：主菜单 → 加载 → 游戏 → 暂停/恢复 → 结果 → 重开。应用模板是单次可撤销操作，适合作为新项目骨架，而不是隐藏生成不可编辑的运行时代码。

## 数据边界

World、UIFlow 和 Subflow 选择器只保存稳定 ID 或资产根相对路径。画布搜索、选择框和拖放状态不进入运行时协议。未知项目 Action 仍可无损往返，但只有注册了元数据的 Action 才能获得完整的类型化参数编辑体验。
