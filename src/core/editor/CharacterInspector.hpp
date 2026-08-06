#pragma once

#include "core/ecs/Entity.hpp"

namespace core {

struct EngineServices;

// Presentation adapter for the character simulation components.
bool drawCharacterInspector(EngineServices& services, EntityId entity);

}  // namespace core
