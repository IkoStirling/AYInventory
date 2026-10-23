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
同一 request ID 的相同重试返回成功，异义重用被拒绝。重复操作不能产生两个实例。
容器/角色还有物品时拒绝移除；游戏需先转移或明确销毁物品。

装备定义可携带现有 AYAbility 配置 ID 和 AYStats 的 stat ID/平值。
需要 Ability 的物品在创建时由 Store 分配持久数值 `abilityBindingId`，不从字符串截断
或哈希得到；删除后也不复用旧数值。
`equippedProfiles(actorId)` 给 Host 一个只读同步输入；Host 在固定 Sim 边界把它映射到
Ability/Stats，并负责撤销离开装备位的效果。Core 不把词条、经济或掉落规则写死。

`snapshot()`/`restore()` 捕获已注册容器、装备角色、实例、Ability ID 水位与转移收据；恢复校验版本、
定义引用、唯一归属和槽位后整体发布。可选 `AYInventory::Json` 提供版本化 JSON，
可作为 T01 `WorldStateLedger` 的一个游戏选择的 payload。定义表由游戏随内容加载，
不在存档中复制。

独立验证（没有 nlohmann_json 时关闭 JSON target）：

```powershell
cmake -S AYRuntime/AYInventory -B out/build/ayinventory -DAY_INVENTORY_BUILD_JSON=OFF
cmake --build out/build/ayinventory
ctest --test-dir out/build/ayinventory --output-on-failure
```

示例：`AYInventoryPickupExample` 演示世界掉落→背包→装备。
详细失败语义见 [design.md](design.md)。
