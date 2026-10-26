#include <AYInventory/WorldCommit.h>
#if defined(AY_WORLD_COMMIT_WITH_PROGRESSION)
#include <AYProgression/Progression.h>
#include <AYProgression/SnapshotJson.h>
#endif

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace ayt::inventory;

namespace {
void check(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

template<class T, class E> void check(const std::expected<T, E>& value,
                                      const char* message)
{
    check(value.has_value(), message);
}

#if defined(AY_WORLD_COMMIT_WITH_PROGRESSION)
void check(const ayt::progression::Result& value, const char* message)
{
    check(static_cast<bool>(value), message);
}
#endif

Store makeStore()
{
    auto created = Store::create(1, {{"wood", 1}});
    check(created, "catalog");
    auto store = std::move(*created);
    check(store.addContainer({"bag", {"slot"}}), "bag");
    return store;
}

struct PickupHost final : IDurablePickupHost {
    bool present = true;
    bool failCleanup = false;
    bool failValidation = false;
    int cleanups = 0;
    Result<void> validateCurrent(const InteractionCandidate&,
                                 std::string_view) override
    {
        if (failValidation || !present)
            return std::unexpected(Diagnostic{Error::Stale, "not current"});
        return {};
    }
    Result<void> authorize(const InteractionCandidate&,
                           std::string_view) override { return {}; }
    Result<void> consumeWorldPickup(const InteractionCandidate&) override
    { return std::unexpected(Diagnostic{Error::Invalid, "wrong path"}); }
    Result<void> cleanupCommittedPickup(const InteractionCandidate&) override
    {
        ++cleanups;
        if (failCleanup)
            return std::unexpected(Diagnostic{Error::Stale, "deferred"});
        present = false;
        return {};
    }
};

struct PlacementHost final : IPlacementHost {
    bool present = false;
    bool prepared = false;
    bool failPublish = false;
    int publications = 0;
    Result<void> verifyPublished(const PlacementRequest&,
                                 const Item&) override
    {
        if (!present)
            return std::unexpected(Diagnostic{Error::Stale, "absent"});
        return {};
    }
    Result<void> prepare(const PlacementRequest&, const Item&) override
    {
        if (present) return std::unexpected(Diagnostic{Error::Duplicate, "exists"});
        prepared = true;
        return {};
    }
    Result<void> publish() override
    {
        if (!prepared || failPublish)
            return std::unexpected(Diagnostic{Error::Stale, "publish fault"});
        present = true;
        prepared = false;
        ++publications;
        return {};
    }
    void abort() noexcept override { prepared = false; }
};

void run()
{
    namespace fs = std::filesystem;
    const auto root = fs::path(AY_INVENTORY_WORLD_COMMIT_TEST_ROOT);
    std::error_code ec;
    fs::remove_all(root, ec);
    check(fs::create_directories(root, ec), "test directory");
    ayt::app::SaveGameService saves({root.string(), "world-commit-test"});
    ayt::app::WorldStateLedger ledger;
    auto store = makeStore();
    constexpr auto objectId = "11111111111111111111111111111111";
    constexpr auto placedId = "22222222222222222222222222222222";
    const Location world{Place::World, objectId, {}};
    const Location bag{Place::Container, "bag", "slot"};
    check(store.createItem({"wood-1", "wood", 1, world}), "world item");
    const InteractionCandidate pickup{InteractionCandidate::Kind::Pickup,
                                       objectId, "wood-1", 1.0};
    PickupHost pickupHost;

    // An unavailable slot leaves every active participant and projection old.
    const auto blocked = root / "blocked";
    { std::ofstream output(blocked); output << "file"; }
    ayt::app::SaveGameService blockedSave({blocked.string(), "test"});
    WorldItemCommit unavailable(store, ledger, blockedSave, "slot");
    check(!unavailable.pickup(pickupHost, pickup, "player", bag,
                              "pickup-1", "pickup/wood-1/bag"), "save fault");
    check(store.findItem("wood-1")->location == world && pickupHost.present
          && !ledger.hasReceipt("pickup-1"), "save fault atomicity");

    WorldItemCommit commit(store, ledger, saves, "slot");
    pickupHost.failCleanup = true;
    auto picked = commit.pickup(pickupHost, pickup, "player", bag,
                                "pickup-1", "pickup/wood-1/bag");
    check(picked && picked->status == WorldCommitStatus::ProjectionPending,
          "saved pickup reports projection debt");
    check(store.findItem("wood-1")->location == bag && pickupHost.present
          && ledger.records().at(objectId).deleted, "durable pickup authority");
    check(!commit.pickup(pickupHost, pickup, "player", bag,
                         "pickup-1", "changed-fingerprint"), "conflicting retry");

    // A new process reconstructs value state from the actual slot, then
    // removes a stale authored drop using the saved tombstone.
    auto restoredStore = makeStore();
    check(restoredStore.createItem({"wood-1", "wood", 1, world}),
          "authored item before restore");
    ayt::app::WorldStateLedger restoredLedger;
    check(restoreWorldItemCommitSlot(restoredStore, restoredLedger, saves, "slot"),
          "load complete slot");
    check(restoredStore.findItem("wood-1")->location == bag
          && restoredLedger.hasReceipt("pickup-1"), "restart pickup state");
    WorldItemCommit resumed(restoredStore, restoredLedger, saves, "slot");
    pickupHost.failCleanup = false;
    check(resumed.reconcileDeleted(pickupHost, objectId), "restart cleanup");
    check(!pickupHost.present, "tombstone removed projection");
    auto replay = resumed.pickup(pickupHost, pickup, "player", bag,
                                 "pickup-1", "pickup/wood-1/bag");
    check(replay && replay->status == WorldCommitStatus::AlreadyCommitted,
          "exact pickup replay");

    PlacementHost placementHost;
    PlacementRequest placement{placedId, {}};
    placement.pose.position = {3.0, 0.0, 0.0};
    placementHost.failPublish = true;
    auto placed = resumed.place(placementHost, "wood-1", bag, placement,
                                "place-1", "place/wood-1/3");
    check(placed && placed->status == WorldCommitStatus::ProjectionPending,
          "saved placement reports projection debt");
    check(restoredStore.findItem("wood-1")->location
              == Location{Place::World, placedId, {}}
          && !placementHost.present, "saved placement is authoritative");
    auto pendingReplay = resumed.place(placementHost, "wood-1", bag,
        placement, "place-1", "place/wood-1/3");
    check(pendingReplay
          && pendingReplay->status == WorldCommitStatus::ProjectionPending,
          "exact retry preserves unresolved projection debt");

    auto finalStore = makeStore();
    check(finalStore.createItem({"wood-1", "wood", 1, bag}), "restart bag item");
    ayt::app::WorldStateLedger finalLedger;
    check(restoreWorldItemCommitSlot(finalStore, finalLedger, saves, "slot"),
          "restart placed slot");
    WorldItemCommit finalCommit(finalStore, finalLedger, saves, "slot");
    placementHost.failPublish = false;
    check(finalCommit.reconcilePlaced(placementHost, placedId),
          "rebuild placed object");
    check(placementHost.present && placementHost.publications == 1,
          "one projected object");
    check(finalCommit.reconcilePlaced(placementHost, placedId),
          "idempotent projection reconcile");
    check(placementHost.publications == 1, "no duplicate world object");
    auto placedReplay = finalCommit.place(placementHost, "wood-1", bag,
        placement, "place-1", "place/wood-1/3");
    check(placedReplay
          && placedReplay->status == WorldCommitStatus::AlreadyCommitted,
          "exact placement replay");
    check(!finalCommit.place(placementHost, "wood-1", bag,
        placement, "place-1", "other-placement"), "placement conflict");

    // A game-owned fact can share the exact durable payload and is not
    // published if saving fails. A post-save publish fault forces reload.
    auto factStore = makeStore();
    check(factStore.createItem({"wood-2", "wood", 1, world}), "fact world item");
    ayt::app::WorldStateLedger factLedger;
    PickupHost factHost;
    int activeFact = 0;
    const WorldCommitExtension factExtension{
        [](ayt::app::SaveParticipantBundle& bundle, std::string& error) {
            ayt::app::SaveParticipantBundle::Participant fact;
            fact.version = 1;
            fact.capture = [](std::string& payload, std::string&) {
                payload = "1";
                return true;
            };
            fact.validate = [](std::uint32_t version, std::string_view payload,
                               std::string&) { return version == 1 && payload == "1"; };
            fact.apply = [](std::uint32_t, std::string_view, std::string&) {
                return true;
            };
            return bundle.registerParticipant("progression", std::move(fact), &error);
        },
        [&activeFact](std::string&) { activeFact = 1; return true; }};
    const InteractionCandidate factPickup{InteractionCandidate::Kind::Pickup,
                                           objectId, "wood-2", 1.0};
    WorldItemCommit factCommit(factStore, factLedger, saves, "fact-slot");
    check(factCommit.pickup(factHost, factPickup, "player", bag,
                            "fact-pickup", "fact/wood-2", factExtension),
          "fact and item commit");
    check(activeFact == 1 && factLedger.hasReceipt("fact-pickup"),
          "fact and receipt published together");
    auto factRestoredStore = makeStore();
    check(factRestoredStore.createItem({"wood-2", "wood", 1, world}),
          "fact restore template");
    ayt::app::WorldStateLedger factRestoredLedger;
    int restoredFact = 0;
    int stagedFact = 0;
    check(restoreWorldItemCommitSlot(factRestoredStore, factRestoredLedger,
        saves, "fact-slot",
        [&stagedFact](ayt::app::SaveParticipantBundle& bundle,
                        std::string& error) {
            ayt::app::SaveParticipantBundle::Participant fact;
            fact.version = 1;
            fact.capture = [](std::string& payload, std::string&) {
                payload = "0";
                return true;
            };
            fact.validate = [](std::uint32_t version, std::string_view payload,
                               std::string&) { return version == 1 && payload == "1"; };
            fact.apply = [&stagedFact](std::uint32_t,
                std::string_view, std::string&) {
                stagedFact = 1;
                return true;
            };
            return bundle.registerParticipant("progression", std::move(fact), &error);
        }, [&] { restoredFact = stagedFact; }), "restore extra fact section");
    check(restoredFact == 1 && factRestoredLedger.hasReceipt("fact-pickup")
          && factRestoredStore.findItem("wood-2")->location == bag,
          "fact, receipt and ownership survive restart");

    auto craftCatalog = [] {
        auto made = Store::create(2, {{"wood", 10}, {"campfire", 1}});
        check(made, "craft catalog");
        auto value = std::move(*made);
        check(value.addContainer({"craft-bag", {"a"}}), "craft bag");
        return value;
    };
    auto craftStore = craftCatalog();
    const Location craftBag{Place::Container, "craft-bag", "a"};
    check(craftStore.createItem({"wood-3", "wood", 3, craftBag}),
          "craft material");
    ayt::app::WorldStateLedger craftLedger;
    WorldItemCommit craftCommit(craftStore, craftLedger, saves, "craft-slot");
    Recipe recipe{"campfire-v1", {{"wood", 2}}, "campfire", 1};
    PlacementRequest craftRequest{"33333333333333333333333333333333", {}};
    craftRequest.pose.position = {5.0, 0.0, 0.0};
    PlacementHost craftHost;
    WorldItemCommit blockedCraft(craftStore, craftLedger, blockedSave, "craft-slot");
    check(!blockedCraft.craftAndPlace(craftHost, recipe, "craft-bag",
        "craft-1", "campfire-1", craftRequest, "craft/campfire/5"),
        "craft save fault");
    check(craftStore.findItem("wood-3")->quantity == 3
          && !craftStore.findItem("campfire-1")
          && !craftLedger.hasReceipt("craft-1")
          && !craftHost.prepared && !craftHost.present,
          "craft save fault leaves all live states old");
    craftHost.failPublish = true;
    auto crafted = craftCommit.craftAndPlace(craftHost, recipe, "craft-bag",
        "craft-1", "campfire-1", craftRequest, "craft/campfire/5");
    check(crafted && crafted->status == WorldCommitStatus::ProjectionPending,
          "durable craft projection pending");
    check(craftStore.findItem("wood-3")->quantity == 1
          && craftStore.findItem("campfire-1")->location
              == Location{Place::World, craftRequest.worldObjectId, {}}
          && craftStore.findCraftReceipt("craft-1"),
          "craft materials and output committed together");
    auto pendingCraftReplay = craftCommit.craftAndPlace(craftHost, recipe,
        "craft-bag", "craft-1", "campfire-1", craftRequest,
        "craft/campfire/5");
    check(pendingCraftReplay
          && pendingCraftReplay->status == WorldCommitStatus::ProjectionPending,
          "craft retry keeps projection pending without new deduction");
    auto craftRestored = craftCatalog();
    ayt::app::WorldStateLedger craftRestoredLedger;
    check(restoreWorldItemCommitSlot(craftRestored, craftRestoredLedger,
        saves, "craft-slot"), "restart craft slot");
    WorldItemCommit craftResumed(craftRestored, craftRestoredLedger,
                                  saves, "craft-slot");
    craftHost.failPublish = false;
    check(craftResumed.reconcilePlaced(craftHost, craftRequest.worldObjectId),
          "restart craft World output");
    check(craftRestored.findItem("wood-3")->quantity == 1
          && craftHost.publications == 1, "craft does not spend twice");
    auto craftReplay = craftResumed.craftAndPlace(craftHost, recipe,
        "craft-bag", "craft-1", "campfire-1", craftRequest,
        "craft/campfire/5");
    check(craftReplay
          && craftReplay->status == WorldCommitStatus::AlreadyCommitted
          && craftRestored.findItem("wood-3")->quantity == 1,
          "craft retry does not consume material");

    auto reloadStore = makeStore();
    check(reloadStore.createItem({"wood-4", "wood", 1, world}),
          "reload template item");
    ayt::app::WorldStateLedger reloadLedger;
    PickupHost reloadHost;
    WorldItemCommit reloadCommit(reloadStore, reloadLedger, saves,
                                  "reload-slot");
    const InteractionCandidate reloadPickup{InteractionCandidate::Kind::Pickup,
                                             objectId, "wood-4", 1.0};
    WorldCommitExtension failedPublication;
    failedPublication.registerCandidateSections =
        factExtension.registerCandidateSections;
    failedPublication.publishCandidate = [](std::string& error) {
        error = "game state swap failed";
        return false;
    };
    auto reloadResult = reloadCommit.pickup(reloadHost, reloadPickup, "player",
        bag, "reload-pickup", "reload/wood-4", failedPublication);
    check(reloadResult
          && reloadResult->status == WorldCommitStatus::ReloadRequired
          && reloadCommit.requiresReload() && reloadHost.present,
          "post-save game publish fault blocks live projection");
    check(!reloadCommit.pickup(reloadHost, reloadPickup, "player", bag,
        "new-request", "reload/wood-4"), "coordinator refuses stale live state");
    auto recoveredStore = makeStore();
    check(recoveredStore.createItem({"wood-4", "wood", 1, world}),
          "recovery template item");
    ayt::app::WorldStateLedger recoveredLedger;
    int recoveredFact = 0;
    int stagedRecoveryFact = 0;
    check(restoreWorldItemCommitSlot(recoveredStore, recoveredLedger,
        saves, "reload-slot",
        [&stagedRecoveryFact](ayt::app::SaveParticipantBundle& bundle,
                              std::string& error) {
            ayt::app::SaveParticipantBundle::Participant fact;
            fact.version = 1;
            fact.capture = [](std::string& payload, std::string&) {
                payload = "0"; return true;
            };
            fact.validate = [](std::uint32_t version, std::string_view payload,
                               std::string&) { return version == 1 && payload == "1"; };
            fact.apply = [&stagedRecoveryFact](std::uint32_t,
                std::string_view, std::string&) {
                stagedRecoveryFact = 1; return true;
            };
            return bundle.registerParticipant("progression", std::move(fact), &error);
        }, [&] { recoveredFact = stagedRecoveryFact; }),
        "reload after publish fault");
    WorldItemCommit recovered(recoveredStore, recoveredLedger, saves,
                               "reload-slot");
    check(recovered.reconcileDeleted(reloadHost, objectId),
          "finish projection after full reload");
    check(recoveredStore.findItem("wood-4")->location == bag
          && !reloadHost.present && recoveredFact == 1,
          "recovered single owner, game fact and no old object");

    check(static_cast<bool>(saves.save("foreign-slot", 1,
        R"({"version":1,"sections":{"foreign":{"version":1,"payloadJson":"{}"}}})")),
        "foreign slot setup");
    auto foreignStore = makeStore();
    check(foreignStore.createItem({"wood-5", "wood", 1, world}),
          "foreign template item");
    ayt::app::WorldStateLedger foreignLedger;
    PickupHost foreignHost;
    WorldItemCommit foreignCommit(foreignStore, foreignLedger, saves,
                                   "foreign-slot");
    const InteractionCandidate foreignPickup{InteractionCandidate::Kind::Pickup,
                                              objectId, "wood-5", 1.0};
    check(!foreignCommit.pickup(foreignHost, foreignPickup, "player", bag,
        "foreign-pickup", "foreign/wood-5"),
        "existing game section cannot be silently dropped");
    check(foreignStore.findItem("wood-5")->location == world
          && foreignHost.present && !foreignLedger.hasReceipt("foreign-pickup"),
          "foreign slot refusal keeps active states old");
#if defined(AY_WORLD_COMMIT_WITH_PROGRESSION)
    namespace progression = ayt::progression;
    progression::Definition quest;
    quest.id = "collect-wood";
    quest.objectives.push_back({"picked", progression::Input::Fact,
                                "pickup_ready", objectId, 1});
    progression::Service activeProgress;
    check(activeProgress.install({quest}), "progression catalog");
    check(activeProgress.start(quest.id), "progression start");
    check(activeProgress.recordFact({"world", 1, "pickup_ready", objectId, 1}),
          "progression fact");
    check(activeProgress.state(quest.id)->status
              == progression::Status::Completed, "quest completed before claim");
    progression::Service stagedProgress;
    check(stagedProgress.install({quest}), "staged progression catalog");
    check(stagedProgress.restore(activeProgress.snapshot()),
          "stage progression snapshot");
    check(stagedProgress.claim(quest.id), "stage quest reward claim");
    auto progressionStore = makeStore();
    check(progressionStore.createItem({"wood-6", "wood", 1, world}),
          "progression world item");
    ayt::app::WorldStateLedger progressionLedger;
    PickupHost progressionHost;
    const InteractionCandidate progressionPickup{
        InteractionCandidate::Kind::Pickup, objectId, "wood-6", 1.0};
    WorldCommitExtension progressionExtension;
    progressionExtension.registerCandidateSections =
        [&stagedProgress](ayt::app::SaveParticipantBundle& bundle,
                          std::string& error) {
            ayt::app::SaveParticipantBundle::Participant section;
            section.version = 1;
            section.capture = [&stagedProgress](std::string& payload,
                                                std::string&) {
                payload = progression::encodeSnapshotJson(
                    stagedProgress.snapshot());
                return true;
            };
            section.validate = [](std::uint32_t, std::string_view,
                                  std::string&) { return true; };
            section.apply = [](std::uint32_t, std::string_view,
                               std::string&) { return true; };
            return bundle.registerParticipant("progression", std::move(section),
                                              &error);
        };
    progressionExtension.publishCandidate = [&activeProgress, &stagedProgress]
        (std::string&) { activeProgress = std::move(stagedProgress); return true; };
    WorldItemCommit blockedProgression(progressionStore, progressionLedger,
                                       blockedSave, "progression-slot");
    check(!blockedProgression.pickup(progressionHost, progressionPickup,
        "player", bag, "pickup-quest", "pickup/quest/wood-6",
        progressionExtension), "progression save fault");
    check(activeProgress.state(quest.id)->status
              == progression::Status::Completed
          && progressionStore.findItem("wood-6")->location == world
          && !progressionLedger.hasReceipt("pickup-quest"),
          "failed slot leaves quest, item and receipt old");
    WorldItemCommit progressionCommit(progressionStore, progressionLedger,
                                      saves, "progression-slot");
    check(progressionCommit.pickup(progressionHost, progressionPickup,
        "player", bag, "pickup-quest", "pickup/quest/wood-6",
        progressionExtension), "quest and pickup commit");
    check(activeProgress.state(quest.id)->status
              == progression::Status::Claimed
          && progressionStore.findItem("wood-6")->location == bag
          && progressionLedger.hasReceipt("pickup-quest"),
          "quest, item, World and receipt share one commit");
    auto progressionRestoredStore = makeStore();
    check(progressionRestoredStore.createItem({"wood-6", "wood", 1, world}),
          "quest restore template");
    ayt::app::WorldStateLedger progressionRestoredLedger;
    progression::Service stagedRestoredProgress;
    check(stagedRestoredProgress.install({quest}), "quest restore catalog");
    progression::Service publishedRestoredProgress;
    check(publishedRestoredProgress.install({quest}), "quest published catalog");
    check(restoreWorldItemCommitSlot(progressionRestoredStore,
        progressionRestoredLedger, saves, "progression-slot",
        [&stagedRestoredProgress, &quest](ayt::app::SaveParticipantBundle& bundle,
                                         std::string& error) {
            ayt::app::SaveParticipantBundle::Participant section;
            section.version = 1;
            section.capture = [](std::string& payload, std::string&) {
                payload = "{}"; return true;
            };
            section.validate = [&quest](std::uint32_t version,
                std::string_view payload, std::string& detail) {
                if (version != 1) { detail = "quest section version"; return false; }
                progression::Snapshot snapshot;
                if (!progression::decodeSnapshotJson(payload, snapshot)) {
                    detail = "quest decode"; return false;
                }
                progression::Service scratch;
                if (!scratch.install({quest}) || !scratch.restore(snapshot)) {
                    detail = "quest restore"; return false;
                }
                return true;
            };
            section.apply = [&stagedRestoredProgress](std::uint32_t,
                std::string_view payload, std::string& detail) {
                progression::Snapshot snapshot;
                if (!progression::decodeSnapshotJson(payload, snapshot)
                    || !stagedRestoredProgress.restore(snapshot)) {
                    detail = "staged quest restore"; return false;
                }
                return true;
            };
            return bundle.registerParticipant("progression", std::move(section),
                                              &error);
        }, [&] { publishedRestoredProgress = std::move(stagedRestoredProgress); }),
        "quest and item restart");
    check(publishedRestoredProgress.state(quest.id)->status
              == progression::Status::Claimed
          && progressionRestoredStore.findItem("wood-6")->location == bag
          && progressionRestoredLedger.hasReceipt("pickup-quest"),
          "restarted quest and pickup remain claimed exactly once");
#endif
    fs::remove_all(root, ec);
}
} // namespace

int main()
{
    try {
        run();
        std::cout << "AYInventory: durable World item commit passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "AYInventory World commit: " << ex.what() << '\n';
        return 1;
    }
}
