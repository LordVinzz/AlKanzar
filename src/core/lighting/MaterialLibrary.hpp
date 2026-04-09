#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "core/events/Signal.hpp"
#include "render/resources/Material.hpp"

namespace core {

struct MaterialHandle {
    static constexpr std::size_t kInvalid = static_cast<std::size_t>(-1);

    std::size_t value{kInvalid};

    [[nodiscard]] bool valid() const {
        return value != kInvalid;
    }

    friend bool operator==(MaterialHandle lhs, MaterialHandle rhs) = default;
};

class MaterialLibrary {
public:
    MaterialHandle add(const std::shared_ptr<render::Material>& material) {
        materials_.push_back(material);
        return MaterialHandle{materials_.size() - 1u};
    }

    [[nodiscard]] std::shared_ptr<render::Material> get(MaterialHandle handle) const {
        if (!handle.valid() || handle.value >= materials_.size()) {
            return nullptr;
        }
        return materials_[handle.value];
    }

    void notifyChanged(MaterialHandle handle) {
        changed_.notify(handle);
    }

    [[nodiscard]] std::size_t size() const {
        return materials_.size();
    }

    void clear() {
        materials_.clear();
    }

    Signal<MaterialHandle>& changed() {
        return changed_;
    }

private:
    std::vector<std::shared_ptr<render::Material>> materials_;
    Signal<MaterialHandle> changed_{};
};

}  // namespace core
