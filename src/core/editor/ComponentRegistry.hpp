#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/ecs/Entity.hpp"
#include "core/presentation/ComponentKind.hpp"

namespace core {

class World;
struct EngineServices;

struct ComponentDescriptor {
    ComponentKind kind{ComponentKind::Transform};
    std::string name;
    std::string category;
    std::function<bool(const World&, EntityId)> hasComponent;
    std::function<void(World&, EntityId)> addComponent;
    std::function<void(World&, EntityId)> removeComponent;
    std::function<bool(EngineServices&, EntityId)> drawInspector;
};

class ComponentRegistry {
public:
    ComponentRegistry();

    [[nodiscard]] const std::vector<ComponentDescriptor>& descriptors() const {
        return descriptors_;
    }

    [[nodiscard]] const ComponentDescriptor* find(ComponentKind kind) const;
    void drawAddComponentButton(EngineServices& services, EntityId entity) const;
    void drawComponentTabs(
        EngineServices& services,
        EntityId entity,
        std::optional<ComponentKind> focusedComponent = std::nullopt
    ) const;

private:
    void registerDirectionalLightDescriptor();
    void registerCharacterDescriptor();
    std::vector<ComponentDescriptor> descriptors_;
};

}  // namespace core
