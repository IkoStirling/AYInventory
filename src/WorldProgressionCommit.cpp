#include <AYInventory/WorldProgressionCommit.h>

#include <AYProgression/SnapshotJson.h>

#include <utility>

namespace ayt::inventory {

WorldProgressionFactCommitAdapter::WorldProgressionFactCommitAdapter(
    progression::Service& service, progression::Fact fact,
    std::string sectionName, WorldFactCandidateRule rule)
    : _service(service), _fact(std::move(fact)),
      _sectionName(std::move(sectionName)), _rule(std::move(rule)) {}

WorldCommitExtension WorldProgressionFactCommitAdapter::extension()
{
    WorldCommitExtension result;
    result.prepareCandidate = [this](const Store& store,
        const app::WorldStateLedger& ledger, std::string& error) {
        _candidate.reset();
        if (!_rule || _sectionName.empty()) {
            error = "progression fact rule or section missing";
            return false;
        }
        if (!_rule(store, ledger, _fact, error)) return false;
        const auto snapshot = _service.snapshot();
        const auto prior = snapshot.factSequences.find(_fact.source);
        if (prior != snapshot.factSequences.end()
            && _fact.sequence <= prior->second) {
            error = "progression fact sequence was already consumed";
            return false;
        }
        _candidate.emplace(_service);
        const auto recorded = _candidate->recordFact(_fact);
        if (!recorded) {
            error = recorded.detail;
            _candidate.reset();
            return false;
        }
        return true;
    };
    result.registerCandidateSections = [this](
        app::SaveParticipantBundle& bundle, std::string& error) {
        if (!_candidate) {
            error = "progression candidate missing";
            return false;
        }
        return registerSection(bundle, error);
    };
    result.publishCandidate = [this](std::string& error) {
        if (!_candidate) {
            error = "progression candidate missing";
            return false;
        }
        _service.swapState(*_candidate);
        _candidate.reset();
        return true;
    };
    return result;
}

bool WorldProgressionFactCommitAdapter::registerSection(
    app::SaveParticipantBundle& bundle, std::string& error)
{
    if (_sectionName.empty() || !_candidate) {
        error = "progression section or candidate missing";
        return false;
    }
    app::SaveParticipantBundle::Participant section;
    section.version = 1;
    section.capture = [this](std::string& payload, std::string& detail) {
        if (!_candidate) {
            detail = "progression candidate missing";
            return false;
        }
        payload = progression::encodeSnapshotJson(_candidate->snapshot());
        return true;
    };
    section.validate = [this](std::uint32_t version,
        std::string_view payload, std::string& detail) {
        if (version != 1) {
            detail = "progression section version";
            return false;
        }
        progression::Snapshot snapshot;
        const auto decoded = progression::decodeSnapshotJson(payload, snapshot);
        if (!decoded) { detail = decoded.detail; return false; }
        auto probe = _service;
        const auto restored = probe.restore(snapshot);
        if (!restored) { detail = restored.detail; return false; }
        return true;
    };
    section.apply = [this](std::uint32_t,
        std::string_view payload, std::string& detail) {
        progression::Snapshot snapshot;
        const auto decoded = progression::decodeSnapshotJson(payload, snapshot);
        if (!decoded) { detail = decoded.detail; return false; }
        if (!_candidate) _candidate.emplace(_service);
        const auto restored = _candidate->restore(snapshot);
        if (!restored) { detail = restored.detail; return false; }
        return true;
    };
    return bundle.registerParticipant(_sectionName, std::move(section), &error);
}

bool WorldProgressionFactCommitAdapter::registerRestoreSection(
    app::SaveParticipantBundle& bundle, std::string& error)
{
    _candidate.emplace(_service);
    return registerSection(bundle, error);
}

bool WorldProgressionFactCommitAdapter::publishRestoredChecked(
    std::string& error)
{
    if (!_candidate) {
        error = "restored progression candidate missing";
        return false;
    }
    _service.swapState(*_candidate);
    _candidate.reset();
    return true;
}

} // namespace ayt::inventory
