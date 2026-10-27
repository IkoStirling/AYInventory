#include <AYInventory/ClaimRewardCommit.h>

#include <new>

namespace ayt::inventory {
namespace {
std::string claimKey(const progression::ClaimIntent& intent) {
    std::string key = "claim:v1:";
    key += std::to_string(intent.scopeId.size());
    key += ':';
    key += intent.scopeId;
    key += ':';
    key += std::to_string(intent.definitionId.size());
    key += ':';
    key += intent.definitionId;
    key += ':';
    key += std::to_string(intent.catalogSignature);
    key += ':';
    key += std::to_string(intent.periodStart);
    return key;
}
}

ClaimRewardResult claimInventoryRewardWithSave(
    Store& store, progression::Service& progressionService,
    app::WorldStateLedger& ledger, std::string_view definitionId,
    std::string_view scopeId, std::string_view rewardFingerprint,
    const GrantInventoryClaim& grant, const SaveInventoryClaim& save) noexcept {
    if (definitionId.empty() || scopeId.empty() || rewardFingerprint.empty() ||
        rewardFingerprint.size() > 256 || !grant || !save)
        return {ClaimRewardError::Invalid, false, "claim arguments"};
    try {
        const auto* state = progressionService.state(definitionId);
        if (!state || (state->status != progression::Status::Completed &&
                       state->status != progression::Status::Claimed))
            return {ClaimRewardError::NotClaimable, false, "status"};
        progression::ClaimIntent intent{std::string(scopeId),
            std::string(definitionId), progressionService.catalogSignature(),
            state->periodStart};
        auto key = claimKey(intent);
        if (key.size() > 256)
            return {ClaimRewardError::Capacity, false, "receipt key"};

        app::WorldStateLedger stagedLedger = ledger;
        const auto receipt = stagedLedger.claimReceiptWithFingerprint(
            std::move(key), std::string(rewardFingerprint));
        using Receipt = app::WorldStateLedger::ReceiptClaimResult;
        if (receipt == Receipt::Conflict)
            return {ClaimRewardError::Conflict, false, "receipt fingerprint"};
        if (receipt == Receipt::Invalid)
            return {ClaimRewardError::Invalid, false, "receipt"};
        if (receipt == Receipt::AllocationFailure)
            return {ClaimRewardError::Capacity, false, "receipt"};
        if (receipt == Receipt::Replay)
            return state->status == progression::Status::Claimed
                ? ClaimRewardResult{ClaimRewardError::None, true, {}}
                : ClaimRewardResult{ClaimRewardError::Inconsistent, false,
                                    "receipt exists before claim status"};
        if (state->status == progression::Status::Claimed)
            return {ClaimRewardError::Inconsistent, false,
                    "claim status lacks receipt"};

        Store stagedStore = store;
        progression::Service stagedProgression = progressionService;
        const auto granted = grant(stagedStore, intent);
        if (!granted)
            return {ClaimRewardError::GrantRejected, false,
                    granted.error().message};
        const auto claimed = stagedProgression.claim(definitionId);
        if (!claimed)
            return {ClaimRewardError::Inconsistent, false, claimed.detail};
        if (!save(stagedStore, stagedProgression, stagedLedger))
            return {ClaimRewardError::SaveRejected, false, "slot"};

        store.swap(stagedStore);
        progressionService.swapState(stagedProgression);
        ledger.swap(stagedLedger);
        return {};
    } catch (const std::bad_alloc&) {
        return {ClaimRewardError::Capacity, false, "allocation"};
    } catch (...) {
        return {ClaimRewardError::CallbackFailed, false, "callback"};
    }
}

} // namespace ayt::inventory
