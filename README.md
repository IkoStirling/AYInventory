# AYInventory

通用物品身份和所有权核心，独立本地 Git 仓库。游戏以稳定字符串 ID 定义物品、实例、
世界掉落对象、容器和装备角色；所有权只在 `Store` 中保存一份。2D、3D、Headless
都可使用，不依赖 Entity、Renderer、物理或特定游戏规则。

## 最小用法

```cpp
#include <AYInventory/Inventory.h>
using namespace ayt::inventory;

Definition sword;
sword.id = "sword";
sword.equipSlots = {"hand"};
auto store = Store::create(1, {sword});
store->addContainer({"player/bag", {"slot-1"}});
store->addEquipmentOwner({"player", {"hand"}});
Location drop{Place::World, "scene/drop-1", ""};
Location bag{Place::Container, "player/bag", "slot-1"};
store->createItem({"sword-1", "sword", 1, drop});
store->transfer("sword-1", drop, bag, "pickup-request-1");
```

`transfer` 先核验预期来源、目标槽、堆叠与装备兼容性，之后一次性更新归属；
同一 request ID 的相同重试在物品仍位于目标时返回成功，异义重用或已被后续操作取代
的旧请求被拒绝。重复操作不能产生两个实例。
`transferBatch` 接收最多 256 个容器、装备或配件位置之间的完整实例移动，
在最终状态中统一校验，因此可直接互换两个已占用槽。任一移动无效则全部不生效；
同一请求 ID 的收据随原有 v6/v7 快照保存，读档后仍可去重。世界实体拾取与放置
仍须使用 WorldTransactions 的 Host 边界，不能用此入口只改 Store。
容器/角色还有物品时拒绝移除；游戏需先转移或明确销毁物品。

装备定义可携带现有 AYAbility 配置 ID 和 AYStats 的 stat ID/平值。
需要 Ability 的物品在创建时由 Store 分配持久数值 `abilityBindingId`，不从字符串截断
或哈希得到；删除后也不复用旧数值。
`equippedProfiles(actorId)` 给 Host 一个只读同步输入；Host 在固定 Sim 边界把它映射到
Ability/Stats，并负责撤销离开装备位的效果。
可选 `AYInventory::EquipmentProjection` 使用 `equippedProfiles` 在固定 Sim 边界
同步 AYStats 修正和 AYAbility 选中装备：先在两份候选运行时中完成预检，再一次性
交换发布。游戏提供稳定的 Stats modifier 身份/zone 工厂，并保持它为纯函数；
适配器保留收起装备的弹药与冷却，跨角色装备时转移相同 Ability 实例，销毁后撤销。
跨角色装备转移可调用 `transferAndSyncEquipment`，同时提供来源与目的角色的
`EquipmentSyncTarget`。它先在候选 Store、两个 Stats 和同一 Ability Runtime 中
完成转移及投影，全部成功才无分配发布；第二个角色校验失败时，来源角色效果与
Store 归属也保持原状。此入口只处理容器与装备位置，不销毁世界拾取实体。
保存恢复时从暂存 Store、Stats、Ability 创建新的适配器再同步，不能在活动状态
上边读档边应用。该可选目标不改变 Inventory Core 的依赖。
定义的 `attachmentSlots` 可指定槽与
可接受的配件定义；配件仍有独立实例 ID，通过 `Place::Attachment` 归属于父物品。
主装备移动时整棵配件树随之移动；`equippedProfiles` 包含已装备树上的配件，
`equipmentDelta(before, after, actorId)` 给出需撤销与新增的效果资料。Core 不把
词条、经济或掉落规则写死。

`snapshot()`/`restore()` 捕获已注册容器、装备角色、实例、已退役实例 ID、Ability ID 水位、转移、合并与制作收据；恢复校验版本、
定义引用、唯一归属和槽位后整体发布。可选 `AYInventory::Json` 提供版本化 JSON，
可作为 T01 `WorldStateLedger` 的一个游戏选择的 payload。定义表由游戏随内容加载，
不在存档中复制。
销毁、整堆合并和制作耗尽会永久退役源实例 ID，旧转移收据不能把后来创建的物品
误认成旧实例。schema v6 保存配件归属、退役集合与堆叠合并收据，仍可读取 v2/v3/v4/v5；v2/v3 旧存档没有历史退役信息，
恢复后只能从当时仍存在的实例开始保证此约束。
需要接受外部重复请求的堆叠合并使用 `mergeOnce(source, target, quantity, requestId)`：
原请求的部分合并或整堆合并不会二次扣量，读档后收据仍有效。若后续操作已改变
两堆的数量，旧请求返回 Stale；同一请求 ID 改用其他内容会失败。普通 `merge`
保留给游戏已保证仅提交一次的内部操作。

独立验证（没有 nlohmann_json 时关闭 JSON target）：

```powershell
cmake -S AYRuntime/AYInventory -B out/build/ayinventory -DAY_INVENTORY_BUILD_JSON=OFF
cmake --build out/build/ayinventory
ctest --test-dir out/build/ayinventory --output-on-failure
```

`reserveCraft(recipe, containerId, requestId)` 按物品定义锁定材料；
`commitCraft` 一次性扣料并产生一个有稳定 ID 的输出实例，失败保持材料与预留；若材料实例上仍有附件且将被完全消耗，需先拆下附件，否则制作失败并保留预留。
`cancelCraft` 释放预留。普通 `snapshot()` 仍生成 schema v6，待提交预留不入快照；
读档相当于取消。`reserveItems(definitionId, quantity, containerId, requestId)` 为普通
工作占用确定的物品实例数量，与制作共用可用量；`cancelItems` 释放，`findItemHold`
返回所选实例，`reservableQuantity` 查询尚未占用的数量。预留的整个实例暂时不能移动、
拆分、合并或销毁。需要继续待办时，显式调用 `snapshotWithReservations()` 生成
schema v8：记录制作和普通预留的请求与精确材料实例。`restore()` 在发布前校验归属和
跨种类预留总量；不完整或超额的预留使整个恢复失败。当前恢复器可读取 v2–v8 快照；
普通 `snapshot()` 仍保持 v6，且不保存待办预留。
`consumeHeldItems(holdRequestId, completionRequestId)` 原子扣除已占用的全部数量，
允许只扣除堆叠的一部分；耗尽的实例 ID 永久退役，有附属配件的父实例不可耗尽。
同一完成请求读档后只返回成功，不重复扣料；同一占用 ID 不可再次预留。
一旦产生消耗收据，普通与显式预留快照均升为 schema v9，JSON 中增加
`consumptions` 收据数组；v9 仍可读取 v2–v8。普通快照仍省略待办预留。
需要直接建造时，`craftAndPlaceWorldItem` 在 Store 候选中预留并扣料，先让
`IPlacementHost` 发布世界对象，再无分配交换候选 Store；空间检查或发布失败
不会消耗材料。完成收据和世界物品 ID 同时持久化，重复调用只核验已发布对象。

`WorldTransactions.h` 接收 Host 从 3D 射线/空间查询映射出的稳定对象 ID。
`chooseFocus` 选最近候选，`activate` 在操作当下重验有效性与权限，完成拾取或返回可打开的容器 ID。
在已聚焦容器与玩家背包之间搬运多件完整实例时，可用 `transferFocusedContainerItems`：
它先暂存 `transferBatch`，再重验当前焦点、目标权限和游戏提供的逐项归属授权；
每条移动必须涉及当前容器，且两端均为容器。可选保存回调接受完整候选后才发布
Store，保存失败不留下半次交换；读档后的精确请求重试不会再次授权或保存。
立即落盘的拾取可用 `activatePickupDurable`：游戏以 `PickupSave` 将候选 Store 与
世界对象墓碑写入同一失败原子的槽；保存成功即提交所有权。实体删除若失败，
结果仍表示已提交，并以 `cleanupPending` 提醒后续重试清理。`ScenePhysicsHost3D`
提供当前 World 中的幂等清理，旧射线焦点不能再次取出已提交物品。
`placeWorldItem` 先暂存所有权转移，调用 Host 空间检查和对象发布；发布成功后才切换 Store。
需要立即保存时可改用 `placeWorldItemDurable` 或 `craftAndPlaceWorldItemDurable`：
游戏的 `PlacementSave` 回调把候选 Store 与放置对象记录写入同一存档槽，失败时
`IDurablePlacementHost` 撤销刚发布的对象并保留原 Store。`ScenePhysicsHost3D`
实现该撤销；回调必须保证保存失败不替换旧槽，读档时从同一槽重建放置对象。
Host 的拾取/发布回调必须具有文档所述的失败原子性；实际 AYScene Entity 与 AYPhysics 查询
可选用 `AYInventory::World3DHost` 的 `ScenePhysicsHost3D` 连接。游戏注入当前
`Scene::world()`、`IPhysicsQuery`、视线与权限回调；新放置对象的 ID 必须是 Entity
支持的 32 位小写十六进制持久 ID。该适配以射线重验拾取，并用当前旋转盒重叠检查放置空间。
独立 Core 构建仍无需 Scene/Physics。

示例：`AYInventoryPickupExample` 演示世界掉落→背包→装备；
`AYInventoryCraftPlaceExample` 演示拾取→直接制作并放置→重复请求。详细失败语义见 [design.md](design.md)。
实际 Host 映射顺序见 [world-host-integration.md](docs/world-host-integration.md)。
`AYInventorySceneRestoreTest` 在支持 Scene/Physics/SaveGame 的构建中验证作者掉落
拾取后的墓碑恢复、背包所有权、重新放置与再次读档。交互/放置 Host 抛异常时返回
错误结果；放置准备或发布抛异常也会执行 `abort()`，Host 须保证失败时世界侧
没有留下半成品。
