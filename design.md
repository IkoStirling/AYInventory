# AYInventory 设计

## 不变量

- 每个 `Item::id` 唯一、数量为正且不超过定义的 `maxStack`。一次实例只能处于
  `World`、`Container`、`Equipment` 三者之一；`Location` 不是额外副本。
- 同一世界掉落实体、容器槽、装备槽至多容纳一个实例。容器和装备角色 ID 应使用
  `Entity::persistentId()` 或同等稳定内容 ID，不能使用会在 World 重建后改变的句柄。
- 装备数量必须为 1，定义须允许目标槽。物品实例 ID 在游戏存档中持续稳定。
- 需要 Ability 的实例在创建时分配单调增长的 `abilityBindingId`。该数值与字符串
  主身份一起存档，删除、合并后不会复用；恢复拒绝重复 ID 和倒退的分配水位。
- `transfer` 在更改归属前验证全部约束。请求收据含源、目标和实例身份；目标仍是
  当前归属时相同重试幂等，后续转移取代旧结果时旧请求返回 Stale；不同内容复用
  请求 ID 报错。收据纳入快照。
- 分堆需要显式新实例 ID；满堆合并会退役源实例 ID。持有该 ID 的游戏引用必须更新。
- 容器/角色尚有物品时拒绝删除。游戏先移动或销毁其中实例，避免世界 Entity
  销毁后出现悬空所有者。Core 无法自动观察 ECS 生命周期。

## 保存和接入

`Store::snapshot()` 输出 schema v3 的值对象，携带 catalogVersion、Ability ID 水位、
转移和制作提交收据。`restore()` 接受无制作收据的 v2 所有权快照。
`restore()` 在临时 Store 中逐条校验定义、容量、归属与请求收据，成功后交换状态；
失败不改变原 Store。`AYInventory::Json` 可选 target 序列化该值对象，解析有 8 MiB
上限。作者内容变更后，游戏须显式升级定义/旧快照，再恢复；不会通过只改版本号
静默接受不兼容物品。

游戏可把 Store 状态作为一个 T01 `WorldStateLedger` 参与者，先加载作者 Scene，
再恢复 Store 和世界掉落实体。在 World 卸载前保存参与者，装载新 World 后重新绑定。
长期分区存档由游戏决定选择哪些实例，Core 不自动捕获整个 World。

AYAbility 的设备实例并非物品所有权。装备 profile 暴露配置 ID 和持久数值实例 ID，供 Host 把实例
绑定到 `AbilitySystem::addEquipment/equip`；Stats 平值映射为游戏拥有的
`StatSet::replaceOwnedModifiers` 组。Host 可调用 Ability 的卸装/移除/转移入口或在
角色 Ability 重建时按 profile 重放；不能把
单纯查询 profile 误称为自动集成。Inventory 内部转移是原子的；Ability/Stats
同步目前是 Host 的独立提交，失败需在 staging 状态中重试或补偿，不能声称跨模块
整体原子。具体属性 ID、计算区与装备规则由游戏定义。

## 交互、制作与放置

- `chooseFocus` 接收 Host 已按当前 World 射线结果映射的候选，过滤无效/超距命中，
  以距离及稳定对象 ID 决定焦点。`activate` 不信任跨帧焦点，要求 Host 当下复核命中、
  距离和权限。拾取先复制 Store 做转移校验，调用 Host 原子移除世界掉落，然后交换
  Store；容器打开只返回稳定容器 ID。具体盗窃、锁具、UI 归游戏处理。
- 配方只声明输入定义与数量、输出定义与数量。`reserveCraft` 在一个注册容器中按实例 ID
  顺序选择可用材料；预留物品在提交/取消前禁止移动、拆分、合并和销毁，其他配方不能
  超额预留。`commitCraft` 在 Store 副本中扣料、建输出和记收据，所有约束通过后交换；
  失败保留原预留和材料。重复提交依据收据返回，不重复扣料。预留不持久化，保存时原
  材料量不变；读档后先重新预留再提交。
- 制作输出通常先进入容器，再由 `placeWorldItem` 放置。放置请求含稳定世界对象 ID、
  有限的三维姿态和正半尺寸。Host 的 `prepare` 在当前 AYPhysics/Scene 世界检查空间
  并暂存对象，`publish` 必须全成或全不成；失败后 `abort` 清理暂存，Store 无变化。
  发布成功后 Store 通过无分配交换写入世界归属。重复请求由 Host 验证已发布对象及
  姿态，不能再次发布。Host 应用相同稳定 ID 重建读档后的实体。
- 事务原子边界限定在 Store 和满足回调契约的 Host 实现。此模块只提供回调协议和
  演示适配，不包含实际 AYPhysics 重叠查询、AYScene Entity 创建或持久化物体姿态。
  集成方须在 World 卸载/恢复边界先保存场景实体和 Store，再在新 World 重建对应对象。

## 后续

交易、钱币、重量、掉落表、可变品质、装备词条和网络复制均由后续需求单独决定。
