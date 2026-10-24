#include <AYInventory/WorldTransactions.h>

#include <cmath>
#include <exception>
#include <utility>

namespace ayt::inventory {
namespace {
template<class T> Result<T> reject(Error code, std::string message)
{
    return std::unexpected(Diagnostic{code, std::move(message)});
}

bool validPose(const PlacementPose& pose)
{
    for (const auto value : pose.position) if (!std::isfinite(value)) return false;
    for (const auto value : pose.rotation) if (!std::isfinite(value)) return false;
    for (const auto value : pose.halfExtents)
        if (!std::isfinite(value) || value <= 0.0) return false;
    double squareNorm = 0.0;
    for (const auto value : pose.rotation) squareNorm += value * value;
    return std::abs(squareNorm - 1.0) <= 0.001;
}

template<class F> Result<void> invokeHost(F&& callback, const char* stage)
{
    try {
        return callback();
    } catch (const std::exception& error) {
        return reject<void>(Error::Stale,
                            std::string(stage) + " threw: " + error.what());
    } catch (...) {
        return reject<void>(Error::Stale,
                            std::string(stage) + " threw an unknown exception");
    }
}

Result<void> publishAndSave(Store& store, Store& staged,
                            IDurablePlacementHost& host,
                            const PlacementRequest& request, const Item& item,
                            const PlacementSave& save)
{
    if (!save) return reject<void>(Error::Invalid, "missing placement save callback");
    if (auto prepared = invokeHost([&] { return host.prepare(request, item); },
                                   "World placement preparation"); !prepared) {
        host.abort();
        return prepared;
    }
    if (auto published = invokeHost([&] { return host.publish(); },
                                    "World placement publication"); !published) {
        host.abort();
        return published;
    }
    auto saved = invokeHost([&]() -> Result<void> {
        if (!save(staged, request))
            return reject<void>(Error::Stale, "placement save rejected candidate");
        return {};
    }, "Placement save");
    if (!saved) {
        if (!host.retractPublished(request))
            return reject<void>(Error::Stale,
                "placement save failed and published object could not be retracted");
        return saved;
    }
    store.swap(staged);
    host.commitPublished();
    return {};
}
} // namespace

std::optional<InteractionCandidate> chooseFocus(
    std::span<const InteractionCandidate> hits, double maximumDistance)
{
    if (!std::isfinite(maximumDistance) || maximumDistance <= 0.0)
        return std::nullopt;
    std::optional<InteractionCandidate> best;
    for (const auto& hit : hits) {
        if ((hit.kind != InteractionCandidate::Kind::Pickup
             && hit.kind != InteractionCandidate::Kind::OpenContainer)
            || hit.objectId.empty() || hit.subjectId.empty()
            || !std::isfinite(hit.distance) || hit.distance < 0.0
            || hit.distance > maximumDistance) continue;
        if (!best || hit.distance < best->distance
            || (hit.distance == best->distance
                && (hit.objectId < best->objectId
                    || (hit.objectId == best->objectId
                        && hit.subjectId < best->subjectId))))
            best = hit;
    }
    return best;
}

Result<Activation> activate(Store& store, IInteractionHost& host,
                            const InteractionCandidate& candidate,
                            std::string_view actorId,
                            std::optional<Location> pickupDestination,
                            std::string requestId)
{
    if (actorId.empty() || candidate.objectId.empty() || candidate.subjectId.empty()
        || !std::isfinite(candidate.distance) || candidate.distance < 0.0)
        return reject<Activation>(Error::Invalid, "invalid activation target or actor");
    const Activation result{candidate.kind, candidate.objectId, candidate.subjectId};
    if (candidate.kind == InteractionCandidate::Kind::Pickup) {
        if (!pickupDestination || pickupDestination->place != Place::Container)
            return reject<Activation>(Error::Invalid, "pickup needs a container slot");
        const auto* item = store.findItem(candidate.subjectId);
        if (!item) return reject<Activation>(Error::Missing, "pickup item missing");
        const Location source{Place::World, candidate.objectId, {}};
        Store staged = store;
        if (auto moved = staged.transfer(candidate.subjectId, source,
                                         *pickupDestination, std::move(requestId)); !moved)
            return std::unexpected(moved.error());
        // A persisted transfer receipt proves an exact retry. The original
        // world object may already have been removed, so no ray is required.
        if (item->location == *pickupDestination) return result;
        if (auto valid = invokeHost([&] {
                return host.validateCurrent(candidate, actorId);
            }, "Interaction validation"); !valid)
            return std::unexpected(valid.error());
        if (auto allowed = invokeHost([&] {
                return host.authorize(candidate, actorId);
            }, "Interaction authorization"); !allowed)
            return std::unexpected(allowed.error());
        if (auto consumed = invokeHost([&] {
                return host.consumeWorldPickup(candidate);
            }, "World pickup consumption"); !consumed)
            return std::unexpected(consumed.error());
        store.swap(staged);
        return result;
    }
    if (candidate.kind != InteractionCandidate::Kind::OpenContainer)
        return reject<Activation>(Error::Invalid, "unknown interaction kind");
    if (!store.hasContainer(candidate.subjectId))
        return reject<Activation>(Error::Missing, "container missing");
    if (auto valid = invokeHost([&] {
            return host.validateCurrent(candidate, actorId);
        }, "Interaction validation"); !valid)
        return std::unexpected(valid.error());
    if (auto allowed = invokeHost([&] {
            return host.authorize(candidate, actorId);
        }, "Interaction authorization"); !allowed)
        return std::unexpected(allowed.error());
    return result;
}

Result<DurablePickupResult> activatePickupDurable(
    Store& store, IDurablePickupHost& host,
    const InteractionCandidate& candidate, std::string_view actorId,
    const Location& destination, std::string requestId,
    const PickupSave& save)
{
    if (candidate.kind != InteractionCandidate::Kind::Pickup
        || actorId.empty() || candidate.objectId.empty()
        || candidate.subjectId.empty() || !std::isfinite(candidate.distance)
        || candidate.distance < 0.0 || destination.place != Place::Container
        || !save)
        return reject<DurablePickupResult>(Error::Invalid,
            "invalid durable pickup target, destination or save callback");
    const auto* item = store.findItem(candidate.subjectId);
    if (!item) return reject<DurablePickupResult>(Error::Missing, "pickup item missing");
    const Location source{Place::World, candidate.objectId, {}};
    Store staged = store;
    if (auto moved = staged.transfer(candidate.subjectId, source, destination,
                                     std::move(requestId)); !moved)
        return std::unexpected(moved.error());
    Activation activation{candidate.kind, candidate.objectId,
                          candidate.subjectId};
    const auto cleanup = [&]() noexcept {
        try { return host.cleanupCommittedPickup(candidate).has_value(); }
        catch (...) { return false; }
    };
    if (item->location == destination)
        return DurablePickupResult{std::move(activation), !cleanup()};
    if (auto valid = invokeHost([&] {
            return host.validateCurrent(candidate, actorId);
        }, "Interaction validation"); !valid)
        return std::unexpected(valid.error());
    if (auto allowed = invokeHost([&] {
            return host.authorize(candidate, actorId);
        }, "Interaction authorization"); !allowed)
        return std::unexpected(allowed.error());
    auto saved = invokeHost([&]() -> Result<void> {
        if (!save(staged, candidate))
            return reject<void>(Error::Stale, "pickup save rejected candidate");
        return {};
    }, "Pickup save");
    if (!saved) return std::unexpected(saved.error());
    // The durable candidate is authoritative now. Cleanup is best effort;
    // never report it as a transfer failure or leave the live Store behind.
    bool consumed = false;
    try { consumed = host.consumeWorldPickup(candidate).has_value(); }
    catch (...) {}
    store.swap(staged);
    return DurablePickupResult{std::move(activation), !consumed && !cleanup()};
}

Result<void> placeWorldItem(Store& store, IPlacementHost& host,
                            std::string_view itemId, const Location& source,
                            PlacementRequest request, std::string requestId)
{
    if (source.place != Place::Container || request.worldObjectId.empty()
        || !validPose(request.pose))
        return reject<void>(Error::Invalid, "invalid placement source or pose");
    const auto* item = store.findItem(itemId);
    if (!item) return reject<void>(Error::Missing, "placement item missing");
    const Location destination{Place::World, request.worldObjectId, {}};
    Store staged = store;
    if (auto moved = staged.transfer(itemId, source, destination, std::move(requestId));
        !moved) return moved;
    if (item->location == destination)
        return invokeHost([&] { return host.verifyPublished(request, *item); },
                          "Published object verification");
    if (auto prepared = invokeHost([&] {
            return host.prepare(request, *item);
        }, "World placement preparation"); !prepared) {
        host.abort();
        return prepared;
    }
    if (auto published = invokeHost([&] { return host.publish(); },
                                    "World placement publication"); !published) {
        host.abort();
        return published;
    }
    store.swap(staged);
    host.commitPublished();
    return {};
}

Result<void> placeWorldItemDurable(Store& store, IDurablePlacementHost& host,
    std::string_view itemId, const Location& source,
    PlacementRequest request, std::string requestId, const PlacementSave& save)
{
    if (source.place != Place::Container || request.worldObjectId.empty()
        || !validPose(request.pose))
        return reject<void>(Error::Invalid, "invalid placement source or pose");
    const auto* item = store.findItem(itemId);
    if (!item) return reject<void>(Error::Missing, "placement item missing");
    const Location destination{Place::World, request.worldObjectId, {}};
    Store staged = store;
    if (auto moved = staged.transfer(itemId, source, destination,
                                     std::move(requestId)); !moved) return moved;
    if (item->location == destination)
        return invokeHost([&] { return host.verifyPublished(request, *item); },
                          "Published object verification");
    return publishAndSave(store, staged, host, request, *item, save);
}

Result<void> craftAndPlaceWorldItem(Store& store, IPlacementHost& host,
                                    const Recipe& recipe,
                                    std::string_view sourceContainerId,
                                    std::string craftRequestId,
                                    std::string outputItemId,
                                    PlacementRequest request)
{
    if (craftRequestId.empty() || outputItemId.empty()
        || request.worldObjectId.empty() || !validPose(request.pose))
        return reject<void>(Error::Invalid, "invalid craft placement identity or pose");
    const Location destination{Place::World, request.worldObjectId, {}};
    if (const auto* receipt = store.findCraftReceipt(craftRequestId)) {
        if (*receipt != CraftReceipt{outputItemId, destination})
            return reject<void>(Error::Duplicate, "craft placement retry changed output");
        const auto* existing = store.findItem(outputItemId);
        if (!existing || existing->location != destination)
            return reject<void>(Error::Stale, "crafted world item has moved or vanished");
        return invokeHost([&] { return host.verifyPublished(request, *existing); },
                          "Crafted object verification");
    }

    Store staged = store;
    if (auto reserved = staged.reserveCraft(recipe, sourceContainerId,
                                            craftRequestId); !reserved)
        return reserved;
    if (auto crafted = staged.commitCraft(craftRequestId, outputItemId,
                                          destination); !crafted)
        return crafted;
    const auto* item = staged.findItem(outputItemId);
    if (auto prepared = invokeHost([&] { return host.prepare(request, *item); },
                                   "Craft placement preparation"); !prepared) {
        host.abort();
        return prepared;
    }
    if (auto published = invokeHost([&] { return host.publish(); },
                                    "Craft placement publication"); !published) {
        host.abort();
        return published;
    }
    store.swap(staged);
    host.commitPublished();
    return {};
}

Result<void> craftAndPlaceWorldItemDurable(Store& store,
    IDurablePlacementHost& host, const Recipe& recipe,
    std::string_view sourceContainerId, std::string craftRequestId,
    std::string outputItemId, PlacementRequest request,
    const PlacementSave& save)
{
    if (craftRequestId.empty() || outputItemId.empty()
        || request.worldObjectId.empty() || !validPose(request.pose))
        return reject<void>(Error::Invalid, "invalid craft placement identity or pose");
    const Location destination{Place::World, request.worldObjectId, {}};
    if (const auto* receipt = store.findCraftReceipt(craftRequestId)) {
        if (*receipt != CraftReceipt{outputItemId, destination})
            return reject<void>(Error::Duplicate, "craft placement retry changed output");
        const auto* existing = store.findItem(outputItemId);
        if (!existing || existing->location != destination)
            return reject<void>(Error::Stale, "crafted world item has moved or vanished");
        return invokeHost([&] { return host.verifyPublished(request, *existing); },
                          "Crafted object verification");
    }
    Store staged = store;
    if (auto reserved = staged.reserveCraft(recipe, sourceContainerId,
                                            craftRequestId); !reserved)
        return reserved;
    if (auto crafted = staged.commitCraft(craftRequestId, outputItemId,
                                          destination); !crafted)
        return crafted;
    return publishAndSave(store, staged, host, request,
                          *staged.findItem(outputItemId), save);
}

} // namespace ayt::inventory
