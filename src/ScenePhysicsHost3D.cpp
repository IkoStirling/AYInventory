#include <AYInventory/ScenePhysicsHost3D.h>

#include <AYEntity.h>
#include <AYEntity/components/ColliderComponent.h>
#include <AYEntity/components/RigidBodyComponent.h>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ayt::inventory {
namespace {
template<class T> Result<T> fail(Error code, const char* message)
{
    return std::unexpected(Diagnostic{code, message});
}

bool finiteFloat(double value) noexcept
{
    return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max();
}

bool validEntityId(const std::string& id) noexcept
{
    return entity::isValidComponentInstanceId(id);
}

physics::PhysResult overlapPlacement(physics::IPhysicsQuery& query,
    const PlacementPose& pose, physics::PhysLayerMask layerMask,
    std::vector<physics::BodyHandle>& overlaps)
{
    return query.overlapBoxSync(
        {static_cast<float>(pose.position[0]), static_cast<float>(pose.position[1]),
         static_cast<float>(pose.position[2])},
        {static_cast<float>(pose.halfExtents[0]), static_cast<float>(pose.halfExtents[1]),
         static_cast<float>(pose.halfExtents[2])},
        {static_cast<float>(pose.rotation[0]), static_cast<float>(pose.rotation[1]),
         static_cast<float>(pose.rotation[2]), static_cast<float>(pose.rotation[3])},
        overlaps, layerMask);
}

bool samePose(const PlacementPose& pose, const entity::Entity& entity)
{
    math::FVector3 position;
    math::FQuaternion rotation;
    if (!entity.worldPose(position, rotation)) return false;
    constexpr float tolerance = 0.0001f;
    const auto closeEnough = [](float a, float b) { return std::abs(a - b) <= tolerance; };
    return closeEnough(position.x, static_cast<float>(pose.position[0]))
        && closeEnough(position.y, static_cast<float>(pose.position[1]))
        && closeEnough(position.z, static_cast<float>(pose.position[2]))
        && closeEnough(rotation.x, static_cast<float>(pose.rotation[0]))
        && closeEnough(rotation.y, static_cast<float>(pose.rotation[1]))
        && closeEnough(rotation.z, static_cast<float>(pose.rotation[2]))
        && closeEnough(rotation.w, static_cast<float>(pose.rotation[3]));
}
} // namespace

ScenePhysicsHost3D::ScenePhysicsHost3D(entity::World& world,
                                       physics::IPhysicsQuery& query, Store& store,
                                       ViewProvider view, Permission permission)
    : _world(world), _query(query), _store(store), _view(std::move(view)),
      _permission(std::move(permission)) {}

Result<std::optional<InteractionCandidate>> ScenePhysicsHost3D::focus(
    std::string_view actorId)
{
    if (actorId.empty() || !_view) return fail<std::optional<InteractionCandidate>>(
        Error::Invalid, "actor or view provider missing");
    if (!_world.isInitialized() || !_query.isReady())
        return fail<std::optional<InteractionCandidate>>(Error::Stale, "world query not ready");
    auto view = _view(actorId);
    if (!view) return std::unexpected(view.error());
    if (!std::isfinite(view->reach) || view->reach <= 0.0 || view->reach > 10000.0)
        return fail<std::optional<InteractionCandidate>>(Error::Invalid, "invalid reach");
    physics::RaycastHit hit{};
    if (_query.raycastSync(view->ray, hit, view->layerMask) != physics::PhysResult::Ok)
        return fail<std::optional<InteractionCandidate>>(Error::Stale, "ray query failed");
    if (!hit.hit || hit.body == physics::InvalidBodyHandle
        || !std::isfinite(hit.distance) || hit.distance < 0.0f
        || hit.distance > view->reach) return std::optional<InteractionCandidate>{};

    entity::Entity* matched = nullptr;
    for (auto* entity : _world.getAllEntities()) {
        if (!entity || !entity->isSpatial()) continue;
        auto* body = entity->getComponent<entity::RigidBodyComponent>();
        if (!body || body->getPhysicsDimension()
                != entity::RigidBodyComponent::PhysicsDimension::ThreeD
            || body->getBodyHandle() != hit.body) continue;
        if (matched) return fail<std::optional<InteractionCandidate>>(
            Error::Stale, "body maps to multiple entities");
        matched = entity;
    }
    if (!matched || !validEntityId(matched->persistentId()))
        return std::optional<InteractionCandidate>{};
    const auto& id = matched->persistentId();
    InteractionCandidate candidate;
    candidate.objectId = id;
    candidate.distance = hit.distance;
    if (auto item = _store.occupying({Place::World, id, {}})) {
        candidate.kind = InteractionCandidate::Kind::Pickup;
        candidate.subjectId = *item;
    } else if (_store.hasContainer(id)) {
        candidate.kind = InteractionCandidate::Kind::OpenContainer;
        candidate.subjectId = id;
    } else {
        return std::optional<InteractionCandidate>{};
    }
    return std::optional<InteractionCandidate>{std::move(candidate)};
}

Result<void> ScenePhysicsHost3D::validateCurrent(
    const InteractionCandidate& candidate, std::string_view actorId)
{
    auto current = focus(actorId);
    if (!current) return std::unexpected(current.error());
    if (!*current || (*current)->kind != candidate.kind
        || (*current)->objectId != candidate.objectId
        || (*current)->subjectId != candidate.subjectId)
        return fail<void>(Error::Stale, "interaction target changed");
    return {};
}

Result<void> ScenePhysicsHost3D::authorize(
    const InteractionCandidate& candidate, std::string_view actorId)
{
    if (!_permission) return fail<void>(Error::Denied, "permission rule missing");
    return _permission(candidate, actorId);
}

Result<void> ScenePhysicsHost3D::consumeWorldPickup(
    const InteractionCandidate& candidate)
{
    if (candidate.kind != InteractionCandidate::Kind::Pickup
        || !validEntityId(candidate.objectId)
        || _store.occupying({Place::World, candidate.objectId, {}})
            != std::optional<std::string>{candidate.subjectId})
        return fail<void>(Error::Stale, "world pickup ownership changed");
    auto* entity = _world.findEntityByPersistentId(candidate.objectId);
    if (!entity || !entity->ownedEntities().empty()
        || !entity->spatialChildren().empty() || !_world.canDestroyEntity(entity))
        return fail<void>(Error::Stale, "world pickup cannot be removed alone");
    try {
        _world.destroyEntity(entity);
    } catch (const std::exception&) {
        return fail<void>(Error::Stale, "world pickup removal failed");
    }
    return {};
}

Result<void> ScenePhysicsHost3D::verifyPublished(
    const PlacementRequest& request, const Item& item)
{
    if (!validEntityId(request.worldObjectId)
        || item.location != Location{Place::World, request.worldObjectId, {}}
        || _store.occupying(item.location) != std::optional<std::string>{item.id})
        return fail<void>(Error::Duplicate, "published object identity differs");
    auto* entity = _world.findEntityByPersistentId(request.worldObjectId);
    if (!entity || !entity->isSpatial() || !samePose(request.pose, *entity))
        return fail<void>(Error::Stale, "published object or pose missing");
    auto* body = entity->getComponent<entity::RigidBodyComponent>();
    auto* collider = entity->getComponent<entity::ColliderComponent>();
    if (!body || !body->isStatic()
        || body->getPhysicsDimension()
            != entity::RigidBodyComponent::PhysicsDimension::ThreeD
        || !collider || collider->shapes.size() != 1
        || collider->shapes.front().shape != entity::ColliderShapeType::Box)
        return fail<void>(Error::Stale, "published shape missing");
    const auto& extent = collider->shapes.front().halfExtents;
    if (extent.x != static_cast<float>(request.pose.halfExtents[0])
        || extent.y != static_cast<float>(request.pose.halfExtents[1])
        || extent.z != static_cast<float>(request.pose.halfExtents[2]))
        return fail<void>(Error::Duplicate, "published footprint differs");
    return {};
}

Result<void> ScenePhysicsHost3D::prepare(const PlacementRequest& request,
                                         const Item& item)
{
    if (_prepared) return fail<void>(Error::Duplicate, "placement already prepared");
    if (!_retractableId.empty())
        return fail<void>(Error::Stale, "previous publication must be committed or retracted");
    if (!validEntityId(request.worldObjectId) || item.id.empty())
        return fail<void>(Error::Invalid, "placement needs an Entity persistent ID");
    if (!_world.isInitialized() || !_query.isReady())
        return fail<void>(Error::Stale, "world query not ready");
    if (_world.findEntityByPersistentId(request.worldObjectId))
        return fail<void>(Error::Duplicate, "world object ID already exists");
    if (!entity::World::isComponentTypeRegistered<entity::RigidBodyComponent>()
        || !entity::World::isComponentTypeRegistered<entity::ColliderComponent>())
        return fail<void>(Error::Invalid, "Entity physics components not registered");
    for (double value : request.pose.position) if (!finiteFloat(value))
        return fail<void>(Error::Invalid, "placement position is not representable");
    for (double value : request.pose.rotation) if (!finiteFloat(value))
        return fail<void>(Error::Invalid, "placement rotation is not representable");
    for (double value : request.pose.halfExtents) {
        if (!finiteFloat(value) || value <= 0.0)
            return fail<void>(Error::Invalid, "placement extent is not representable");
    }
    std::vector<physics::BodyHandle> overlaps;
    if (overlapPlacement(_query, request.pose, _placementLayerMask, overlaps)
            != physics::PhysResult::Ok)
        return fail<void>(Error::Stale, "occupancy query failed");
    if (!overlaps.empty()) return fail<void>(Error::Occupied, "placement space occupied");
    _prepared = request;
    return {};
}

Result<void> ScenePhysicsHost3D::publish()
{
    if (!_prepared) return fail<void>(Error::Invalid, "no prepared placement");
    const auto request = *_prepared;
    entity::Entity* entity = nullptr;
    try {
        if (!_world.isInitialized() || !_query.isReady())
            return fail<void>(Error::Stale, "world query not ready at publish");
        if (_world.findEntityByPersistentId(request.worldObjectId))
            return fail<void>(Error::Duplicate, "world object ID appeared before publish");
        std::vector<physics::BodyHandle> overlaps;
        if (overlapPlacement(_query, request.pose, _placementLayerMask, overlaps)
                != physics::PhysResult::Ok)
            return fail<void>(Error::Stale, "occupancy query failed at publish");
        if (!overlaps.empty())
            return fail<void>(Error::Occupied, "placement space changed before publish");
        entity = _world.createSpatialEntity();
        if (!entity || !entity->restorePersistentId(request.worldObjectId))
            throw std::runtime_error("cannot create stable Entity identity");
        const math::FVector3 position{
            static_cast<float>(request.pose.position[0]),
            static_cast<float>(request.pose.position[1]),
            static_cast<float>(request.pose.position[2])};
        const math::FQuaternion rotation{
            static_cast<float>(request.pose.rotation[0]),
            static_cast<float>(request.pose.rotation[1]),
            static_cast<float>(request.pose.rotation[2]),
            static_cast<float>(request.pose.rotation[3])};
        if (!entity->setWorldPose(position, rotation))
            throw std::runtime_error("cannot set placement pose");
        auto* body = entity->addComponent<entity::RigidBodyComponent>();
        auto* collider = entity->addComponent<entity::ColliderComponent>();
        if (!body || !collider) throw std::runtime_error("cannot add placement physics");
        body->setStatic(true);
        body->setPhysicsDimension(entity::RigidBodyComponent::PhysicsDimension::ThreeD);
        body->setSyncMode(entity::RigidBodyComponent::SyncMode::EntityToPhysics);
        auto& shape = collider->addShape();
        shape.shape = entity::ColliderShapeType::Box;
        shape.halfExtents = {
            static_cast<float>(request.pose.halfExtents[0]),
            static_cast<float>(request.pose.halfExtents[1]),
            static_cast<float>(request.pose.halfExtents[2])};
        _retractableId = request.worldObjectId;
        _prepared.reset();
        return {};
    } catch (const std::exception&) {
        if (entity) {
            try { _world.destroyEntity(entity); } catch (const std::exception&) {}
        }
        return fail<void>(Error::Stale, "world object publication failed");
    }
}

void ScenePhysicsHost3D::abort() noexcept
{
    _prepared.reset();
}

void ScenePhysicsHost3D::commitPublished() noexcept
{
    _retractableId.clear();
}

bool ScenePhysicsHost3D::retractPublished(
    const PlacementRequest& request) noexcept
{
    try {
        if (_retractableId.empty() || _retractableId != request.worldObjectId)
            return false;
        auto* entity = _world.findEntityByPersistentId(request.worldObjectId);
        if (!entity) return false;
        _world.destroyEntity(entity);
        _retractableId.clear();
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace ayt::inventory
