#include <AYInventory/Inventory.h>

#include <limits>
#include <algorithm>
#include <utility>

namespace ayt::inventory {
namespace {
template<class T> Result<T> reject(Error code, std::string message)
{
    return std::unexpected(Diagnostic{code, std::move(message)});
}

void appendField(std::string& out, std::string_view value)
{
    out += std::to_string(value.size());
    out += ':';
    out.append(value);
}
} // namespace

Result<Store> Store::create(std::uint32_t catalogVersion,
                            std::vector<Definition> definitions)
{
    if (catalogVersion == 0) return reject<Store>(Error::Invalid, "zero catalog version");
    Store store(catalogVersion);
    for (auto& definition : definitions) {
        if (definition.id.empty() || definition.maxStack == 0)
            return reject<Store>(Error::Invalid, "item definition needs ID and maxStack");
        for (const auto& slot : definition.equipSlots)
            if (slot.empty()) return reject<Store>(Error::Invalid, "empty equipment slot");
        for (const auto& bonus : definition.statBonuses)
            if (bonus.statId == 0) return reject<Store>(Error::Invalid, "zero stat ID");
        for (const auto& [slot, accepted] : definition.attachmentSlots)
            if (slot.empty() || accepted.empty() || accepted.contains(""))
                return reject<Store>(Error::Invalid, "invalid attachment slot or accepted definition");
        const auto id = definition.id;
        if (!store._definitions.emplace(id, std::move(definition)).second)
            return reject<Store>(Error::Duplicate, "duplicate item definition: " + id);
    }
    for (const auto& [_, definition] : store._definitions)
        for (const auto& [slot, accepted] : definition.attachmentSlots)
            for (const auto& childId : accepted)
                if (!store._definitions.contains(childId))
                    return reject<Store>(Error::Missing, "attachment slot '" + slot
                        + "' references an unknown definition: " + childId);
    return store;
}

Result<void> Store::addContainer(Container container)
{
    if (container.id.empty() || container.slots.empty()
        || container.slots.contains(""))
        return reject<void>(Error::Invalid, "invalid container ID or slots");
    const auto id = container.id;
    if (!_containers.emplace(id, std::move(container)).second)
        return reject<void>(Error::Duplicate, "duplicate container: " + id);
    return {};
}

Result<void> Store::addEquipmentOwner(EquipmentOwner actor)
{
    if (actor.id.empty() || actor.slots.empty() || actor.slots.contains(""))
        return reject<void>(Error::Invalid, "invalid equipment owner ID or slots");
    const auto id = actor.id;
    if (!_actors.emplace(id, std::move(actor)).second)
        return reject<void>(Error::Duplicate, "duplicate equipment owner: " + id);
    return {};
}

std::optional<std::string> Store::occupying(const Location& location) const
{
    for (const auto& [id, item] : _items)
        if (item.location == location) return id;
    return std::nullopt;
}

Result<void> Store::validateItem(const Item& item,
                                 std::optional<std::string_view> ignoreId) const
{
    if (item.id.empty() || item.definitionId.empty() || item.quantity == 0)
        return reject<void>(Error::Invalid, "item needs ID, definition and quantity");
    const auto definition = _definitions.find(item.definitionId);
    if (definition == _definitions.end())
        return reject<void>(Error::Missing, "unknown item definition");
    if (item.quantity > definition->second.maxStack)
        return reject<void>(Error::Capacity, "stack limit exceeded");
    if ((definition->second.abilityConfiguration == 0) !=
        (item.abilityBindingId == 0))
        return reject<void>(Error::Invalid, "Ability binding does not match definition");
    if (item.abilityBindingId != 0) {
        for (const auto& [id, existing] : _items)
            if (existing.abilityBindingId == item.abilityBindingId
                && (!ignoreId || id != *ignoreId))
                return reject<void>(Error::Duplicate, "Ability binding ID is already used");
    }
    const auto& loc = item.location;
    if (loc.ownerId.empty())
        return reject<void>(Error::Invalid, "owner needs stable ID");
    switch (loc.place) {
    case Place::World:
        if (!loc.slot.empty()) return reject<void>(Error::Invalid, "world drop has no slot");
        break;
    case Place::Container: {
        const auto container = _containers.find(loc.ownerId);
        if (container == _containers.end())
            return reject<void>(Error::Missing, "container is not registered");
        if (!container->second.slots.contains(loc.slot))
            return reject<void>(Error::Capacity, "container has no such slot");
        break;
    }
    case Place::Equipment: {
        const auto actor = _actors.find(loc.ownerId);
        if (actor == _actors.end())
            return reject<void>(Error::Missing, "equipment owner is not registered");
        if (!actor->second.slots.contains(loc.slot)
            || !definition->second.equipSlots.contains(loc.slot)
            || item.quantity != 1)
            return reject<void>(Error::Incompatible, "item does not fit equipment slot");
        break;
    }
    case Place::Attachment: {
        const auto parent = _items.find(loc.ownerId);
        if (parent == _items.end())
            return reject<void>(Error::Missing, "attachment parent item is missing");
        if (parent->second.quantity != 1)
            return reject<void>(Error::Incompatible, "attachment parent must be one instance");
        const auto& parentDefinition = _definitions.at(parent->second.definitionId);
        const auto slot = parentDefinition.attachmentSlots.find(loc.slot);
        if (slot == parentDefinition.attachmentSlots.end()
            || !slot->second.contains(item.definitionId) || item.quantity != 1)
            return reject<void>(Error::Incompatible, "part does not fit attachment slot");
        std::set<std::string> ancestors;
        auto current = loc.ownerId;
        while (true) {
            if (current == item.id || !ancestors.insert(current).second)
                return reject<void>(Error::Incompatible, "attachment cycle");
            const auto found = _items.find(current);
            if (found == _items.end()
                || found->second.location.place != Place::Attachment) break;
            current = found->second.location.ownerId;
        }
        break;
    }
    default:
        return reject<void>(Error::Invalid, "unknown owner kind");
    }
    const auto occupant = occupying(loc);
    if (occupant && (!ignoreId || *occupant != *ignoreId))
        return reject<void>(Error::Occupied, "destination already owns an item");
    return {};
}

Result<void> Store::createItem(Item item)
{
    if (_items.contains(item.id))
        return reject<void>(Error::Duplicate, "duplicate item instance ID");
    if (_retiredItemIds.contains(item.id))
        return reject<void>(Error::Stale, "retired item instance ID cannot be reused");
    const auto definition = _definitions.find(item.definitionId);
    if (definition == _definitions.end())
        return reject<void>(Error::Missing, "unknown item definition");
    std::uint64_t nextBinding = _nextAbilityBindingId;
    if (definition->second.abilityConfiguration != 0) {
        if (item.abilityBindingId == 0) item.abilityBindingId = nextBinding;
        if (item.abilityBindingId < nextBinding
            || item.abilityBindingId == std::numeric_limits<std::uint64_t>::max())
            return reject<void>(Error::Overflow, "Ability binding ID is retired or exhausted");
        nextBinding = item.abilityBindingId + 1;
    }
    if (auto valid = validateItem(item); !valid) return valid;
    const auto id = item.id;
    _items.emplace(id, std::move(item));
    _nextAbilityBindingId = nextBinding;
    return {};
}

std::string Store::transferFingerprint(std::string_view itemId,
                                      const Location& from,
                                      const Location& to) const
{
    std::string result;
    appendField(result, itemId);
    for (const auto& loc : {from, to}) {
        result += std::to_string(static_cast<int>(loc.place));
        result += ':';
        appendField(result, loc.ownerId);
        appendField(result, loc.slot);
    }
    return result;
}

Result<void> Store::transfer(std::string_view itemId,
                              const Location& expectedSource,
                              Location destination, std::string requestId)
{
    if (requestId.empty())
        return reject<void>(Error::Invalid, "transfer needs a request ID");
    const auto fingerprint = transferFingerprint(itemId, expectedSource, destination);
    if (const auto done = _completedTransfers.find(requestId);
        done != _completedTransfers.end()) {
        if (done->second == fingerprint) {
            const auto current = _items.find(std::string(itemId));
            if (current != _items.end() && current->second.location == destination)
                return {};
            return reject<void>(Error::Stale, "transfer was superseded after receipt");
        }
        return reject<void>(Error::Duplicate, "request ID reused for another transfer");
    }
    const auto found = _items.find(std::string(itemId));
    if (found == _items.end()) return reject<void>(Error::Missing, "item missing");
    if (isReserved(itemId))
        return reject<void>(Error::Occupied, "item has reserved crafting units");
    if (found->second.location != expectedSource)
        return reject<void>(Error::Stale, "item source changed");
    if (destination == expectedSource)
        return reject<void>(Error::Invalid, "transfer destination equals source");
    Item candidate = found->second;
    candidate.location = std::move(destination);
    if (auto valid = validateItem(candidate, itemId); !valid) return valid;
    // All domain checks precede the only owner write. Preserve the receipt
    // before publication so an allocation failure cannot publish a transfer.
    _completedTransfers.emplace(requestId, fingerprint);
    found->second.location = std::move(candidate.location);
    return {};
}

Result<void> Store::split(std::string_view itemId, std::uint32_t quantity,
                          std::string newItemId, Location destination)
{
    const auto source = _items.find(std::string(itemId));
    if (source == _items.end()) return reject<void>(Error::Missing, "source item missing");
    if (isReserved(itemId))
        return reject<void>(Error::Occupied, "item has reserved crafting units");
    if (newItemId.empty() || _items.contains(newItemId))
        return reject<void>(Error::Duplicate, "new instance ID is empty or used");
    if (quantity == 0 || quantity >= source->second.quantity)
        return reject<void>(Error::Invalid, "split quantity must leave source units");
    Item part{newItemId, source->second.definitionId, quantity, std::move(destination)};
    if (auto created = createItem(std::move(part)); !created) return created;
    source->second.quantity -= quantity;
    return {};
}

Result<void> Store::merge(std::string_view sourceId, std::string_view targetId,
                          std::uint32_t quantity)
{
    if (sourceId == targetId) return reject<void>(Error::Invalid, "self merge");
    const auto source = _items.find(std::string(sourceId));
    const auto target = _items.find(std::string(targetId));
    if (source == _items.end() || target == _items.end())
        return reject<void>(Error::Missing, "merge item missing");
    if (isReserved(sourceId) || isReserved(targetId))
        return reject<void>(Error::Occupied, "merge item has reserved crafting units");
    if (source->second.definitionId != target->second.definitionId)
        return reject<void>(Error::Incompatible, "merge definitions differ");
    if (quantity == 0 || quantity > source->second.quantity)
        return reject<void>(Error::Invalid, "invalid merge quantity");
    const auto maximum = _definitions.at(source->second.definitionId).maxStack;
    if (quantity > maximum - target->second.quantity)
        return reject<void>(Error::Capacity, "target stack full");
    if (source->second.location.place == Place::Equipment
        || target->second.location.place == Place::Equipment
        || source->second.location.place == Place::Attachment
        || target->second.location.place == Place::Attachment)
        return reject<void>(Error::Incompatible, "cannot merge equipped or attached items");
    for (const auto& [_, item] : _items)
        if (item.location.place == Place::Attachment
            && (item.location.ownerId == sourceId || item.location.ownerId == targetId))
            return reject<void>(Error::Occupied, "cannot merge a parent with attached parts");
    if (quantity == source->second.quantity)
        _retiredItemIds.insert(source->first);
    target->second.quantity += quantity;
    source->second.quantity -= quantity;
    if (source->second.quantity == 0) _items.erase(source);
    return {};
}

Result<void> Store::destroyItem(std::string_view itemId)
{
    if (isReserved(itemId))
        return reject<void>(Error::Occupied, "item has reserved crafting units");
    const auto found = _items.find(std::string(itemId));
    if (found == _items.end())
        return reject<void>(Error::Missing, "item missing");
    for (const auto& [_, item] : _items)
        if (item.location.place == Place::Attachment && item.location.ownerId == itemId)
            return reject<void>(Error::Occupied, "detach parts before destroying parent");
    _retiredItemIds.insert(found->first);
    _items.erase(found);
    return {};
}

bool Store::hasContainer(std::string_view id) const
{
    return _containers.contains(std::string(id));
}

bool Store::isReserved(std::string_view itemId) const
{
    const auto found = _reservedByItem.find(std::string(itemId));
    return found != _reservedByItem.end() && found->second != 0;
}

Result<void> Store::reserveCraft(const Recipe& recipe,
                                 std::string_view sourceContainerId,
                                 std::string requestId)
{
    if (requestId.empty() || recipe.id.empty() || recipe.inputs.empty()
        || recipe.outputDefinitionId.empty() || recipe.outputQuantity == 0)
        return reject<void>(Error::Invalid, "incomplete recipe or request ID");
    if (_completedCrafts.contains(requestId))
        return reject<void>(Error::Duplicate, "craft request already completed");
    if (const auto existing = _reservations.find(requestId);
        existing != _reservations.end()) {
        if (existing->second.recipe == recipe
            && existing->second.sourceContainerId == sourceContainerId) return {};
        return reject<void>(Error::Duplicate, "craft request ID reused");
    }
    if (!hasContainer(sourceContainerId))
        return reject<void>(Error::Missing, "material container missing");
    const auto output = findDefinition(recipe.outputDefinitionId);
    if (!output) return reject<void>(Error::Missing, "output definition missing");
    if (recipe.outputQuantity > output->maxStack)
        return reject<void>(Error::Capacity, "output exceeds stack limit");
    Reservation staged{recipe, std::string(sourceContainerId), {}};
    std::set<std::string> seenDefinitions;
    for (const auto& ingredient : recipe.inputs) {
        if (ingredient.definitionId.empty() || ingredient.quantity == 0
            || !seenDefinitions.insert(ingredient.definitionId).second)
            return reject<void>(Error::Invalid, "invalid or repeated recipe input");
        if (!findDefinition(ingredient.definitionId))
            return reject<void>(Error::Missing, "input definition missing");
        auto remaining = ingredient.quantity;
        for (const auto& [id, item] : _items) {
            if (item.location.place != Place::Container
                || item.location.ownerId != sourceContainerId
                || item.definitionId != ingredient.definitionId) continue;
            const auto reserved = _reservedByItem.contains(id) ? _reservedByItem.at(id) : 0;
            if (reserved > item.quantity)
                return reject<void>(Error::Invalid, "reservation state exceeds item quantity");
            const auto take = std::min(remaining, item.quantity - reserved);
            if (take != 0) staged.inputs.push_back({id, take});
            remaining -= take;
            if (remaining == 0) break;
        }
        if (remaining != 0)
            return reject<void>(Error::Insufficient, "recipe materials unavailable");
    }
    // Publish only after all material groups have been selected.
    const auto [entry, inserted] = _reservations.emplace(std::move(requestId),
                                                          std::move(staged));
    (void)inserted;
    for (const auto& input : entry->second.inputs)
        _reservedByItem[input.itemId] += input.quantity;
    return {};
}

Result<void> Store::cancelCraft(std::string_view requestId)
{
    const auto found = _reservations.find(std::string(requestId));
    if (found == _reservations.end())
        return reject<void>(Error::Missing, "craft reservation missing");
    for (const auto& input : found->second.inputs) {
        auto reserved = _reservedByItem.find(input.itemId);
        reserved->second -= input.quantity;
        if (reserved->second == 0) _reservedByItem.erase(reserved);
    }
    _reservations.erase(found);
    return {};
}

Result<void> Store::commitCraft(std::string_view requestId,
                                std::string outputItemId, Location destination)
{
    const CraftReceipt receipt{outputItemId, destination};
    if (const auto completed = _completedCrafts.find(std::string(requestId));
        completed != _completedCrafts.end()) {
        if (completed->second == receipt) return {};
        return reject<void>(Error::Duplicate, "craft request replay changed output");
    }
    const auto found = _reservations.find(std::string(requestId));
    if (found == _reservations.end())
        return reject<void>(Error::Missing, "craft reservation missing");
    if (outputItemId.empty()) return reject<void>(Error::Invalid, "output ID missing");
    Store candidate = *this;
    const auto reservation = candidate._reservations.at(std::string(requestId));
    (void)candidate.cancelCraft(requestId);
    for (const auto& input : reservation.inputs) {
        auto item = candidate._items.find(input.itemId);
        if (item == candidate._items.end() || item->second.quantity < input.quantity)
            return reject<void>(Error::Stale, "reserved material changed");
        item->second.quantity -= input.quantity;
        if (item->second.quantity == 0) {
            candidate._retiredItemIds.insert(item->first);
            candidate._items.erase(item);
        }
    }
    if (auto created = candidate.createItem({outputItemId,
            reservation.recipe.outputDefinitionId,
            reservation.recipe.outputQuantity, std::move(destination)}); !created)
        return created;
    candidate._completedCrafts.emplace(std::string(requestId), receipt);
    _items.swap(candidate._items);
    _retiredItemIds.swap(candidate._retiredItemIds);
    _reservations.swap(candidate._reservations);
    _reservedByItem.swap(candidate._reservedByItem);
    _completedCrafts.swap(candidate._completedCrafts);
    _nextAbilityBindingId = candidate._nextAbilityBindingId;
    return {};
}

Result<void> Store::removeContainer(std::string_view id)
{
    const auto found = _containers.find(std::string(id));
    if (found == _containers.end()) return reject<void>(Error::Missing, "container missing");
    for (const auto& [_, item] : _items)
        if (item.location.place == Place::Container && item.location.ownerId == id)
            return reject<void>(Error::Occupied, "container still owns items");
    _containers.erase(found);
    return {};
}

Result<void> Store::removeEquipmentOwner(std::string_view id)
{
    const auto found = _actors.find(std::string(id));
    if (found == _actors.end()) return reject<void>(Error::Missing, "actor missing");
    for (const auto& [_, item] : _items)
        if (item.location.place == Place::Equipment && item.location.ownerId == id)
            return reject<void>(Error::Occupied, "actor still owns equipment");
    _actors.erase(found);
    return {};
}

const Item* Store::findItem(std::string_view id) const
{
    const auto found = _items.find(std::string(id));
    return found == _items.end() ? nullptr : &found->second;
}

const CraftReceipt* Store::findCraftReceipt(std::string_view requestId) const
{
    const auto found = _completedCrafts.find(std::string(requestId));
    return found == _completedCrafts.end() ? nullptr : &found->second;
}

const Definition* Store::findDefinition(std::string_view id) const
{
    const auto found = _definitions.find(std::string(id));
    return found == _definitions.end() ? nullptr : &found->second;
}

void Store::swap(Store& other) noexcept
{
    using std::swap;
    swap(_catalogVersion, other._catalogVersion);
    swap(_nextAbilityBindingId, other._nextAbilityBindingId);
    _definitions.swap(other._definitions);
    _containers.swap(other._containers);
    _actors.swap(other._actors);
    _items.swap(other._items);
    _retiredItemIds.swap(other._retiredItemIds);
    _completedTransfers.swap(other._completedTransfers);
    _completedCrafts.swap(other._completedCrafts);
    _reservations.swap(other._reservations);
    _reservedByItem.swap(other._reservedByItem);
}

Snapshot Store::snapshot() const
{
    Snapshot result;
    result.catalogVersion = _catalogVersion;
    result.nextAbilityBindingId = _nextAbilityBindingId;
    for (const auto& [_, container] : _containers) result.containers.push_back(container);
    for (const auto& [_, actor] : _actors) result.actors.push_back(actor);
    for (const auto& [_, item] : _items) result.items.push_back(item);
    result.retiredItemIds = _retiredItemIds;
    result.completedTransfers = _completedTransfers;
    result.completedCrafts = _completedCrafts;
    return result;
}

Result<void> Store::restore(const Snapshot& saved)
{
    if ((saved.schemaVersion != 2 && saved.schemaVersion != 3
         && saved.schemaVersion != 4 && saved.schemaVersion != 5)
        || saved.catalogVersion != _catalogVersion)
        return reject<void>(Error::ContentVersion, "inventory snapshot version mismatch");
    if (saved.schemaVersion == 2 && !saved.completedCrafts.empty())
        return reject<void>(Error::Invalid, "v2 snapshot cannot contain craft receipts");
    if (saved.schemaVersion < 4 && !saved.retiredItemIds.empty())
        return reject<void>(Error::Invalid, "legacy snapshot cannot contain retired IDs");
    Store next(_catalogVersion);
    next._definitions = _definitions;
    for (const auto& container : saved.containers)
        if (auto result = next.addContainer(container); !result) return result;
    for (const auto& actor : saved.actors)
        if (auto result = next.addEquipmentOwner(actor); !result) return result;
    std::map<std::string, Item> pending;
    for (const auto& item : saved.items) {
        if (saved.schemaVersion < 5 && item.location.place == Place::Attachment)
            return reject<void>(Error::ContentVersion, "legacy snapshot cannot contain attachments");
        if (!pending.emplace(item.id, item).second)
            return reject<void>(Error::Duplicate, "duplicate item instance ID");
    }
    while (!pending.empty()) {
        auto eligible = pending.end();
        for (auto it = pending.begin(); it != pending.end(); ++it) {
            if (it->second.location.place == Place::Attachment
                && !next._items.contains(it->second.location.ownerId)) continue;
            if (eligible == pending.end()
                || it->second.abilityBindingId < eligible->second.abilityBindingId)
                eligible = it;
        }
        if (eligible == pending.end())
            return reject<void>(Error::Incompatible, "attachment parent missing or cyclic");
        if (auto result = next.createItem(eligible->second); !result) return result;
        pending.erase(eligible);
    }
    for (const auto& id : saved.retiredItemIds) {
        if (id.empty() || next._items.contains(id))
            return reject<void>(Error::Invalid, "retired item ID is empty or still active");
    }
    next._retiredItemIds = saved.retiredItemIds;
    if (saved.nextAbilityBindingId < next._nextAbilityBindingId
        || saved.nextAbilityBindingId == 0)
        return reject<void>(Error::Invalid, "Ability binding allocator watermark is stale");
    for (const auto& [request, fingerprint] : saved.completedTransfers) {
        if (request.empty() || fingerprint.empty())
            return reject<void>(Error::Invalid, "invalid transfer receipt");
    }
    next._completedTransfers = saved.completedTransfers;
    for (const auto& [request, receipt] : saved.completedCrafts) {
        if (request.empty() || receipt.outputItemId.empty()
            || receipt.destination.ownerId.empty()
            || receipt.destination.place > Place::Attachment
            || (receipt.destination.place == Place::World
                ? !receipt.destination.slot.empty()
                : receipt.destination.slot.empty()))
            return reject<void>(Error::Invalid, "invalid craft receipt");
    }
    next._completedCrafts = saved.completedCrafts;
    next._nextAbilityBindingId = saved.nextAbilityBindingId;
    _containers.swap(next._containers);
    _actors.swap(next._actors);
    _items.swap(next._items);
    _retiredItemIds.swap(next._retiredItemIds);
    _completedTransfers.swap(next._completedTransfers);
    _completedCrafts.swap(next._completedCrafts);
    _reservations.clear();
    _reservedByItem.clear();
    _nextAbilityBindingId = next._nextAbilityBindingId;
    return {};
}

std::vector<Store::EquippedProfile> Store::equippedProfiles(
    std::string_view actorId) const
{
    std::vector<EquippedProfile> result;
    for (const auto& [id, item] : _items) {
        const Item* current = &item;
        std::string slotPath = item.location.slot;
        while (current->location.place == Place::Attachment) {
            const auto parent = _items.find(current->location.ownerId);
            if (parent == _items.end()) { current = nullptr; break; }
            current = &parent->second;
            slotPath = current->location.slot + "/" + slotPath;
        }
        if (!current || current->location.place != Place::Equipment
            || current->location.ownerId != actorId) continue;
        const auto& definition = _definitions.at(item.definitionId);
        result.push_back({id, std::string(actorId), std::move(slotPath),
                          item.abilityBindingId,
                          definition.abilityConfiguration, definition.statBonuses});
    }
    return result;
}

EquipmentDelta equipmentDelta(const Store& before, const Store& after,
                               std::string_view actorId)
{
    EquipmentDelta result;
    const auto oldProfiles = before.equippedProfiles(actorId);
    const auto newProfiles = after.equippedProfiles(actorId);
    std::map<std::string, Store::EquippedProfile> oldById;
    std::map<std::string, Store::EquippedProfile> newById;
    for (const auto& profile : oldProfiles) oldById.emplace(profile.itemId, profile);
    for (const auto& profile : newProfiles) newById.emplace(profile.itemId, profile);
    for (const auto& [id, oldProfile] : oldById) {
        const auto fresh = newById.find(id);
        if (fresh == newById.end() || fresh->second != oldProfile)
            result.removals.push_back(oldProfile);
    }
    for (const auto& [id, freshProfile] : newById) {
        const auto old = oldById.find(id);
        if (old == oldById.end() || old->second != freshProfile)
            result.additions.push_back(freshProfile);
    }
    return result;
}

} // namespace ayt::inventory
