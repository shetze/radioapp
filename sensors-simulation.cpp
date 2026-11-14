#include "sensors-simulation.hpp"

#include <algorithm>
#include <string>

namespace sensors {
namespace {

// Deterministic triangle motion, with a close approach and a receding obstacle.
unsigned motion(std::uint32_t step) {
    const unsigned phase = step % 120;
    return phase < 60 ? phase : 120 - phase;
}

void plot(CameraFrame &frame, int x, int y, char character, std::uint8_t fg,
          std::uint8_t bg = 0) {
    if (x < 0 || y < 0 || x >= frame.width || y >= frame.height)
        return;
    frame.cells[static_cast<std::size_t>(y) * frame.width + x] =
            {character, static_cast<std::uint8_t>((bg << 4) | (fg & 15))};
}

void label(CameraFrame &frame, int x, int y, const std::string &text, std::uint8_t fg) {
    for (char c : text)
        plot(frame, x++, y, c, fg);
}

std::uint8_t distance_color(std::uint16_t mm) {
    return mm < 500 ? 12 : mm < 1500 ? 14 : 10;
}

} // namespace

Ultrasonic simulate_ultrasonic(std::uint32_t step, std::uint32_t epoch,
                               std::uint32_t sequence) {
    const int approach = static_cast<int>(motion(step));
    Ultrasonic result;
    result.epoch = epoch;
    result.sequence = sequence;
    result.valid_mask = 0x0f;
    result.millimeters = {{
        static_cast<std::uint16_t>(2800 - approach * 16),
        static_cast<std::uint16_t>(1900 - approach * 24),
        static_cast<std::uint16_t>(1550 - approach * 21),
        static_cast<std::uint16_t>(3000 - approach * 13)
    }};
    return result;
}

CameraFrame simulate_camera(const SimulationConfig &config, std::uint32_t step,
                            std::uint32_t epoch, std::uint32_t sequence) {
    CameraFrame frame;
    if (config.width < kMinCameraWidth || config.width > kMaxCameraWidth ||
        config.height < kMinCameraHeight || config.height > kMaxCameraHeight)
        return frame;
    frame.width = static_cast<std::uint16_t>(config.width);
    frame.height = static_cast<std::uint16_t>(config.height);
    frame.epoch = epoch;
    frame.sequence = sequence;
    frame.cells.resize(static_cast<std::size_t>(config.width) * config.height,
                       Cell{' ', 0x07});
    const int center = config.width / 2;
    const auto echoes = simulate_ultrasonic(step, epoch, sequence);
    const unsigned approach = motion(step);

    label(frame, 2, 1, "REAR VIEW / SIMULATED CAMERA", 11);
    label(frame, config.width - 16, 1, "REVERSE ACTIVE", 14);
    const int horizon = std::max(5, config.height / 5);
    const int floor = config.height - 4;
    for (int y = horizon; y < floor; ++y) {
        const int spread = (y - horizon) * (config.width / 3) /
                           std::max(1, floor - horizon);
        plot(frame, center - spread - 10, y, '/', 8);
        plot(frame, center + spread + 10, y, '\\', 8);
        if (y % 3 == 0) {
            plot(frame, center - spread / 2, y, '|', 10);
            plot(frame, center + spread / 2, y, '|', 10);
        }
        if (y % 5 == 0)
            for (int x = 2; x < config.width - 2; x += 7)
                plot(frame, x, y, '.', 8);
    }
    // A nearby parked car changes size and distance in step with the echoes.
    const int car_width = config.width / 6 + static_cast<int>(approach) / 5;
    const int car_height = std::max(5, config.height / 7 + static_cast<int>(approach) / 12);
    const int y_bottom = std::min(floor - 4, horizon + car_height +
                                  static_cast<int>(approach) * (floor - horizon) / 125);
    const int left = center - car_width / 2;
    const int top = y_bottom - car_height;
    for (int x = left + 1; x < left + car_width - 1; ++x) {
        plot(frame, x, top, '-', 7);
        plot(frame, x, y_bottom, '=', 7);
    }
    for (int y = top + 1; y < y_bottom; ++y) {
        plot(frame, left, y, '|', 7);
        plot(frame, left + car_width - 1, y, '|', 7);
    }
    plot(frame, left, top, '+', 7);
    plot(frame, left + car_width - 1, top, '+', 7);
    plot(frame, left, y_bottom, '+', 7);
    plot(frame, left + car_width - 1, y_bottom, '+', 7);
    if (car_width > 13 && car_height > 5) {
        label(frame, left + 3, top + 2, "[ REAR WINDOW ]", 11);
        plot(frame, left + 2, y_bottom - 1, '#', 12);
        plot(frame, left + car_width - 3, y_bottom - 1, '#', 12);
    }
    // Rear parking guide bars use the distance classification.
    const std::uint8_t proximity = distance_color(std::min(echoes.millimeters[1],
                                                            echoes.millimeters[2]));
    for (int x = center - config.width / 4; x <= center + config.width / 4; ++x) {
        plot(frame, x, config.height - 7, '-', 10);
        plot(frame, x, config.height - 5, '=', 14);
        plot(frame, x, config.height - 3, '=', proximity);
    }
    label(frame, 2, config.height - 1, "L: " +
          std::to_string(echoes.millimeters[0] / 100) + "cm", distance_color(echoes.millimeters[0]));
    label(frame, center - 12, config.height - 1, "ML: " +
          std::to_string(echoes.millimeters[1] / 100) + "cm", distance_color(echoes.millimeters[1]));
    label(frame, center + 5, config.height - 1, "MR: " +
          std::to_string(echoes.millimeters[2] / 100) + "cm", distance_color(echoes.millimeters[2]));
    label(frame, config.width - 12, config.height - 1, "R: " +
          std::to_string(echoes.millimeters[3] / 100) + "cm", distance_color(echoes.millimeters[3]));
    return frame;
}

} // namespace sensors
