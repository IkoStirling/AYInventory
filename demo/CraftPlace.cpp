#include <AYInventory/WorldTransactions.h>

#include <array>
#include <iostream>

using namespace ayt::inventory;

namespace {
struct World final : IInteractionHost, IPlacementHost {
    std::string drop = "home/drop-wood";
    std::string staged;
    std::string published;
    Result<void> validateCurrent(const InteractionCandidate& candidate,
                                 std::string_view) override
    {
        if (candidate.kind == InteractionCandidate::Kind::Pickup
            && candidate.objectId != drop)
            return std::unexpected(Diagnostic{Error::Stale, "drop is gone"});
        return {};
    }
    Result<void> authorize(const InteractionCandidate&, std::string_view) override
    { return {}; }
    Result<void> consumeWorldPickup(const InteractionCandidate& candidate) override
    {
        if (candidate.objectId != drop)
            return std::unexpected(Diagnostic{Error::Stale, "drop is gone"});
        drop.clear();
        return {};
    }
    Result<void> verifyPublished(const PlacementRequest& request,
                                 const Item&) override
    {
        if (published != request.worldObjectId)
            return std::unexpected(Diagnostic{Error::Stale, "world object missing"});
        return {};
    }
    Result<void> prepare(const PlacementRequest& request, const Item&) override
    {
        // A game adapter checks AYPhysics overlap and stages a Scene Entity here.
        if (!published.empty())
            return std::unexpected(Diagnostic{Error::Occupied, "space occupied"});
        staged = request.worldObjectId;
        return {};
    }
    Result<void> publish() override
    {
        published = staged;
        staged.clear();
        return {};
    }
    void abort() noexcept override { staged.clear(); }
};
}

int main()
{
    auto built = Store::create(1, {{"wood", 10}, {"campfire", 1}});
    if (!built) return 1;
    auto store = std::move(*built);
    const Location bagWood{Place::Container, "player-bag", "wood"};
    if (!store.addContainer({"player-bag", {"wood"}})
        || !store.createItem({"wood-1", "wood", 4,
                              {Place::World, "home/drop-wood", {}}})) return 1;
    World world;
    const std::array hits{InteractionCandidate{
        InteractionCandidate::Kind::Pickup, "home/drop-wood", "wood-1", 1.0}};
    const auto focus = chooseFocus(hits, 2.0);
    if (!focus || !activate(store, world, *focus, "player", bagWood, "pickup-1"))
        return 1;
    const Recipe recipe{"simple-campfire", {{"wood", 3}}, "campfire", 1};
    PlacementRequest placement{"home/campfire-1", {}};
    placement.pose.position = {2.0, 0.0, 3.0};
    if (!craftAndPlaceWorldItem(store, world, recipe, "player-bag",
                                "build-1", "campfire-1", placement)
        || !craftAndPlaceWorldItem(store, world, recipe, "player-bag",
                                   "build-1", "campfire-1", placement)) return 1;
    const auto snapshot = store.snapshot();
    std::cout << snapshot.items.size() << " items; "
              << store.findItem("campfire-1")->location.ownerId
              << " is the stable placed object ID\n";
    return 0;
}
