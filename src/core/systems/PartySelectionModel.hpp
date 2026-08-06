#pragma once

#include <optional>

#include "core/ecs/Entity.hpp"
#include "core/events/Signal.hpp"

namespace core {

class PartySelectionModel {
public:
    void setLeader(EntityId entity) {
        setLeader(entity.valid() ? std::optional<EntityId>{entity} : std::nullopt);
    }

    void setLeader(std::optional<EntityId> entity) {
        if (leader_ == entity) {
            return;
        }
        leader_ = entity;
        changed_.notify(leader_);
    }

    [[nodiscard]] const std::optional<EntityId>& leader() const {
        return leader_;
    }

    void clear() {
        setLeader(std::nullopt);
    }

    Signal<const std::optional<EntityId>&>& changed() {
        return changed_;
    }

private:
    std::optional<EntityId> leader_{};
    Signal<const std::optional<EntityId>&> changed_{};
};

}  // namespace core
