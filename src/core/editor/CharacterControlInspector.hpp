#pragma once

#include "core/ecs/Entity.hpp"

namespace core {

struct EngineServices;

bool drawCharacterControlInspector(EngineServices& services, EntityId entity);

}  // namespace core
