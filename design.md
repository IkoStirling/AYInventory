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
- 销毁、整堆合并和制作耗尽的实例 ID 进入持久退役集合；新建实例不能复用它。
  这样旧转移/制作收据不会匹配后来占用同一 ID 的另一件物品。部分扣料和部分
  合并仍保留源实例 ID。
- 容器/角色尚有物品时拒绝删除。游戏先移动或销毁其中实例，避免世界 Entity
  销毁后出现悬空所有者。Core 无法自动观察 ECS 生命周期。

## 保存和接入

`Store::snapshot()` 输出 schema v4 的值对象，携带 catalogVersion、Ability ID 水位、
退役实例 ID、转移和制作提交收据。`restore()` 接受 v2/v3 旧快照；旧快照未记录
此前已消失实例的 ID，迁移后无法追溯那段历史。
`restore()` 在临时 Store 中逐条校验定义、容量、归属与请求收据，成功后交换状态；
退役 ID 与活动实例重叠、为空或在 JSON 中重复都会拒绝，失败不改变原 Store。
`AYInventory::Json` 可选 target 序列化该值对象，解析有 8 MiB
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
- `ScenePhysicsHost3D` 接受当前 Scene 的 `World&`、`IPhysicsQuery&`、`Store&`，
  以及游戏拥有的当前视线和权限回调。焦点和激活各自发起当前 raycast，按有效 3D
  刚体句柄解析 Entity，再用 `persistentId` 对应 Store 的世界物品或容器。
  拾取仅删除没有子实体的当前 Entity；任何失败都不让 Store 发布预提交转移。
- 放置将物品请求的稳定世界 ID 直接用作 Entity 持久 ID，因此具体适配要求 32 位
  小写十六进制 ID。`prepare` 以半尺寸外接球调用当前 overlapSphere，拒绝占用和
  重复 ID；`publish` 创建 Spatial Entity、世界姿态、静态 3D 刚体和盒碰撞描述。
  创建过程失败清理新 Entity，成功后 Store 才交换所有权。重试验证 ID、姿态与碰撞
  半尺寸；实际物理体在 EntityPhysicsBridge 的下一次预物理同步产生。
- 球体检测对旋转盒和细长对象保守，可能拒绝本可放置的空间；游戏可按需另行实现更
  精确的 IPlacementHost。适配不隐式注册 Entity 组件、保存场景或实现游戏权限；调用者
  在 Sim 边界使用它，World/Store 卸载和恢复时重新注入当前实例。

## 后续

交易、钱币、重量、掉落表、可变品质、装备词条和网络复制均由后续需求单独决定。
