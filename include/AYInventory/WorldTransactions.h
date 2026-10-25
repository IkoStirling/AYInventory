#pragma once

#include <AYInventory/Inventory.h>

#include <array>
#include <functional>
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

/// Optional durable pickup cleanup. After ownership has been saved and
/// published, remove a still-present World projection by persistent ID.
/// Return success if it is already absent; do not restore World ownership.
class IDurablePickupHost : public IInteractionHost {
public:
    virtual Result<void> cleanupCommittedPickup(
        const InteractionCandidate& candidate) = 0;
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

/// A game-owned rule for the actor's access to every non-focused container
/// and item in a proposed exchange. The focused target authorization alone
/// cannot prove ownership of the actor's bag or another container.
using ContainerBatchAuthorization = std::function<Result<void>(
    const InteractionCandidate&, std::string_view actorId,
    std::span<const TransferMove>)>;
/// Optional failure-atomic save of the complete candidate Store. Returning
/// false or throwing must mean no durable payload was published.
using ContainerBatchSave = std::function<bool(
    const Store& candidate, const InteractionCandidate& focused)>;

/// Exchange 1..256 items between registered containers after rechecking the
/// current focused container, game target permission and every move's access.
/// Every move must involve the focused container. No World/equipment endpoints
/// or Entity side effects are performed. With save, publish Store only after
/// the complete candidate is accepted; exact retries do not save again.
Result<Activation> transferFocusedContainerItems(
    Store& store, IInteractionHost& host,
    const InteractionCandidate& focused, std::string_view actorId,
    std::span<const TransferMove> moves, std::string requestId,
    const ContainerBatchAuthorization& authorizeMoves,
    const ContainerBatchSave& save = {});

struct DurablePickupResult {
    Activation activation;
    /// Authoritative ownership and save have committed. The World projection
    /// could not be removed yet; retry with the same request ID or reconcile
    /// the saved tombstone after load. This is not a transfer failure.
    bool cleanupPending = false;
};

/// Persist the candidate Store and an object tombstone as one failure-atomic
/// game save. A false/throwing callback leaves live state untouched. After a
/// successful save, the Store commits even if World cleanup fails; the result
/// explicitly reports pending cleanup. Exact retries never save or transfer
/// again and may retry cleanup. Use at a serialized quiescent Sim boundary.
using PickupSave = std::function<bool(const Store& candidate,
                                      const InteractionCandidate& pickup)>;
[[nodiscard]] Result<DurablePickupResult> activatePickupDurable(
    Store& store, IDurablePickupHost& host,
    const InteractionCandidate& candidate, std::string_view actorId,
    const Location& destination, std::string requestId,
    const PickupSave& save);

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
    /// Release any temporary rollback token after the Store is published.
    virtual void commitPublished() noexcept {}
};

/// Optional Host contract for a placement whose candidate Store and world
/// object are also committed to one game-owned durable save boundary. After
/// publish succeeds, retractPublished must remove exactly that object on a
/// failed save callback. It must not touch the Store or any preexisting object.
class IDurablePlacementHost : public IPlacementHost {
public:
    /// Only the object from the immediately preceding successful publish may
    /// be retracted; return false if that exact rollback cannot be completed.
    virtual bool retractPublished(const PlacementRequest& request) noexcept = 0;
};

/// Save a candidate Store and placement record in the same game-owned slot.
/// The callback must be failure-atomic: true means the complete durable
/// payload has been accepted; false means no new payload was published. It
/// must not mutate the Store, World or Host. The game must restore the placed
/// object from that same payload after a crash. File durability remains the
/// callback's responsibility.
using PlacementSave = std::function<bool(const Store& candidate,
                                         const PlacementRequest& request)>;

/// Stage the Store transfer, ask the Host to reserve clear space, publish the
/// world object, then swap the staged Store without allocation. Exact request
/// retries do not republish the object. Host exceptions become diagnostics and
/// failed preparation/publication calls abort(). The Host owns Scene/Physics.
Result<void> placeWorldItem(Store& store, IPlacementHost& host,
                            std::string_view itemId, const Location& source,
                            PlacementRequest request, std::string requestId);

/// As above, but save the candidate before publishing Store. If saving fails,
/// retract the already published World object and retain the original Store.
/// A failed retraction is reported as Stale and requires Host recovery.
Result<void> placeWorldItemDurable(Store& store, IDurablePlacementHost& host,
    std::string_view itemId, const Location& source,
    PlacementRequest request, std::string requestId, const PlacementSave& save);

/// Reserve materials, commit output directly to one stable World identity,
/// and publish its object as a single Store/World handoff. Failure leaves the
/// Store and its prior reservations unchanged; Host publication must be
/// failure-atomic. The craft request ID is the durable retry key. An exact
/// retry verifies the existing world object instead of spending again.
Result<void> craftAndPlaceWorldItem(Store& store, IPlacementHost& host,
                                    const Recipe& recipe,
                                    std::string_view sourceContainerId,
                                    std::string craftRequestId,
                                    std::string outputItemId,
                                    PlacementRequest request);

/// Durable variant for the combined craft/placement operation. Pending craft
/// reservations remain runtime-only; the saved candidate includes consumed
/// materials, output ownership and the completed craft receipt.
Result<void> craftAndPlaceWorldItemDurable(Store& store,
    IDurablePlacementHost& host, const Recipe& recipe,
    std::string_view sourceContainerId, std::string craftRequestId,
    std::string outputItemId, PlacementRequest request,
    const PlacementSave& save);

} // namespace ayt::inventory
