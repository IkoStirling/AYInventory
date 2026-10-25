# AYInventory 设计

## 不变量

- 每个 `Item::id` 唯一、数量为正且不超过定义的 `maxStack`。一次实例只能处于
  `World`、`Container`、`Equipment`、`Attachment` 四者之一；`Location` 不是额外副本。
- 配件位置的 ownerId 是父物品实例 ID；父定义的槽必须接受子定义，父子数量
  均为 1。同槽至多一件、禁止循环、父物品仍有配件时拒绝销毁或合并。
  父物品在世界、容器、装备之间移动不重写后代归属；装备资料查询沿父链追踪
  到角色装备槽，并生成稳定的层级槽路径。
- 同一世界掉落实体、容器槽、装备槽至多容纳一个实例。容器和装备角色 ID 应使用
  `Entity::persistentId()` 或同等稳定内容 ID，不能使用会在 World 重建后改变的句柄。
- 装备数量必须为 1，定义须允许目标槽。物品实例 ID 在游戏存档中持续稳定。
- 需要 Ability 的实例在创建时分配单调增长的 `abilityBindingId`。该数值与字符串
  主身份一起存档，删除、合并后不会复用；恢复拒绝重复 ID 和倒退的分配水位。
- `transfer` 在更改归属前验证全部约束。请求收据含源、目标和实例身份；目标仍是
  当前归属时相同重试幂等，后续转移取代旧结果时旧请求返回 Stale；不同内容复用
  请求 ID 报错。收据纳入快照。
- `transferBatch` 将 1..256 条内部归属移动按实例 ID 规范化，先检查来源和制作预留，
  再用完整候选快照重建并验证最终槽位、装备和配件约束；允许两个占用槽直接互换。
  成功后一起交换所有权与单个规范化请求收据；失败不发布任何移动。内部验证使用
  v7 候选，以保留运行中的制作预留；普通保存仍保持 v6 默认。精确重试要求全部
  物品仍在提交的目标位置，后续移动使旧收据变为 Stale。World 端点被拒绝，
  因为世界实体的发布/撤销必须走独立 Host 事务。
- 分堆需要显式新实例 ID；满堆合并会退役源实例 ID。持有该 ID 的游戏引用必须更新。
- `mergeOnce` 在候选 Store 上做部分或整堆合并，把请求身份、来源/目标、数量与
  合并后两堆数量一同发布。精确重试只核对收据和当前后置数量，不再次扣量；
  内容冲突或后置数量已变化时拒绝。普通 `merge` 不承担重试去重。
- 销毁、整堆合并和制作耗尽的实例 ID 进入持久退役集合；新建实例不能复用它。
  这样旧转移/制作收据不会匹配后来占用同一 ID 的另一件物品。部分扣料和部分
  合并仍保留源实例 ID。
- 容器/角色尚有物品时拒绝删除。游戏先移动或销毁其中实例，避免世界 Entity
  销毁后出现悬空所有者。Core 无法自动观察 ECS 生命周期。

## 保存和接入

`Store::snapshot()` 输出 schema v6 的值对象，携带 catalogVersion、Ability ID 水位、
退役实例 ID、转移、合并和制作提交收据。`restore()` 接受 v2/v3 旧快照；旧快照未记录
此前已消失实例的 ID，迁移后无法追溯那段历史。
v4/v5 快照也可读取；v2–v4 不允许含配件位置。v5/v6 恢复先处理父实例，再处理子实例，
缺失或循环的父链拒绝且不修改现有 Store。
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
`equipmentDelta` 按实例 ID 比较变更前后的完整装备树，返回先撤销、后新增的
确定性计划；Host 在一个 Sim 边界应用，仍负责 Ability/Stats 一侧的原子发布。

## 交互、制作与放置

- `chooseFocus` 接收 Host 已按当前 World 射线结果映射的候选，过滤无效/超距命中，
  以距离及稳定对象 ID 决定焦点。`activate` 不信任跨帧焦点，要求 Host 当下复核命中、
  距离和权限。拾取先复制 Store 做转移校验，调用 Host 原子移除世界掉落，然后交换
  Store；容器打开只返回稳定容器 ID。具体盗窃、锁具、UI 归游戏处理。
- `transferFocusedContainerItems` 以已聚焦容器、Actor 和 1..256 条容器间移动为输入，
  先在候选 Store 完成批量校验，再重查当前焦点和目标权限；游戏还必须提供逐项授权
  回调，以核对非焦点容器及物品归属。每条移动至少一端属于焦点容器。
  可选保存回调先持久化完整候选再交换 Store，失败不改变归属；持久收据的精确
  重试不再次授权或保存。它不改变 World Entity，也不自动同步装备效果。
- `activatePickupDurable` 在同样的当场复核后先把候选 Store 和世界对象墓碑写入
  一个失败原子的游戏存档槽，再尝试删除实体、以无异常交换提交 Store。
  成功保存后，实体清理失败不能回退已提交的持久所有权；返回成功并带
  `cleanupPending`，原请求重试只执行幂等清理，不再保存或转移。
  Scene 适配器清理前确认原 World 位置已无物品，避免误删新对象。
- 配方只声明输入定义与数量、输出定义与数量。`reserveCraft` 在一个注册容器中按实例 ID
  顺序选择可用材料；预留物品在提交/取消前禁止移动、拆分、合并和销毁，其他配方不能
  超额预留。`commitCraft` 在 Store 副本中扣料、建输出和记收据，所有约束通过后交换；
  失败保留原预留和材料。重复提交依据收据返回，不重复扣料。预留不持久化，保存时原
  材料量不变；读档后先重新预留再提交。
- 普通 `reserveItems` 在一个注册容器内按稳定实例 ID 选取指定定义的数量，
  与制作预留共用 `_reservedByItem`，因此其他转移、拆分、合并、销毁和制作都不能
  消费已经占用的实例。相同请求和参数重复调用保留原选择；更改参数拒绝。
  `cancelItems` 只释放本请求。普通快照 v6 省略待办；显式 v8 快照同时保存制作和
  普通预留，恢复时在候选 Store 内验证精确实例、容器、定义及跨预留总数量。
  v7 制作快照仍可读取。普通预留不自行扣料、转移或提交游戏工作结果；Host 负责
  把后续结算与工作收据组合到同一模拟边界。
- 制作输出通常先进入容器，再由 `placeWorldItem` 放置。放置请求含稳定世界对象 ID、
  有限的三维姿态和正半尺寸。Host 的 `prepare` 在当前 AYPhysics/Scene 世界检查空间
  并暂存对象，`publish` 必须全成或全不成；失败后 `abort` 清理暂存，Store 无变化。
  发布成功后 Store 通过无分配交换写入世界归属。重复请求由 Host 验证已发布对象及
  姿态，不能再次发布。Host 应用相同稳定 ID 重建读档后的实体。
- `craftAndPlaceWorldItem` 将预留、扣料、输出实例创建与世界放置放在一份候选
  Store 中。Host `prepare/publish` 失败时丢弃候选，活动材料/预留/收据不变；
  成功后才交换。原制作请求 ID 是完整交易的持久重试键，重试核验输出身份及
  已发布对象。Host 仍须保证发布失败不留下部分世界对象；跨进程崩溃的读档
  一致性仍依赖游戏在暂停模拟边界把 World 账本与 Store 写入同一 SaveGame payload。
- 可选 durable 放置/制作要求 `IDurablePlacementHost`。世界发布后，游戏回调
  把候选 Store 与放置对象身份/姿态写入同一可恢复存档槽；保存失败或抛异常时
  立即撤销刚发布的对象，Store 候选不交换。保存成功后无分配交换 Store 并
  释放撤销令牌。`ScenePhysicsHost3D` 只允许撤销紧前一次未提交的发布，避免
  误删先前对象。旧槽必须在保存失败时保持原样；崩溃恢复须用成功写入的同一
  槽重建 World 和 Store。该路径不自动保存整个 Scene。拾取的立即落盘使用
  上述保存优先路径；游戏必须把墓碑与候选 Store 同槽提交。
- `ScenePhysicsHost3D` 接受当前 Scene 的 `World&`、`IPhysicsQuery&`、`Store&`，
  以及游戏拥有的当前视线和权限回调。焦点和激活各自发起当前 raycast，按有效 3D
  刚体句柄解析 Entity，再用 `persistentId` 对应 Store 的世界物品或容器。
  拾取仅删除没有子实体的当前 Entity；任何失败都不让 Store 发布预提交转移。
- 放置将物品请求的稳定世界 ID 直接用作 Entity 持久 ID，因此具体适配要求 32 位
  小写十六进制 ID。`prepare` 与 `publish` 用当前 overlapBox 检查旋转盒占用，
  拒绝占用和重复 ID；`publish` 创建 Spatial Entity、世界姿态、静态 3D 刚体和盒碰撞描述。
  创建过程失败清理新 Entity，成功后 Store 才交换所有权。重试验证 ID、姿态与碰撞
  半尺寸；实际物理体在 EntityPhysicsBridge 的下一次预物理同步产生。
- `activate` 和 `placeWorldItem` 把 Host 回调异常转为失败诊断。放置的 `prepare` 或
  `publish` 抛异常时仍执行 `abort`；拾取的 `consumeWorldPickup` 即使抛异常也必须
  保持世界侧失败原子性。Host 回调不得改写或重入同一个 Store；Store 的暂存状态
  仅在 Host 全部成功后交换。
- Scene/Store 联合读档需先解码独立候选账本，在暂存 Scene 注册作者掉落的墓碑
  回调与 Store 快照参与者，全部恢复成功再发布。放置后同时保存动态 Entity 和
  Store 的世界归属 ID；`AYInventorySceneRestoreTest` 验证来回恢复不复制物品。
- 旋转盒占用只按物理层返回刚体，地形吸附、游戏建造规则与权限仍由游戏提供。
  不支持盒查询的后端会返回失败，不降级到较大的外接球。适配不隐式注册 Entity 组件、保存场景或实现游戏权限；调用者
  在 Sim 边界使用它，World/Store 卸载和恢复时重新注入当前实例。

## 装备投影

`EquipmentProjection` 是可选跨模块桥，Core 仍无 Stats/Ability 依赖。
每个 actor 在串行 Sim 边界获取 Store 的完整装备剖面，游戏的纯 modifier 工厂
把各个 stat bonus 映射到稳定唯一 handle 与合法 zone。桥接先在 Stats 和 Ability
候选副本中验证全部更改：Stats 的 owned modifier 批量替换、Ability 的实例增加、
跨角色所有权转移、选择和已销毁实例移除。Ability 先无分配交换候选，Stats 后
无异常交换候选，最后发布桥内部账本；任何预检失败均不触及活动运行时。
收起但仍存在的物品保留 Ability 弹药/冷却，复装不重建实例。游戏保持 actor
选择的独占写入权，并在同一身份域内装配所有 actor。
`transferAndSyncEquipment` 对容器/装备间转移复制 Store、共享 Ability Runtime、
全部受影响角色的 Stats 与投影内部账本；每个角色在候选上执行原有 `sync`。
来源与目的装备角色必须各有一个目标，拒绝重复指针/身份。所有候选通过后，
先以可拒绝的 Runtime 交换为发布门，再做无异常 Stats、投影、Store 交换；
因此目的角色的 modifier 校验失败也不会留下已清除的来源角色效果。
世界掉落对象的生命周期不在此接口内。读档时在暂存状态重建桥，重新投影。

## 后续

交易、钱币、重量、掉落表、可变品质、装备词条和网络复制均由后续需求单独决定。
