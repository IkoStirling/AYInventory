#include <AYInventory/WorldEquipmentCommit.h>

#include <AYAbility/Authoring.h>
#include <AYStats/AssetIO.h>

#include <exception>
#include <utility>

namespace ayt::inventory {

WorldEquipmentCommitAdapter::WorldEquipmentCommitAdapter(
    stats::StatSet& stats, ability::Runtime& ability,
    EquipmentProjection& projection, std::string actorId,
    std::string selectedItemId, std::string sectionPrefix,
    ModifierFactory modifierFactory)
    : _stats(stats), _ability(ability), _projection(projection),
      _actorId(std::move(actorId)),
      _selectedItemId(std::move(selectedItemId)),
      _sectionPrefix(std::move(sectionPrefix)),
      _modifierFactory(std::move(modifierFactory)) {}

WorldCommitExtension WorldEquipmentCommitAdapter::extension()
{
    WorldCommitExtension result;
    result.prepareCandidate = [this](const Store& store,
        const app::WorldStateLedger&, std::string& error) {
        try {
            _candidateStats = _stats;
            _candidateAbility = _ability;
            _candidateProjection = _projection;
            const auto synced = _candidateProjection->sync(store, _actorId,
                _selectedItemId, *_candidateStats, *_candidateAbility,
                _modifierFactory);
            if (!synced) { error = synced.error().message; return false; }
            return true;
        } catch (const std::exception& ex) {
            error = ex.what(); return false;
        }
    };
    result.registerCandidateSections = [this](app::SaveParticipantBundle& bundle,
                                               std::string& error) {
        return registerSections(bundle, error, false);
    };
    result.publishCandidate = [this](std::string& error) {
        if (!_candidateStats || !_candidateAbility || !_candidateProjection) {
            error = "equipment candidate missing"; return false;
        }
        if (!_ability.swapState(*_candidateAbility)) {
            error = "Ability runtime is committing"; return false;
        }
        using std::swap;
        swap(_stats, *_candidateStats);
        _projection.swapState(*_candidateProjection);
        return true;
    };
    return result;
}

bool WorldEquipmentCommitAdapter::registerSections(
    app::SaveParticipantBundle& bundle, std::string& error, bool restoring)
{
    if (_sectionPrefix.empty() || _actorId.empty() || !_modifierFactory) {
        error = "incomplete equipment adapter"; return false;
    }
    if (restoring) {
        _candidateStats = _stats;
        _candidateAbility = _ability;
        _candidateProjection.reset();
    } else if (!_candidateStats || !_candidateAbility || !_candidateProjection) {
        error = "equipment candidate was not prepared"; return false;
    }
    app::SaveParticipantBundle::Participant statsSection;
    statsSection.version = 1;
    statsSection.capture = [this](std::string& payload, std::string& detail) {
        if (!_candidateStats) { detail = "Stats candidate missing"; return false; }
        auto encoded = stats::encodeState(*_candidateStats);
        if (!encoded) { detail = encoded.error().message; return false; }
        payload = std::move(*encoded);
        return true;
    };
    statsSection.validate = [this](std::uint32_t version,
        std::string_view payload, std::string& detail) {
        if (version != 1) { detail = "Stats section version"; return false; }
        auto decoded = stats::decodeState(payload, _stats.schema());
        if (!decoded) { detail = decoded.error().message; return false; }
        return true;
    };
    statsSection.apply = [this](std::uint32_t, std::string_view payload,
        std::string& detail) {
        auto decoded = stats::decodeState(payload, _stats.schema());
        if (!decoded) { detail = decoded.error().message; return false; }
        _candidateStats = std::move(*decoded);
        return true;
    };
    if (!bundle.registerParticipant(_sectionPrefix + ".stats",
                                    std::move(statsSection), &error)) return false;

    app::SaveParticipantBundle::Participant abilitySection;
    abilitySection.version = 1;
    abilitySection.capture = [this](std::string& payload, std::string& detail) {
        if (!_candidateAbility) { detail = "Ability candidate missing"; return false; }
        auto snapshot = _candidateAbility->checkpoint();
        if (!snapshot) { detail = snapshot.error().message; return false; }
        auto encoded = ability::encodeRuntimeCheckpoint(*snapshot);
        if (!encoded) { detail = encoded.error().message; return false; }
        payload = std::move(*encoded);
        return true;
    };
    abilitySection.validate = [this](std::uint32_t version,
        std::string_view payload, std::string& detail) {
        if (version != 1) { detail = "Ability section version"; return false; }
        auto decoded = ability::decodeRuntimeCheckpoint(payload);
        if (!decoded) { detail = decoded.error().message; return false; }
        auto candidate = _ability;
        auto restored = candidate.restoreCheckpoint(*decoded);
        if (!restored) { detail = restored.error().message; return false; }
        return true;
    };
    abilitySection.apply = [this](std::uint32_t, std::string_view payload,
        std::string& detail) {
        auto decoded = ability::decodeRuntimeCheckpoint(payload);
        if (!decoded) { detail = decoded.error().message; return false; }
        auto candidate = _ability;
        auto restored = candidate.restoreCheckpoint(*decoded);
        if (!restored) { detail = restored.error().message; return false; }
        _candidateAbility = std::move(candidate);
        return true;
    };
    return bundle.registerParticipant(_sectionPrefix + ".ability",
                                      std::move(abilitySection), &error);
}

bool WorldEquipmentCommitAdapter::registerRestoreSections(
    app::SaveParticipantBundle& bundle, std::string& error)
{
    return registerSections(bundle, error, true);
}

bool WorldEquipmentCommitAdapter::prepareRestored(
    const Store& store, const app::WorldStateLedger&, std::string& error)
{
    if (!_candidateStats || !_candidateAbility) {
        error = "restored equipment sections missing"; return false;
    }
    auto adopted = EquipmentProjection::adoptRestored(
        _projection.actorIdentity(), _projection.actorIdentity().domain,
        store, _actorId, *_candidateStats, *_candidateAbility,
        _modifierFactory);
    if (!adopted) { error = adopted.error().message; return false; }
    _candidateProjection = std::move(*adopted);
    return true;
}

bool WorldEquipmentCommitAdapter::publishRestoredChecked(
    std::string& error)
{
    if (!_candidateStats || !_candidateAbility || !_candidateProjection) {
        error = "restored equipment candidate missing"; return false;
    }
    if (!_ability.swapState(*_candidateAbility)) {
        error = "Ability runtime is committing"; return false;
    }
    using std::swap;
    swap(_stats, *_candidateStats);
    _projection.swapState(*_candidateProjection);
    return true;
}

WorldEquipmentGroupCommitAdapter::WorldEquipmentGroupCommitAdapter(
    ability::Runtime& ability,
    std::vector<WorldEquipmentActorBinding> actors,
    std::string sectionPrefix)
    : _ability(ability), _actors(std::move(actors)),
      _sectionPrefix(std::move(sectionPrefix)) {}

bool WorldEquipmentGroupCommitAdapter::validBindings(
    std::string& error) const
{
    if (_sectionPrefix.empty() || _actors.empty() || _actors.size() > 32) {
        error = "equipment group needs a section prefix and 1..32 actors";
        return false;
    }
    std::string_view previous;
    for (std::size_t index = 0; index < _actors.size(); ++index) {
        const auto& actor = _actors[index];
        if (!actor.stats || !actor.projection || !actor.modifierFactory
            || actor.actorId.empty()
            || (!previous.empty() && actor.actorId <= previous)) {
            error = "equipment group bindings are incomplete or unordered";
            return false;
        }
        for (std::size_t prior = 0; prior < index; ++prior)
            if (_actors[prior].stats == actor.stats
                || _actors[prior].projection == actor.projection
                || _actors[prior].projection->actorIdentity()
                    == actor.projection->actorIdentity()) {
                error = "equipment group repeats a Stats or Ability owner";
                return false;
            }
        previous = actor.actorId;
    }
    return true;
}

WorldCommitExtension WorldEquipmentGroupCommitAdapter::extension()
{
    WorldCommitExtension result;
    result.prepareCandidate = [this](const Store& store,
        const app::WorldStateLedger&, std::string& error) {
        if (!validBindings(error)) return false;
        try {
            _candidateAbility = _ability;
            _candidateStats.clear();
            _candidateProjections.clear();
            _candidateStats.reserve(_actors.size());
            _candidateProjections.reserve(_actors.size());
            for (const auto& actor : _actors) {
                _candidateStats.push_back(*actor.stats);
                _candidateProjections.push_back(*actor.projection);
                const auto synced = _candidateProjections.back().sync(
                    store, actor.actorId, actor.selectedItemId,
                    _candidateStats.back(), *_candidateAbility,
                    actor.modifierFactory);
                if (!synced) { error = synced.error().message; return false; }
            }
            return true;
        } catch (const std::exception& ex) {
            error = ex.what(); return false;
        }
    };
    result.registerCandidateSections = [this](
        app::SaveParticipantBundle& bundle, std::string& error) {
        return registerSections(bundle, error, false);
    };
    result.publishCandidate = [this](std::string& error) {
        return publishChecked(error);
    };
    return result;
}

bool WorldEquipmentGroupCommitAdapter::registerSections(
    app::SaveParticipantBundle& bundle, std::string& error, bool restoring)
{
    if (!validBindings(error)) return false;
    try {
        if (restoring) {
            _candidateAbility = _ability;
            _candidateStats.clear();
            _candidateProjections.clear();
            _candidateStats.reserve(_actors.size());
            for (const auto& actor : _actors)
                _candidateStats.push_back(*actor.stats);
        } else if (!_candidateAbility
            || _candidateStats.size() != _actors.size()
            || _candidateProjections.size() != _actors.size()) {
            error = "equipment group candidate was not prepared";
            return false;
        }
        for (std::size_t index = 0; index < _actors.size(); ++index) {
            app::SaveParticipantBundle::Participant section;
            section.version = 1;
            section.capture = [this, index](std::string& payload,
                                             std::string& detail) {
                auto encoded = stats::encodeState(_candidateStats[index]);
                if (!encoded) { detail = encoded.error().message; return false; }
                payload = std::move(*encoded);
                return true;
            };
            section.validate = [this, index](std::uint32_t version,
                std::string_view payload, std::string& detail) {
                if (version != 1) { detail = "Stats section version"; return false; }
                auto decoded = stats::decodeState(payload,
                    _actors[index].stats->schema());
                if (!decoded) { detail = decoded.error().message; return false; }
                return true;
            };
            section.apply = [this, index](std::uint32_t,
                std::string_view payload, std::string& detail) {
                auto decoded = stats::decodeState(payload,
                    _actors[index].stats->schema());
                if (!decoded) { detail = decoded.error().message; return false; }
                _candidateStats[index] = std::move(*decoded);
                return true;
            };
            if (!bundle.registerParticipant(_sectionPrefix + ".stats."
                    + _actors[index].actorId, std::move(section), &error))
                return false;
        }
        app::SaveParticipantBundle::Participant abilitySection;
        abilitySection.version = 1;
        abilitySection.capture = [this](std::string& payload,
                                        std::string& detail) {
            auto snapshot = _candidateAbility->checkpoint();
            if (!snapshot) { detail = snapshot.error().message; return false; }
            auto encoded = ability::encodeRuntimeCheckpoint(*snapshot);
            if (!encoded) { detail = encoded.error().message; return false; }
            payload = std::move(*encoded);
            return true;
        };
        abilitySection.validate = [this](std::uint32_t version,
            std::string_view payload, std::string& detail) {
            if (version != 1) { detail = "Ability section version"; return false; }
            auto decoded = ability::decodeRuntimeCheckpoint(payload);
            if (!decoded) { detail = decoded.error().message; return false; }
            auto candidate = _ability;
            auto restored = candidate.restoreCheckpoint(*decoded);
            if (!restored) { detail = restored.error().message; return false; }
            return true;
        };
        abilitySection.apply = [this](std::uint32_t,
            std::string_view payload, std::string& detail) {
            auto decoded = ability::decodeRuntimeCheckpoint(payload);
            if (!decoded) { detail = decoded.error().message; return false; }
            auto candidate = _ability;
            auto restored = candidate.restoreCheckpoint(*decoded);
            if (!restored) { detail = restored.error().message; return false; }
            _candidateAbility = std::move(candidate);
            return true;
        };
        return bundle.registerParticipant(_sectionPrefix + ".ability",
            std::move(abilitySection), &error);
    } catch (const std::exception& ex) {
        error = ex.what(); return false;
    }
}

bool WorldEquipmentGroupCommitAdapter::registerRestoreSections(
    app::SaveParticipantBundle& bundle, std::string& error)
{
    return registerSections(bundle, error, true);
}

bool WorldEquipmentGroupCommitAdapter::prepareRestored(
    const Store& store, const app::WorldStateLedger&, std::string& error)
{
    if (!_candidateAbility || _candidateStats.size() != _actors.size()) {
        error = "restored equipment group sections missing"; return false;
    }
    try {
        _candidateProjections.clear();
        _candidateProjections.reserve(_actors.size());
        for (std::size_t index = 0; index < _actors.size(); ++index) {
            const auto& actor = _actors[index];
            const auto identity = actor.projection->actorIdentity();
            auto adopted = EquipmentProjection::adoptRestored(identity,
                identity.domain, store, actor.actorId,
                _candidateStats[index], *_candidateAbility,
                actor.modifierFactory);
            if (!adopted) { error = adopted.error().message; return false; }
            _candidateProjections.push_back(std::move(*adopted));
        }
        return true;
    } catch (const std::exception& ex) {
        error = ex.what(); return false;
    }
}

bool WorldEquipmentGroupCommitAdapter::publishChecked(std::string& error)
{
    if (!_candidateAbility || _candidateStats.size() != _actors.size()
        || _candidateProjections.size() != _actors.size()) {
        error = "equipment group candidate missing"; return false;
    }
    if (!_ability.swapState(*_candidateAbility)) {
        error = "Ability runtime is committing"; return false;
    }
    using std::swap;
    for (std::size_t index = 0; index < _actors.size(); ++index) {
        swap(*_actors[index].stats, _candidateStats[index]);
        _actors[index].projection->swapState(_candidateProjections[index]);
    }
    return true;
}

bool WorldEquipmentGroupCommitAdapter::publishRestoredChecked(
    std::string& error)
{
    return publishChecked(error);
}

} // namespace ayt::inventory
