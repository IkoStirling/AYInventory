#include <AYInventory/Inventory.h>
#include <AYInventory/ScenePhysicsHost3D.h>
#include <AYInventory/SnapshotJson.h>
#include <AYApplication/SaveGameService.h>
#include <AYApplication/WorldStateLedger.h>
#include <AYEntity.h>
#include <AYEntity/EntityModule.h>
#include <AYEntity/components/RigidBodyComponent.h>
#include <AYScene.h>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace ayt::inventory;

namespace {
constexpr auto dropId = "11111111111111111111111111111111";
constexpr auto placedId = "22222222222222222222222222222222";

void check(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

template<class T, class E> void check(const std::expected<T, E>& value,
                                      const char* message)
{
    check(value.has_value(), message);
}

Store makeStore(std::uint32_t catalogVersion = 1)
{
    auto created = Store::create(catalogVersion, {{"wood", 1}});
    check(created, "catalog");
    auto store = std::move(*created);
    check(store.addContainer({"player/bag", {"slot"}}), "bag");
    return store;
}

ayt::app::WorldStateLedger::Participant bindStore(Store& store)
{
    ayt::app::WorldStateLedger::Participant binding;
    binding.version = 1;
    binding.capture = [&store](std::string& payload, std::string& error) {
        auto encoded = encodeSnapshotJson(store.snapshot());
        if (!encoded) { error = encoded.error().message; return false; }
        payload = std::move(*encoded);
        return true;
    };
    binding.validate = [&store](std::uint32_t version, std::string_view payload,
                                std::string& error) {
        if (version != 1) { error = "inventory participant version"; return false; }
        auto decoded = decodeSnapshotJson(payload);
        if (!decoded) { error = decoded.error().message; return false; }
        auto candidate = store;
        auto restored = candidate.restore(*decoded);
        if (!restored) { error = restored.error().message; return false; }
        return true;
    };
    binding.apply = [&store](std::uint32_t, std::string_view payload,
                             std::string& error) {
        auto decoded = decodeSnapshotJson(payload);
        if (!decoded) { error = decoded.error().message; return false; }
        auto restored = store.restore(*decoded);
        if (!restored) { error = restored.error().message; return false; }
        return true;
    };
    binding.remove = [](std::string&) { return false; };
    return binding;
}

ayt::app::WorldStateLedger::Participant bindDrop(ayt::scene::Scene& scene)
{
    ayt::app::WorldStateLedger::Participant binding;
    binding.version = 1;
    binding.capture = [](std::string& payload, std::string&) {
        payload = "{}";
        return true;
    };
    binding.validate = [](std::uint32_t, std::string_view, std::string&) {
        return true;
    };
    binding.apply = [](std::uint32_t, std::string_view, std::string&) {
        return true;
    };
    binding.remove = [&scene](std::string&) {
        if (auto* drop = scene.world().findEntityByPersistentId(dropId))
            scene.world().destroyEntity(drop);
        return true;
    };
    return binding;
}

struct Query final : ayt::physics::IPhysicsQuery {
    ayt::physics::RaycastHit hit{};
    bool isReady() const override { return true; }
    ayt::physics::PhysResult raycastSync(const ayt::math::Ray&,
        ayt::physics::RaycastHit& out, ayt::physics::PhysLayerMask) override
    {
        out = hit;
        return ayt::physics::PhysResult::Ok;
    }
    ayt::physics::PhysResult overlapSphereSync(const ayt::math::FVector3&,
        float, std::vector<ayt::physics::BodyHandle>& out,
        ayt::physics::PhysLayerMask) override
    {
        out.clear();
        return ayt::physics::PhysResult::Ok;
    }
    ayt::physics::PhysResult overlapBoxSync(const ayt::math::FVector3&,
        const ayt::math::FVector3&, const ayt::math::FQuaternion&,
        std::vector<ayt::physics::BodyHandle>& out,
        ayt::physics::PhysLayerMask) override
    {
        out.clear();
        return ayt::physics::PhysResult::Ok;
    }
};

ScenePhysicsHost3D makeHost(ayt::scene::Scene& scene, Query& query, Store& store)
{
    return {scene.world(), query, store,
        [](std::string_view) -> Result<InteractionView3D> {
            return InteractionView3D{ayt::math::Ray{{0, 0, 0}, {0, 0, 1}}, 3.0};
        },
        [](const InteractionCandidate&, std::string_view) -> Result<void> {
            return {};
        }};
}

void run()
{
    namespace fs = std::filesystem;
    const fs::path root = fs::path(AY_INVENTORY_SCENE_TEST_ROOT);
    std::error_code ec;
    fs::remove_all(root, ec);
    check(fs::create_directories(root, ec), "test directory");
    const auto authorPath = (root / "author.ayscene").string();
    const auto runtimePath = (root / "runtime.ayscene").string();
    ayt::entity::registerEntityComponents();
    {
        ayt::scene::Scene author(ayt::scene::SceneMode::Edit);
        auto* drop = author.world().createSpatialEntity();
        check(drop && drop->restorePersistentId(dropId), "authored stable drop");
        check(author.save(authorPath), "save authored scene");
    }

    const Location worldDrop{Place::World, dropId, {}};
    const Location bag{Place::Container, "player/bag", "slot"};
    ayt::scene::Scene active(ayt::scene::SceneMode::Play);
    check(active.load(authorPath), "load authored scene");
    auto* activeDrop = active.world().findEntityByPersistentId(dropId);
    check(activeDrop != nullptr, "active drop exists");
    auto* body = activeDrop->addComponent<ayt::entity::RigidBodyComponent>();
    check(body != nullptr, "drop body");
    body->setBodyHandle(41);
    auto activeStore = makeStore();
    check(activeStore.createItem({"wood-1", "wood", 1, worldDrop}), "world item");
    Query query;
    query.hit = {.hit = true, .body = 41, .distance = 1.0f};
    auto activeHost = makeHost(active, query, activeStore);
    auto focused = activeHost.focus("player");
    check(focused && *focused, "focus current drop");
    check(activate(activeStore, activeHost, **focused, "player", bag, "pickup-1"),
          "pick up current drop");
    check(active.world().findEntityByPersistentId(dropId) == nullptr,
          "pickup removed current Entity");

    ayt::app::WorldStateLedger activeLedger;
    check(activeLedger.registerParticipant(dropId, bindDrop(active)), "bind drop");
    check(activeLedger.registerParticipant("inventory/world", bindStore(activeStore)),
          "bind inventory");
    check(activeLedger.markDeleted(dropId), "tombstone consumed authored drop");
    check(activeLedger.capture(), "capture inventory and tombstone");
    std::string payload;
    check(activeLedger.encode(payload), "encode world state");
    ayt::app::SaveGameService saves({root.string(), "inventory-world-restore"});
    check(static_cast<bool>(saves.save("slot", 1, payload)), "save slot");
    const auto saved = saves.load("slot", 1);
    check(static_cast<bool>(saved), "load slot");

    ayt::scene::Scene staged(ayt::scene::SceneMode::Play);
    check(staged.load(authorPath), "rebuild authored world");
    ayt::app::WorldStateLedger candidate;
    check(candidate.decode(saved.payloadJson), "decode candidate ledger");
    auto wrongCatalog = makeStore(2);
    check(!candidate.restoreWithParticipants({
        {dropId, bindDrop(staged)}, {"inventory/world", bindStore(wrongCatalog)}}),
        "catalog mismatch rejects staged restore");
    check(staged.world().findEntityByPersistentId(dropId) != nullptr,
          "preflight failure leaves staged authored drop intact");
    check(activeStore.findItem("wood-1")->location == bag,
          "failed candidate does not change active Store");

    auto restoredStore = makeStore();
    check(candidate.restoreWithParticipants({
        {dropId, bindDrop(staged)}, {"inventory/world", bindStore(restoredStore)}}),
        "restore staged scene and inventory");
    check(staged.world().findEntityByPersistentId(dropId) == nullptr
          && restoredStore.findItem("wood-1")->location == bag,
          "authored drop does not duplicate bag item");
    PlacementRequest placement{placedId, {}};
    placement.pose.position = {2.0, 0.0, 0.0};
    auto stagedHost = makeHost(staged, query, restoredStore);
    check(placeWorldItem(restoredStore, stagedHost, "wood-1", bag,
                         placement, "place-1"), "place item into restored World");
    check(staged.world().findEntityByPersistentId(placedId) != nullptr,
          "placed Entity exists");
    check(staged.save(runtimePath), "save runtime scene");
    check(candidate.capture(), "capture placed ownership");
    check(candidate.encode(payload), "encode placed state");
    check(static_cast<bool>(saves.save("slot", 1, payload)), "save placed slot");

    ayt::scene::Scene reloaded(ayt::scene::SceneMode::Play);
    check(reloaded.load(runtimePath), "reload placed scene");
    auto reloadedStore = makeStore();
    ayt::app::WorldStateLedger reloadedLedger;
    check(reloadedLedger.decode(saves.load("slot", 1).payloadJson),
          "decode placed ledger");
    check(reloadedLedger.restoreWithParticipants({
        {dropId, bindDrop(reloaded)},
        {"inventory/world", bindStore(reloadedStore)}}),
        "restore placed ownership");
    check(reloaded.world().findEntityByPersistentId(dropId) == nullptr
          && reloaded.world().findEntityByPersistentId(placedId) != nullptr
          && reloadedStore.findItem("wood-1")->location
              == Location{Place::World, placedId, {}},
          "reloaded Entity and single owner agree");
    reloadedLedger.clearParticipants();
    candidate.clearParticipants();
    activeLedger.clearParticipants();
    fs::remove_all(root, ec);
}
} // namespace

int main()
{
    try {
        run();
        std::cout << "AYInventory: Scene pickup, tombstone and placement restore passed\n";
    } catch (const std::exception& error) {
        std::cerr << "AYInventory Scene restore: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
