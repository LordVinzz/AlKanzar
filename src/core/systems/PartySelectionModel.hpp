#pragma once

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include "core/ecs/Entity.hpp"
#include "core/events/Signal.hpp"

namespace core {

class PartySelectionModel {
public:
    void setLeader(EntityId entity) {
        setLeader(entity.valid() ? std::optional<EntityId>{entity} : std::nullopt);
    }

    void setLeader(std::optional<EntityId> entity) {
        if (!entity.has_value() || !entity->valid()) {
            clear();
            return;
        }

        std::vector<EntityId> selection = selected_;
        const auto existing = std::find(selection.begin(), selection.end(), *entity);
        if (existing != selection.end()) {
            selection.erase(existing);
        }
        selection.insert(selection.begin(), *entity);
        setSelection(std::move(selection));
    }

    void setSelection(std::vector<EntityId> entities) {
        std::vector<EntityId> normalized{};
        normalized.reserve(entities.size());
        for (const EntityId entity : entities) {
            if (!entity.valid() || std::find(normalized.begin(), normalized.end(), entity) != normalized.end()) {
                continue;
            }
            normalized.push_back(entity);
        }
        if (selected_ == normalized) {
            return;
        }

        selected_ = std::move(normalized);
        leader_ = selected_.empty() ? std::nullopt : std::optional<EntityId>{selected_.front()};
        changed_.notify(selected_);
    }

    [[nodiscard]] const std::optional<EntityId>& leader() const {
        return leader_;
    }

    [[nodiscard]] const std::vector<EntityId>& selected() const {
        return selected_;
    }

    [[nodiscard]] bool contains(EntityId entity) const {
        return std::find(selected_.begin(), selected_.end(), entity) != selected_.end();
    }

    void clear() {
        setSelection({});
    }

    Signal<const std::vector<EntityId>&>& changed() {
        return changed_;
    }

private:
    std::vector<EntityId> selected_{};
    std::optional<EntityId> leader_{};
    Signal<const std::vector<EntityId>&> changed_{};
};

}  // namespace core
