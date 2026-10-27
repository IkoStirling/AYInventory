#pragma once

#include <AYApplication/WorldStateLedger.h>
#include <AYInventory/Inventory.h>
#include <AYProgression/Progression.h>

#include <functional>
#include <string>
#include <string_view>

namespace ayt::inventory {

enum class ClaimRewardError {
    None, Invalid, NotClaimable, Conflict, Inconsistent,
    GrantRejected, SaveRejected, Capacity, CallbackFailed
};

struct ClaimRewardResult {
    ClaimRewardError error = ClaimRewardError::None;
    bool replay = false;
    std::string detail;
    [[nodiscard]] explicit operator bool() const noexcept {
        return error == ClaimRewardError::None;
    }
};

/// Pure game-authored reward rule. It may modify only the candidate Store;
/// external effects require a separate durable outbox. A stable ClaimIntent
/// identifies the scope, definition, catalog and recurrence instance.
using GrantInventoryClaim = std::function<Result<void>(
    Store&, const progression::ClaimIntent&)>;

/// Save all three candidates in one failure-atomic complete game slot. Return
/// true only after durable commit; false/throw must leave the old slot intact.
/// The callback must not mutate or reenter the live owners.
using SaveInventoryClaim = std::function<bool(const Store&,
    const progression::Service&, const app::WorldStateLedger&)>;

/// Claim a completed task and grant its inventory reward with one durable
/// commit boundary. The key is a canonical bounded encoding of ClaimIntent;
/// rewardFingerprint is the game's stable description of this reward. An exact
/// saved retry returns replay without granting again. A previously claimed
/// task without the matching receipt is inconsistent and cannot be inferred
/// to have received a reward. Failure leaves all three live owners unchanged.
/// Run on their serialized simulation owner; the game must save every other
/// affected state in the same slot and restore into disposable candidates.
[[nodiscard]] ClaimRewardResult claimInventoryRewardWithSave(
    Store& store, progression::Service& progressionService,
    app::WorldStateLedger& ledger, std::string_view definitionId,
    std::string_view scopeId, std::string_view rewardFingerprint,
    const GrantInventoryClaim& grant, const SaveInventoryClaim& save) noexcept;

} // namespace ayt::inventory
