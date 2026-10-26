#pragma once

#include <AYInventory/WorldTransactions.h>
#include <AYApplication/SaveGameService.h>
#include <AYApplication/SaveParticipantBundle.h>
#include <AYApplication/WorldStateLedger.h>

#include <functional>

namespace ayt::inventory {

/// The durable slot is authoritative once Committed, ProjectionPending or
/// ReloadRequired is returned. ProjectionPending means the caller must
/// reconcile the named object before allowing it to be interacted with again.
enum class WorldCommitStatus {
    Committed, AlreadyCommitted, ProjectionPending, ReloadRequired
};

struct WorldCommitResult {
    WorldCommitStatus status = WorldCommitStatus::Committed;
    std::string objectId;
};

/// Optional game-owned candidate sections, such as progression or a quest
/// reward. registerCandidateSections must bind only staged values; publish
/// runs after the slot and core values commit. A failed publish blocks further
/// commands until the owner reloads the complete slot into staged state.
struct WorldCommitExtension {
    /// Stage game-owned effects from the complete candidate Store before any
    /// slot write. For direct World-to-equipment pickup this must include the
    /// affected Stats/Ability projection and its durable sections.
    std::function<bool(const Store&, const app::WorldStateLedger&,
                       std::string&)> prepareCandidate;
    std::function<bool(app::SaveParticipantBundle&, std::string&)> registerCandidateSections;
    std::function<bool(std::string&)> publishCandidate;
};

/// One-world, one-slot item/World commit boundary. The owner serializes calls
/// at a quiet simulation point. The slot must contain the complete selected
/// game state: pass every other section through WorldCommitExtension. No
/// external notifications may be sent in a candidate or publish callback;
/// use a durable game outbox for those effects. An existing slot with a
/// different section set is rejected rather than silently losing game state.
class WorldItemCommit {
public:
    WorldItemCommit(Store& store, app::WorldStateLedger& ledger,
                    const app::SaveGameService& saves, std::string slot);

    /// Validate a current pickup, then durably save Store ownership, the
    /// object's tombstone and fingerprinted receipt together. Destination
    /// may be a container or equipment slot; equipment requires a complete
    /// WorldCommitExtension for its game-owned Stats/Ability projection.
    /// World removal happens after the slot write and is retryable.
    [[nodiscard]] Result<WorldCommitResult> pickup(
        IDurablePickupHost& host, const InteractionCandidate& candidate,
        std::string_view actorId, const Location& destination,
        std::string requestId, std::string fingerprint,
        const WorldCommitExtension& extension = {});

    /// Check occupancy before saving, but publish the object only after the
    /// candidate Store, live object record and receipt reach the same slot.
    /// The record payload is written by this API and contains the item ID and
    /// pose required for restart reconciliation.
    [[nodiscard]] Result<WorldCommitResult> place(
        IPlacementHost& host, std::string_view itemId, const Location& source,
        PlacementRequest request, std::string requestId,
        std::string fingerprint, std::string contentSignature = {},
        const WorldCommitExtension& extension = {});

    /// Reserve and consume authored materials in the candidate Store, create
    /// one output instance with World ownership, and use the same durable
    /// record/projection recovery path as place(). A failed save retains the
    /// active materials and any preexisting craft reservation.
    [[nodiscard]] Result<WorldCommitResult> craftAndPlace(
        IPlacementHost& host, const Recipe& recipe,
        std::string_view sourceContainerId, std::string craftRequestId,
        std::string outputItemId, PlacementRequest request,
        std::string fingerprint, std::string contentSignature = {},
        const WorldCommitExtension& extension = {});

    /// Reconcile a saved tombstone against a rebuilt or still-live World.
    /// An already absent object succeeds. Call after restoring the slot and
    /// whenever pickup returns ProjectionPending.
    [[nodiscard]] Result<void> reconcileDeleted(
        IDurablePickupHost& host, std::string_view objectId);

    /// Rebuild or verify a saved placed object. The Host must use the same
    /// stable object ID and reject a mismatched preexisting projection.
    [[nodiscard]] Result<void> reconcilePlaced(
        IPlacementHost& host, std::string_view objectId);

    /// A failed post-save game-section publish requires a fresh staged reload.
    [[nodiscard]] bool requiresReload() const noexcept { return _requiresReload; }

private:
    Store& _store;
    app::WorldStateLedger& _ledger;
    const app::SaveGameService& _saves;
    std::string _slot;
    bool _requiresReload = false;

    [[nodiscard]] Result<void> saveCandidate(
        const Store& candidateStore,
        const app::WorldStateLedger& candidateLedger,
        const WorldCommitExtension& extension);
    [[nodiscard]] bool publishCandidate(
        Store& candidateStore, app::WorldStateLedger& candidateLedger,
        const WorldCommitExtension& extension) noexcept;
};

/// Load the core sections into private candidates and publish them only after
/// every section validates. A game with additional sections registers their
/// staged restore callbacks and supplies a non-throwing final publication
/// callback. World projection is reconciled separately.
[[nodiscard]] Result<void> restoreWorldItemCommitSlot(
    Store& store, app::WorldStateLedger& ledger,
    const app::SaveGameService& saves, std::string_view slot,
    const std::function<bool(app::SaveParticipantBundle&, std::string&)>&
        registerAdditionalSections = {},
    const std::function<void()>& publishAdditionalSections = {});

} // namespace ayt::inventory
