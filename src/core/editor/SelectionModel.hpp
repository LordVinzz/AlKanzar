#pragma once

#include <optional>

#include "core/ecs/ComponentRegistry.hpp"
#include "core/ecs/Entity.hpp"
#include "core/events/Signal.hpp"

namespace core {

struct SelectionTarget {
    EntityId entity{};
    std::optional<ComponentKind> component{};

    [[nodiscard]] bool valid() const {
        return entity.valid();
    }

    friend bool operator==(const SelectionTarget&, const SelectionTarget&) = default;
};

class SelectionModel {
public:
    void set(EntityId entity) {
        set(entity.valid() ? std::optional<SelectionTarget>{SelectionTarget{entity, std::nullopt}} : std::nullopt);
    }

    void set(std::optional<EntityId> selection) {
        if (!selection.has_value()) {
            set(std::optional<SelectionTarget>{});
            return;
        }
        set(SelectionTarget{*selection, std::nullopt});
    }

    void set(std::optional<SelectionTarget> selection) {
        if (selection_ == selection) {
            return;
        }
        selection_ = selection;
        changed_.notify(selection_);
    }

    [[nodiscard]] const std::optional<SelectionTarget>& current() const {
        return selection_;
    }

    void clear() {
        set(std::optional<SelectionTarget>{});
    }

    Signal<const std::optional<SelectionTarget>&>& changed() {
        return changed_;
    }

private:
    std::optional<SelectionTarget> selection_{};
    Signal<const std::optional<SelectionTarget>&> changed_{};
};

}  // namespace core
