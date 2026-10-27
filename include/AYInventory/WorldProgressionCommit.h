#pragma once

#include <AYInventory/WorldCommit.h>
#include <AYProgression/Progression.h>

#include <functional>
#include <optional>

namespace ayt::inventory {

/// Game rule that binds a Fact to the exact staged Store and World ledger.
/// It must be read-only, deterministic for this command, and reject a missing
/// or mismatched item/object transition. The game chooses signal and subject.
using WorldFactCandidateRule = std::function<bool(const Store&,
    const app::WorldStateLedger&, const progression::Fact&, std::string&)>;

/// Optional same-slot progression section for WorldItemCommit. Keep the
/// adapter alive until its extension and restore callbacks have returned.
/// The caller supplies a fact with a stable source/sequence and a rule tying
/// it to the candidate transaction. No fact reaches the live Service before
/// the Store, World ledger and progression section have been saved together.
/// Unlike Service::recordFact's replay no-op, a new World transaction with an
/// already consumed source sequence is rejected; an exact transaction retry
/// is handled by WorldItemCommit before this adapter runs.
class WorldProgressionFactCommitAdapter {
public:
    WorldProgressionFactCommitAdapter(progression::Service& service,
        progression::Fact fact, std::string sectionName,
        WorldFactCandidateRule rule);

    /// Use as WorldItemCommit's extension, or compose with other extensions.
    [[nodiscard]] WorldCommitExtension extension();

    /// Register a validated progression section for staged slot restore.
    /// Pass publishRestoredChecked as restoreWorldItemCommitSlot's final checked
    /// publication callback. A failed restore never mutates the live Service.
    bool registerRestoreSection(app::SaveParticipantBundle& bundle,
                                std::string& error);
    bool publishRestoredChecked(std::string& error);

private:
    progression::Service& _service;
    progression::Fact _fact;
    std::string _sectionName;
    WorldFactCandidateRule _rule;
    std::optional<progression::Service> _candidate;

    bool registerSection(app::SaveParticipantBundle& bundle,
                         std::string& error);
};

} // namespace ayt::inventory
