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

struct PlacementHost final : IPlacementHost {
    bool clear = true;
    bool canPublish = true;
    bool samePose = true;
    bool throwPrepare = false;
    bool throwPublish = false;
    bool throwVerify = false;
    int prepared = 0;
    int published = 0;
    int aborted = 0;
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
};

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
    check(saved.completedCrafts.size() == 1 && saved.schemaVersion == 4,
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
} // namespace

int main()
{
    try {
        craftReservations();
        craftFailuresAndSaveBoundary();
        craftAcrossStacks();
        concurrentReservations();
        interactionAndPlacement();
        throwingHostsDoNotPublishHalfTransactions();
        std::cout << "AYInventory: world transactions passed\n";
    } catch (const std::exception& error) {
        std::cerr << "AYInventory: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
