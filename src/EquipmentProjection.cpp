#include <AYInventory/EquipmentProjection.h>

#include <exception>
#include <map>
#include <set>
#include <type_traits>
#include <utility>
#include <vector>

namespace ayt::inventory {
namespace {
template<class T> Result<T> reject(Error code, std::string message) {
    return std::unexpected(Diagnostic{code, std::move(message)});
}
}

Result<EquipmentProjection> EquipmentProjection::create(
    ability::Identity actor, ability::Id itemDomain) {
    if (!actor.valid() || itemDomain != actor.domain)
        return reject<EquipmentProjection>(Error::Invalid, "Invalid actor or item domain");
    return EquipmentProjection(actor, itemDomain);
}

Result<EquipmentProjection> EquipmentProjection::adoptRestored(
    ability::Identity actor, ability::Id itemDomain,
    const Store& store, std::string_view actorId,
    const stats::StatSet& statSet, const ability::Runtime& runtime,
    const ModifierFactory& modifierFactory) {
    auto created = create(actor, itemDomain);
    if (!created || actorId.empty() || !modifierFactory)
        return reject<EquipmentProjection>(Error::Invalid,
            "Incomplete restored equipment projection context");
    try {
        auto projection = std::move(*created);
        const auto snapshot = store.snapshot();
        std::map<ability::Id, std::string> itemBindings;
        for (const auto& item : snapshot.items) {
            if (item.abilityBindingId) {
                if (!itemBindings.emplace(item.abilityBindingId, item.id).second)
                    return reject<EquipmentProjection>(Error::Duplicate,
                        "Duplicate restored Ability item binding");
            }
        }
        auto checkpoint = runtime.checkpoint();
        if (!checkpoint)
            return reject<EquipmentProjection>(Error::Stale,
                "Ability checkpoint unavailable during projection restore");
        for (const auto& equipment : checkpoint->equipment) {
            if (equipment.identity.domain != itemDomain
                || equipment.owner != actor) continue;
            const auto found = itemBindings.find(equipment.identity.object);
            if (found == itemBindings.end()
                || equipment.identity.generation != 1)
                return reject<EquipmentProjection>(Error::Stale,
                    "Orphan restored inventory Ability equipment");
            const auto* item = store.findItem(found->second);
            const auto* definition = item
                ? store.findDefinition(item->definitionId) : nullptr;
            if (!definition || definition->abilityConfiguration
                    != equipment.configuration)
                return reject<EquipmentProjection>(Error::Incompatible,
                    "Restored Ability configuration differs from item");
            projection._knownItems.emplace(item->id, equipment.identity);
        }
        std::set<std::uint64_t> modifierHandles;
        std::set<ability::Identity> equippedBindings;
        for (const auto& profile : store.equippedProfiles(actorId)) {
            if (profile.abilityConfiguration) {
                const ability::Identity id{itemDomain,
                    profile.abilityBindingId, 1};
                const auto state = runtime.equipment(id);
                if (!id.valid() || !state || state->owner != actor
                    || state->configuration != profile.abilityConfiguration)
                    return reject<EquipmentProjection>(Error::Stale,
                        "Restored equipped item has no matching Ability state");
                equippedBindings.insert(id);
            }
            for (std::size_t index = 0;
                 index < profile.statBonuses.size(); ++index) {
                const auto modifier = modifierFactory(profile, index);
                const auto& bonus = profile.statBonuses[index];
                if (!modifier.handle || modifier.target != bonus.statId
                    || modifier.value != bonus.flat
                    || !modifierHandles.insert(modifier.handle).second)
                    return reject<EquipmentProjection>(Error::Invalid,
                        "Restored equipment modifier factory mismatch");
                bool found = false;
                for (const auto& persisted : statSet.modifiers())
                    if (persisted == modifier) { found = true; break; }
                if (!found)
                    return reject<EquipmentProjection>(Error::Stale,
                        "Restored Stats omit equipment modifier");
                projection._ownedModifiers.push_back(modifier);
            }
        }
        const auto selected = runtime.equipped(actor);
        if (!selected)
            return reject<EquipmentProjection>(Error::Stale,
                "Restored Ability actor unavailable");
        if (*selected) {
            if (!equippedBindings.contains(**selected))
                return reject<EquipmentProjection>(Error::Stale,
                    "Restored Ability selection is not an equipped item");
            projection._selected = **selected;
        }
        return projection;
    } catch (const std::exception& ex) {
        return reject<EquipmentProjection>(Error::Invalid, ex.what());
    }
}

void EquipmentProjection::swapState(EquipmentProjection& other) noexcept {
    static_assert(std::is_nothrow_swappable_v<decltype(_knownItems)>);
    static_assert(std::is_nothrow_swappable_v<decltype(_ownedModifiers)>);
    using std::swap;
    swap(_actor, other._actor);
    swap(_itemDomain, other._itemDomain);
    _knownItems.swap(other._knownItems);
    _ownedModifiers.swap(other._ownedModifiers);
    swap(_selected, other._selected);
}

Result<void> EquipmentProjection::sync(const Store& store, std::string_view actorId,
    std::string_view selectedItemId, stats::StatSet& statSet,
    ability::Runtime& runtime, const ModifierFactory& modifierFactory) {
    static_assert(std::is_nothrow_swappable_v<stats::StatSet>);
    if (actorId.empty() || !modifierFactory)
        return reject<void>(Error::Invalid, "Missing actor ID or modifier factory");
    try {
        const auto profiles = store.equippedProfiles(actorId);
        std::vector<stats::Modifier> nextModifiers;
        std::set<std::uint64_t> handles;
        ability::Identity desired{};
        for (const auto& profile : profiles) {
            if (profile.itemId == selectedItemId) {
                if (!profile.abilityConfiguration || !profile.abilityBindingId)
                    return reject<void>(Error::Incompatible, "Selected item has no Ability binding");
                desired = {_itemDomain, profile.abilityBindingId, 1};
            }
            for (std::size_t index = 0; index < profile.statBonuses.size(); ++index) {
                const auto modifier = modifierFactory(profile, index);
                const auto& bonus = profile.statBonuses[index];
                if (!modifier.handle || modifier.target != bonus.statId
                    || modifier.value != bonus.flat || !handles.insert(modifier.handle).second)
                    return reject<void>(Error::Invalid, "Invalid or duplicate equipment modifier");
                nextModifiers.push_back(modifier);
            }
        }
        if (!selectedItemId.empty() && !desired.valid())
            return reject<void>(Error::Missing, "Selected item is not equipped");

        auto nextStats = statSet;
        auto nextRuntime = runtime;
        auto nextKnown = _knownItems;
        for (const auto& profile : profiles) {
            if (!profile.abilityConfiguration) continue;
            if (!profile.abilityBindingId)
                return reject<void>(Error::Invalid, "Ability item has no binding ID");
            const ability::Identity id{_itemDomain, profile.abilityBindingId, 1};
            const auto existing = nextRuntime.equipment(id);
            if (existing) {
                if (existing->configuration != profile.abilityConfiguration)
                    return reject<void>(Error::Incompatible, "Ability configuration changed for an instance");
                if (existing->owner != _actor) {
                    if (!nextRuntime.transferEquipment(existing->owner, _actor, id))
                        return reject<void>(Error::Denied, "Ability transfer refused");
                }
            } else if (!nextRuntime.addEquipment(_actor, id, profile.abilityConfiguration)) {
                return reject<void>(Error::Denied, "Ability configuration refused");
            }
            nextKnown[profile.itemId] = id;
        }
        for (auto it = nextKnown.begin(); it != nextKnown.end();) {
            if (store.findItem(it->first)) { ++it; continue; }
            const auto existing = nextRuntime.equipment(it->second);
            if (existing && existing->owner == _actor
                && !nextRuntime.removeEquipment(_actor, it->second))
                return reject<void>(Error::Denied, "Ability removal refused");
            it = nextKnown.erase(it);
        }
        if (desired.valid()) {
            if (!nextRuntime.equip(_actor, desired))
                return reject<void>(Error::Denied, "Ability selection refused");
        } else if (_selected.valid() && !nextRuntime.unequip(_actor)) {
            return reject<void>(Error::Denied, "Ability deselection refused");
        }
        if (!nextStats.replaceOwnedModifiers(_ownedModifiers, nextModifiers))
            return reject<void>(Error::Incompatible, "Stats equipment modifier publication refused");
        if (!runtime.swapState(nextRuntime))
            return reject<void>(Error::Denied, "Ability runtime is committing");
        using std::swap;
        swap(statSet, nextStats);
        _knownItems.swap(nextKnown);
        _ownedModifiers.swap(nextModifiers);
        _selected = desired;
        return {};
    } catch (const std::exception& ex) {
        return reject<void>(Error::Invalid, ex.what());
    }
}

Result<void> transferAndSyncEquipment(Store& store,
    std::string_view itemId, const Location& source, const Location& destination,
    std::string requestId, ability::Runtime& runtime,
    std::span<const EquipmentSyncTarget> targets) {
    static_assert(std::is_nothrow_swappable_v<stats::StatSet>);
    if ((source.place != Place::Container && source.place != Place::Equipment)
        || (destination.place != Place::Container
            && destination.place != Place::Equipment)
        || targets.empty() || targets.size() > 32)
        return reject<void>(Error::Invalid,
            "Equipment transfer needs container/equipment locations and 1..32 actors");
    try {
        std::set<std::string_view> actorIds;
        std::set<ability::Identity> identities;
        std::set<EquipmentProjection*> projections;
        std::set<stats::StatSet*> statSets;
        for (const auto& target : targets) {
            if (!target.projection || !target.stats || target.actorId.empty()
                || !target.modifierFactory
                || !actorIds.insert(target.actorId).second
                || !identities.insert(target.projection->actorIdentity()).second
                || !projections.insert(target.projection).second
                || !statSets.insert(target.stats).second)
                return reject<void>(Error::Invalid,
                    "Duplicate or incomplete equipment projection target");
        }
        if ((source.place == Place::Equipment
                && !actorIds.contains(source.ownerId))
            || (destination.place == Place::Equipment
                && !actorIds.contains(destination.ownerId)))
            return reject<void>(Error::Invalid,
                "Affected equipment owner is missing from projection targets");

        Store stagedStore = store;
        if (auto moved = stagedStore.transfer(itemId, source, destination,
                                              std::move(requestId)); !moved)
            return moved;
        auto stagedRuntime = runtime;
        struct Staged {
            EquipmentProjection projection;
            stats::StatSet stats;
        };
        std::vector<Staged> staged;
        staged.reserve(targets.size());
        for (const auto& target : targets) {
            staged.push_back({*target.projection, *target.stats});
            if (auto synced = staged.back().projection.sync(stagedStore,
                    target.actorId, target.selectedItemId, staged.back().stats,
                    stagedRuntime, target.modifierFactory); !synced)
                return synced;
        }
        if (!runtime.swapState(stagedRuntime))
            return reject<void>(Error::Denied, "Ability runtime is committing");
        using std::swap;
        for (std::size_t i = 0; i < targets.size(); ++i) {
            swap(*targets[i].stats, staged[i].stats);
            targets[i].projection->swapState(staged[i].projection);
        }
        store.swap(stagedStore);
        return {};
    } catch (const std::exception& error) {
        return reject<void>(Error::Invalid, error.what());
    }
}
} // namespace ayt::inventory
