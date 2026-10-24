#include <AYInventory/EquipmentProjection.h>

#include <exception>
#include <set>
#include <type_traits>
#include <utility>

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
} // namespace ayt::inventory
