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
    physics::BodyHandle ignoredBody = physics::InvalidBodyHandle;
};

/// Concrete single-World adapter for ordinary 3D Entity/Physics scenes.
/// The caller supplies the current actor view and game permission rule. World
/// object IDs must be canonical 32-digit Entity persistent IDs. Keep the
/// borrowed World, Store and query alive; use only on their owning Sim thread.
class ScenePhysicsHost3D final : public IDurablePickupHost,
                                 public IDurablePlacementHost {
public:
    using ViewProvider = std::function<Result<InteractionView3D>(std::string_view actorId)>;
    using Permission = std::function<Result<void>(const InteractionCandidate&,
                                                   std::string_view actorId)>;

    ScenePhysicsHost3D(entity::World& world, physics::IPhysicsQuery& query,
                       Store& store, ViewProvider view, Permission permission);

    /// One current generic 3D interaction focus. An opaque Physics body is
    /// resolved against live 3D RigidBodyComponents and then to an Entity
    /// persistent ID. ignoredBody excludes the actor's own collider when the
    /// query backend supports that operation.
    Result<std::optional<InteractionCandidate>> focus(std::string_view actorId);

    Result<void> validateCurrent(const InteractionCandidate& candidate,
                                 std::string_view actorId) override;
    Result<void> authorize(const InteractionCandidate& candidate,
                           std::string_view actorId) override;
    Result<void> consumeWorldPickup(const InteractionCandidate& candidate) override;
    /// Idempotently remove the residual Entity after a durable Store pickup.
    /// A World location with any current item is never cleaned up.
    Result<void> cleanupCommittedPickup(
        const InteractionCandidate& candidate) override;

    /// The current 3D oriented box query tests the authored footprint at
    /// prepare and publish. Layers and permission policy remain game supplied.
    void setPlacementLayerMask(physics::PhysLayerMask mask) noexcept {
        _placementLayerMask = mask;
    }
    Result<void> verifyPublished(const PlacementRequest& request,
                                 const Item& item) override;
    Result<void> prepare(const PlacementRequest& request,
                         const Item& item) override;
    Result<void> publish() override;
    void abort() noexcept override;
    void commitPublished() noexcept override;
    /// Retract exactly the object this request just published. Call only
    /// before any later World mutation at the same quiescent Sim boundary.
    bool retractPublished(const PlacementRequest& request) noexcept override;

private:
    entity::World& _world;
    physics::IPhysicsQuery& _query;
    Store& _store;
    ViewProvider _view;
    Permission _permission;
    physics::PhysLayerMask _placementLayerMask = 0xFFFFFFFFu;
    std::optional<PlacementRequest> _prepared;
    std::string _retractableId;
};

} // namespace ayt::inventory
