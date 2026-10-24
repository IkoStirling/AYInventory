#include <AYInventory/ScenePhysicsHost3D.h>

#include <AYEntity.h>
#include <AYEntity/EntityModule.h>
#include <AYEntity/components/ColliderComponent.h>
#include <AYEntity/components/RigidBodyComponent.h>

#include <iostream>
#include <stdexcept>

using namespace ayt;
using namespace ayt::inventory;

namespace {
constexpr const char* dropId = "11111111111111111111111111111111";
constexpr const char* placedId = "22222222222222222222222222222222";
constexpr const char* blockerId = "33333333333333333333333333333333";
constexpr const char* craftedId = "44444444444444444444444444444444";

void check(bool value, const char* label)
{
    if (!value) throw std::runtime_error(label);
}

template<class T, class E> void check(const std::expected<T, E>& value,
                                      const char* label)
{
    check(value.has_value(), label);
}

struct Query final : physics::IPhysicsQuery {
    bool ready = true;
    bool rayFails = false;
    bool overlapFails = false;
    physics::RaycastHit current{};
    std::vector<physics::BodyHandle> occupied;
    int rayCalls = 0;
    int overlapCalls = 0;

    bool isReady() const override { return ready; }
    physics::PhysResult raycastSync(const math::Ray&, physics::RaycastHit& out,
                                    physics::PhysLayerMask) override
    {
        ++rayCalls;
        out = rayFails ? physics::RaycastHit{} : current;
        return rayFails ? physics::PhysResult::BackendError : physics::PhysResult::Ok;
    }
    physics::PhysResult overlapSphereSync(const math::FVector3&, float,
                                           std::vector<physics::BodyHandle>& out,
                                           physics::PhysLayerMask) override
    {
        ++overlapCalls;
        out = overlapFails ? std::vector<physics::BodyHandle>{} : occupied;
        return overlapFails ? physics::PhysResult::BackendError : physics::PhysResult::Ok;
    }
};

struct WorldScope {
    entity::World& world = entity::World::processWorld();
    WorldScope()
    {
        check(world.initialize(), "world initialization");
        entity::registerEntityComponents();
    }
    ~WorldScope() { world.shutdown(); }
};

void run()
{
    WorldScope scope;
    auto& world = scope.world;
    auto created = Store::create(1, {{"wood", 1}, {"campfire", 1}});
    check(created.has_value(), "store catalog");
    auto store = std::move(*created);
    check(store.addContainer({"bag", {"slot"}}).has_value(), "bag");
    check(store.createItem({"wood-1", "wood", 1, {Place::World, dropId, {}}})
              .has_value(), "world item");
    auto* drop = world.createSpatialEntity();
    check(drop && drop->restorePersistentId(dropId), "stable drop entity");
    auto* dropBody = drop->addComponent<entity::RigidBodyComponent>();
    check(dropBody != nullptr, "drop body");
    dropBody->setBodyHandle(41);

    Query query;
    query.current = {.hit = true, .body = 41, .distance = 1.0f};
    bool allowed = true;
    ScenePhysicsHost3D host(world, query, store,
        [](std::string_view actor) -> Result<InteractionView3D> {
            if (actor != "player")
                return std::unexpected(Diagnostic{Error::Denied, "unknown actor"});
            return InteractionView3D{math::Ray{{0, 0, 0}, {0, 0, 1}}, 3.0};
        },
        [&](const InteractionCandidate&, std::string_view) -> Result<void> {
            if (!allowed) return std::unexpected(Diagnostic{Error::Denied, "locked"});
            return {};
        });
    auto focus = host.focus("player");
    check(focus && *focus && (*focus)->objectId == dropId
          && (*focus)->subjectId == "wood-1", "ray resolved stable pickup");
    query.current.body = 99;
    check(!activate(store, host, **focus, "player",
                    Location{Place::Container, "bag", "slot"}, "pickup-1"),
          "stale ray rejected");
    check(world.findEntityByPersistentId(dropId) == drop, "stale ray kept drop");
    query.current.body = 41;
    allowed = false;
    check(!activate(store, host, **focus, "player",
                    Location{Place::Container, "bag", "slot"}, "pickup-1"),
          "permission rejected");
    allowed = true;
    check(activate(store, host, **focus, "player",
                   Location{Place::Container, "bag", "slot"}, "pickup-1"),
          "pickup accepted");
    check(world.findEntityByPersistentId(dropId) == nullptr
          && store.findItem("wood-1")->location.place == Place::Container,
          "world and inventory changed together");
    check(activate(store, host, **focus, "player",
                   Location{Place::Container, "bag", "slot"}, "pickup-1"),
          "exact pickup retry does not need ray");

    const Location bag{Place::Container, "bag", "slot"};
    PlacementRequest request{placedId, {}};
    request.pose.position = {2.0, 0.0, 0.0};
    query.occupied = {77};
    check(!placeWorldItem(store, host, "wood-1", bag, request, "place-1"),
          "occupied footprint rejected");
    check(store.findItem("wood-1")->location == bag
          && world.findEntityByPersistentId(placedId) == nullptr,
          "occupied failure left no object");
    query.occupied.clear();
    check(placeWorldItem(store, host, "wood-1", bag, request, "place-1"),
          "placed item with real spatial entity");
    auto* placed = world.findEntityByPersistentId(placedId);
    check(placed && placed->getComponent<entity::RigidBodyComponent>()->isStatic()
          && placed->getComponent<entity::ColliderComponent>()->shapes.size() == 1,
          "published object has stable ID and queryable physics authoring");
    check(store.findItem("wood-1")->location == Location{Place::World, placedId, {}},
          "published owner matches Entity ID");
    check(placeWorldItem(store, host, "wood-1", bag, request, "place-1"),
          "exact placement retry verifies Entity");
    request.pose.position[0] = 3.0;
    check(!placeWorldItem(store, host, "wood-1", bag, request, "place-1"),
          "changed pose retry rejected");

    check(!host.prepare({"scene/not-a-persistent-id", {}}, *store.findItem("wood-1")),
          "noncanonical Entity ID rejected");
    PlacementRequest pending{blockerId, {}};
    check(host.prepare(pending, *store.findItem("wood-1")), "separate prepare");
    query.occupied = {88};
    check(!host.publish(), "publish rechecks changing occupancy");
    host.abort();
    check(world.findEntityByPersistentId(blockerId) == nullptr,
          "changed occupancy left no object");
    query.occupied.clear();
    check(host.prepare(pending, *store.findItem("wood-1")), "prepare after abort");
    auto* blocker = world.createSpatialEntity();
    check(blocker && blocker->restorePersistentId(blockerId), "concurrent blocker");
    check(!host.publish(), "publication rechecks ID conflict");
    host.abort();
    check(world.findEntityByPersistentId(blockerId) == blocker,
          "failed publish left other entity intact");
    check(world.findEntityByPersistentId(placedId) == placed,
          "failed publish left prior published item intact");
    check(store.createItem({"wood-2", "wood", 1, bag}), "new material stack");
    const Recipe recipe{"campfire-v1", {{"wood", 1}}, "campfire", 1};
    PlacementRequest crafted{craftedId, {}};
    crafted.pose.position = {5.0, 0.0, 0.0};
    check(craftAndPlaceWorldItem(store, host, recipe, "bag", "build-1",
                                 "campfire-1", crafted), "craft and publish through real Scene host");
    check(world.findEntityByPersistentId(craftedId) != nullptr
          && store.findItem("wood-2") == nullptr
          && store.findItem("campfire-1")->location
             == Location{Place::World, craftedId, {}},
          "crafted object and consumed material agree");
    check(craftAndPlaceWorldItem(store, host, recipe, "bag", "build-1",
                                 "campfire-1", crafted), "real host retry verifies published object");
    check(query.rayCalls >= 3 && query.overlapCalls >= 2, "live queries used");
}
} // namespace

int main()
{
    try {
        run();
        std::cout << "AYInventory: Scene/Physics 3D Host passed\n";
    } catch (const std::exception& e) {
        std::cerr << "AYInventory: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
