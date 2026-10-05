# 新增场景渲染控制

体积光／雾默认关闭，提供密度与 Final / Transmit / Scatter 诊断，Edit/Play 注册时同步。
默认选第一个场景灯，全局介质；局部盒、散射颜色和各向异性由高级 Host settings 设置。
这些新旋钮仍为 live preview，不额外更改偏好/场景文件格式。

Render 面板的 SSR 默认关闭，提供强度与 0 Final / 1 Hits / 2 Radiance。
绑定由 EditorSession 在 Edit/Play 的 Renderer 注册后应用，面板启动前回调安全无操作。
首版需要 Deferred + 活动 IBL v2 + 粗糙度低于0.6的不透明 PBR 表面；
白色 hit 表示有效反射命中，黑色表示回退。不支持离屏/透明反射。
这是 live preview，不含这些新旋钮的用户偏好持久化；原有偏好字段保持不变。
引擎设置和算法限制以 AYRenderer README/design 为准。
布局/控件存在性证据：Test_EditorShell.cpp；场景美术效果仍由用户验收。
