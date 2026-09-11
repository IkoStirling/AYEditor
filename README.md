# AYEditor

AYEditor 是 AY Engine 的编辑器产品层，负责 Editor Shell、Edit/Play 会话、场景隔离、视口合成、导入流程和编辑器工具窗口。

**当前状态：** v0.3 编辑器壳层，包含 Edit/Play Scene、Transport Bar、网络客户端、
通过 `AYRenderer/UIRenderBackend.h` 完成的单窗口 UI/3D 合成，以及直接复用
`AY2DEditorCore` 的 Tilemap 作者工作区。Tilemap 已接入统一文档、命令、脏状态与
恢复体系；PNG 图集现在通过宿主 authoring-image cache 接入原始像素与共享 GPU
纹理，主编辑器内可使用模态切片导入、Source Sheet 原图选砖和真实纹理画布；
导入支持 Tiled/TexturePacker 伴随元数据识别、手工自由矩形，以及可保存和撤销的
多格 Stamp 笔刷。Tilemap 也提供独立 Shadow 模式、16 种四象限遮罩、显隐和地图级
RGBA 阴影颜色；该数据保存在作者源中，不借用 3D Shadow Map。
主界面可通过 `Tools -> 2D Tilemap Editor...` 或第二行网格图标直接打开同一套
独立非模态 Tilemap 工具窗口；窗口复用 AYEditor 的文档/命令体系，内部提供可拖动
三栏、可滚动侧栏和可横向溢出的工具栏，不再要求先在 Content Browser 中找到并
双击地图资产。该工具窗使用 AYUI 自绘标题栏和最小化、最大化/还原、关闭按钮；
拖动内部 Tilemap 文档标签只操作 Dock 页签，不再带动整个窗口。图集原始像素会为
独立窗口的绘制后端建立本地纹理，因此导入预览、Source Sheet 与画布铺砖共用同一
份可见图像，不会误用主窗口的后端句柄。

项目 UI Flow 创作阶段四也已接入：`.uiflow.json` 可由 Content Browser 创建、识别和双击打开，
或通过 `Tools -> UI Flow Editor...` 打开项目描述符声明的 Flow。独立 Flow 工具窗提供完整模型
Outline、Region/State/Transition 与 Graph 画布、Layer/Screen/Context/Transition Inspector、
实时诊断、Signal 模拟和 Mock Action trace。预览直接复用生产 `UIFlowRuntime`；本阶段展示逻辑
mounted Screen，真实 Widget 像素预览与动画交接留给生产视觉集成阶段。

## 公开接口

```cpp
#include <AYEditor.h>
#include <AYEditor/EditorApp.h>
#include <AYEditor/EditorSession.h>
#include <AYEditor/EditorPlayRuntime.h>
```

## 依赖

主要依赖 AYUI、AYGameLoop、AYDevice、AYApplication；运行时集成还使用 AYEntity、AYRenderer、AYResource、AYScene、AYPhysics、AYAudio、AYScript 与 AYNetwork。

阶段、模式矩阵和编辑器 UI 契约见 [design.md](design.md)。
