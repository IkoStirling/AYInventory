#include <AYInventory/Inventory.h>
#include <AYInventory/WorldTransactions.h>
#ifdef AY_INVENTORY_JSON
#include <AYInventory/SnapshotJson.h>
#endif

#include <array>
#include <iostream>
#include <stdexcept>

using namespace ayt::inventory;

namespace {
void check(bool okay, const char* label)
{
    if (!okay) throw std::runtime_error(label);
}

template<class T, class E>
void check(const std::expected<T, E>& result, const char* label)
{
    check(result.has_value(), label);
}

Store makeStore()
{
    auto created = Store::create(7, {{"wood", 10}, {"iron", 10}, {"campfire", 1}});
    check(created.has_value(), "catalog");
    auto store = std::move(*created);
    check(store.addContainer({"bag", {"a", "b", "c", "d"}}).has_value(), "bag");
    check(store.addContainer({"chest", {"a"}}).has_value(), "chest");
    check(store.createItem({"wood-1", "wood", 5,
                            {Place::Container, "bag", "a"}}).has_value(), "wood");
    check(store.createItem({"iron-1", "iron", 2,
                            {Place::Container, "bag", "b"}}).has_value(), "iron");
    return store;
}

Recipe campfire()
{
    return {"campfire-v1", {{"wood", 3}, {"iron", 1}}, "campfire", 1};
}

struct InteractionHost final : IInteractionHost {
    bool current = true;
    bool permitted = true;
    bool consume = true;
    bool throwValidation = false;
    bool throwAuthorization = false;
    bool throwConsumption = false;
    int consumed = 0;
    Result<void> validateCurrent(const InteractionCandidate&, std::string_view) override
    {
        if (throwValidation) throw std::runtime_error("validation fault");
        if (!current) return std::unexpected(Diagnostic{Error::Stale, "ray expired"});
        return {};
    }
    Result<void> authorize(const InteractionCandidate&, std::string_view) override
    {
        if (throwAuthorization) throw std::runtime_error("authorization fault");
        if (!permitted) return std::unexpected(Diagnostic{Error::Denied, "not permitted"});
        return {};
    }
    Result<void> consumeWorldPickup(const InteractionCandidate&) override
    {
        if (throwConsumption) throw std::runtime_error("consumption fault");
        if (!consume) return std::unexpected(Diagnostic{Error::Stale, "despawn failed"});
        ++consumed;
        return {};
    }
};

void focusedContainerBatchUsesCurrentAuthorityAndOneSave()
{
    auto store = makeStore();
    const Location bagA{Place::Container, "bag", "a"};
    const Location chestA{Place::Container, "chest", "a"};
    check(store.createItem({"wood-2", "wood", 1, chestA}), "chest item");
    const InteractionCandidate focused{InteractionCandidate::Kind::OpenContainer,
        "scene/chest", "chest", 1.0};
    const std::vector<TransferMove> moves{
        {"wood-1", bagA, chestA}, {"wood-2", chestA, bagA}};
    InteractionHost host;
    int moveChecks = 0, saves = 0;
    const ContainerBatchAuthorization authorize =
        [&](const InteractionCandidate& target, std::string_view actor,
            std::span<const TransferMove> proposed) -> Result<void> {
        ++moveChecks;
        if (target != focused || actor != "player" || proposed.size() != 2)
            return std::unexpected(Diagnostic{Error::Denied, "other container"});
        return {};
    };
    host.current = false;
    check(!transferFocusedContainerItems(store, host, focused, "player", moves,
                                         "container-swap", authorize),
          "stale focus rejects exchange");
    host.current = true;
    host.permitted = false;
    check(!transferFocusedContainerItems(store, host, focused, "player", moves,
                                         "container-swap", authorize),
          "target permission rejects exchange");
    host.permitted = true;
    const ContainerBatchAuthorization denyMoves = [](const InteractionCandidate&,
        std::string_view, std::span<const TransferMove>) -> Result<void> {
        return std::unexpected(Diagnostic{Error::Denied, "bag is locked"});
    };
    check(!transferFocusedContainerItems(store, host, focused, "player", moves,
                                         "container-swap", denyMoves),
          "move-level permission rejects exchange");
    const ContainerBatchAuthorization throwMoves = [](const InteractionCandidate&,
        std::string_view, std::span<const TransferMove>) -> Result<void> {
        throw std::runtime_error("authorization fault");
    };
    check(!transferFocusedContainerItems(store, host, focused, "player", moves,
                                         "container-swap", throwMoves),
          "throwing move authorization rejects exchange");
    const ContainerBatchSave rejected = [&](const Store& candidate,
                                            const InteractionCandidate&) {
        ++saves;
        check(candidate.findItem("wood-2")->location == bagA
              && store.findItem("wood-2")->location == chestA,
              "save sees complete candidate before publication");
        return false;
    };
    check(!transferFocusedContainerItems(store, host, focused, "player", moves,
                                         "container-swap", authorize, rejected),
          "failed save rejects exchange");
    check(store.findItem("wood-1")->location == bagA
          && store.findItem("wood-2")->location == chestA
          && !store.snapshot().completedTransfers.contains("container-swap"),
          "failed exchange leaves both owners and receipt untouched");
    const ContainerBatchSave accepted = [&](const Store&, const InteractionCandidate&) {
        ++saves;
        return true;
    };
    check(transferFocusedContainerItems(store, host, focused, "player", moves,
                                        "container-swap", authorize, accepted),
          "authorized exchange commits");
    check(store.findItem("wood-1")->location == chestA
          && store.findItem("wood-2")->location == bagA
          && saves == 2 && moveChecks == 2,
          "exchange publishes both owners once");
    auto loaded = makeStore();
    check(loaded.restore(store.snapshot()), "exchange receipt reloads");
    host.current = false;
    check(transferFocusedContainerItems(loaded, host, focused, "player", moves,
                                        "container-swap", authorize, accepted),
          "exact retry after load does not need stale focus");
    check(saves == 2 && moveChecks == 2, "retry never saves or authorizes again");
    const std::vector<TransferMove> unrelated{{"iron-1",
        {Place::Container, "bag", "b"}, {Place::Container, "bag", "c"}}};
    check(!transferFocusedContainerItems(loaded, host, focused, "player", unrelated,
                                         "unrelated", authorize),
          "focused interaction cannot move unrelated inventory");
}

struct DurableInteractionHost final : IDurablePickupHost {
    bool worldPresent = true;
    bool consumeSucceeds = false;
    bool cleanupSucceeds = false;
    int consumeCalls = 0;
    int cleanupCalls = 0;
    Result<void> validateCurrent(const InteractionCandidate&,
                                 std::string_view) override { return {}; }
    Result<void> authorize(const InteractionCandidate&,
                           std::string_view) override { return {}; }
    Result<void> consumeWorldPickup(const InteractionCandidate&) override {
        ++consumeCalls;
        if (!consumeSucceeds)
            return std::unexpected(Diagnostic{Error::Stale, "world removal failed"});
        worldPresent = false;
        return {};
    }
    Result<void> cleanupCommittedPickup(const InteractionCandidate&) override {
        ++cleanupCalls;
        if (!cleanupSucceeds)
            return std::unexpected(Diagnostic{Error::Stale, "cleanup still pending"});
        worldPresent = false;
        return {};
    }
};

void durablePickupCommitsOwnershipBeforeBestEffortWorldCleanup()
{
    auto store = makeStore();
    const Location bag{Place::Container, "bag", "a"};
    const Location drop{Place::World, "scene/durable-drop", {}};
    const Location chest{Place::Container, "chest", "a"};
    check(store.transfer("wood-1", bag, drop, "seed-drop"), "seed world drop");
    const InteractionCandidate pickup{InteractionCandidate::Kind::Pickup,
        "scene/durable-drop", "wood-1", 1.0};
    DurableInteractionHost host;
    int saves = 0;
    const PickupSave rejected = [&](const Store& candidate,
                                    const InteractionCandidate& target) {
        ++saves;
        check(target == pickup && candidate.findItem("wood-1")->location == chest
            && store.findItem("wood-1")->location == drop,
            "save sees candidate Store and world tombstone target");
        return false;
    };
    check(!activatePickupDurable(store, host, pickup, "player", chest,
                                 "durable-pickup", rejected),
          "failed save refuses pickup");
    check(saves == 1 && host.consumeCalls == 0
        && store.findItem("wood-1")->location == drop,
        "failed save leaves live ownership and World untouched");
    const PickupSave accepted = [&](const Store& candidate,
                                    const InteractionCandidate& target) {
        ++saves;
        return target == pickup && candidate.findItem("wood-1")->location == chest;
    };
    const auto committed = activatePickupDurable(store, host, pickup, "player",
        chest, "durable-pickup", accepted);
    check(committed && committed->cleanupPending && host.worldPresent
        && store.findItem("wood-1")->location == chest && saves == 2,
        "cleanup failure is reported after authoritative pickup commit");
    host.cleanupSucceeds = true;
    const auto retried = activatePickupDurable(store, host, pickup, "player",
        chest, "durable-pickup", accepted);
    check(retried && !retried->cleanupPending && !host.worldPresent
        && saves == 2 && host.consumeCalls == 1 && host.cleanupCalls == 2,
        "exact retry only reconciles the committed World projection");
    check(!activatePickupDurable(store, host, pickup, "player", chest,
                                 "different-request", accepted),
          "changed request cannot reuse committed ownership");
}

struct PlacementHost final : IDurablePlacementHost {
    bool clear = true;
    bool canPublish = true;
    bool samePose = true;
    bool throwPrepare = false;
    bool throwPublish = false;
    bool throwVerify = false;
    bool canRetract = true;
    int prepared = 0;
    int published = 0;
    int aborted = 0;
    int retracted = 0;
    Result<void> verifyPublished(const PlacementRequest&, const Item&) override
    {
        if (throwVerify) throw std::runtime_error("verify fault");
        if (!samePose) return std::unexpected(Diagnostic{Error::Duplicate, "pose changed"});
        return {};
    }
    Result<void> prepare(const PlacementRequest&, const Item&) override
    {
        ++prepared;
        if (throwPrepare) throw std::runtime_error("prepare fault");
        if (!clear) return std::unexpected(Diagnostic{Error::Occupied, "space blocked"});
        return {};
    }
    Result<void> publish() override
    {
        if (throwPublish) throw std::runtime_error("publish fault");
        if (!canPublish) return std::unexpected(Diagnostic{Error::Stale, "world publish failed"});
        ++published;
        return {};
    }
    void abort() noexcept override { ++aborted; }
    bool retractPublished(const PlacementRequest&) noexcept override {
        ++retracted;
        return canRetract;
    }
};

void durablePlacementSavesCandidateOrRetractsWorld()
{
    auto store = makeStore();
    PlacementHost host;
    const Location bag{Place::Container, "bag", "a"};
    const PlacementRequest request{"scene/durable-wood", {}};
    int saveCalls = 0;
    const PlacementSave rejected = [&](const Store& candidate,
                                       const PlacementRequest& proposed) {
        ++saveCalls;
        check(proposed.worldObjectId == request.worldObjectId
              && candidate.findItem("wood-1")->location
                 == Location{Place::World, request.worldObjectId, {}}
              && store.findItem("wood-1")->location == bag,
              "save sees candidate while live Store remains old");
        return false;
    };
    check(!placeWorldItemDurable(store, host, "wood-1", bag, request,
                                 "durable-place", rejected),
          "failed durable save rejects placement");
    check(host.retracted == 1 && store.findItem("wood-1")->location == bag,
          "failed save retracts world and retains inventory");
    check(placeWorldItemDurable(store, host, "wood-1", bag, request,
                                "durable-place", [&](const Store& candidate,
                                    const PlacementRequest&) {
                                    ++saveCalls;
                                    return candidate.findItem("wood-1")->location.place
                                        == Place::World;
                                }), "save retry publishes ownership");
    check(host.published == 2 && host.retracted == 1 && saveCalls == 2,
          "retry publishes once after failed save");
    check(placeWorldItemDurable(store, host, "wood-1", bag, request,
                                "durable-place", rejected)
          && saveCalls == 2, "exact retry does not save twice");

    auto throwingStore = makeStore();
    PlacementHost throwingHost;
    check(!placeWorldItemDurable(throwingStore, throwingHost, "wood-1", bag,
        request, "throw-save", [](const Store&, const PlacementRequest&) -> bool {
            throw std::runtime_error("disk fault");
        }) && throwingHost.retracted == 1
          && throwingStore.findItem("wood-1")->location == bag,
          "throwing save retracts publication and retains Store");
    auto unretractableStore = makeStore();
    PlacementHost unretractableHost;
    unretractableHost.canRetract = false;
    const auto rollbackFailure = placeWorldItemDurable(unretractableStore,
        unretractableHost, "wood-1", bag, request, "bad-rollback",
        [](const Store&, const PlacementRequest&) { return false; });
    check(!rollbackFailure && rollbackFailure.error().code == Error::Stale
          && unretractableHost.retracted == 1
          && unretractableStore.findItem("wood-1")->location == bag,
          "failed retraction reports Host recovery instead of claiming success");

    auto craftStore = makeStore();
    PlacementHost craftHost;
    const PlacementRequest crafted{"scene/durable-campfire", {}};
    check(!craftAndPlaceWorldItemDurable(craftStore, craftHost,
        campfire(), "bag", "durable-build", "campfire-1", crafted,
        [](const Store&, const PlacementRequest&) { return false; }),
        "failed craft save rejects candidate");
    check(craftHost.retracted == 1
          && craftStore.findCraftReceipt("durable-build") == nullptr
          && craftStore.findItem("wood-1")->quantity == 5
          && craftStore.findItem("iron-1")->quantity == 2,
          "failed craft save preserves materials and receipt state");
    check(craftAndPlaceWorldItemDurable(craftStore, craftHost,
        campfire(), "bag", "durable-build", "campfire-1", crafted,
        [](const Store& candidate, const PlacementRequest& request) {
            return candidate.findItem("campfire-1")->location
                == Location{Place::World, request.worldObjectId, {}}
                && candidate.findCraftReceipt("durable-build") != nullptr;
        }), "craft save retry commits output");
    check(craftStore.findItem("wood-1")->quantity == 2
          && craftStore.findItem("iron-1")->quantity == 1,
          "craft materials consumed once");
}

void throwingHostsDoNotPublishHalfTransactions()
{
    auto store = makeStore();
    const Location drop{Place::World, "scene/throwing-drop", {}};
    const Location bag{Place::Container, "bag", "c"};
    check(store.createItem({"throwing-item", "wood", 1, drop}), "world item");
    const InteractionCandidate candidate{InteractionCandidate::Kind::Pickup,
        drop.ownerId, "throwing-item", 1.0};
    InteractionHost interaction;
    interaction.throwValidation = true;
    check(!activate(store, interaction, candidate, "player", bag, "pickup-throw"),
          "validation exception rejected");
    interaction.throwValidation = false;
    interaction.throwAuthorization = true;
    check(!activate(store, interaction, candidate, "player", bag, "pickup-throw"),
          "authorization exception rejected");
    interaction.throwAuthorization = false;
    interaction.throwConsumption = true;
    check(!activate(store, interaction, candidate, "player", bag, "pickup-throw"),
          "consumption exception rejected");
    check(store.findItem("throwing-item")->location == drop
          && interaction.consumed == 0,
          "failed pickup leaves both owners unchanged");
    interaction.throwConsumption = false;
    check(activate(store, interaction, candidate, "player", bag, "pickup-throw"),
          "retry after host fault succeeds once");

    PlacementHost placement;
    PlacementRequest request{"scene/throwing-placement", {}};
    placement.throwPrepare = true;
    check(!placeWorldItem(store, placement, "throwing-item", bag,
                          request, "place-throw"), "preparation exception rejected");
    check(placement.aborted == 1 && store.findItem("throwing-item")->location == bag,
          "preparation exception aborts staged world state");
    placement.throwPrepare = false;
    placement.throwPublish = true;
    check(!placeWorldItem(store, placement, "throwing-item", bag,
                          request, "place-throw"), "publication exception rejected");
    check(placement.aborted == 2 && store.findItem("throwing-item")->location == bag,
          "publication exception aborts staged world state");
    placement.throwPublish = false;
    check(placeWorldItem(store, placement, "throwing-item", bag,
                         request, "place-throw"), "retry publishes once");
    placement.throwVerify = true;
    check(!placeWorldItem(store, placement, "throwing-item", bag,
                          request, "place-throw"), "verification exception rejected");
    check(placement.published == 1 && store.findItem("throwing-item")->location
          == Location{Place::World, request.worldObjectId, {}},
          "verification exception does not duplicate the object");
}

void craftReservations()
{
    auto store = makeStore();
    const Location output{Place::Container, "bag", "c"};
    check(store.reserveCraft(campfire(), "bag", "craft-1").has_value(), "reserve");
    check(store.reserveCraft(campfire(), "bag", "craft-1").has_value(), "reserve retry");
    check(!store.transfer("wood-1", {Place::Container, "bag", "a"},
                          {Place::Container, "bag", "d"}, "move-1"),
          "reserved stack cannot transfer");
    check(!store.destroyItem("iron-1"), "reserved item cannot disappear");
    check(!store.reserveCraft(campfire(), "bag", "craft-2"),
          "second recipe cannot oversubscribe reserved iron");
    check(store.commitCraft("craft-1", "campfire-1", output).has_value(), "commit");
    check(store.findItem("wood-1")->quantity == 2
          && store.findItem("iron-1")->quantity == 1, "materials consumed once");
    check(store.findItem("campfire-1")->location == output, "output sole owner");
    check(store.commitCraft("craft-1", "campfire-1", output).has_value(),
          "commit retry safe");
    check(!store.commitCraft("craft-1", "another", output),
          "request reuse rejects different output");
    auto saved = store.snapshot();
    check(saved.completedCrafts.size() == 1 && saved.schemaVersion == 6,
          "craft receipt saved");
    auto loaded = makeStore();
    check(loaded.restore(saved).has_value(), "restore crafted inventory");
    check(loaded.commitCraft("craft-1", "campfire-1", output).has_value(),
          "replay after restore");
    check(loaded.findItem("wood-1")->quantity == 2, "replay did not spend twice");
    auto v3 = saved;
    v3.schemaVersion = 3;
    check(loaded.restore(v3).has_value(), "v3 craft save remains readable");
    auto corrupt = saved;
    corrupt.completedCrafts.at("craft-1").destination.place = Place::World;
    check(!loaded.restore(corrupt), "typed malformed craft receipt rejected");
    check(loaded.findItem("wood-1")->quantity == 2,
          "failed receipt restore changed no inventory");
#ifdef AY_INVENTORY_JSON
    auto encoded = encodeSnapshotJson(saved);
    check(encoded.has_value(), "encode craft receipt");
    auto decoded = decodeSnapshotJson(*encoded);
    check(decoded.has_value() && loaded.restore(*decoded).has_value(), "JSON receipt");
    check(loaded.commitCraft("craft-1", "campfire-1", output).has_value(),
          "JSON replay safe");
    auto oldEncoded = encodeSnapshotJson(v3);
    check(oldEncoded.has_value(), "encode v3 fixture");
    auto oldDecoded = decodeSnapshotJson(*oldEncoded);
    check(oldDecoded.has_value() && loaded.restore(*oldDecoded).has_value(),
          "v3 JSON craft save remains readable");
#endif
}

void craftFailuresAndSaveBoundary()
{
    auto store = makeStore();
    check(store.reserveCraft(campfire(), "bag", "craft-2").has_value(), "reserve");
    check(!store.commitCraft("craft-2", "campfire-1",
                             {Place::Container, "bag", "a"}),
          "occupied output rejected");
    check(store.findItem("wood-1")->quantity == 5
          && store.findItem("iron-1")->quantity == 2,
          "failed commit changed no inputs");
    const auto savedWhileReserved = store.snapshot();
    auto loaded = makeStore();
    check(loaded.restore(savedWhileReserved).has_value(), "restore pending save");
    check(loaded.transfer("wood-1", {Place::Container, "bag", "a"},
                          {Place::Container, "bag", "d"}, "loaded-move"),
          "pending reservations canceled by save/load");
    check(store.cancelCraft("craft-2").has_value(), "cancel releases materials");
    check(store.transfer("wood-1", {Place::Container, "bag", "a"},
                         {Place::Container, "bag", "d"}, "move-2"),
          "cancel enables transfer");
    check(store.findItem("wood-1")->quantity == 5, "cancel refunded all units");
    auto v2 = savedWhileReserved;
    v2.schemaVersion = 2;
    check(loaded.restore(v2).has_value(), "v2 ownership save migrates");
#ifdef AY_INVENTORY_JSON
    auto oldPayload = encodeSnapshotJson(v2);
    check(oldPayload.has_value(), "encode v2 fixture");
    auto oldDecoded = decodeSnapshotJson(*oldPayload);
    check(oldDecoded.has_value() && loaded.restore(*oldDecoded).has_value(),
          "v2 JSON ownership save migrates");
#endif
    auto invalid = campfire();
    invalid.inputs.push_back({"wood", 1});
    check(!loaded.reserveCraft(invalid, "bag", "bad-recipe"),
          "duplicate ingredient rejected");
}

void craftAcrossStacks()
{
    auto store = makeStore();
    check(store.createItem({"wood-2", "wood", 3,
                            {Place::Container, "bag", "d"}}), "second wood stack");
    auto recipe = campfire();
    recipe.inputs.front().quantity = 6;
    check(store.reserveCraft(recipe, "bag", "multi-stack"), "reserve across stacks");
    check(store.commitCraft("multi-stack", "fire-multi",
                            {Place::Container, "bag", "c"}),
          "commit across stacks");
    check(store.findItem("wood-1") == nullptr
          && store.findItem("wood-2")->quantity == 2,
          "selected two stacks without extra consumption");
    check(store.snapshot().retiredItemIds.contains("wood-1"),
          "consumed material identity retired");
    check(!store.createItem({"wood-1", "wood", 1,
                             {Place::Container, "bag", "a"}}),
          "consumed material ID cannot be reused");
}

void concurrentReservations()
{
    auto store = makeStore();
    auto recipe = campfire();
    recipe.inputs.front().quantity = 2;
    check(store.reserveCraft(recipe, "bag", "craft-a"), "first reservation");
    check(store.reserveCraft(recipe, "bag", "craft-b"), "second reservation");
    check(store.commitCraft("craft-a", "fire-a",
                            {Place::Container, "bag", "c"}), "first commit");
    check(store.commitCraft("craft-b", "fire-b",
                            {Place::Container, "bag", "d"}), "second commit");
    check(store.findItem("wood-1")->quantity == 1
          && store.findItem("iron-1") == nullptr,
          "overlapping reservations each spent once");
    check(store.snapshot().retiredItemIds.contains("iron-1"),
          "fully consumed ingredient retired");
}

void pendingCraftsCanBeRestoredByExplicitSchemaSevenSnapshot()
{
    auto store = makeStore();
    check(store.createItem({"wood-2", "wood", 3,
                            {Place::Container, "bag", "d"}}), "second material stack");
    auto recipe = campfire();
    recipe.inputs.front().quantity = 6;
    check(store.reserveCraft(recipe, "bag", "long-craft"), "reserve long craft");
    const auto ordinary = store.snapshot();
    check(ordinary.schemaVersion == 6 && ordinary.pendingCrafts.empty(),
          "ordinary snapshot keeps compatibility behavior");
    auto saved = store.snapshotWithReservations();
    check(saved.schemaVersion == 7 && saved.pendingCrafts.size() == 1
          && saved.pendingCrafts.at("long-craft").inputs.size() == 3,
          "schema seven records exact material stacks");
    auto loaded = makeStore();
    check(loaded.restore(saved), "restore pending craft");
    check(!loaded.transfer("wood-1", {Place::Container, "bag", "a"},
                           {Place::Container, "chest", "a"}, "blocked-by-craft"),
          "restored material remains reserved");
    check(loaded.reserveCraft(recipe, "bag", "long-craft"),
          "exact pending request retry survives restore");
    const auto before = loaded.snapshotWithReservations();
    auto corrupt = saved;
    corrupt.pendingCrafts.at("long-craft").inputs.front().quantity = 99;
    check(!loaded.restore(corrupt), "excess reserved quantity rejected");
    check(loaded.snapshotWithReservations().pendingCrafts == before.pendingCrafts,
          "bad reservation restore is atomic");
    corrupt = saved;
    corrupt.pendingCrafts.emplace("duplicate-job", corrupt.pendingCrafts.at("long-craft"));
    check(!loaded.restore(corrupt), "oversubscribed concurrent jobs rejected");
    corrupt = saved;
    corrupt.schemaVersion = 6;
    check(!loaded.restore(corrupt), "legacy schema cannot carry pending jobs");
    check(loaded.commitCraft("long-craft", "long-output",
                             {Place::Container, "bag", "c"}),
          "resumed craft consumes original stack selection");
    check(loaded.findItem("wood-1") == nullptr
          && loaded.findItem("wood-2")->quantity == 2,
          "restored craft spent exactly selected stacks");
#ifdef AY_INVENTORY_JSON
    auto encoded = encodeSnapshotJson(saved);
    check(encoded, "encode pending craft JSON");
    auto decoded = decodeSnapshotJson(*encoded);
    check(decoded && decoded->pendingCrafts == saved.pendingCrafts,
          "decode exact pending craft JSON");
    auto fromJson = makeStore();
    check(fromJson.createItem({"wood-2", "wood", 3,
                               {Place::Container, "bag", "d"}}),
          "restore catalog has second stack");
    check(fromJson.restore(*decoded), "restore pending craft JSON");
    check(fromJson.cancelCraft("long-craft"), "restored cancellation releases materials");
    check(fromJson.transfer("wood-1", {Place::Container, "bag", "a"},
                            {Place::Container, "chest", "a"}, "after-cancel"),
          "restored cancellation permits transfer");
#endif
}

void interactionAndPlacement()
{
    auto store = makeStore();
    const Location drop{Place::World, "scene/drop-1", {}};
    check(store.createItem({"ground-wood", "wood", 1, drop}), "world drop");
    std::array hits{
        InteractionCandidate{InteractionCandidate::Kind::Pickup,
                             "scene/far", "ground-wood", 9.0},
        InteractionCandidate{InteractionCandidate::Kind::Pickup,
                             "scene/drop-1", "ground-wood", 1.5}};
    const auto focus = chooseFocus(hits, 3.0);
    check(focus && focus->objectId == "scene/drop-1", "closest in-range focus");
    check(!chooseFocus(hits, 0.0), "invalid reach rejected");
    std::array equalHits{
        InteractionCandidate{InteractionCandidate::Kind::OpenContainer,
                             "scene/z", "chest", 1.0},
        InteractionCandidate{InteractionCandidate::Kind::OpenContainer,
                             "scene/a", "chest", 1.0}};
    check(chooseFocus(equalHits, 2.0)->objectId == "scene/a",
          "equal-distance focus stable by object ID");
    InteractionHost interaction;
    const Location chest{Place::Container, "chest", "a"};
    interaction.current = false;
    check(!activate(store, interaction, *focus, "player", chest, "pickup-1"),
          "stale ray rejected");
    interaction.current = true;
    interaction.permitted = false;
    check(!activate(store, interaction, *focus, "player", chest, "pickup-1"),
          "permission rejected");
    interaction.permitted = true;
    interaction.consume = false;
    check(!activate(store, interaction, *focus, "player", chest, "pickup-1"),
          "world despawn failure keeps ownership");
    check(store.findItem("ground-wood")->location == drop, "failed pickup left drop");
    interaction.consume = true;
    check(activate(store, interaction, *focus, "player", chest, "pickup-1"),
          "pickup succeeds");
    check(interaction.consumed == 1
          && store.findItem("ground-wood")->location == chest, "pickup moved once");
    interaction.current = false;
    check(activate(store, interaction, *focus, "player", chest, "pickup-1"),
          "exact retry needs no expired ray");
    check(interaction.consumed == 1, "retry did not despawn again");
    interaction.current = true;
    const InteractionCandidate open{InteractionCandidate::Kind::OpenContainer,
                                     "scene/chest", "chest", 1.0};
    check(activate(store, interaction, open, "player", std::nullopt, "open-1"),
          "open container returns identity");

    PlacementHost placement;
    PlacementRequest request{"scene/placed-1", {}};
    placement.clear = false;
    check(!placeWorldItem(store, placement, "ground-wood", chest, request, "place-1"),
          "occupied footprint rejected");
    check(store.findItem("ground-wood")->location == chest, "space failure rollback");
    placement.clear = true;
    placement.canPublish = false;
    check(!placeWorldItem(store, placement, "ground-wood", chest, request, "place-1"),
          "publish failure rejected");
    check(store.findItem("ground-wood")->location == chest, "publish failure rollback");
    placement.canPublish = true;
    check(placeWorldItem(store, placement, "ground-wood", chest, request, "place-1"),
          "world placement");
    check(store.findItem("ground-wood")->location ==
              Location{Place::World, "scene/placed-1", ""},
          "stable world owner");
    check(placement.published == 1 && placement.aborted == 2,
          "one publish, two aborts");
    check(placeWorldItem(store, placement, "ground-wood", chest, request, "place-1"),
          "placement retry verified");
    check(placement.published == 1, "placement not republished");
    placement.samePose = false;
    check(!placeWorldItem(store, placement, "ground-wood", chest, request, "place-1"),
          "changed published pose rejected by Host");
    check(!placeWorldItem(store, placement, "ground-wood", chest,
                          {"scene/placed-2", {}}, "place-1"),
          "request reuse with other world ID rejected");
    auto reloaded = makeStore();
    check(reloaded.restore(store.snapshot()).has_value(), "restore placement");
    check(reloaded.findItem("ground-wood")->location.ownerId == "scene/placed-1",
          "placed object save ID survives restore");
    const Location placed{Place::World, "scene/placed-1", {}};
    check(reloaded.transfer("ground-wood", placed, chest, "retrieve-1"),
          "later retrieval");
    placement.samePose = true;
    check(!placeWorldItem(reloaded, placement, "ground-wood", chest,
                          request, "place-1"),
          "superseded placement receipt cannot republish object");
}

void directCraftPlacementIsOneRecoverableHandoff()
{
    auto store = makeStore();
    PlacementHost host;
    const PlacementRequest request{"world/campfire-1", {}};
    host.clear = false;
    check(!craftAndPlaceWorldItem(store, host, campfire(), "bag", "build-1",
                                  "campfire-1", request), "blocked craft placement");
    check(host.published == 0 && host.aborted == 1
          && store.findCraftReceipt("build-1") == nullptr
          && store.findItem("campfire-1") == nullptr
          && store.findItem("wood-1")->quantity == 5,
          "blocked space retains materials and no receipt");
    host.clear = true;
    host.throwPublish = true;
    check(!craftAndPlaceWorldItem(store, host, campfire(), "bag", "build-1",
                                  "campfire-1", request), "throwing publication");
    check(host.aborted == 2 && store.findCraftReceipt("build-1") == nullptr,
          "publication failure keeps original Store");
    host.throwPublish = false;
    check(craftAndPlaceWorldItem(store, host, campfire(), "bag", "build-1",
                                 "campfire-1", request), "craft directly into world");
    check(host.published == 1 && store.findItem("wood-1")->quantity == 2
          && store.findItem("iron-1")->quantity == 1
          && store.findItem("campfire-1")->location
             == Location{Place::World, request.worldObjectId, {}},
          "materials and object published together");
    check(craftAndPlaceWorldItem(store, host, campfire(), "bag", "build-1",
                                 "campfire-1", request), "exact build retry");
    check(host.published == 1, "retry does not publish again");
    check(!craftAndPlaceWorldItem(store, host, campfire(), "bag", "build-1",
                                  "another", request), "changed output conflicts");
    auto loaded = makeStore();
    check(loaded.restore(store.snapshot()), "restore built item and receipt");
    check(craftAndPlaceWorldItem(loaded, host, campfire(), "bag", "build-1",
                                 "campfire-1", request), "retry after load");
    check(host.published == 1 && loaded.findItem("wood-1")->quantity == 2,
          "restored retry spends nothing");
}
} // namespace

int main()
{
    try {
        craftReservations();
        craftFailuresAndSaveBoundary();
        craftAcrossStacks();
        concurrentReservations();
        pendingCraftsCanBeRestoredByExplicitSchemaSevenSnapshot();
        interactionAndPlacement();
        focusedContainerBatchUsesCurrentAuthorityAndOneSave();
        directCraftPlacementIsOneRecoverableHandoff();
        durablePickupCommitsOwnershipBeforeBestEffortWorldCleanup();
        durablePlacementSavesCandidateOrRetractsWorld();
        throwingHostsDoNotPublishHalfTransactions();
        std::cout << "AYInventory: world transactions passed\n";
    } catch (const std::exception& error) {
        std::cerr << "AYInventory: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
