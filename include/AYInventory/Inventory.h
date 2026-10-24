#pragma once

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ayt::inventory {

enum class Error {
    Invalid, Missing, Duplicate, Occupied, Capacity, Stale, Incompatible,
    ContentVersion, Overflow, Insufficient, Denied
};

struct Diagnostic {
    Error code = Error::Invalid;
    std::string message;
};

template<class T> using Result = std::expected<T, Diagnostic>;

/// Game authored item type. Values are data; economic/drop rules stay in game code.
struct Definition {
    std::string id;
    std::uint32_t maxStack = 1;
    std::set<std::string> equipSlots;
    /// Existing AYAbility configuration ID; zero means no Ability binding.
    std::uint64_t abilityConfiguration = 0;
    /// Existing AYStats IDs and flat values, applied only while equipped.
    struct StatBonus {
        std::uint64_t statId = 0;
        std::int64_t flat = 0;
        bool operator==(const StatBonus&) const = default;
    };
    std::vector<StatBonus> statBonuses;
    /// Item-local attachment slot -> accepted child definition IDs. Empty
    /// accepted sets are invalid; games author compatibility, not Store.
    std::map<std::string, std::set<std::string>> attachmentSlots;
};

enum class Place { World, Container, Equipment, Attachment };

/// ownerId is a persistent Entity/content ID: drop object, container, or actor.
/// World uses an empty slot; Container/Equipment use stable slot names.
/// Attachment uses the parent item instance ID as ownerId and its part slot.
struct Location {
    Place place = Place::World;
    std::string ownerId;
    std::string slot;
    bool operator==(const Location&) const = default;
};

struct Item {
    std::string id;
    std::string definitionId;
    std::uint32_t quantity = 1;
    Location location;
    /// Unique, monotonic AYAbility instance token. Zero for non-Ability items;
    /// Store assigns it once at creation and persists it through snapshot.
    std::uint64_t abilityBindingId = 0;
    bool operator==(const Item&) const = default;
};

struct Container {
    std::string id;
    std::set<std::string> slots;
    bool operator==(const Container&) const = default;
};

struct EquipmentOwner {
    std::string id;
    std::set<std::string> slots;
    bool operator==(const EquipmentOwner&) const = default;
};

/// A data-authored exchange. Each input definition may occur once; a game
/// chooses the recipe and its eligibility rules before calling Store.
struct Recipe {
    struct Ingredient {
        std::string definitionId;
        std::uint32_t quantity = 0;
        bool operator==(const Ingredient&) const = default;
    };
    std::string id;
    std::vector<Ingredient> inputs;
    std::string outputDefinitionId;
    std::uint32_t outputQuantity = 0;
    bool operator==(const Recipe&) const = default;
};

struct CraftReceipt {
    std::string outputItemId;
    Location destination;
    bool operator==(const CraftReceipt&) const = default;
};

struct ReservedCraftInput {
    std::string itemId;
    std::uint32_t quantity = 0;
    bool operator==(const ReservedCraftInput&) const = default;
};

/// A pending request's exact material instances. Restoring them preserves
/// which stack will be consumed when several stacks share a definition.
struct PendingCraft {
    Recipe recipe;
    std::string sourceContainerId;
    std::vector<ReservedCraftInput> inputs;
    bool operator==(const PendingCraft&) const = default;
};

/// A completed stack merge and its immediate post-merge quantities. Exact
/// retries succeed only while those quantities still describe the same
/// items; mismatching later quantities make the old request stale.
struct MergeReceipt {
    std::string sourceItemId;
    std::string targetItemId;
    std::uint32_t quantity = 0;
    std::uint32_t sourceAfter = 0;
    std::uint32_t targetAfter = 0;
    bool operator==(const MergeReceipt&) const = default;
};

/// Versioned value snapshot. Restore validates every item, its sole owner and
/// retired-ID disjointness before swapping; invalid saves leave Store intact.
struct Snapshot {
    std::uint32_t schemaVersion = 6;
    std::uint32_t catalogVersion = 0;
    std::uint64_t nextAbilityBindingId = 1;
    std::vector<Container> containers;
    std::vector<EquipmentOwner> actors;
    std::vector<Item> items;
    /// Instance IDs removed by destruction, full merge or material consumption.
    /// They cannot be reused after load. Legacy v2/v3 saves lack this history.
    std::set<std::string> retiredItemIds;
    std::map<std::string, std::string> completedTransfers;
    std::map<std::string, CraftReceipt> completedCrafts;
    std::map<std::string, MergeReceipt> completedMerges;
    /// Schema v7 only. Ordinary snapshot() stays v6 and omits pending work.
    std::map<std::string, PendingCraft> pendingCrafts;
};

/// Single-owner inventory state. Do not retain Item references across writes.
/// Commands are atomic on validation failure. Transfer receipts and retired
    /// instance IDs survive Snapshot/restore for retry safety. Attached parts
    /// remain owned by their parent instance when it moves; a parent cannot
    /// be destroyed while parts are still attached. Not thread-safe;
/// the tick owner serializes it.
class Store {
public:
    static Result<Store> create(std::uint32_t catalogVersion,
                                std::vector<Definition> definitions);

    Result<void> addContainer(Container container);
    Result<void> addEquipmentOwner(EquipmentOwner actor);
    /// Creates a fresh instance; an ID retired by destruction, full merge or
    /// crafting may not be reused, including after a v6 snapshot restore.
    Result<void> createItem(Item item);

    /// Transfer one entire instance. expectedSource prevents stale repeats;
    /// requestId makes exact retries idempotent, including after load.
    Result<void> transfer(std::string_view itemId, const Location& expectedSource,
                          Location destination, std::string requestId);
    /// Split a stack into a new explicitly identified instance at destination.
    Result<void> split(std::string_view itemId, std::uint32_t quantity,
                       std::string newItemId, Location destination);
    /// Merge same-definition stacks; source identity retires on full merge
    /// and remains unavailable after a v6 snapshot restore.
    Result<void> merge(std::string_view sourceId, std::string_view targetId,
                       std::uint32_t quantity);
    /// Replay-safe partial or full merge. The stable request ID and resulting
    /// quantities survive snapshot/restore. Exact retries do not merge twice;
    /// retries with mismatching later quantities report Stale instead of replaying.
    Result<void> mergeOnce(std::string_view sourceId, std::string_view targetId,
                           std::uint32_t quantity, std::string requestId);
    /// Explicit removal retires the instance ID. A container or actor with
    /// owned items cannot be unregistered until the game transfers/disposes
    /// those items first.
    Result<void> destroyItem(std::string_view itemId);
    Result<void> removeContainer(std::string_view id);
    Result<void> removeEquipmentOwner(std::string_view id);

    /// Lock recipe inputs in a registered container without consuming them.
    /// The ordinary v6 snapshot omits pending reservations, leaving original
    /// material counts available after load. Use snapshotWithReservations()
    /// when a game must resume an in-progress craft exactly.
    Result<void> reserveCraft(const Recipe& recipe,
                              std::string_view sourceContainerId,
                              std::string requestId);
    /// Commit all deductions and one output as one Store state swap. A failed
    /// commit keeps the reservation and all input units untouched.
    Result<void> commitCraft(std::string_view requestId,
                             std::string outputItemId, Location destination);
    Result<void> cancelCraft(std::string_view requestId);
    [[nodiscard]] bool hasContainer(std::string_view id) const;

    [[nodiscard]] const Item* findItem(std::string_view id) const;
    /// Durable completion receipt; pointer expires on Store mutation.
    [[nodiscard]] const CraftReceipt* findCraftReceipt(std::string_view requestId) const;
    [[nodiscard]] const Definition* findDefinition(std::string_view id) const;
    [[nodiscard]] std::optional<std::string> occupying(const Location& location) const;
    [[nodiscard]] Snapshot snapshot() const;
    /// Opt into schema v7 to persist exact pending material reservations.
    /// Restore validates every reserved instance and aggregate quantity before
    /// publishing; the default snapshot() intentionally discards pending work.
    [[nodiscard]] Snapshot snapshotWithReservations() const;
    Result<void> restore(const Snapshot& snapshot);
    /// Exchange complete states without allocation; used after an external
    /// world transaction has published a matching staged transfer.
    void swap(Store& other) noexcept;
    [[nodiscard]] std::uint32_t catalogVersion() const noexcept { return _catalogVersion; }

    /// Read-only descriptor for a gameplay adapter: map the stable instance
    /// ID to AYAbility equipment and AYStats owned modifiers at a Sim boundary.
    /// Attached descendants of equipped roots are included with a slash-
    /// separated slot path, e.g. hand/scope.
    struct EquippedProfile {
        std::string itemId;
        std::string actorId;
        std::string slot;
        std::uint64_t abilityBindingId = 0;
        std::uint64_t abilityConfiguration = 0;
        std::vector<Definition::StatBonus> statBonuses;
        bool operator==(const EquippedProfile&) const = default;
    };
    [[nodiscard]] std::vector<EquippedProfile> equippedProfiles(
        std::string_view actorId) const;

private:
    explicit Store(std::uint32_t version) : _catalogVersion(version) {}
    Result<void> validateItem(const Item& item,
                              std::optional<std::string_view> ignoreId = {}) const;
    std::string transferFingerprint(std::string_view itemId,
                                    const Location& from,
                                    const Location& to) const;

    std::uint32_t _catalogVersion = 0;
    std::uint64_t _nextAbilityBindingId = 1;
    std::map<std::string, Definition> _definitions;
    std::map<std::string, Container> _containers;
    std::map<std::string, EquipmentOwner> _actors;
    std::map<std::string, Item> _items;
    std::set<std::string> _retiredItemIds;
    std::map<std::string, std::string> _completedTransfers;
    std::map<std::string, PendingCraft> _reservations;
    std::map<std::string, std::uint32_t> _reservedByItem;
    std::map<std::string, CraftReceipt> _completedCrafts;
    std::map<std::string, MergeReceipt> _completedMerges;
    [[nodiscard]] bool isReserved(std::string_view itemId) const;
};

/// Deterministic transition plan between two complete Store states. Apply all
/// removals, then additions at one game-owned Sim boundary; the game's
/// Ability/Stats adapter remains responsible for its own atomic publication.
struct EquipmentDelta {
    std::vector<Store::EquippedProfile> removals;
    std::vector<Store::EquippedProfile> additions;
};
[[nodiscard]] EquipmentDelta equipmentDelta(const Store& before,
                                             const Store& after,
                                             std::string_view actorId);

} // namespace ayt::inventory
