#pragma once

#include <optional>

#include "Entity.hpp"
#include "Signal.hpp"

namespace core {

class SelectionModel {
public:
    void set(std::optional<EntityId> selection) {
        if (selection_ == selection) {
            return;
        }
        selection_ = selection;
        changed_.notify(selection_);
    }

    [[nodiscard]] const std::optional<EntityId>& current() const {
        return selection_;
    }

    void clear() {
        set(std::nullopt);
    }

    Signal<const std::optional<EntityId>&>& changed() {
        return changed_;
    }

private:
    std::optional<EntityId> selection_{};
    Signal<const std::optional<EntityId>&> changed_{};
};

}  // namespace core
