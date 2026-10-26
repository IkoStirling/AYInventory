#pragma once

#include <AYInventory/EquipmentProjection.h>
#include <AYInventory/WorldCommit.h>

#include <optional>
#include <vector>

namespace ayt::inventory {

/// Optional one-actor adapter for a direct durable World-to-equipment pickup.
/// Own this object through the whole command and restore call. The game owns
/// the Store, author catalogs, slot, actor identity and stable section prefix.
/// It must still pass all other game sections to WorldItemCommit separately.
/// Calls run at one quiet Sim boundary; the selected Ability Runtime must not
/// be inside a fire pre-commit hook during publication.
class WorldEquipmentCommitAdapter {
public:
    WorldEquipmentCommitAdapter(stats::StatSet& stats,
        ability::Runtime& ability, EquipmentProjection& projection,
        std::string actorId, std::string selectedItemId,
        std::string sectionPrefix, ModifierFactory modifierFactory);

    /// Candidate Stats, Ability and projection are validated from the staged
    /// Store, encoded in two complete slot sections, then published after the
    /// core slot succeeds. Keep this adapter alive while callbacks run.
    [[nodiscard]] WorldCommitExtension extension();

    /// Register Stats/Ability restore into private candidates. Pass the next
    /// two methods as prepareAdditional and publishAdditionalChecked to
    /// restoreWorldItemCommitSlot, in that order. No live owner changes when
    /// validation or checked publication fails.
    bool registerRestoreSections(app::SaveParticipantBundle& bundle,
                                 std::string& error);
    bool prepareRestored(const Store& store,
                         const app::WorldStateLedger& ledger,
                         std::string& error);
    bool publishRestoredChecked(std::string& error);

private:
    stats::StatSet& _stats;
    ability::Runtime& _ability;
    EquipmentProjection& _projection;
    std::string _actorId, _selectedItemId, _sectionPrefix;
    ModifierFactory _modifierFactory;
    std::optional<stats::StatSet> _candidateStats;
    std::optional<ability::Runtime> _candidateAbility;
    std::optional<EquipmentProjection> _candidateProjection;

    bool registerSections(app::SaveParticipantBundle& bundle,
                          std::string& error, bool restoring);
};

struct WorldEquipmentActorBinding {
    stats::StatSet* stats = nullptr;
    EquipmentProjection* projection = nullptr;
    std::string actorId;
    std::string selectedItemId;
    ModifierFactory modifierFactory;
};

/// Multi-actor direct World equipment adapter. Supply 1..32 bindings in
/// strictly ascending stable actorId order, covering every affected equipment
/// owner. One complete Ability Runtime is shared and saved once; each actor's
/// complete Stats state has its own stable section named prefix.stats.actorId.
/// Prefix and actor IDs must form valid SaveParticipantBundle section names.
/// Keep this adapter and its referenced owners alive through command/restore.
class WorldEquipmentGroupCommitAdapter {
public:
    WorldEquipmentGroupCommitAdapter(ability::Runtime& ability,
        std::vector<WorldEquipmentActorBinding> actors,
        std::string sectionPrefix);

    [[nodiscard]] WorldCommitExtension extension();
    bool registerRestoreSections(app::SaveParticipantBundle& bundle,
                                 std::string& error);
    bool prepareRestored(const Store& store,
                         const app::WorldStateLedger& ledger,
                         std::string& error);
    bool publishRestoredChecked(std::string& error);

private:
    ability::Runtime& _ability;
    std::vector<WorldEquipmentActorBinding> _actors;
    std::string _sectionPrefix;
    std::optional<ability::Runtime> _candidateAbility;
    std::vector<stats::StatSet> _candidateStats;
    std::vector<EquipmentProjection> _candidateProjections;

    bool validBindings(std::string& error) const;
    bool registerSections(app::SaveParticipantBundle& bundle,
                          std::string& error, bool restoring);
    bool publishChecked(std::string& error);
};

} // namespace ayt::inventory
