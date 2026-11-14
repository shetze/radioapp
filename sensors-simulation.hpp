#pragma once

#include "sensors-protocol.hpp"

namespace sensors {

struct SimulationConfig {
    int width = 100;
    int height = 30;
    unsigned fps = kDefaultFps;
};

// Pure deterministic simulation: no real camera, capture device or USB access.
// The same step drives the moving rear-view scene and the ultrasonic echoes.
Ultrasonic simulate_ultrasonic(std::uint32_t step, std::uint32_t epoch,
                               std::uint32_t sequence);
CameraFrame simulate_camera(const SimulationConfig &config, std::uint32_t step,
                            std::uint32_t epoch, std::uint32_t sequence);

} // namespace sensors
