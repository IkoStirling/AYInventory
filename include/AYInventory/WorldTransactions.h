#pragma once

#include <AYInventory/Inventory.h>

#include <array>
#include <optional>
#include <span>

namespace ayt::inventory {

/// A current-World ray/query hit mapped by the Host to a persistent object ID.
/// Scene/physics handles are deliberately absent: they expire on World rebuild.
struct InteractionCandidate {
    enum class Kind { Pickup, OpenContainer };
    Kind kind = Kind::Pickup;
    std::string objectId;
    std::string subjectId; // Item ID for Pickup, Container ID for OpenContainer.
    double distance = 0.0;
    bool operator==(const InteractionCandidate&) const = default;
};

/// Choose the nearest valid candidate within reach, resolving ties by ID.
[[nodiscard]] std::optional<InteractionCandidate> chooseFocus(
    std::span<const InteractionCandidate> hits, double maximumDistance);

/// Host must recheck current-world hit/reach and game-specific permission at
/// activation time. A candidate retained across frames is never authority.
/// Callbacks must not mutate or reenter the Store used by the active command.
class IInteractionHost {
public:
    virtual ~IInteractionHost() = default;
    virtual Result<void> validateCurrent(const InteractionCandidate& candidate,
                                         std::string_view actorId) = 0;
    virtual Result<void> authorize(const InteractionCandidate& candidate,
                                   std::string_view actorId) = 0;
    /// Atomically remove the current world drop or fail without removing it,
    /// including when this callback throws. Store publishes staged ownership
    /// only on success.
    virtual Result<void> consumeWorldPickup(const InteractionCandidate& candidate) = 0;
};

struct Activation {
    InteractionCandidate::Kind kind;
    std::string objectId;
    std::string subjectId;
};

/// Pickup transfers one world item into a named container slot. Opening a
/// container returns its stable ID without changing inventory state. Host
/// exceptions become failure diagnostics; callbacks must obey their atomic
/// world-side contract even when throwing.
Result<Activation> activate(Store& store, IInteractionHost& host,
                            const InteractionCandidate& candidate,
                            std::string_view actorId,
                            std::optional<Location> pickupDestination,
                            std::string requestId);

struct PlacementPose {
    std::array<double, 3> position{};
    std::array<double, 4> rotation{0.0, 0.0, 0.0, 1.0};
    std::array<double, 3> halfExtents{0.5, 0.5, 0.5};
};

struct PlacementRequest {
    std::string worldObjectId; // Stable save identity for the placed object.
    PlacementPose pose;
};

/// World adapter stages an object and its occupancy check in prepare().
/// publish() must either make the object fully visible or leave it absent.
/// abort() discards any prepared state, including after a failed or throwing
/// prepare()/publish(). It must not throw. Callbacks must not mutate or
/// reenter the Store used by the active command.
class IPlacementHost {
public:
    virtual ~IPlacementHost() = default;
    virtual Result<void> verifyPublished(const PlacementRequest& request,
                                         const Item& item) = 0;
    virtual Result<void> prepare(const PlacementRequest& request,
                                 const Item& item) = 0;
    virtual Result<void> publish() = 0;
    virtual void abort() noexcept = 0;
};

/// Stage the Store transfer, ask the Host to reserve clear space, publish the
/// world object, then swap the staged Store without allocation. Exact request
/// retries do not republish the object. Host exceptions become diagnostics and
/// failed preparation/publication calls abort(). The Host owns Scene/Physics.
Result<void> placeWorldItem(Store& store, IPlacementHost& host,
                            std::string_view itemId, const Location& source,
                            PlacementRequest request, std::string requestId);

} // namespace ayt::inventory
