#include <AYInventory/ClaimRewardCommit.h>
#include <AYInventory/SnapshotJson.h>
#include <AYProgression/SnapshotJson.h>
#include <AYApplication/SaveGameService.h>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

using namespace ayt;

namespace {
void check(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}

inventory::Store makeStore() {
    auto created = inventory::Store::create(1, {{"badge", 1}});
    check(created.has_value(), "create catalog");
    auto store = std::move(*created);
    check(store.addContainer({"bag", {"first", "second"}}).has_value(),
          "register reward slots");
    return store;
}

progression::Definition quest() {
    progression::Definition value;
    value.id = "daily.win";
    value.objectives.push_back({"wins", progression::Input::Metric,
                                "match_win", "", 1});
    value.schedule = {progression::Clock::Game,
                      progression::Cadence::Daily, 0};
    return value;
}

void complete(progression::Service& service, std::int64_t time) {
    service.advanceTime(time, 0);
    check(bool(service.start("daily.win")), "start quest");
    check(bool(service.recordMetric("match_win")), "complete quest");
    check(service.state("daily.win")->status ==
          progression::Status::Completed, "completed status");
}

void saved_claim_reloads_and_replays_once() {
    auto store = makeStore();
    progression::Service progress;
    check(bool(progress.install({quest()})), "install quest");
    complete(progress, 10);
    app::WorldStateLedger ledger;
    app::SaveGameService saves({AY_INVENTORY_CLAIM_TEST_ROOT, "claim-test"});
    int grants = 0;
    bool checkFirstStage = true;
    auto grant = [&](inventory::Store& candidate,
                     const progression::ClaimIntent& intent)
        -> inventory::Result<void> {
        ++grants;
        const auto slot = intent.periodStart == 0 ? "first" : "second";
        return candidate.createItem({"badge." + std::to_string(intent.periodStart),
            "badge", 1, {inventory::Place::Container, "bag", slot}});
    };
    auto save = [&](const inventory::Store& items,
                    const progression::Service& quests,
                    const app::WorldStateLedger& receipts) {
        if (checkFirstStage) {
            check(store.findItem("badge.0") == nullptr &&
                  progress.state("daily.win")->status ==
                      progression::Status::Completed,
                  "live owners unpublished during save");
            checkFirstStage = false;
        }
        const auto itemJson = inventory::encodeSnapshotJson(items.snapshot());
        std::string ledgerJson;
        if (!itemJson || !receipts.encode(ledgerJson)) return false;
        nlohmann::json payload;
        payload["items"] = nlohmann::json::parse(*itemJson);
        payload["quests"] = nlohmann::json::parse(
            progression::encodeSnapshotJson(quests.snapshot()));
        payload["receipts"] = nlohmann::json::parse(ledgerJson);
        return bool(saves.save("claim", 1, payload.dump()));
    };
    const auto first = inventory::claimInventoryRewardWithSave(store,
        progress, ledger, "daily.win", "player.1", "badge-v1", grant, save);
    check(bool(first) && !first.replay, "first claim committed");
    check(grants == 1 && store.findItem("badge.0") != nullptr,
          "one inventory grant");
    check(progress.state("daily.win")->status ==
          progression::Status::Claimed, "claim status saved");
    const auto retry = inventory::claimInventoryRewardWithSave(store,
        progress, ledger, "daily.win", "player.1", "badge-v1", grant, save);
    check(bool(retry) && retry.replay && grants == 1, "exact replay is inert");
    const auto conflict = inventory::claimInventoryRewardWithSave(store,
        progress, ledger, "daily.win", "player.1", "badge-v2", grant, save);
    check(conflict.error == inventory::ClaimRewardError::Conflict,
          "changed reward fingerprint conflicts");

    const auto disk = saves.load("claim", 1);
    check(bool(disk), "durable slot loads");
    const auto payload = nlohmann::json::parse(disk.payloadJson);
    auto restoredStore = makeStore();
    auto restoredProgress = progression::Service{};
    check(bool(restoredProgress.install({quest()})), "restore catalog");
    app::WorldStateLedger restoredLedger;
    auto itemSnapshot = inventory::decodeSnapshotJson(
        payload.at("items").dump());
    check(itemSnapshot.has_value(), "decode durable items");
    check(restoredStore.restore(*itemSnapshot).has_value(),
          "restore durable items");
    progression::Snapshot questSnapshot;
    check(bool(progression::decodeSnapshotJson(
        payload.at("quests").dump(), questSnapshot)), "decode durable quest");
    check(bool(restoredProgress.restore(questSnapshot)),
          "restore durable quest");
    check(restoredLedger.decode(payload.at("receipts").dump()),
          "restore durable receipt");
    const auto replayAfterLoad = inventory::claimInventoryRewardWithSave(
        restoredStore, restoredProgress, restoredLedger, "daily.win",
        "player.1", "badge-v1", grant, save);
    check(bool(replayAfterLoad) && replayAfterLoad.replay && grants == 1,
          "replay after restart never grants again");

    complete(restoredProgress, 86410);
    const auto second = inventory::claimInventoryRewardWithSave(
        restoredStore, restoredProgress, restoredLedger, "daily.win",
        "player.1", "badge-v1", grant, save);
    check(bool(second) && !second.replay && grants == 2,
          "next recurrence has a distinct receipt");
    check(restoredStore.findItem("badge.86400") != nullptr,
          "next occurrence granted separately");
}

void failed_grant_or_save_does_not_publish() {
    auto store = makeStore();
    progression::Service progress;
    check(bool(progress.install({quest()})), "install failure quest");
    complete(progress, 10);
    app::WorldStateLedger ledger;
    const auto rejected = inventory::claimInventoryRewardWithSave(store,
        progress, ledger, "daily.win", "player.1", "badge-v1",
        [](inventory::Store& candidate, const progression::ClaimIntent&)
            -> inventory::Result<void> {
            auto added = candidate.createItem({"badge.0", "badge", 1,
                {inventory::Place::Container, "bag", "first"}});
            if (!added) return added;
            return std::unexpected(inventory::Diagnostic{
                inventory::Error::Denied, "game rule"});
        }, [](const auto&, const auto&, const auto&) { return true; });
    check(rejected.error == inventory::ClaimRewardError::GrantRejected,
          "grant rejection reported");
    check(store.findItem("badge.0") == nullptr &&
          progress.state("daily.win")->status ==
              progression::Status::Completed, "failed grant rolls back owners");

    int grants = 0;
    auto grant = [&](inventory::Store& candidate,
                     const progression::ClaimIntent&) {
        ++grants;
        return candidate.createItem({"badge.0", "badge", 1,
            {inventory::Place::Container, "bag", "first"}});
    };
    const auto unsaved = inventory::claimInventoryRewardWithSave(store,
        progress, ledger, "daily.win", "player.1", "badge-v1", grant,
        [](const auto&, const auto&, const auto&) { return false; });
    check(unsaved.error == inventory::ClaimRewardError::SaveRejected,
          "save failure reported");
    check(store.findItem("badge.0") == nullptr &&
          progress.state("daily.win")->status ==
              progression::Status::Completed, "failed save rolls back owners");
    const auto accepted = inventory::claimInventoryRewardWithSave(store,
        progress, ledger, "daily.win", "player.1", "badge-v1", grant,
        [](const auto&, const auto&, const auto&) { return true; });
    check(bool(accepted) && grants == 2, "retry grants once to live Store");
    check(store.findItem("badge.0") != nullptr, "retry published item");

    auto unrelated = makeStore();
    progression::Service alreadyClaimed;
    check(bool(alreadyClaimed.install({quest()})), "install claimed quest");
    complete(alreadyClaimed, 10);
    check(bool(alreadyClaimed.claim("daily.win")), "external claim");
    app::WorldStateLedger noReceipt;
    const auto inconsistent = inventory::claimInventoryRewardWithSave(
        unrelated, alreadyClaimed, noReceipt, "daily.win", "player.1",
        "badge-v1", grant,
        [](const auto&, const auto&, const auto&) { return true; });
    check(inconsistent.error == inventory::ClaimRewardError::Inconsistent &&
          unrelated.findItem("badge.0") == nullptr,
          "claim without matching reward receipt rejected");
}
}

int main() {
    saved_claim_reloads_and_replays_once();
    failed_grant_or_save_does_not_publish();
    std::cout << "ClaimRewardCommitTest passed\n";
}
