#pragma once

namespace core::navigation_detail {

bool shortestYawStep(float currentYaw, float desiredYaw, float maxStep, float& outYaw);

}  // namespace core::navigation_detail
