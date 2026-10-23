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
   `consumeWorldPickup` must remove the drop atomically: on failure it leaves
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
   or leave it absent, and `abort` must remove staging. AYInventory then makes
   the item world-owned. `verifyPublished` compares the saved object and pose on
   an exact request retry. A save records both the Store's world owner ID and
   the game/Scene object's transform. Rebuild both under that same ID on load.

The current public physics query facade has ray and sphere overlap queries.
Sphere overlap is conservative for oriented boxes or irregular footprints.
The Host may use a more exact project-specific spatial query where required.
The module's sample Host demonstrates the protocol; it does not install a
concrete Scene/Physics bridge or write game-specific permission rules.
