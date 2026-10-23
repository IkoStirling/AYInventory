#pragma once

#include <AYInventory/WorldTransactions.h>
#include <AYPhysics/IPhysicsQuery.h>

#include <functional>
#include <optional>

namespace ayt::entity { class World; }

namespace ayt::inventory {

/// A game-supplied current view. Resolve it again for every activation; a
/// focus retained from an earlier frame never grants authority by itself.
struct InteractionView3D {
    math::Ray ray;
    double reach = 0.0;
    physics::PhysLayerMask layerMask = 0xFFFFFFFFu;
};

/// Concrete single-World adapter for ordinary 3D Entity/Physics scenes.
/// The caller supplies the current actor view and game permission rule. World
/// object IDs must be canonical 32-digit Entity persistent IDs. Keep the
/// borrowed World, Store and query alive; use only on their owning Sim thread.
class ScenePhysicsHost3D final : public IInteractionHost, public IPlacementHost {
public:
    using ViewProvider = std::function<Result<InteractionView3D>(std::string_view actorId)>;
    using Permission = std::function<Result<void>(const InteractionCandidate&,
                                                   std::string_view actorId)>;

    ScenePhysicsHost3D(entity::World& world, physics::IPhysicsQuery& query,
                       Store& store, ViewProvider view, Permission permission);

    /// One current raycast. An opaque Physics body is resolved against live
    /// 3D RigidBodyComponents and then to an Entity persistent ID.
    Result<std::optional<InteractionCandidate>> focus(std::string_view actorId);

    Result<void> validateCurrent(const InteractionCandidate& candidate,
                                 std::string_view actorId) override;
    Result<void> authorize(const InteractionCandidate& candidate,
                           std::string_view actorId) override;
    Result<void> consumeWorldPickup(const InteractionCandidate& candidate) override;

    /// A bounding sphere conservatively tests the authored box footprint.
    /// Layers and finer shape/permission policy remain game supplied.
    void setPlacementLayerMask(physics::PhysLayerMask mask) noexcept {
        _placementLayerMask = mask;
    }
    Result<void> verifyPublished(const PlacementRequest& request,
                                 const Item& item) override;
    Result<void> prepare(const PlacementRequest& request,
                         const Item& item) override;
    Result<void> publish() override;
    void abort() noexcept override;

private:
    entity::World& _world;
    physics::IPhysicsQuery& _query;
    Store& _store;
    ViewProvider _view;
    Permission _permission;
    physics::PhysLayerMask _placementLayerMask = 0xFFFFFFFFu;
    std::optional<PlacementRequest> _prepared;
};

} // namespace ayt::inventory
