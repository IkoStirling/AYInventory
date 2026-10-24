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
容器/角色还有物品时拒绝移除；游戏需先转移或明确销毁物品。

装备定义可携带现有 AYAbility 配置 ID 和 AYStats 的 stat ID/平值。
需要 Ability 的物品在创建时由 Store 分配持久数值 `abilityBindingId`，不从字符串截断
或哈希得到；删除后也不复用旧数值。
`equippedProfiles(actorId)` 给 Host 一个只读同步输入；Host 在固定 Sim 边界把它映射到
Ability/Stats，并负责撤销离开装备位的效果。Core 不把词条、经济或掉落规则写死。

`snapshot()`/`restore()` 捕获已注册容器、装备角色、实例、已退役实例 ID、Ability ID 水位、转移与制作收据；恢复校验版本、
定义引用、唯一归属和槽位后整体发布。可选 `AYInventory::Json` 提供版本化 JSON，
可作为 T01 `WorldStateLedger` 的一个游戏选择的 payload。定义表由游戏随内容加载，
不在存档中复制。
销毁、整堆合并和制作耗尽会永久退役源实例 ID，旧转移收据不能把后来创建的物品
误认成旧实例。schema v4 保存退役集合，仍可读取 v2/v3；旧存档没有历史退役信息，
恢复后只能从当时仍存在的实例开始保证此约束。

独立验证（没有 nlohmann_json 时关闭 JSON target）：

```powershell
cmake -S AYRuntime/AYInventory -B out/build/ayinventory -DAY_INVENTORY_BUILD_JSON=OFF
cmake --build out/build/ayinventory
ctest --test-dir out/build/ayinventory --output-on-failure
```

`reserveCraft(recipe, containerId, requestId)` 按物品定义锁定材料；
`commitCraft` 一次性扣料并产生一个有稳定 ID 的输出实例，失败保持材料与预留；
`cancelCraft` 释放预留。保存时待提交预留不入快照，材料仍在原槽；读档相当于取消。
快照 schema v4 记录制作收据和退役实例 ID，可读取 v2/v3 所有权存档。

`WorldTransactions.h` 接收 Host 从 3D 射线/空间查询映射出的稳定对象 ID。
`chooseFocus` 选最近候选，`activate` 在操作当下重验有效性与权限，完成拾取或返回可打开的容器 ID。
`placeWorldItem` 先暂存所有权转移，调用 Host 空间检查和对象发布；发布成功后才切换 Store。
Host 的拾取/发布回调必须具有文档所述的失败原子性；实际 AYScene Entity 与 AYPhysics 查询
可选用 `AYInventory::World3DHost` 的 `ScenePhysicsHost3D` 连接。游戏注入当前
`Scene::world()`、`IPhysicsQuery`、视线与权限回调；新放置对象的 ID 必须是 Entity
支持的 32 位小写十六进制持久 ID。该适配以射线重验拾取，并用保守球体重叠检查放置空间。
独立 Core 构建仍无需 Scene/Physics。

示例：`AYInventoryPickupExample` 演示世界掉落→背包→装备；
`AYInventoryCraftPlaceExample` 演示制作→世界放置。详细失败语义见 [design.md](design.md)。
实际 Host 映射顺序见 [world-host-integration.md](docs/world-host-integration.md)。
`AYInventorySceneRestoreTest` 在支持 Scene/Physics/SaveGame 的构建中验证作者掉落
拾取后的墓碑恢复、背包所有权、重新放置与再次读档。交互/放置 Host 抛异常时返回
错误结果；放置准备或发布抛异常也会执行 `abort()`，Host 须保证失败时世界侧
没有留下半成品。
