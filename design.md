# AYInventory 设计

## 不变量

- 每个 `Item::id` 唯一、数量为正且不超过定义的 `maxStack`。一次实例只能处于
  `World`、`Container`、`Equipment` 三者之一；`Location` 不是额外副本。
- 同一世界掉落实体、容器槽、装备槽至多容纳一个实例。容器和装备角色 ID 应使用
  `Entity::persistentId()` 或同等稳定内容 ID，不能使用会在 World 重建后改变的句柄。
- 装备数量必须为 1，定义须允许目标槽。物品实例 ID 在游戏存档中持续稳定。
- 需要 Ability 的实例在创建时分配单调增长的 `abilityBindingId`。该数值与字符串
  主身份一起存档，删除、合并后不会复用；恢复拒绝重复 ID 和倒退的分配水位。
- `transfer` 在更改归属前验证全部约束。请求收据含源、目标和实例身份；相同重试
  幂等，不同内容复用请求 ID 报错。收据纳入快照。
- 分堆需要显式新实例 ID；满堆合并会退役源实例 ID。持有该 ID 的游戏引用必须更新。
- 容器/角色尚有物品时拒绝删除。游戏先移动或销毁其中实例，避免世界 Entity
  销毁后出现悬空所有者。Core 无法自动观察 ECS 生命周期。

## 保存和接入

`Store::snapshot()` 输出 schema v2 的值对象，携带 catalogVersion 和 Ability ID 水位。
`restore()` 在临时 Store 中逐条校验定义、容量、归属与请求收据，成功后交换状态；
失败不改变原 Store。`AYInventory::Json` 可选 target 序列化该值对象，解析有 8 MiB
上限。作者内容变更后，游戏须显式升级定义/旧快照，再恢复；不会通过只改版本号
静默接受不兼容物品。

游戏可把 Store 状态作为一个 T01 `WorldStateLedger` 参与者，先加载作者 Scene，
再恢复 Store 和世界掉落实体。在 World 卸载前保存参与者，装载新 World 后重新绑定。
长期分区存档由游戏决定选择哪些实例，Core 不自动捕获整个 World。

AYAbility 的设备实例并非物品所有权。装备 profile 暴露配置 ID 和持久数值实例 ID，供 Host 把实例
绑定到 `AbilitySystem::addEquipment/equip`；Stats 平值映射为游戏拥有的
`StatSet::replaceOwnedModifiers` 组。当前 Ability 公开接口缺少显式卸装/移除实例，
因此 Host 需要在角色 Ability 重建时按 profile 重放，或等待该撤销入口；不能把
单纯查询 profile 误称为自动集成。Inventory 内部转移是原子的；Ability/Stats
同步目前是 Host 的独立提交，失败需在 staging 状态中重试或补偿，不能声称跨模块
整体原子。具体属性 ID、计算区与装备规则由游戏定义。

## 后续

T03 将在同一唯一所有权模型上建立交互候选、配方预留与放置事务。
交易、钱币、重量、掉落表、可变品质、装备词条和网络复制均由后续需求单独决定。
