# World interaction Host wiring

AYInventory keeps stable ownership and transaction receipts. The game Host owns
the current Scene, Physics query facade, and object presentation. Keep these
layers on the same Sim thread while running one interaction command.

1. Cast with `IEngineHost::physicsQuery()->raycastSync` after checking
   `isReady()`. Resolve the returned current-world handle to an Entity and its
   persistent object ID. Convert each eligible hit to `InteractionCandidate`;
   do not persist the physics handle. `chooseFocus` applies reach and stable
   tie breaking.
2. In `IInteractionHost::validateCurrent`, recheck the live object, generation,
   hit and reach. In `authorize`, enforce game-owned access rules. For pickup,
   `consumeWorldPickup` must remove the drop atomically: on failure or throw it leaves
   the drop present. AYInventory publishes the already-validated Store transfer
   only after that call succeeds. For a container, `activate` returns its ID;
   the game opens its own UI.
3. Craft in a registered material container. Reserve before entering the
   crafting animation; cancel on interruption; commit with a new stable item
   ID and an empty output slot. Pending reservations are not saved. A load
   restores the materials untouched and a caller can reserve again.
4. For placement, map `PlacementPose` and extents to the Scene's world pose and
   the object's occupancy policy. `IPlacementHost::prepare` checks space and
   stages one object under `worldObjectId`; `publish` must expose it completely
   or leave it absent, and `abort` must remove staging even when prepare or
   publish throws. AYInventory then makes
   the item world-owned. `verifyPublished` compares the saved object and pose on
   an exact request retry. A save records both the Store's world owner ID and
   the game/Scene object's transform. Rebuild both under that same ID on load.

For a save assembled from an authored Scene, mark the picked-up authored drop
as deleted in `WorldStateLedger`. On load, decode a separate candidate ledger,
restore the tombstone and Store participant into a disposable Scene, then
activate it. The Scene restore test also saves a placed runtime Entity with
its Store world owner ID and checks both after a second load.

The current public physics query facade has ray and sphere overlap queries.
Sphere overlap is conservative for oriented boxes or irregular footprints.
The Host may use a more exact project-specific spatial query where required.
For ordinary 3D Entity scenes, link `AYInventory::World3DHost` and construct
`ScenePhysicsHost3D(scene.world(), *engineHost.physicsQuery(), store, view,
permission)` after registering Entity physics components. The view callback
supplies a fresh ray and reach for each focus or activation; the permission
callback owns game rules. World object IDs used by this concrete adapter are
canonical 32-digit lowercase hexadecimal Entity persistent IDs. Published
objects carry a static 3D rigid-body and box collider description; run the
normal EntityPhysicsBridge pre-physics sync to make them physically queryable.
The bounding-sphere occupancy check is conservative, so a game needing exact
rotated-box placement can provide its own `IPlacementHost`.
