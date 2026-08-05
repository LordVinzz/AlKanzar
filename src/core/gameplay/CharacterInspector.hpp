#pragma once

#include "core/ecs/Entity.hpp"

namespace core {

struct EngineServices;

bool drawCharacterInspector(EngineServices& services, EntityId entity);

}  // namespace core
