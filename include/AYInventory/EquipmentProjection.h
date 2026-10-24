#pragma once

#include <AYInventory/Inventory.h>
#include <AYAbility/Runtime.h>
#include <AYStats/Stats.h>

#include <functional>
#include <span>

namespace ayt::inventory {

/// Map a selected inventory stat bonus to a uniquely owned Stats modifier.
/// The game supplies its stable handle/effect/rule and target zone namespace;
/// target and flat value must match the authored bonus. Do not mutate either
/// runtime from this callback.
using ModifierFactory = std::function<stats::Modifier(
    const Store::EquippedProfile&, std::size_t bonusIndex)>;

/// One actor's optional equipment-effects adapter. Sync at a serialized Sim
/// boundary after a Store transaction, or against disposable staged runtimes
/// while restoring a save. It preserves Ability ammunition/cadence while an
/// item is stowed, removes destroyed instances, and atomically publishes this
/// actor's Stats and Ability candidate copies when all validation succeeds.
/// The game remains the exclusive Ability selection writer for this actor and
/// must use the actor's Ability identity domain in every projection.
class EquipmentProjection {
public:
    static Result<EquipmentProjection> create(ability::Identity actor,
        ability::Id itemDomain);
    /// selectedItemId must identify one currently equipped Ability item, or
    /// be empty to clear this projection's prior selection. Cross-actor
    /// transfer uses Ability::Runtime::transferEquipment on the candidate.
    Result<void> sync(const Store& store, std::string_view actorId,
        std::string_view selectedItemId, stats::StatSet& stats,
        ability::Runtime& ability, const ModifierFactory& modifierFactory);
    /// Exchange only validated projection policy state; used to publish a
    /// multi-actor candidate without allocation after all checks pass.
    void swapState(EquipmentProjection& other) noexcept;
    [[nodiscard]] ability::Identity actorIdentity() const noexcept { return _actor; }

private:
    EquipmentProjection(ability::Identity actor, ability::Id domain)
        : _actor(actor), _itemDomain(domain) {}
    ability::Identity _actor;
    ability::Id _itemDomain = 0;
    std::map<std::string, ability::Identity> _knownItems;
    std::vector<stats::Modifier> _ownedModifiers;
    ability::Identity _selected{};
};

struct EquipmentSyncTarget {
    EquipmentProjection* projection = nullptr;
    std::string_view actorId;
    std::string_view selectedItemId;
    stats::StatSet* stats = nullptr;
    ModifierFactory modifierFactory;
};

/// Transfer between containers/equipment and reconcile all affected actors
/// in one candidate Store, Stats and Ability transaction. Include a target
/// for each source/destination equipment owner. A failed modifier, Ability or
/// Store check leaves every live owner unchanged. World pickup/removal is a
/// separate transaction and is not accepted here.
[[nodiscard]] Result<void> transferAndSyncEquipment(Store& store,
    std::string_view itemId, const Location& source, const Location& destination,
    std::string requestId, ability::Runtime& ability,
    std::span<const EquipmentSyncTarget> targets);

} // namespace ayt::inventory
