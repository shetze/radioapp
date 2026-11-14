#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

// Versioned, bounded, endian-independent on-wire formats.  The camera stream
// contains simple printable ASCII cells with libcaca-compatible 4-bit ANSI
// foreground/background colours; it does not carry terminal escape codes.
// All multi-byte integers are network-order (big-endian).
namespace sensors {

constexpr unsigned kSensorCount = 4;
constexpr int kMinCameraWidth = 100;
constexpr int kMinCameraHeight = 30;
constexpr int kMaxCameraWidth = 240;
constexpr int kMaxCameraHeight = 80;
constexpr unsigned kDefaultFps = 10;

using Bytes = std::vector<std::uint8_t>;

enum class CameraState : std::uint8_t {
    Inactive = 0, Active = 1, Fault = 2, Unknown = 255
};

struct Cell {
    char glyph = ' ';
    std::uint8_t colors = 0x07; // FG low nibble, BG high nibble; libcaca ANSI order
};

struct CameraFrame {
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint32_t epoch = 0;
    std::uint32_t sequence = 0;
    std::vector<Cell> cells;

    const Cell &at(int x, int y) const {
        return cells[static_cast<std::size_t>(y) * width + x];
    }
};

struct CameraStatus {
    CameraState state = CameraState::Unknown;
    std::uint32_t epoch = 0;
    std::uint32_t sequence = 0;
};

struct Ultrasonic {
    std::uint32_t epoch = 0;
    std::uint32_t sequence = 0;
    std::uint8_t valid_mask = 0;
    // Rear sensors, left to right, in millimeters; 0 means no valid distance.
    std::array<std::uint16_t, kSensorCount> millimeters{{0, 0, 0, 0}};
};

inline void append_u16(Bytes &out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}
inline void append_u32(Bytes &out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}
inline std::uint16_t read_u16(const std::uint8_t *p) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}
inline std::uint32_t read_u32(const std::uint8_t *p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}
inline bool magic_matches(const std::uint8_t *data, std::size_t length,
                          char a, char b, char c, char d) {
    return data != nullptr && length >= 4 &&
           data[0] == a && data[1] == b && data[2] == c && data[3] == d;
}

constexpr std::size_t kFrameHeaderSize = 16;
constexpr std::size_t kStatusWireSize = 13;
constexpr std::size_t kUltrasonicWireSize = 21;

inline bool valid_size(std::uint16_t width, std::uint16_t height) {
    return width >= kMinCameraWidth && width <= kMaxCameraWidth &&
           height >= kMinCameraHeight && height <= kMaxCameraHeight;
}
inline Bytes encode(const CameraFrame &frame) {
    if (!valid_size(frame.width, frame.height) ||
        frame.cells.size() != static_cast<std::size_t>(frame.width) * frame.height)
        return {};
    Bytes out;
    out.reserve(kFrameHeaderSize + frame.cells.size() * 2);
    out.insert(out.end(), {'C', 'A', 'M', '1'});
    append_u16(out, frame.width);
    append_u16(out, frame.height);
    append_u32(out, frame.epoch);
    append_u32(out, frame.sequence);
    for (const auto &cell : frame.cells) {
        const auto glyph = static_cast<std::uint8_t>(cell.glyph);
        if (glyph < 32 || glyph > 126)
            return {};
        out.push_back(glyph);
        out.push_back(cell.colors);
    }
    return out;
}
inline bool decode(const std::uint8_t *data, std::size_t size, CameraFrame &out) {
    if (!magic_matches(data, size, 'C', 'A', 'M', '1') || size < kFrameHeaderSize)
        return false;
    const std::uint16_t width = read_u16(data + 4);
    const std::uint16_t height = read_u16(data + 6);
    if (!valid_size(width, height) ||
        size != kFrameHeaderSize + static_cast<std::size_t>(width) * height * 2)
        return false;
    CameraFrame parsed;
    parsed.width = width;
    parsed.height = height;
    parsed.epoch = read_u32(data + 8);
    parsed.sequence = read_u32(data + 12);
    parsed.cells.reserve(static_cast<std::size_t>(width) * height);
    for (std::size_t i = kFrameHeaderSize; i < size; i += 2) {
        if (data[i] < 32 || data[i] > 126)
            return false;
        parsed.cells.push_back({static_cast<char>(data[i]), data[i + 1]});
    }
    out = std::move(parsed);
    return true;
}
inline Bytes encode(const CameraStatus &status) {
    if (status.state == CameraState::Unknown)
        return {};
    Bytes out{'C', 'S', 'T', '1', static_cast<std::uint8_t>(status.state)};
    append_u32(out, status.epoch);
    append_u32(out, status.sequence);
    return out;
}
inline bool decode(const std::uint8_t *data, std::size_t size, CameraStatus &out) {
    if (size != kStatusWireSize || !magic_matches(data, size, 'C', 'S', 'T', '1') ||
        data[4] > static_cast<std::uint8_t>(CameraState::Fault))
        return false;
    out = {static_cast<CameraState>(data[4]), read_u32(data + 5), read_u32(data + 9)};
    return true;
}
inline Bytes encode(const Ultrasonic &distances) {
    Bytes out{'U', 'L', 'T', '1'};
    append_u32(out, distances.epoch);
    append_u32(out, distances.sequence);
    out.push_back(distances.valid_mask & 0x0f);
    for (unsigned i = 0; i < kSensorCount; ++i)
        append_u16(out, distances.millimeters[i]);
    return out;
}
inline bool decode(const std::uint8_t *data, std::size_t size, Ultrasonic &out) {
    if (size != kUltrasonicWireSize || !magic_matches(data, size, 'U', 'L', 'T', '1') ||
        (data[12] & 0xf0) != 0)
        return false;
    Ultrasonic parsed;
    parsed.epoch = read_u32(data + 4);
    parsed.sequence = read_u32(data + 8);
    parsed.valid_mask = data[12];
    for (unsigned i = 0; i < kSensorCount; ++i)
        parsed.millimeters[i] = read_u16(data + 13 + i * 2);
    out = parsed;
    return true;
}

} // namespace sensors
