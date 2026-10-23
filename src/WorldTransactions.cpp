#include <AYInventory/WorldTransactions.h>

#include <cmath>
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
        if (auto valid = host.validateCurrent(candidate, actorId); !valid)
            return std::unexpected(valid.error());
        if (auto allowed = host.authorize(candidate, actorId); !allowed)
            return std::unexpected(allowed.error());
        if (auto consumed = host.consumeWorldPickup(candidate); !consumed)
            return std::unexpected(consumed.error());
        store.swap(staged);
        return result;
    }
    if (candidate.kind != InteractionCandidate::Kind::OpenContainer)
        return reject<Activation>(Error::Invalid, "unknown interaction kind");
    if (!store.hasContainer(candidate.subjectId))
        return reject<Activation>(Error::Missing, "container missing");
    if (auto valid = host.validateCurrent(candidate, actorId); !valid)
        return std::unexpected(valid.error());
    if (auto allowed = host.authorize(candidate, actorId); !allowed)
        return std::unexpected(allowed.error());
    return result;
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
        return host.verifyPublished(request, *item);
    if (auto prepared = host.prepare(request, *item); !prepared) {
        host.abort();
        return prepared;
    }
    if (auto published = host.publish(); !published) {
        host.abort();
        return published;
    }
    store.swap(staged);
    return {};
}

} // namespace ayt::inventory
