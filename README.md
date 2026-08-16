# AYEditor

AYEditor 是 AY Engine 的编辑器产品层，负责 Editor Shell、Edit/Play 会话、场景隔离、视口合成、导入流程和编辑器工具窗口。

**当前状态：** v0.3 编辑器壳层，包含 Edit/Play Scene、Transport Bar、网络客户端与通过 `AYRenderer/UIRenderBackend.h` 完成的单窗口 UI/3D 合成。

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
