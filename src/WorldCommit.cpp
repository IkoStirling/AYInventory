#include <AYInventory/WorldCommit.h>
#include <AYInventory/SnapshotJson.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <exception>
#include <set>
#include <utility>

namespace ayt::inventory {
namespace {
using Json = nlohmann::json;

template<class T> Result<T> fail(Error code, std::string message)
{
    return std::unexpected(Diagnostic{code, std::move(message)});
}

template<class F> Result<void> callHost(F&& callback, const char* stage)
{
    try { return callback(); }
    catch (const std::exception& ex) {
        return fail<void>(Error::Stale, std::string(stage) + ": " + ex.what());
    } catch (...) {
        return fail<void>(Error::Stale, std::string(stage) + " threw");
    }
}

bool validPose(const PlacementPose& pose)
{
    double norm = 0.0;
    for (double value : pose.position) if (!std::isfinite(value)) return false;
    for (double value : pose.rotation) {
        if (!std::isfinite(value)) return false;
        norm += value * value;
    }
    for (double value : pose.halfExtents)
        if (!std::isfinite(value) || value <= 0.0) return false;
    return std::abs(norm - 1.0) <= 0.001;
}

std::array<double, 3> envelopeExtents(const PlacementPose& pose)
{
    const auto& q = pose.rotation;
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    const std::array<std::array<double, 3>, 3> rotation{{
        {1 - 2 * (y*y + z*z), 2 * (x*y - z*w), 2 * (x*z + y*w)},
        {2 * (x*y + z*w), 1 - 2 * (x*x + z*z), 2 * (y*z - x*w)},
        {2 * (x*z - y*w), 2 * (y*z + x*w), 1 - 2 * (x*x + y*y)}
    }};
    std::array<double, 3> result{};
    for (std::size_t axis = 0; axis < 3; ++axis)
        for (std::size_t component = 0; component < 3; ++component)
            result[axis] += std::abs(rotation[axis][component])
                * pose.halfExtents[component];
    return result;
}

bool overlappingEnvelopes(const PlacementPose& a, const PlacementPose& b)
{
    const auto ea = envelopeExtents(a), eb = envelopeExtents(b);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(ea[axis]) || !std::isfinite(eb[axis])) return true;
        if (std::abs(a.position[axis] - b.position[axis])
                >= ea[axis] + eb[axis]) return false;
    }
    return true;
}

std::string placementPayload(std::string_view itemId,
                             const PlacementPose& pose)
{
    return Json{{"format", "AliyatWorldItemProjection"}, {"version", 1},
                {"itemId", itemId}, {"position", pose.position},
                {"rotation", pose.rotation},
                {"halfExtents", pose.halfExtents}}.dump();
}

Result<std::pair<std::string, PlacementPose>> decodePlacement(
    std::string_view payload)
{
    try {
        const auto data = Json::parse(payload.begin(), payload.end());
        if (!data.is_object() || data.size() != 6
            || data.value("format", std::string{}) != "AliyatWorldItemProjection"
            || data.value("version", 0) != 1 || !data.contains("itemId")
            || !data["itemId"].is_string()
            || !data.contains("position") || !data.contains("rotation")
            || !data.contains("halfExtents"))
            return fail<std::pair<std::string, PlacementPose>>(
                Error::Invalid, "invalid saved placement record");
        auto itemId = data["itemId"].get<std::string>();
        PlacementPose pose;
        pose.position = data["position"].get<std::array<double, 3>>();
        pose.rotation = data["rotation"].get<std::array<double, 4>>();
        pose.halfExtents = data["halfExtents"].get<std::array<double, 3>>();
        if (itemId.empty() || !validPose(pose))
            return fail<std::pair<std::string, PlacementPose>>(
                Error::Invalid, "invalid saved placement identity or pose");
        return std::pair{std::move(itemId), pose};
    } catch (const std::exception&) {
        return fail<std::pair<std::string, PlacementPose>>(
            Error::Invalid, "malformed saved placement record");
    }
}

bool registerCaptureSections(app::SaveParticipantBundle& bundle,
                             const Store& store,
                             const app::WorldStateLedger& ledger,
                             std::string& error)
{
    app::SaveParticipantBundle::Participant inventory;
    inventory.version = 1;
    inventory.capture = [&store](std::string& payload, std::string& detail) {
        auto encoded = encodeSnapshotJson(store.snapshotWithReservations());
        if (!encoded) { detail = encoded.error().message; return false; }
        payload = std::move(*encoded);
        return true;
    };
    inventory.validate = [](std::uint32_t, std::string_view, std::string&) {
        return true;
    };
    inventory.apply = [](std::uint32_t, std::string_view, std::string&) {
        return true;
    };
    if (!bundle.registerParticipant("inventory", std::move(inventory), &error))
        return false;
    app::SaveParticipantBundle::Participant world;
    world.version = 1;
    world.capture = [&ledger](std::string& payload, std::string& detail) {
        return ledger.encode(payload, &detail);
    };
    world.validate = [](std::uint32_t, std::string_view, std::string&) {
        return true;
    };
    world.apply = [](std::uint32_t, std::string_view, std::string&) {
        return true;
    };
    return bundle.registerParticipant("world", std::move(world), &error);
}

bool registerRestoreSections(app::SaveParticipantBundle& bundle,
                             Store& store, app::WorldStateLedger& ledger,
                             std::string& error)
{
    app::SaveParticipantBundle::Participant inventory;
    inventory.version = 1;
    inventory.capture = [&store](std::string& payload, std::string& detail) {
        auto encoded = encodeSnapshotJson(store.snapshotWithReservations());
        if (!encoded) { detail = encoded.error().message; return false; }
        payload = std::move(*encoded);
        return true;
    };
    inventory.validate = [&store](std::uint32_t version,
                                  std::string_view payload,
                                  std::string& detail) {
        if (version != 1) { detail = "unsupported inventory section"; return false; }
        auto decoded = decodeSnapshotJson(payload);
        if (!decoded) { detail = decoded.error().message; return false; }
        auto staged = store;
        auto result = staged.restore(*decoded);
        if (!result) { detail = result.error().message; return false; }
        return true;
    };
    inventory.apply = [&store](std::uint32_t, std::string_view payload,
                               std::string& detail) {
        auto decoded = decodeSnapshotJson(payload);
        if (!decoded) { detail = decoded.error().message; return false; }
        auto result = store.restore(*decoded);
        if (!result) { detail = result.error().message; return false; }
        return true;
    };
    if (!bundle.registerParticipant("inventory", std::move(inventory), &error))
        return false;
    app::SaveParticipantBundle::Participant world;
    world.version = 1;
    world.capture = [&ledger](std::string& payload, std::string& detail) {
        return ledger.encode(payload, &detail);
    };
    world.validate = [](std::uint32_t version, std::string_view payload,
                        std::string& detail) {
        if (version != 1) { detail = "unsupported world section"; return false; }
        app::WorldStateLedger staged;
        return staged.decode(payload, &detail);
    };
    world.apply = [&ledger](std::uint32_t, std::string_view payload,
                            std::string& detail) {
        return ledger.decode(payload, &detail);
    };
    return bundle.registerParticipant("world", std::move(world), &error);
}

Result<void> receiptError(app::WorldStateLedger::ReceiptClaimResult claim)
{
    using Claim = app::WorldStateLedger::ReceiptClaimResult;
    if (claim == Claim::Invalid)
        return fail<void>(Error::Invalid, "invalid request ID or fingerprint");
    if (claim == Claim::Conflict)
        return fail<void>(Error::Duplicate, "request ID has another operation fingerprint");
    if (claim == Claim::AllocationFailure)
        return fail<void>(Error::Stale, "cannot allocate durable receipt");
    return {};
}

bool hasExactBatchTransferSet(const Store& store,
                              std::string_view requestId,
                              std::size_t count)
{
    const auto receipts = store.snapshot().completedTransfers;
    const std::string prefix = std::string(requestId) + "/item/";
    std::size_t found = 0;
    for (const auto& [key, fingerprint] : receipts) {
        (void)fingerprint;
        if (key.starts_with(prefix)) ++found;
    }
    if (found != count) return false;
    for (std::size_t i = 0; i < count; ++i)
        if (!receipts.contains(prefix + std::to_string(i))) return false;
    return true;
}
} // namespace

WorldItemCommit::WorldItemCommit(Store& store, app::WorldStateLedger& ledger,
    const app::SaveGameService& saves, std::string slot)
    : _store(store), _ledger(ledger), _saves(saves), _slot(std::move(slot)) {}

Result<void> WorldItemCommit::saveCandidate(const Store& candidateStore,
    const app::WorldStateLedger& candidateLedger,
    const WorldCommitExtension& extension)
{
    if (_slot.empty()) return fail<void>(Error::Invalid, "missing save slot");
    app::SaveParticipantBundle bundle;
    std::string error;
    try {
        if (extension.prepareCandidate
            && !extension.prepareCandidate(candidateStore, candidateLedger,
                                           error))
            return fail<void>(Error::Stale, "candidate preparation: " + error);
        if (!registerCaptureSections(bundle, candidateStore, candidateLedger, error)
            || (extension.registerCandidateSections
                && !extension.registerCandidateSections(bundle, error)))
            return fail<void>(Error::Invalid, "save section: " + error);
        std::string payload;
        if (!bundle.capture(payload, &error))
            return fail<void>(Error::Stale, "candidate capture: " + error);
        const auto previous = _saves.load(_slot, 1);
        if (previous.status != app::SaveGameStatus::NotFound) {
            if (!previous)
                return fail<void>(Error::Stale,
                    "cannot inspect existing slot: " + previous.error);
            const auto oldRoot = Json::parse(previous.payloadJson.begin(),
                                             previous.payloadJson.end(), nullptr, false);
            const auto newRoot = Json::parse(payload.begin(), payload.end(),
                                             nullptr, false);
            if (!oldRoot.is_object() || !oldRoot.contains("sections")
                || !oldRoot["sections"].is_object()
                || !newRoot.is_object() || !newRoot.contains("sections")
                || !newRoot["sections"].is_object())
                return fail<void>(Error::Stale,
                    "existing slot needs an explicit bundle migration");
            const auto& oldSections = oldRoot["sections"];
            const auto& newSections = newRoot["sections"];
            if (oldSections.size() != newSections.size())
                return fail<void>(Error::Stale,
                    "candidate would drop or add a save section");
            for (const auto& [name, section] : oldSections.items()) {
                (void)section;
                if (!newSections.contains(name))
                    return fail<void>(Error::Stale,
                        "candidate would drop save section: " + name);
            }
        }
        const auto saved = _saves.save(_slot, 1, payload);
        if (!saved) return fail<void>(Error::Stale, "slot save: " + saved.error);
        return {};
    } catch (const std::exception& ex) {
        return fail<void>(Error::Stale, std::string("candidate save threw: ") + ex.what());
    } catch (...) {
        return fail<void>(Error::Stale, "candidate save threw");
    }
}

bool WorldItemCommit::publishCandidate(Store& candidateStore,
    app::WorldStateLedger& candidateLedger,
    const WorldCommitExtension& extension) noexcept
{
    _store.swap(candidateStore);
    _ledger.swap(candidateLedger);
    if (!extension.publishCandidate) return true;
    try {
        std::string error;
        if (extension.publishCandidate(error)) return true;
    } catch (...) {}
    _requiresReload = true;
    return false;
}

Result<WorldCommitResult> WorldItemCommit::pickup(
    IDurablePickupHost& host, const InteractionCandidate& candidate,
    std::string_view actorId, const Location& destination,
    std::string requestId, std::string fingerprint,
    const WorldCommitExtension& extension)
{
    if (_requiresReload)
        return fail<WorldCommitResult>(Error::Stale, "reload committed slot first");
    if (candidate.kind != InteractionCandidate::Kind::Pickup
        || candidate.objectId.empty() || candidate.subjectId.empty()
        || actorId.empty()
        || (destination.place != Place::Container
            && destination.place != Place::Equipment)
        || !std::isfinite(candidate.distance) || candidate.distance < 0.0)
        return fail<WorldCommitResult>(Error::Invalid, "invalid pickup identity");
    if (destination.place == Place::Equipment
        && (!extension.prepareCandidate
            || !extension.registerCandidateSections
            || !extension.publishCandidate))
        return fail<WorldCommitResult>(Error::Invalid,
            "direct equipment pickup needs a staged durable projection");
    auto nextLedger = _ledger;
    const auto claim = nextLedger.claimReceiptWithFingerprint(
        requestId, std::move(fingerprint));
    if (auto result = receiptError(claim); !result)
        return std::unexpected(result.error());
    using Claim = app::WorldStateLedger::ReceiptClaimResult;
    if (claim == Claim::Replay) {
        const auto record = _ledger.records().find(candidate.objectId);
        if (record != _ledger.records().end() && record->second.deleted) {
            auto cleaned = callHost([&] { return host.cleanupCommittedPickup(candidate); },
                                    "pickup replay cleanup");
            return WorldCommitResult{cleaned ? WorldCommitStatus::AlreadyCommitted
                                              : WorldCommitStatus::ProjectionPending,
                                     candidate.objectId};
        }
        return WorldCommitResult{WorldCommitStatus::AlreadyCommitted,
                                 candidate.objectId};
    }
    Store nextStore = _store;
    const Location source{Place::World, candidate.objectId, {}};
    if (auto moved = nextStore.transfer(candidate.subjectId, source,
                                        destination, requestId); !moved)
        return std::unexpected(moved.error());
    if (auto valid = callHost([&] { return host.validateCurrent(candidate, actorId); },
                              "pickup validation"); !valid)
        return std::unexpected(valid.error());
    if (auto authorized = callHost([&] { return host.authorize(candidate, actorId); },
                                   "pickup authorization"); !authorized)
        return std::unexpected(authorized.error());
    std::string error;
    if (!nextLedger.markDeleted(candidate.objectId, &error))
        return fail<WorldCommitResult>(Error::Invalid, error);
    if (auto saved = saveCandidate(nextStore, nextLedger, extension); !saved)
        return std::unexpected(saved.error());
    if (!publishCandidate(nextStore, nextLedger, extension))
        return WorldCommitResult{WorldCommitStatus::ReloadRequired,
                                 candidate.objectId};
    auto cleaned = callHost([&] { return host.cleanupCommittedPickup(candidate); },
                            "pickup cleanup");
    return WorldCommitResult{cleaned ? WorldCommitStatus::Committed
                                      : WorldCommitStatus::ProjectionPending,
                             candidate.objectId};
}

Result<WorldBatchCommitResult> WorldItemCommit::pickupBatch(
    IDurablePickupHost& host, std::span<const WorldBatchPickup> pickups,
    std::string_view actorId, std::string requestId,
    std::string fingerprint, const WorldCommitExtension& extension)
{
    if (_requiresReload)
        return fail<WorldBatchCommitResult>(Error::Stale,
            "reload committed slot first");
    if (pickups.empty() || pickups.size() > 64 || actorId.empty())
        return fail<WorldBatchCommitResult>(Error::Invalid,
            "batch pickup needs one actor and 1..64 objects");
    std::set<std::string> objectIds, itemIds;
    bool equipment = false;
    for (const auto& pickup : pickups) {
        const auto& candidate = pickup.candidate;
        if (candidate.kind != InteractionCandidate::Kind::Pickup
            || candidate.objectId.empty() || candidate.subjectId.empty()
            || (pickup.destination.place != Place::Container
                && pickup.destination.place != Place::Equipment)
            || !std::isfinite(candidate.distance) || candidate.distance < 0.0)
            return fail<WorldBatchCommitResult>(Error::Invalid,
                "invalid batch pickup identity");
        if (!objectIds.insert(candidate.objectId).second
            || !itemIds.insert(candidate.subjectId).second)
            return fail<WorldBatchCommitResult>(Error::Duplicate,
                "batch repeats a World object or item");
        equipment |= pickup.destination.place == Place::Equipment;
    }
    if (equipment && (!extension.prepareCandidate
        || !extension.registerCandidateSections
        || !extension.publishCandidate))
        return fail<WorldBatchCommitResult>(Error::Invalid,
            "batch equipment pickup needs a staged durable projection");
    auto nextLedger = _ledger;
    const auto claim = nextLedger.claimReceiptWithFingerprint(
        requestId, std::move(fingerprint));
    if (auto result = receiptError(claim); !result)
        return std::unexpected(result.error());
    using Claim = app::WorldStateLedger::ReceiptClaimResult;
    if (claim == Claim::Replay) {
        if (!hasExactBatchTransferSet(_store, requestId, pickups.size()))
            return fail<WorldBatchCommitResult>(Error::Stale,
                "batch pickup replay member count differs");
        for (const auto& pickup : pickups) {
            const auto& candidate = pickup.candidate;
            const auto record = _ledger.records().find(candidate.objectId);
            const auto* item = _store.findItem(candidate.subjectId);
            if (record == _ledger.records().end() || !record->second.deleted
                || !item || item->location != pickup.destination)
                return fail<WorldBatchCommitResult>(Error::Stale,
                    "batch pickup replay differs from committed ownership");
        }
        WorldBatchCommitResult replay{WorldCommitStatus::AlreadyCommitted, {}};
        for (const auto& pickup : pickups) {
            const auto cleaned = callHost([&] {
                return host.cleanupCommittedPickup(pickup.candidate);
            }, "batch pickup replay cleanup");
            if (!cleaned)
                replay.pendingObjectIds.push_back(pickup.candidate.objectId);
        }
        if (!replay.pendingObjectIds.empty())
            replay.status = WorldCommitStatus::ProjectionPending;
        return replay;
    }
    Store nextStore = _store;
    for (std::size_t i = 0; i < pickups.size(); ++i) {
        const auto& pickup = pickups[i];
        const auto& candidate = pickup.candidate;
        if (auto moved = nextStore.transfer(candidate.subjectId,
                {Place::World, candidate.objectId, {}},
                pickup.destination,
                requestId + "/item/" + std::to_string(i)); !moved)
            return std::unexpected(moved.error());
        if (auto valid = callHost([&] {
                return host.validateCurrent(candidate, actorId);
            }, "batch pickup validation"); !valid)
            return std::unexpected(valid.error());
        if (auto authorized = callHost([&] {
                return host.authorize(candidate, actorId);
            }, "batch pickup authorization"); !authorized)
            return std::unexpected(authorized.error());
        std::string error;
        if (!nextLedger.markDeleted(candidate.objectId, &error))
            return fail<WorldBatchCommitResult>(Error::Invalid, error);
    }
    if (auto saved = saveCandidate(nextStore, nextLedger, extension); !saved)
        return std::unexpected(saved.error());
    if (!publishCandidate(nextStore, nextLedger, extension)) {
        WorldBatchCommitResult reload{WorldCommitStatus::ReloadRequired, {}};
        for (const auto& pickup : pickups)
            reload.pendingObjectIds.push_back(pickup.candidate.objectId);
        return reload;
    }
    WorldBatchCommitResult committed{WorldCommitStatus::Committed, {}};
    for (const auto& pickup : pickups) {
        const auto cleaned = callHost([&] {
            return host.cleanupCommittedPickup(pickup.candidate);
        }, "batch pickup cleanup");
        if (!cleaned)
            committed.pendingObjectIds.push_back(pickup.candidate.objectId);
    }
    if (!committed.pendingObjectIds.empty())
        committed.status = WorldCommitStatus::ProjectionPending;
    return committed;
}

Result<WorldCommitResult> WorldItemCommit::place(
    IPlacementHost& host, std::string_view itemId, const Location& source,
    PlacementRequest request, std::string requestId,
    std::string fingerprint, std::string contentSignature,
    const WorldCommitExtension& extension)
{
    if (_requiresReload)
        return fail<WorldCommitResult>(Error::Stale, "reload committed slot first");
    if (itemId.empty() || source.place != Place::Container
        || request.worldObjectId.empty() || !validPose(request.pose))
        return fail<WorldCommitResult>(Error::Invalid, "invalid placement identity or pose");
    auto nextLedger = _ledger;
    const auto claim = nextLedger.claimReceiptWithFingerprint(
        requestId, std::move(fingerprint));
    if (auto result = receiptError(claim); !result)
        return std::unexpected(result.error());
    using Claim = app::WorldStateLedger::ReceiptClaimResult;
    if (claim == Claim::Replay) {
        const auto record = _ledger.records().find(request.worldObjectId);
        const auto* owned = _store.findItem(itemId);
        if (record != _ledger.records().end() && !record->second.deleted
            && owned && owned->location
                == Location{Place::World, request.worldObjectId, {}}) {
            auto reconciled = reconcilePlaced(host, request.worldObjectId);
            return WorldCommitResult{reconciled
                ? WorldCommitStatus::AlreadyCommitted
                : WorldCommitStatus::ProjectionPending,
                request.worldObjectId};
        }
        return WorldCommitResult{WorldCommitStatus::AlreadyCommitted,
                                 request.worldObjectId};
    }
    if (_ledger.records().contains(request.worldObjectId))
        return fail<WorldCommitResult>(Error::Duplicate,
            "placement would reuse a saved world object ID");
    Store nextStore = _store;
    const Location destination{Place::World, request.worldObjectId, {}};
    if (auto moved = nextStore.transfer(itemId, source, destination,
                                        requestId); !moved)
        return std::unexpected(moved.error());
    const auto* stagedItem = nextStore.findItem(itemId);
    if (!stagedItem)
        return fail<WorldCommitResult>(Error::Missing, "staged placement item missing");
    std::string error;
    if (!nextLedger.putRecord(request.worldObjectId,
            {1, placementPayload(itemId, request.pose), false,
             std::move(contentSignature)}, &error))
        return fail<WorldCommitResult>(Error::Invalid, error);
    if (auto prepared = callHost([&] { return host.prepare(request, *stagedItem); },
                                 "placement preparation"); !prepared) {
        host.abort();
        return std::unexpected(prepared.error());
    }
    if (auto saved = saveCandidate(nextStore, nextLedger, extension); !saved) {
        host.abort();
        return std::unexpected(saved.error());
    }
    if (!publishCandidate(nextStore, nextLedger, extension)) {
        host.abort();
        return WorldCommitResult{WorldCommitStatus::ReloadRequired,
                                 request.worldObjectId};
    }
    auto published = callHost([&] { return host.publish(); },
                              "placement publication");
    if (!published) {
        host.abort();
        return WorldCommitResult{WorldCommitStatus::ProjectionPending,
                                 request.worldObjectId};
    }
    host.commitPublished();
    return WorldCommitResult{WorldCommitStatus::Committed,
                             request.worldObjectId};
}

Result<WorldBatchCommitResult> WorldItemCommit::placeBatch(
    IPlacementHost& host, std::span<const WorldBatchPlacement> placements,
    std::string requestId, std::string fingerprint,
    const WorldCommitExtension& extension)
{
    if (_requiresReload)
        return fail<WorldBatchCommitResult>(Error::Stale,
            "reload committed slot first");
    if (placements.empty() || placements.size() > 64)
        return fail<WorldBatchCommitResult>(Error::Invalid,
            "batch placement requires 1..64 objects");
    std::set<std::string> itemIds, objectIds;
    for (std::size_t i = 0; i < placements.size(); ++i) {
        const auto& placement = placements[i];
        if (placement.itemId.empty()
            || placement.source.place != Place::Container
            || placement.request.worldObjectId.empty()
            || !validPose(placement.request.pose))
            return fail<WorldBatchCommitResult>(Error::Invalid,
                "invalid batch placement identity or pose");
        if (!itemIds.insert(placement.itemId).second
            || !objectIds.insert(placement.request.worldObjectId).second)
            return fail<WorldBatchCommitResult>(Error::Duplicate,
                "batch repeats an item or World object ID");
        for (std::size_t j = 0; j < i; ++j)
            if (overlappingEnvelopes(placement.request.pose,
                                     placements[j].request.pose))
                return fail<WorldBatchCommitResult>(Error::Occupied,
                    "batch placement envelopes overlap");
    }
    auto nextLedger = _ledger;
    const auto claim = nextLedger.claimReceiptWithFingerprint(
        requestId, std::move(fingerprint));
    if (auto result = receiptError(claim); !result)
        return std::unexpected(result.error());
    using Claim = app::WorldStateLedger::ReceiptClaimResult;
    if (claim == Claim::Replay) {
        if (!hasExactBatchTransferSet(_store, requestId,
                                      placements.size()))
            return fail<WorldBatchCommitResult>(Error::Stale,
                "batch placement replay member count differs");
        for (const auto& placement : placements) {
            const auto record = _ledger.records().find(
                placement.request.worldObjectId);
            const auto* item = _store.findItem(placement.itemId);
            if (record == _ledger.records().end() || record->second.deleted
                || record->second.payloadJson != placementPayload(
                    placement.itemId, placement.request.pose)
                || !item || item->location != Location{Place::World,
                    placement.request.worldObjectId, {}})
                return fail<WorldBatchCommitResult>(Error::Stale,
                    "batch replay differs from the committed objects");
        }
        WorldBatchCommitResult replay{WorldCommitStatus::AlreadyCommitted, {}};
        for (const auto& placement : placements)
            if (!reconcilePlaced(host, placement.request.worldObjectId))
                replay.pendingObjectIds.push_back(
                    placement.request.worldObjectId);
        if (!replay.pendingObjectIds.empty())
            replay.status = WorldCommitStatus::ProjectionPending;
        return replay;
    }

    Store nextStore = _store;
    for (std::size_t i = 0; i < placements.size(); ++i) {
        const auto& placement = placements[i];
        const auto& objectId = placement.request.worldObjectId;
        if (_ledger.records().contains(objectId))
            return fail<WorldBatchCommitResult>(Error::Duplicate,
                "batch would reuse a saved World object ID");
        if (auto moved = nextStore.transfer(placement.itemId,
                placement.source, {Place::World, objectId, {}},
                requestId + "/item/" + std::to_string(i)); !moved)
            return std::unexpected(moved.error());
        std::string error;
        if (!nextLedger.putRecord(objectId,
                {1, placementPayload(placement.itemId,
                                     placement.request.pose), false,
                 placement.contentSignature}, &error))
            return fail<WorldBatchCommitResult>(Error::Invalid, error);
    }
    // Check the current World before saving. The conservative envelope check
    // above also catches collisions among the not-yet-visible batch members.
    for (const auto& placement : placements) {
        const auto* item = nextStore.findItem(placement.itemId);
        auto prepared = callHost([&] {
            return host.prepare(placement.request, *item);
        }, "batch placement preparation");
        host.abort();
        if (!prepared) return std::unexpected(prepared.error());
    }
    if (auto saved = saveCandidate(nextStore, nextLedger, extension); !saved)
        return std::unexpected(saved.error());
    if (!publishCandidate(nextStore, nextLedger, extension)) {
        WorldBatchCommitResult reload{WorldCommitStatus::ReloadRequired, {}};
        for (const auto& placement : placements)
            reload.pendingObjectIds.push_back(
                placement.request.worldObjectId);
        return reload;
    }
    WorldBatchCommitResult committed{WorldCommitStatus::Committed, {}};
    for (const auto& placement : placements)
        if (!reconcilePlaced(host, placement.request.worldObjectId))
            committed.pendingObjectIds.push_back(
                placement.request.worldObjectId);
    if (!committed.pendingObjectIds.empty())
        committed.status = WorldCommitStatus::ProjectionPending;
    return committed;
}

Result<WorldCommitResult> WorldItemCommit::craftAndPlace(
    IPlacementHost& host, const Recipe& recipe,
    std::string_view sourceContainerId, std::string craftRequestId,
    std::string outputItemId, PlacementRequest request,
    std::string fingerprint, std::string contentSignature,
    const WorldCommitExtension& extension)
{
    if (_requiresReload)
        return fail<WorldCommitResult>(Error::Stale, "reload committed slot first");
    if (sourceContainerId.empty() || craftRequestId.empty()
        || outputItemId.empty() || request.worldObjectId.empty()
        || !validPose(request.pose))
        return fail<WorldCommitResult>(Error::Invalid,
            "invalid craft placement identity or pose");
    auto nextLedger = _ledger;
    const auto claim = nextLedger.claimReceiptWithFingerprint(
        craftRequestId, std::move(fingerprint));
    if (auto result = receiptError(claim); !result)
        return std::unexpected(result.error());
    using Claim = app::WorldStateLedger::ReceiptClaimResult;
    if (claim == Claim::Replay) {
        const auto record = _ledger.records().find(request.worldObjectId);
        const auto* owned = _store.findItem(outputItemId);
        if (record != _ledger.records().end() && !record->second.deleted
            && owned && owned->location
                == Location{Place::World, request.worldObjectId, {}}) {
            auto reconciled = reconcilePlaced(host, request.worldObjectId);
            return WorldCommitResult{reconciled
                ? WorldCommitStatus::AlreadyCommitted
                : WorldCommitStatus::ProjectionPending,
                request.worldObjectId};
        }
        return WorldCommitResult{WorldCommitStatus::AlreadyCommitted,
                                 request.worldObjectId};
    }
    if (_ledger.records().contains(request.worldObjectId))
        return fail<WorldCommitResult>(Error::Duplicate,
            "craft would reuse a saved world object ID");
    Store nextStore = _store;
    if (auto reserved = nextStore.reserveCraft(recipe, sourceContainerId,
                                               craftRequestId); !reserved)
        return std::unexpected(reserved.error());
    const Location destination{Place::World, request.worldObjectId, {}};
    if (auto crafted = nextStore.commitCraft(craftRequestId, outputItemId,
                                             destination); !crafted)
        return std::unexpected(crafted.error());
    const auto* stagedItem = nextStore.findItem(outputItemId);
    if (!stagedItem)
        return fail<WorldCommitResult>(Error::Missing, "crafted output missing");
    std::string error;
    if (!nextLedger.putRecord(request.worldObjectId,
            {1, placementPayload(outputItemId, request.pose), false,
             std::move(contentSignature)}, &error))
        return fail<WorldCommitResult>(Error::Invalid, error);
    if (auto prepared = callHost([&] { return host.prepare(request, *stagedItem); },
                                 "craft placement preparation"); !prepared) {
        host.abort();
        return std::unexpected(prepared.error());
    }
    if (auto saved = saveCandidate(nextStore, nextLedger, extension); !saved) {
        host.abort();
        return std::unexpected(saved.error());
    }
    if (!publishCandidate(nextStore, nextLedger, extension)) {
        host.abort();
        return WorldCommitResult{WorldCommitStatus::ReloadRequired,
                                 request.worldObjectId};
    }
    auto published = callHost([&] { return host.publish(); },
                              "craft placement publication");
    if (!published) {
        host.abort();
        return WorldCommitResult{WorldCommitStatus::ProjectionPending,
                                 request.worldObjectId};
    }
    host.commitPublished();
    return WorldCommitResult{WorldCommitStatus::Committed,
                             request.worldObjectId};
}

Result<void> WorldItemCommit::reconcileDeleted(
    IDurablePickupHost& host, std::string_view objectId)
{
    if (_requiresReload)
        return fail<void>(Error::Stale, "reload committed slot first");
    const auto found = _ledger.records().find(std::string(objectId));
    if (found == _ledger.records().end() || !found->second.deleted
        || _store.occupying({Place::World, std::string(objectId), {}}))
        return fail<void>(Error::Stale, "object is not a committed pickup tombstone");
    InteractionCandidate candidate;
    candidate.kind = InteractionCandidate::Kind::Pickup;
    candidate.objectId = std::string(objectId);
    return callHost([&] { return host.cleanupCommittedPickup(candidate); },
                    "tombstone reconciliation");
}

Result<void> WorldItemCommit::reconcilePlaced(
    IPlacementHost& host, std::string_view objectId)
{
    if (_requiresReload)
        return fail<void>(Error::Stale, "reload committed slot first");
    const auto found = _ledger.records().find(std::string(objectId));
    if (found == _ledger.records().end() || found->second.deleted
        || found->second.version != 1)
        return fail<void>(Error::Missing, "placed object record missing");
    auto decoded = decodePlacement(found->second.payloadJson);
    if (!decoded) return std::unexpected(decoded.error());
    PlacementRequest request{std::string(objectId), decoded->second};
    const auto* item = _store.findItem(decoded->first);
    if (!item || item->location != Location{Place::World, request.worldObjectId, {}})
        return fail<void>(Error::Stale, "placed object and inventory ownership differ");
    if (auto verified = callHost([&] { return host.verifyPublished(request, *item); },
                                 "placement verification"); verified)
        return {};
    if (auto prepared = callHost([&] { return host.prepare(request, *item); },
                                 "placement recovery preparation"); !prepared) {
        host.abort();
        return prepared;
    }
    if (auto published = callHost([&] { return host.publish(); },
                                  "placement recovery publication"); !published) {
        host.abort();
        return published;
    }
    host.commitPublished();
    return {};
}

Result<void> restoreWorldItemCommitSlot(
    Store& store, app::WorldStateLedger& ledger,
    const app::SaveGameService& saves, std::string_view slot,
    const std::function<bool(app::SaveParticipantBundle&, std::string&)>&
        registerAdditionalSections,
    const std::function<void()>& publishAdditionalSections,
    const std::function<bool(const Store&, const app::WorldStateLedger&,
                             std::string&)>& prepareAdditional,
    const std::function<bool(std::string&)>& publishAdditionalChecked)
{
    const auto loaded = saves.load(slot, 1);
    if (!loaded) return fail<void>(Error::Stale, "slot load: " + loaded.error);
    Store nextStore = store;
    app::WorldStateLedger nextLedger = ledger;
    app::SaveParticipantBundle bundle;
    std::string error;
    try {
        if (!registerRestoreSections(bundle, nextStore, nextLedger, error)
            || (registerAdditionalSections
                && !registerAdditionalSections(bundle, error)))
            return fail<void>(Error::Invalid, "restore section: " + error);
        if (!bundle.restore(loaded.payloadJson, &error))
            return fail<void>(Error::Stale, "slot restore: " + error);
        if (prepareAdditional
            && !prepareAdditional(nextStore, nextLedger, error))
            return fail<void>(Error::Stale,
                "additional restore preparation: " + error);
        if (publishAdditionalChecked && !publishAdditionalChecked(error))
            return fail<void>(Error::Stale,
                "additional restore publication: " + error);
        store.swap(nextStore);
        ledger.swap(nextLedger);
        if (publishAdditionalSections) publishAdditionalSections();
        return {};
    } catch (const std::exception& ex) {
        return fail<void>(Error::Stale, std::string("slot restore threw: ") + ex.what());
    } catch (...) {
        return fail<void>(Error::Stale, "slot restore threw");
    }
}

} // namespace ayt::inventory
