#include <AYInventory/WorldProgressionCommit.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace ayt;
using namespace ayt::inventory;

namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class T, class E> void check(const std::expected<T, E>& value,
    const char* message) { check(value.has_value(), message); }
void check(const progression::Result& value, const char* message) {
    check(static_cast<bool>(value), message);
}

constexpr auto objectId = "11111111111111111111111111111111";
const Location world{Place::World, objectId, {}};
const Location bag{Place::Container, "bag", "slot"};

Store makeStore() {
    auto made = Store::create(1, {{"wood", 1}});
    check(made, "catalog");
    auto store = std::move(*made);
    check(store.addContainer({"bag", {"slot"}}), "bag");
    check(store.createItem({"wood-1", "wood", 1, world}), "drop");
    return store;
}

progression::Definition quest() {
    progression::Definition value;
    value.id = "collect-wood";
    value.objectives.push_back({"picked", progression::Input::Fact,
        "picked_world_item", objectId, 1});
    return value;
}

progression::Service makeProgression() {
    progression::Service service;
    check(service.install({quest()}), "progression catalog");
    check(service.start("collect-wood"), "progression start");
    return service;
}

struct Host final : IDurablePickupHost {
    bool present = true;
    Result<void> validateCurrent(const InteractionCandidate&,
        std::string_view) override {
        if (!present) return std::unexpected(Diagnostic{Error::Stale, "absent"});
        return {};
    }
    Result<void> authorize(const InteractionCandidate&,
        std::string_view) override { return {}; }
    Result<void> consumeWorldPickup(const InteractionCandidate&) override {
        return std::unexpected(Diagnostic{Error::Invalid, "wrong path"});
    }
    Result<void> cleanupCommittedPickup(const InteractionCandidate&) override {
        present = false;
        return {};
    }
};

void run() {
    namespace fs = std::filesystem;
    const fs::path root(AY_INVENTORY_WORLD_PROGRESSION_TEST_ROOT);
    std::error_code ec;
    fs::remove_all(root, ec);
    check(fs::create_directories(root, ec), "test root");
    app::SaveGameService saves({root.string(), "world-progression-test"});
    auto store = makeStore();
    auto progress = makeProgression();
    app::WorldStateLedger ledger;
    Host host;
    const InteractionCandidate pickup{InteractionCandidate::Kind::Pickup,
        objectId, "wood-1", 1.0};
    const progression::Fact fact{"pickup/player", 1,
        "picked_world_item", objectId, 1};
    bool allowFact = false;
    WorldProgressionFactCommitAdapter adapter(progress, fact, "progression",
        [&allowFact](const Store& candidate,
            const app::WorldStateLedger& candidateLedger,
            const progression::Fact& candidateFact, std::string& error) {
            const auto* item = candidate.findItem("wood-1");
            const auto record = candidateLedger.records().find(objectId);
            if (!allowFact || !item || item->location != bag
                || record == candidateLedger.records().end()
                || !record->second.deleted
                || candidateFact.subject != objectId) {
                error = "pickup fact does not match candidate";
                return false;
            }
            return true;
        });
    WorldItemCommit commit(store, ledger, saves, "slot");
    check(!commit.pickup(host, pickup, "player", bag,
        "pickup-1", "pickup/wood-1", adapter.extension()),
        "mismatched fact candidate rejected");
    check(store.findItem("wood-1")->location == world
        && progress.state("collect-wood")->status == progression::Status::Active
        && !ledger.hasReceipt("pickup-1") && host.present,
        "rule failure leaves all live owners untouched");

    allowFact = true;
    const auto blocked = root / "blocked";
    { std::ofstream file(blocked); file << "file"; }
    app::SaveGameService blockedSave({blocked.string(), "test"});
    WorldItemCommit blockedCommit(store, ledger, blockedSave, "slot");
    check(!blockedCommit.pickup(host, pickup, "player", bag,
        "pickup-1", "pickup/wood-1", adapter.extension()),
        "failed slot save rejected");
    check(store.findItem("wood-1")->location == world
        && progress.state("collect-wood")->status == progression::Status::Active
        && !ledger.hasReceipt("pickup-1") && host.present,
        "failed save leaves fact and ownership untouched");

    auto committed = commit.pickup(host, pickup, "player", bag,
        "pickup-1", "pickup/wood-1", adapter.extension());
    check(committed && committed->status == WorldCommitStatus::Committed,
        "pickup and fact committed");
    check(store.findItem("wood-1")->location == bag
        && progress.state("collect-wood")->status == progression::Status::Completed
        && ledger.hasReceipt("pickup-1") && !host.present,
        "fact, owner and tombstone published together");
    auto replay = commit.pickup(host, pickup, "player", bag,
        "pickup-1", "pickup/wood-1", adapter.extension());
    check(replay && replay->status == WorldCommitStatus::AlreadyCommitted,
        "exact replay does not redeliver fact");
    auto duplicateExtension = adapter.extension();
    std::string duplicateError;
    check(!duplicateExtension.prepareCandidate(store, ledger, duplicateError),
        "new transaction cannot reuse an older fact sequence");

    auto restoredStore = makeStore();
    auto restoredProgress = makeProgression();
    app::WorldStateLedger restoredLedger;
    WorldProgressionFactCommitAdapter restoreAdapter(restoredProgress, fact,
        "progression", [](const Store&, const app::WorldStateLedger&,
            const progression::Fact&, std::string&) { return true; });
    check(restoreWorldItemCommitSlot(restoredStore, restoredLedger,
        saves, "slot",
        [&restoreAdapter](app::SaveParticipantBundle& bundle,
            std::string& error) {
            return restoreAdapter.registerRestoreSection(bundle, error);
        }, {}, {}, [&restoreAdapter](std::string& error) {
            return restoreAdapter.publishRestoredChecked(error);
        }), "complete slot restore");
    check(restoredStore.findItem("wood-1")->location == bag
        && restoredLedger.hasReceipt("pickup-1")
        && restoredProgress.state("collect-wood")->status
            == progression::Status::Completed,
        "restored fact, owner and tombstone agree");

    auto wrongStore = makeStore();
    app::WorldStateLedger wrongLedger;
    progression::Service wrongProgress;
    auto different = quest();
    different.id = "different-catalog";
    check(wrongProgress.install({different}), "wrong catalog");
    WorldProgressionFactCommitAdapter wrongAdapter(wrongProgress, fact,
        "progression", [](const Store&, const app::WorldStateLedger&,
            const progression::Fact&, std::string&) { return true; });
    check(!restoreWorldItemCommitSlot(wrongStore, wrongLedger, saves, "slot",
        [&wrongAdapter](app::SaveParticipantBundle& bundle,
            std::string& error) {
            return wrongAdapter.registerRestoreSection(bundle, error);
        }, {}, {}, [&wrongAdapter](std::string& error) {
            return wrongAdapter.publishRestoredChecked(error);
        }), "mismatched progression catalog rejects whole slot");
    check(wrongStore.findItem("wood-1")->location == world
        && !wrongLedger.hasReceipt("pickup-1")
        && wrongProgress.state("different-catalog")->status
            == progression::Status::Available,
        "failed restore leaves all live owners untouched");
}
}

int main() {
    try {
        run();
        std::cout << "AYInventory: world progression fact commit passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "AYInventory: world progression fact commit failed: "
                  << ex.what() << '\n';
        return 1;
    }
}
