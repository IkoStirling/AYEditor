# AYEditor

## 重导入的混合确认（2026-10-22）

已有某来源的视图刷新问题时，该来源的一项Updated/NotResident不能替另一项Unconfirmed清除提示。
先显示本次明确失败/保留结果；没有明确失败但仍有未知结果时保留该来源的待确认提示，
全部相关结果确认后再解除。首次没有旧问题的未知结果仍按原策略处理，不新增设备失败判断。
这只完善当前Session内的来源提示，不改变导入文件成功状态或建立跨消费者GPU事务。

## 渲染资源观察（2026-10-22）

Scene视口打开Debug Visualization，启用Resource分类，在记录列表选择Renderer texture residency，
查看当前资源表和逻辑纹理存储；单纹理记录显示尺寸、格式、层级、来源/创建负载、采样默认值和缓存身份。
默认关闭，最多24个纹理行和一个摘要；与CPU加载/缓存记录共用已有资源分类。
GPU拒绝新版本时仍显示当前成功版本，而不是仅依据新文件描述推断画面。
这些数值不包含所有渲染目标或驱动开销，创建负载也不是累计带宽。
独立EditorHost使用UI后端已借用的Renderer，无需另一套资源管理器。
记录控件和详情在滚动面板中；冻结保留已捕获结果，单步采集下一帧。

## 重导入刷新确认（2026-10-22）

Session分别处理文件导入、CPU刷新和消费者结果。消费者保留旧版时提示“部分视图仍使用旧版本”，
状态未知的失败则提示重试；未驻留、不处理和legacy未确认不伪报为设备失败。
提示按来源身份保存并标注文件名；普通后台/手动资源扫描不会用“已索引”覆盖它。
其他来源导入成功不能清除尚未解决的问题；同来源得到更新/未驻留确认后才清除，
后续只有未知结果时保留“此前问题尚未确认解决”。CPU已刷新的只读材质文档仍可更新，独立未保存草稿继续保持。
提示有中英文翻译。新增Session状态使Source ABI升至45，所有消费方需重编。
完整Editor465例/12380断言、导入28例/380断言通过；两后端实际文件发布/GPU拒绝、
Scene与预览旧图保持、持久提示、修复重试、保存重开和稳定句柄通过。详见[测试维护](docs/testing.md)。

## 贴图采样设置（2026-10-22）

选中源模型/图片或其导入结果，在 Inspector 打开导入设置，可选择资源默认/最近邻/双线性/
三线性/各向异性，以及放大过滤、U/V寻址、保留已有层级或仅基础级。
覆盖需要“转换为引擎贴图”输出；DDS可保留其编码层级，不自动补齐缺失层。
各向异性采用设备最大值并使用线性放大；边框目前按边缘钳制，窗口会明确显示。
Apply持久化项目配方并重导入，Cancel不改资源；完成后刷新Scene和只读导入材质预览。
窗口使用可滚动内容和固定错误区，按钮与校验提示不随长表单滚走。
采样阶段以Source ABI44完成两后端验收，包含采样修改、取消、保存重开和坏源恢复；当前源码ABI45需重编消费方。

## 灯光创作（2026-10-21）

Hierarchy 的“+”和场景右键菜单可创建方向光、点光源和聚光灯，一次操作可撤销。
选中后 Inspector 的灯光设置支持类型、启用、线性颜色/强度、米制范围、锥半角、阴影及优先级。
Apply 读取未失焦的输入，完整校验后作为一次命令；Cancel 不修改 Scene。
窗口打开后 Scene 已变更时拒绝覆盖，要求重新打开。Point 阴影仍不支持。

3D Scene 显示固定屏幕尺寸的程序图标，选中时绘制发光方向、范围球/内外锥辅助线，
范围不随 Transform scale 改变。图标和点击使用同一投影数据；禁用灯光仍显示灰图标。
Viewport Options → 灯光辅助显示可关闭；关闭后纯灯光对象不参与兜底球形拾取。
显示开关保存在用户 `.ayeditor/workspace.json`，缺少键时默认开启，不写入 Scene/游戏数据。
图标作为置顶编辑辅助显示，不以场景遮挡决定可点性；2D/Play 不显示这些辅助。

`Test_EditorLightAuthoring` 验证真实菜单、弹窗、一次undo/redo、无效值、冲突与Scene保存重开。
`AYEditor_LightAuthoringRenderTest` 在D3D11/D3D12读回实际灯光、Spot阴影、禁用、删除、恢复和重开。
当前 Source ABI45，消费方需要重编。本阶段提供功能，不新增模型资产包。

## 来源导入设置与重导入（2026-10-21）

选中源模型或已有导入产物，可在 Inspector 打开导入设置，调整内容类型、源坐标轴/单位、
UV 原点、法线 Y、贴图输出及按材质名称覆盖表面模式。Apply 校验未失焦的字段并排队；
无效数值保持窗口打开。Cancel/Esc 不排队。

打开窗口时从当前磁盘配方读取，避免资源索引尚未刷新时显示旧设置。
Apply 前复核该来源的配方：外部修改、删除、同源新建或项目切换时拒绝覆盖并提示重新打开，
不产生导入任务；其他来源的设置变化不阻止本次编辑。这是乐观冲突保护，不是跨进程原子CAS。
项目经目录联接访问时，来源关联按解析后的根路径匹配，保持原来的浏览器逻辑ID；
根解析每次重建仅执行一次，不为每条资源记录增加文件系统查询。

“重新指定源文件”选择相同扩展名的现存来源；Apply 才保存并导入，Cancel 不改变配方。
outputBaseName/sourceId/namespace 固定原输出路径，支持主文件改名和来源目录搬移；
拒绝将另一个配方已拥有的同根来源绑定过来。模型子对象和外部图片 URI 的重命名不在此映射范围。

作者设置在转换前保存到项目 `.ayeditor/imports`，清除派生缓存后仍能重建。
新来源按独立 namespace 输出，避免同名文件互相覆盖；资源浏览器显示来源与导入状态。
完成队列逐项通知 Session，转换成功/有效缓存命中都刷新驻留资源及缩略图，失败保留旧渲染。
真实隐藏窗口 D3D11/D3D12 夹具已检查导入网格与其自身材质、按钮重导入、失败保留、
修复重试及 Scene 编辑保存重开；该夹具使用显式 Scene Host，不等于默认 GameLoop 全面验收。
当前 Source ABI45，依赖者需重新编译。

BC4/BC5 颜色贴图的真实两后端用例与独立 RGBA8 图像逐像素对照，并验证重导入恢复 BC1；
实际上传由 Renderer 处理 sRGB 兼容，不改变导入文件编码/mip 链。数据贴图保留原生 R/RG，
不自动重建法线 Z；完整mip链已上传，合法但不完整的链明确回退基础级。

## 3D 场景搭建效率（2026-10-21）

Viewport Options → 变换吸附设置可编辑移动米制步长、旋转角度、缩放比例增量。
按原拖动姿态计算增量，保持局部/世界约束轴和未选分量；Ctrl 临时反转吸附状态。
拖动结束保留既有一次 undo，取消不写入 Scene；设置存用户偏好 schema9，旧偏好默认关闭。
Viewport Options → 选中物体取景或 F 按所选 3D Spatial 及空间子节点的世界 Mesh 包围盒调整
编辑器镜头，没有 Mesh 时使用单位局部盒。文本输入时 F 不触发；Play/2D/拖动中/超过当前
1000米镜头显示范围会拒绝。操作不修改 Scene 或运行时相机。

真实指针拖动检查吸附、Ctrl 精细拖动、一次撤销和 Scene 保存重开；另有设置模态校验/偏好恢复、
局部轴和比例缩放测试。俯仰镜头拾取射线现与实际 lookAt 正交基一致；3D 分数 Dock 尺寸归一到
相机像素范围，避免投影和鼠标偏移。证据 Test_EditorSceneEfficiency.cpp / Test_EditorTransformGizmo.cpp。

Ctrl+D / Edit → 创建选中对象副本将空间子树与生命周期归属闭包复制为一次命令，最多4096个实体。
根节点自动分配可区分名称，所有实体获得新 persistentId；父子、owner 和内部相机 target 随副本重映射，
外部引用及实体内部 component slot ID 保留。Mesh/材质等资源路径共享，不新增模型或深拷贝资源。
任一组件不可序列化时拒绝整体复制。未知游戏字符串引用不推断重写；复制不是自动同步 Prefab。
Actor副本获得新实例身份，保留类引用、实体内组件槽、默认值基线和覆盖；撤销重做保留副本身份，
保存重开后仍以类文件的新默认值合并实例差异。该行为不扩展为多实体Prefab的结构同步。
撤销前若有人绕过历史添加外部结构依赖，会拒绝而不删除这些对象。

3D 拾取、取景、模型拖放读取 Mesh 元数据前按已挂载资源根解析虚拟路径；备用球体只应用一次
世界缩放，避免大地板抢走点击。真实两后端用实际物体选择/Ctrl+D/移动/undo/redo/Scene重开验证。

## 原生 PBR 材质创作（2026-10-20）

Assets 双击 `.mat/.aymat` 打开独立材质 Dock，支持颜色、PBR/发光/opacity 因子、
UV、法线强度、粗糙度偏置、Mask/Blend、opacity 通道、双面与七个贴图槽。
球体、非均匀缩放球、UV 平面使用独立工作室灯光/HDR 显示，未保存修改不改 Scene 缓存。
Enter/失焦/Apply 原子提交字段；Save/Ctrl+S 先提交有效字段，再检查磁盘冲突并保存。
本地 128 项 undo/redo 保留保存前后的历史；Reload 失败保留当前文档。
源文件最大 4 MiB，保留未知参数与 shader/name/GUID。源资源及已导入 textures/ 可通过搜索式
贴图选择器赋值，原图 DDS 须先导入。源贴图写 owner-relative 引用，已导入贴图保留虚拟路径。
导入材质保持只读；成功重导入或命中有效输出缓存并刷新驻留资源后，已打开且干净的只读材质文档重新读取内容，
包含非活动 Dock，revision 驱动参数与预览更新。可写文档及未保存草稿不自动重载；失败导入
保留现有视图。“创建可编辑副本”把当前状态另存到项目源根的新 .mat 并打开独立 Dock。
副本保留未知值/纹理维度并重定位相对引用，生成独立 GUID；不覆盖现存文件或修改原文档历史。
副本先在同卷源根外写完整文件，再排他硬链接发布；文件系统不支持硬链接时明确失败。

Assets 中的材质可拖到 3D Edit 视口的既有 Mesh：一次可撤销赋值，不创建几何；Scene 保存重开保留。
关闭贴图选择器后，无输入框焦点的可见材质页仍处理 Ctrl+S；隐藏页和外部焦点不抢命令。
MaterialAuthoringWorkflowRenderTest 用真实 Session/导入/按钮/拖放/GPU 串联此流程，并检查预览
关闭后 Scene 的共享程序仍有效。它使用显式 Scene Host，不等于默认 GameLoop 或系统鼠标验收。

File → New PBR Material 创建唯一 `.mat` 和独立相邻 shader，跳过孤立 shader 文件。
`AYEditor_MaterialSampleTool` 为 [Material Workshop](../AYRenderer/demo/MaterialWorkshop/README.md)
生成规范 `.msh`、104 个 Scene（九组场景和十七种近景，各四状态）与项目描述，复用正式资源与场景接口；初始视角通过既有
Scene View workspace 写入，保留用户已有视角。Editor ABI37 和 Session 布局保持不变。

## 色彩映射起点（2026-10-19）

Value Bands新增到颜色映射选择，面板说明最大RGB分量分阶；Cel Ink/Cel Paint可直接选为创作起点。
18项选择和偏好恢复共用Renderer preset count，schema8偏好内嵌look v4；结构布局仍为source ABI37。

## 从参考图创作色板（2026-10-19）

在Assets选中PNG/JPG/TGA等源图，Render面板设置2..16色并点“从选中图片创建风格”。
生成唯一LOOK作品并应用，保留当前轮廓、印刷、显示和时间设置，仅替换mapping/色板并启用look。
透明像素不参与，半透明按alpha加权；源图不会成为look的运行时依赖。解码/提取失败保留当前参数。
图片解码由共用EditorRasterImage服务提供，不要求GPU上传；同一服务供预览和原图作者数据使用。

## Look 资产工作流（2026-10-19）

File → New Render Look把当前参数存成`Assets/looks/NewLook.rsl`，重名自动编号。
资产浏览器以LOOK卡片识别并支持类型过滤；双击应用，显示Render面板并选中作品保存路径。
创建工厂的独立调用写默认关闭look；Session菜单复制当前look。失败加载保留既有参数，
操作不修改Scene或撤销历史；缓存可恢复新类型，也会重新识别旧索引中的Unknown `.rsl`。
模板目录经解析后必须在项目内，拒绝Assets子目录链接逃逸。`.rsl`仍为显式Host消费的Experimental作者文件。
物理目录跟随project.prj的assetRoot（默认Assets），浏览器继续显示虚拟Assets前缀。

## 时间风格创作（2026-10-19）

Render面板增加独立时间反馈开关、残影/整帧停格/分块刷新模式，历史混合、周期、块大小、
偏移和种子按适用模式展开；隐藏控件保留值。Reset history只重置艺术历史，不改TAA或曝光。
偏好仍用schema8、作品look写schema4并读1/2/3/4；Source ABI37，消费方必须完整重编。

## Render Style 创作（2026-10-19）

Render面板提供十八套起点预设、像素/RGB/双色/自定义2..16色板、印刷和轮廓控制。
颜色使用#RRGGBB，色板为空格分隔2..16项；非法编辑保留既有配置并显示原因。
文件路径支持当前项目相对路径或项目内绝对路径，保存/加载UTF-8 `.rsl`，保存可创建项目内父目录；
拒绝项目外路径及符号链接逃逸。Reset恢复默认关闭配置，加载失败不改变当前look。
这些操作可由`renderStyleLook`、`setRenderStyleLook`、`saveRenderStyleLook`、`loadRenderStyleLook`驱动。
作品文件独立于Scene和用户偏好；schema8偏好通过Renderer版本化serializer保存全部参数，
旧schema7标量继续迁移。Source ABI37；完整重编消费方，避免结构偏移不一致。
颜色与轮廓绑定实现移至`AYEditorRenderStyle.cpp`，不继续向主Session TU堆积参数转换。


## 空间与数据实体作者流程（2026-10-14）

创建菜单显式提供“创建空间实体”和“创建数据实体”。Inspector 解释当前类型，
提供可撤销的显式转换；Transform 显示为固定 Spatial Property，不能添加/移除，
数据实体的组件候选过滤空间依赖。空间父节点与拥有者各有选择器：改父节点显式选择保持世界或局部姿态，
归属只决定生命周期；均走文档命令与撤销/重做，不直接绕过历史。

Outliner 按真实空间树组织节点，数据实体为非空间根行；折叠子树保留逻辑选择。
Inspector 编辑局部 TRS，视口选择、Gizmo 使用世界姿态，拖动结果检查后转回局部，
失败不写近似 TRS。删除/恢复保留 persistentId、owned 后代及外部空间子节点的关系。
新场景保存为 schema5，旧场景读取由 AYEntity 迁移。Actor 类数据继承不是空间父子关系。
证据：`Test_EditorP0Core.cpp` 的类型/关系历史和删除恢复，以及 `Test_EditorHierarchy.cpp` 创建入口。


Logia 作者分析使用 checked 编译器，并从引用它的 Actor 类读取有效属性声明。Compile 保留旧 Component/Tool 文档兼容；Actor、checked 新语法或显式 checked 上下文启用严格校验。
**Complete** 菜单插入属性、helper 与注册接口提示，**Definition** 导航已解析声明及导入模块。
嵌入宿主可用 `EditorDslDocument::setLogiaHostContext` 指定 schema/provider。
见[Logia 能力与作者工作流](../../AYDocs/LOGIA-CHECKED-CAPABILITIES.md)。

Phoskia DSL 文档编译按钮现使用 AYShader 共享作者分析核心，分析当前未保存文本，
以文档路径解析相对模块。在原报告面板展示参数/default提示、UBO布局、模块来源、
原始诊断行列、CPU耗时/源码字节及 variant 组合预算警告。Windows源码生成不启动
shaderc/设备，不写源文件，也不是GPU验证；实际离线变体数量仍受 manifest 预算控制。

启用 `AY_ENABLE_AVATAR=ON` 时，**Tools → Avatar Creator…** 和资源浏览器的 `.avb/.avp`
文档接入参数化角色编辑。角色视口复用编辑器右键观察、WASD/QE 移动、Shift 加速、滚轮和 F 聚焦，
保存只写参数预设与编辑器素体关联，不改写 bundle。
可从资源浏览器拖入 `.avb/.avp`，并用正视、侧视、俯视按钮回正正交画面。
资源扫描、显示名称和索引统一使用 UTF-8；旧版索引自动重建，中文文件无需改名。
见 [Avatar Creator 操作与生命周期](docs/avatar-creator.zh-CN.md)。

测试入口、隔离策略与用例报告见 [测试维护文档](docs/testing.md)。
崩溃恢复目录轮换使用 AYIO 的限时 Windows 文件争用重试；超过预算明确返回错误，
保留旧会话文件，释放争用后可重试恢复。

AYEditor 是 AY Engine 的编辑器产品层，负责 Editor Shell、Edit/Play 会话、场景隔离、视口合成、导入流程和编辑器工具窗口。

粒子文档的独立预览支持 2D 与透视 3D 的 CPU/GPU/Auto 切换。支持的 3D 相机朝向粒子
在 D3D11 上使用 GPU 模拟，随相机环绕始终朝向镜头，并受测试方块深度遮挡；3D 定向
平面等 GPU 不支持的配置会显示原因并回退 CPU。预览选择和测试场景不写入 `.pfx`。

资源管理器提供 **Folders** 与 **All files** 两种视图：前者显示项目资源根的真实
目录结构（含空目录），后者按相对路径平铺全部资源。两种视图共享搜索、类型过滤、
选择及资源操作；在平铺视图可用 **Locate** 跳转到所选资源的目录。项目配置的
`assetRoot` 映射为逻辑 `Assets`。索引启动时优先恢复缓存，随后后台校验磁盘；
文件和目录通知增量更新映射，界面在空闲帧无全量遍历。详见
[项目工作流](docs/project-workflow.md)。

Scene 实体可由 Hierarchy 顶部的 `+` 或树内右键菜单创建：空实体、Sprite、Tilemap
和 2D Camera。创建后自动展开层级树并选中实体，在 Inspector 继续配置组件；创建
使用同一 Scene 命令历史，支持撤销/重做。Play 和 Paused 状态下创建入口禁用。
Inspector 组件标题的展开/折叠只改变界面状态；面板在点击派发结束后的宿主更新中
重建，并在释放旧控件前清除焦点、悬停和捕获引用（包含独立窗口），不写入 Scene 历史。

**当前状态：** v0.3 编辑器壳层，包含 Edit/Play Scene、Transport Bar、网络客户端、
通过 `AYRenderer/UIRenderBackend.h` 完成的单窗口 UI/3D 合成，以及直接复用
`AY2DEditorCore` 的 Tilemap 作者工作区。Tilemap 已接入统一文档、命令、脏状态与
恢复体系；PNG 图集现在通过宿主 authoring-image cache 接入原始像素与共享 GPU
纹理，主编辑器内可使用模态切片导入、Source Sheet 原图选砖和真实纹理画布；
导入支持 Tiled/TexturePacker 伴随元数据识别、手工自由矩形，以及可保存和撤销的
多格 Stamp 笔刷。Tilemap 也提供独立 Shadow 模式、16 种四象限遮罩、显隐和地图级
RGBA 阴影颜色；该数据保存在作者源中，不借用 3D Shadow Map。
主编辑器的工具栏还提供 Shadow Fill 和 Shadow Rectangle：前者一次替换四向连通、
原遮罩相同的区域，后者拖拽矩形并预览范围；两者复用 AY2DEditorCore 的作者模型，
保留四象限遮罩，均为单步撤销，不修改图块或地形来源。
主界面可通过 `Tools -> 2D Tilemap Editor...` 或第二行网格图标直接打开同一套
独立非模态 Tilemap 工具窗口；窗口复用 AYEditor 的文档/命令体系，内部提供可拖动
三栏、可滚动侧栏和可横向溢出的工具栏，不再要求先在 Content Browser 中找到并
双击地图资产。该工具窗使用 AYUI 自绘标题栏和最小化、最大化/还原、关闭按钮；
拖动内部 Tilemap 文档标签只操作 Dock 页签，不再带动整个窗口。图集原始像素会为
独立窗口的绘制后端建立本地纹理，因此导入预览、Source Sheet 与画布铺砖共用同一
份可见图像，不会误用主窗口的后端句柄。

普通 2D Scene 中的规则网格 Sprite Sheet 动画通过 Inspector 的
`2D / Sprite Animation` 组件创作。添加该组件会自动补齐 Sprite 与 Transform；
面板提供网格列/行、起始格、帧数、统一帧时长、Loop/Once 与播放开关，并限制无效
数值。第二行播放图标或 `Tools -> Sprite Animation Editor...` 会打开中心 Dock 中的
可视化序列帧工作区：左侧原图支持拖选连续格、缩放和平移，右侧提供实时预览、
逐帧、播放/暂停/停止与 scrub。上一行/下一行和 Loop Row 可选择整行循环预览，
首尾行循环切换。默认跳过区间末尾全透明格，保留开头和帧间空白；关闭开关恢复
请求范围，状态栏显示有效/请求帧数。检测要求原图像素和每格至少一像素，也支持
非整除的 UV 网格；原图尺寸及非整数格警告在左侧常驻显示。全透明范围
会提示并禁止 Apply（关闭开关可显式使用）。Apply 将有效帧数等参数作为一条 Scene 命令写回选中的
Sprite，统一支持 `Ctrl+Z` 与 `Ctrl+Shift+Z`。该工具刻意不复用骨骼动画时间轴；命名
Clip、逐帧时长与事件仍属于后续独立 Sprite Clip 资产。

左侧 Cell size 提供 8/16/32/64 像素预设和自定义宽高；只有点击 Apply Cell Size
才从原图换算行列并选择完整一行，尺寸不整除时拒绝并保留旧网格。不覆盖后续
手动行列设置，不改写源图片。隐藏的 Sprite Animation 页签停止后台预览；
播放只刷新帧相关控件，不重复标脏静态网格/状态文字。
单帧预览将采样 UV 内缩半纹素，避免放大时线性过滤读取相邻行/列；源图网格和
组件的原始帧区间不变，单像素帧采样范围收敛到像素中心。

项目 UI Flow 创作阶段十一也已接入：`.uiflow.json` 可由 Content Browser 创建、识别和双击打开，
或通过 `Tools -> UI Flow Editor...` 打开项目描述符声明的 Flow。独立 Flow 工具窗提供完整模型
Outline、Region/State/Transition 与 Graph 画布、Layer/Screen/Context/Transition Inspector、
实时诊断、Signal 模拟和 Mock Action trace。预览直接复用生产 `UIFlowRuntime`，并在裁剪视口中
通过生产 Screen Host 加载项目 `assetRoot` 下的真实 Widget 布局和进出场动画，同时保留 mounted
Screen 与运行 trace 列表用于诊断。
阶段六进一步把模型诊断扩展到完整资产闭包：所有 Screen 引用的布局、进出场动画及轨道目标会在
编辑时验证；项目内容验证器同时输出去重的 `Flow -> Layout -> Screens` 依赖，供部署打包复用。
Screen Inspector 现在可把布局语义 handler 映射到 Flow Signal，真实预览中的控件点击会直接驱动
生产状态机；Graph 节点类型和兼容 Pin 由可扩展注册表提供。预览会经生产 Graph Executor 逐节点执行，
画布显示类型化端口和贝塞尔连线，连接目标在选择 source 后按 Pin 类型过滤，并在编辑时进行严格诊断。
节点调试器支持断点、Pause Next、单步和继续；暂停发生在节点副作用之前，画布会标出当前节点，
暂停状态与 Runtime Trace 可检查解析后的输入和节点完成输出。
异步 Graph 的暂停、Trace、Region 和 Screen 变化通过 presentation revision 增量刷新；独立工具窗还以
1280×720、1440×860、1920×1080 三档布局/裁剪渲染契约防止窄窗口控件越界。
完整使用流程、对象关系、控件事件映射和 Graph 调试步骤见
[UI Flow Editor 操作指南](docs/ui-flow-editor-guide.zh-CN.md)。

GameFlow Editor 已从通用文本属性面板升级为可视化流程编排：状态端口可直接拉线，
文档引用使用下拉选择，Action 参数按注册表元数据生成布尔、数值、枚举与项目引用控件；
同时提供 Action 拖放、专用 Subflow 节点、常用流程模板、自动布局、框选、批量删除和
类型化复制粘贴。完整操作与数据边界见
[GameFlow 可视化编排指南](docs/game-flow-editor-guide.zh-CN.md)。

UI Designer 项目工作流现在建立 `.ui.json` 与 `.uiflow.json` 的双向索引。Layout Designer 的
Workflow 菜单可跳到第一个引用当前布局的 Screen，并可扫描布局中的声明式 handler，为所有引用
Screen 补齐 `handler -> Signal` 映射；Flow Editor 选中 Screen 后可用 **Open Layout** 回到其布局。
`EditorUiDesignerWorkflow` 还提供 Layout asset、Flow signal、Widget handler 和 Widget ID 的类型化
跨文档重命名计划。应用前会验证全部源文件仍与规划时一致，再以同目录临时文件和备份执行多文件
事务；任何一步失败都会回滚，避免部分引用已改、部分未改。已打开且 dirty 的目标文档不会被磁盘
工作流静默覆盖。该能力现已进入 Layout Designer 的 **Workflow -> Safe Rename References** 与右侧
Project Safe Rename 面板：可以选择引用类型、预览文件级改动及诊断，再执行安全提交，不再要求开发者
直接调用底层工作流 API。

项目索引现在是长生命周期服务，而非每次菜单操作都重新解析全工程。它以 UI authoring 文件的大小和
写入时间维护 revision，500ms 轮询只做元数据扫描；`.ui.json`/`.uiflow.json` 新增、删除或变化时才
重建语义索引。外部变化会自动重载 Workspace 中对应的干净文档；dirty、已删除或不可重载文档保留
内存内容并报告冲突，避免后台同步覆盖编辑。

## 公开接口

```cpp
#include <AYEditor.h>
#include <AYEditor/EditorApp.h>
#include <AYEditor/EditorSession.h>
#include <AYEditor/EditorPlayRuntime.h>
```

## 依赖

主要依赖 AYUI、AYGameLoop、AYDevice、AYApplication；运行时集成还使用 AYEntity、AYRenderer、AYResource、AYScene、AYPhysics、AYAudio、AYScript 与 AYNetwork。

扫描、缩略图、导入和项目构建统一使用 AYTask 的 owned 后台任务。
扫描/预览关闭时取消并丢弃观察者，不再由 future 析构隐式等待；worker 独立拥有
路径和 CPU 数据，纹理上传/释放保持在编辑器线程。generation 隔离重开后的结果。
导入/构建尚不能安全中断资源写入，关闭时显式等待收尾，不允许遗留写入与新页面冲突。
后台取消是协作信号，解码库或外部构建自身不可取消的阶段不保证立即停止。
既有 UIJobPresentation 仍只负责显示，Foundation 不依赖 UI。

阶段、模式矩阵和编辑器 UI 契约见 [design.md](design.md)。

### Audio authoring and virtual piano

Tools → Audio Editor opens the shared workbench: instrument performance,
audio resources and current engine mixing. Piano exists only on performance;
its strip pans and wheel-zooms, with signed semitone transpose (-24..+24).
Mixer rows have a name, slider and editable percent. A compact upper-right vertical L/R
output meter shows real peaks/overload. Resource audition pause/stop is local;
engine pause and bus/effect controls are explicitly global. Individual pages
scroll when needed and read staged EngineAssets, not AssetRepo at runtime.
Single-resource edits use the same timeline-v2 sidecar as audio documents;
complex metadata is read-only in the workbench. Missing export, envelope editing,
preset libraries and independent preview routing are disabled placeholders.

Audio asset documents expose resource metadata, preview begin/end and looping,
plus editable source offset, duration, gain and fade-in/out on the selected clip.
Apply Range / Apply Clip validate before recording undo. Play Range auditions
the selection; source PCM stays intact. Save uses an atomic `*.timeline.json`
schema v2 sidecar; schema v1 remains readable. One source and one audible clip
at a time are supported; multi-file/multi-track export is deferred. The existing
Timeline remains the waveform and clip-arrangement surface.

`EditorAudioTests.WorkbenchAttachesAfterNativeHostLayoutWithoutResize` covers
viewport/load/layout before session attachment, reopening, immediate tab
switching and standalone resize. This prevents the native child from rendering
the dynamic workbench at default sizes in the top-left corner.

Audio child input preserves Left Shift/Left Alt and comma through the native
key bridge. EditorAudioTests verifies temporary transpose, the lower-octave
white-key row, latched releases/duplicate mappings, text-focus suppression
and focus-loss recovery, as well as compact header meter bounds.

Left Shift temporarily raises new performance notes by one octave (C4 -> C5);
Left Alt lowers one octave (C4 -> C3). Both held together cancel.


### Focused audio responsiveness

In Edit mode, focusing the native Audio Editor keeps the background scene's
last composite and services native input independently of the scene render.
Message-aware waiting wakes immediately for input; audio telemetry and the
focused child UI refresh at up to 60 Hz. Returning to the main window resumes
the full editor update/composite path immediately. Shared engine/bus controls
and note ownership remain unchanged. Play keeps its existing simulation/render
path; this optimization does not change Play's scheduling.
`EditorAudioTests.FocusedAudioPacingLeavesInputLiveBetweenUiFrames` and the
focused child tick/close regression cover cadence, direct input between paints,
background isolation, invalid elapsed time and deferred teardown.

The host disconnects its input bridge and clears focus/resize/close/message
callbacks before session/UI shutdown. Native window destruction can emit focus
events after the session's scope ends; no callback may retain that borrow.


The Audio Editor now contains a Recording tab with OS/virtual endpoint choices,
capture/loopback, waveform and WAV save. Its default virtual path is
`audio/recordings/Take_<timestamp>.wav` under the open project's configured asset
root. Save rescans the project; saved takes can open in the audio Resources tab.
Device selection is local to recording, without replacing the shared Host audio
engine. The workbench uses a continuous header and a waveform-visible selection.


### Four-lane chart tab and preview

The shared Audio Editor now has a fifth 谱面编辑 tab. Four D/F/J/K lanes support
taps/holds, BPM/beat offset, snapping, waveform navigation and note undo/redo.
Save/Open use project-relative `audio/charts/*.chart.json`; the existing asset
root provider/save callback pins the project and refreshes its asset view.
Standalone users choose an asset folder. Running preserves the document and
launches the current unsaved snapshot in an independent audio host window.

AYEditorStage depends on and stages AYAudio_AudioEditor plus its UI/sample assets
next to the portable runtime executable. The preview owns a separate audio engine
and music voice; focused audio-child pacing remains unchanged. EditorAudioChartTests
covers the shared five-tab session and preview input/lifetime integration.
The model and timing restrictions are documented in AYAudio/design.md; this demo
preview does not expose a new recommended engine gameplay API.

## Logia 引擎函数提示

默认 Logia 文档使用共享 engineLogiaHostContext，提供 audio/animation/physics/ui 的 26 个基础函数提示；
检测到这些调用时编译开启 checked 参数校验。显式 setLogiaHostContext 保留嵌入宿主自己的契约。
Editor Play Script 装配安装相同签名的实际 provider，UI 仍只操作游戏 Screen。验证见 AYEditor_DslDocument。

Logia 异步作者链路支持 `tasks.start(helper, args...)` 与 `await wait_event("alias") into (name:type, ...)`。
未保存文档检查参数/payload 类型，Complete/Definition 提供 payload 局部声明与导航；见 AYEditor_DslDocument。

The shared 演奏与编排 tab now supports piano performance clips, real-time head quantization,
fixed-duration step chords, editable piano roll, direct track selection and clip arrangement.
Ruler/blank-timeline dragging pans independently of wheel zoom. Piano keys remain pinned below
the scrolling authoring area. Document gains/mute/solo are separate from global audio buses.
Save/open versioned *.performance.json under project Assets and export whole/range PCM16 WAV.
EditorAudioPerformanceTests covers recording/transpose, step history/cursor, UTF-8 virtual paths,
preview/export ownership, background audition continuation and direct-track/pan interactions.
Arrangement/clip/range playback continues across focus loss; live keys are released and recording is finalized. See AUDIO-AUTHORING §8.

Audio input polish: Alt raises one semitone; Q..I play upper-octave white keys. Record end snapping
defaults on, and record-backing monitoring is switchable. Continuous upper-page scrolling no longer
turns into piano-roll zoom when the pointer crosses it. GDI tool-window text uses the same CJK family
and character-size convention as the standalone renderer, with real text measurement and antialiasing.

### Rhythm-assisted audio chart editing (2026-10-06)

The shared chart tab now performs background BPM/beat-phase estimation, reversible calibration,
shared waveform/chart beat rulers and playback seeking/follow, and editable generated rhythmic taps.
Saved chart timing is preserved; automatic estimates cannot overwrite manual edits during import.
Independent preview is horizontal DFJK with per-note timing accuracy/errors, reduced selectable
countdown and reusable decoded PCM cache. Decode/analysis/preparation jobs remain off the UI thread;
registration and widget/engine mutations stay on the owner thread. Shutdown cancels/joins owned jobs.
AudioChartTests and EditorAudioChartTests cover synthetic tempo/phase, quiet/steady input, cancellation,
draft validity/density/history, large-note ordered judgement, waveform/chart cursor alignment,
asynchronous import/manual-setting priority, snapshot isolation and horizontal tap/hold feedback.
See AYAudio/design.md and AUDIO-AUTHORING §11 for metrics, fixed-tempo limits and Temp cache lifetime.

Rhythm draft correction (2026-10-06): the shared tab now uses frequency-band attack flux, preserves
actual attack timestamps by default, selects stronger nearby events instead of filling every beat,
and offers only ≤25ms optional snapping. Yellow attack ticks and a half-beat grid fit explain the
generation basis. Low/high-frequency evidence chooses lane pairs; this is a coarse editable draft.
Irregular attacks, long rests, density suppression and retained song timing have focused regressions.
The standalone preview dismisses on close before normal worker/audio/GPU cleanup; see AYAudio/design.md.

### Logia Actor preview debugger

The Logia code toolbar adds Breakpoint at caret, Debug Actor, Continue, Into, Over,
Out and Stop. The diagnostics panel shows original source frames, locals, state
and waits; current-file pauses move the caret. This isolated on_start preview
requires an inferred .act or explicit Actor property schema. It seeds zero/empty
schema defaults and returns unavailable values from default Host providers. Use
the native AYScript debugger on a quiesced Host to inspect actual play instances.
Editing/reopening source or changing schema closes the preview session.

Logia checked inference recognizes project_event, persist and asset declarations even without a bound Actor schema. Project-event declarations appear as typed symbols in completion.

## Unified debug visualization (2026-10-15)

Scene viewport options -> Debug Visualization... opens the nonmodal debug
card; Enable All Debug Displays enables all25 categories. Individual
channels cover colliders, queries, contacts, character controllers, velocity
and trails, joints, cameras, skeletons and abilities. The card offers physical
layer-mask and selected-entity filters, x-ray/depth-tested drawing, freeze,
next-frame capture, lifetime selection and per-category clear.
It displays the current frame, bounded record/line counts and dropped records.
Click a displayed line to inspect its record and select an associated Entity.
Queries preserve dimension/request/filter, hit point/normal, distance and status.

Live physics shapes come from an opt-in owned native snapshot; Edit-mode or
unbound component primitives use authored pose/offset/no-scale geometry. Camera
frusta use the actual component projection including near/far clipping; bone
links and axes use the evaluated skeleton pose, falling back to bind pose.
Ability adapters publish tested regions and actual affected/denied targets.
The optional Combat integration inspects owned area sweeps without evaluating
providers or taking over gameplay callbacks. Last-requested velocity arrows
and measured displacement/trails are separate; trail history is bounded to
256 bodies and 2..600 frames (default 120).

These settings and observations are session-only and do not dirty scene
assets. World replacement and shutdown clear history; pause retains it.
Freeze applies to diagnostic collection, not simulation. Jolt solved contact
impulse is reported unavailable; Box2D supplies its real solved normal impulse.
Tests: AYEditor_PhysicsDebug, native PhysicsDebugSnapshotTests, ability physics
integration suites and Renderer diagnostic GPU fixtures.

Verification: final audited full inventory 402/402 passed after the scroll-card
layout and World replacement cleanup changes, including default module assembly.
AYEditorShell_Demo built successfully. Final report:
`out/debug-visualization-20261015/editor-final/038752a28cff433d9c6f9efc08ab1f54/cases.json`.

## Debug content extension (2026-10-15)

The first content extension added categories9..14 (the current25-category mask is0x1ffffff), covering animation, combat/stages, lighting/render, audio, particles and registered2D decisions. A record browser exposes geometry-free state and follows live details; geometry picking keeps physics details. New content shares bounded realtime/timed/persistent history, category clear, selected-only filter, freeze and root-bone trails. Combat events are deduplicated by source sequence and retain original completion times. Optional integrations are linked only when present. GPU particle readback and LOD selection remain unavailable and are explicitly labeled.

Module-local regression sources were updated. Runtime tests and interactive editor acceptance are deferred by the current task request; previous validation reports cover the earlier baseline. These changes remain uncommitted until focused runtime checks are requested and pass.

## Extended diagnostic observations (2026-10-15)

Debug Visualization now exposes25 categories (bits0..24; kPhysicsDebugAllCategories=0x1ffffff). The new deterministic/services adapters read same-Session checkpoints and existing runtime snapshots for paths/interactions, respawn, stats, camera behavior, UI/input, network, sequences and resources. UI/input draw in a non-intercepting logical-screen overlay; world lines retain existing depth/picking behavior. New categories share freeze/history/clear and combined world+screen budgets. Category round-robin prevents early native records monopolizing the batch; service subquotas reserve calculations, input axes and network connections. Sources and deferred acceptance are documented in AYDocs/DEBUG-VISUALIZATION.md.

Regression sources are included. Runtime tests and interactive editor acceptance are deferred for this content task; untested changes remain uncommitted.

## Shared audio processing and preview routing (2026-10-16)

The shared AudioEditorSession now exposes SOFA selection, ordered bus effects,
Windows MIDI input, input compensation and instrument/mixer preset persistence.
Instrument/resource auditions and arrangement/chart listen actions default to
the independent Preview bus; chart gameplay music retains Music routing.
The shared translation unit uses /bigobj and explicitly tracks its new includes.
Audio/chart/performance integration passed 19/19 cases (833 checks), shared
workbench tests passed 4/4, and both hosts built in Windows Debug. See
AYAudio/README.md and AYDocs/sdk/contracts/audio.md for contracts and limits.
Physical MIDI and manual UI acceptance remain separate from these automated checks.

本次本地验证与限制见 [`空间实体实施记录`](../../AYDocs/ENTITY-SPATIAL-HIERARCHY.md#实际验证windowsmsvc-release2026-10-14)。

## Camera / spatial hierarchy integration (2026-10-14)

Camera controllers bind persistent Entity identity with a World-scoped runtime handle; explicit missing targets cannot rebind by name. Presentation control samples world-space ancestor poses and converts once back to local position/orientation through the same alpha, preserving local scale and refusing singular/sheared parents.

Entity.PhysicsIntegration installs the default Jolt sphere-sweep adapter. Host custom queries override it; collisionEnabled explicitly disables per controller. The finite-volume query ignores sensors/self/target bodies, checks generation handles and reports initial penetration at zero center travel. Null/Mock/2D return Unsupported. Orbit distance plus collision radius/padding implement the camera arm and damped recovery. Explicit manager adapters must be detached before manager destruction.

Editor has stable target selection, collision parameters, and undoable main-window hierarchy dragging. CameraRigProject is a portable visual/authoring fixture; its Wall is visual-only, while AYEntity_CameraRigIntegrationTests exercises actual Jolt wall avoidance, lens effects/blending, self-ignore and recovery. See ../../AYDocs/CAMERA-SYSTEM.md and the integration evidence in ../../AYDocs/ENTITY-SPATIAL-HIERARCHY.md. Native Computer Use acceptance is deferred by user request.

## Editor camera icons (2026-10-14)

The 3D Edit viewport automatically submits one icon per PerspectiveCameraComponent,
including inactive lenses. Each icon uses scale-free world pose plus that lens's
localOffset/localRotation; spatial parents affect it exactly as they affect the
runtime lens. No MeshComponent, collider or extra Entity is added to the Scene.

Viewport Options offers Camera Icons: Normal / Wireframe / Hidden. Normal uses
the compact camera model; Wireframe uses its orange contour model. Preference
schema6 stores Editor.Viewport.CameraIcons outside scene data; source ABI is33.
Legacy preferences default to Normal. The global Mesh visibility toggle also
hides icons and their pick targets. 2D and Play submit no 3D camera icons.

Display height targets48 logical pixels, calculated from editor projection and
camera-space depth (orthographic zoom instead of depth). Rendering and ray picking
share the same scale-free iconWorld transform and canonical model bounds. Picking
selects the owning Entity; multiple lenses on one Entity are individually drawn
but share Entity selection and transform handles. Hidden camera-only Entities do
not have a phantom fallback sphere in viewport picking; Hierarchy remains usable.

The runtime-selected lens is bright, other lenses dim, selected Entity lenses
brighter. Selection adds near/far frustum edges from sanitized lens parameters and
the current editor viewport aspect. Frustum size is independent of icon scale.
Multi-view outputs with a different aspect are not previewed at their output aspect.

The session registers an owner-scoped scene builder and detaches it before teardown.
Uploads are editor-owned, have no shadows, and retain authored contour geometry
even with global wireframe enabled. Asset/material and projection caches avoid
reloading or rebuilding unchanged geometry. Renderer destruction before session
shutdown leaves no borrowed GPU destructor call. Source poses/pointers are borrowed
only during extraction; render packets contain values/handles, not Entity pointers.

Evidence: Test_EditorCameraVisualization.cpp covers hierarchy/multi-lens poses,
perspective/orthographic size, hidden/Play/invalid exclusion, picking offsets,
perspective/orthographic frustum geometry, per-viewport selection, menu preferences,
and Noop submissions using the real exported assets and distinct highlight states.
Native Computer Use acceptance remains deferred at the user's request.

### Verification evidence

Editor shell and aggregate test target build successfully (Release/Ninja).

| Suite | Cases | Assertions |
|---|---:|---:|
| AYEditor_CameraVisualization | 10/10 | 76 |
| AYEditor_Hierarchy | 13/13 | 268 |
| AYEditor_P0Core | 15/15 | 193 |
| Editor2DToolsTests | 18/18 | 340 |
| AYEditor_SceneBridge | 8/8 | 66 |
| AYEditor_TransformGizmo | 11/11 | 73 |

Total: 75 cases, 1016 assertions, no failures/skips. The camera suite
includes actual frame extraction, mock pointer selection, selected frustum packets,
mode/pref switching, Play exclusion and callback teardown. Noop frames use the
real camera assets and live material/mesh handles. SDK doctor:116 capabilities,
53 modules,82 searches,467 documented entry symbols,0 errors/0 warnings.
Reports are under out/camera-editor in the source checkout. Native Computer Use
interaction and visual acceptance remain deferred; no screenshot acceptance is claimed.


## 复古色彩控制与用户偏好（2026-10-19）

Render 面板追加 Color Stylization 开关、Neutral/Pixel Retro/Low Color/Dither Retro 预设、
Pixelation、Posterization、pixelSize、每通道色阶和抖动强度。实时通过已注册 RendererSubSystem
调用 Renderer::setColorStylizationSettings；未注册时仍允许编辑/保存设置。
切换预设填充全部控件；加载偏好时不重新应用预设，所以手调值可以恢复。整数滑条四舍五入。
数值标签与开关支持 en-US/zh-CN；预设专名保持一致。UI 在 Present 后，不受此颜色效果影响。
EditorPreferences schema7 在尾部追加7个字段，旧文件缺字段时保持默认关闭；
Editor.Render.ColorStylization.* 通过现有用户配置保存，不改变场景资源格式。
此处增加结构大小，source ABI 从33升到34，跨 demo/library 的宿主须同步重新编译。单测见 Test_EditorShell.cpp 的
editor_color_stylization_presets_custom_values_and_preferences；启动/关闭/再初始化覆盖自定义值。

显示模拟补充：Arcade/Broadcast与独立display开关/参数，面板按所选效果显示相关控制，隐藏时保留值。
偏好仍为schema8，内嵌look升为v2并可读v1；Editor source ABI36与完整重编匹配新增值布局。

主 Tilemap 画布的动画预览现在复用 AY2D 的 `TilemapPreviewClock`，保留每帧不足
1 毫秒的时间并在暂停期间保持计时；作者数据与 Scene 播放不受影响。

在阴影画笔、填充或矩形工具下，可用鼠标中键从地图格取样四象限阴影形状；
其他工具的中键仍取样图块。取样只改变当前画笔，不写入地图。
