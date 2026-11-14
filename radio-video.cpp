#include "radio-video.hpp"

#include <caca.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <utility>

#ifndef RADIO_DEFAULT_VIDEO_DIR
#define RADIO_DEFAULT_VIDEO_DIR "/usr/share/auto-apps/animations"
#endif

namespace radio_video {
namespace {

constexpr std::size_t kMaxEntries = 512;

struct DirectoryCloser {
    void operator()(DIR *directory) const {
        if (directory) closedir(directory);
    }
};
constexpr std::size_t kMaxFrames = 4096;
constexpr std::size_t kMaxFileBytes = 32 * 1024 * 1024;
constexpr std::size_t kMaxTotalBytes = 64 * 1024 * 1024;
constexpr std::uint64_t kMaxFrameCells = 256 * 512;
constexpr std::uint64_t kMaxTotalCells = 16 * 1024 * 1024;

std::string join_path(const std::string &directory, const std::string &name) {
    return directory + (directory.empty() || directory.back() == '/' ? "" : "/") + name;
}

bool ends_with(const std::string &name, const std::string &suffix) {
    if (name.size() < suffix.size())
        return false;
    return std::equal(suffix.rbegin(), suffix.rend(), name.rbegin(),
                      [](char a, char b) {
                          return (a >= 'A' && a <= 'Z' ? a + ('a' - 'A') : a) ==
                                 (b >= 'A' && b <= 'Z' ? b + ('a' - 'A') : b);
                      });
}

bool is_frame_file(const std::string &name) {
    return ends_with(name, ".caca") || ends_with(name, ".ansi") ||
           ends_with(name, ".ans") || ends_with(name, ".txt") ||
           ends_with(name, ".asc");
}

bool is_regular_file(const std::string &path) {
    struct stat st{};
    // Never follow symlinks in a scanned media directory.
    return lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool is_directory(const std::string &path) {
    struct stat st{};
    return lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool contains_frames(const std::string &directory) {
    DIR *raw = opendir(directory.c_str());
    if (!raw)
        return false;
    std::unique_ptr<DIR, DirectoryCloser> dir(raw);
    struct dirent *item;
    while ((item = readdir(dir.get())) != nullptr) {
        const std::string name(item->d_name);
        if (is_frame_file(name) && is_regular_file(join_path(directory, name)))
            return true;
    }
    return false;
}

std::vector<std::string> frame_paths(const std::string &directory, std::string &error) {
    std::vector<std::string> paths;
    DIR *raw = opendir(directory.c_str());
    if (!raw) {
        error = "Cannot open media directory: " + std::string(std::strerror(errno));
        return paths;
    }
    std::unique_ptr<DIR, DirectoryCloser> dir(raw);
    struct dirent *item;
    while ((item = readdir(dir.get())) != nullptr) {
        const std::string name(item->d_name);
        if (!is_frame_file(name))
            continue;
        const std::string path = join_path(directory, name);
        if (is_regular_file(path)) {
            paths.push_back(path);
            if (paths.size() > kMaxFrames) {
                error = "Too many animation frames (limit 4096)";
                return {};
            }
        }
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

// Accept a small UTF-8 subset detector so old CP437 .ans artworks can still
// be imported by libcaca's 'ansi' decoder. ASCII/UTF-8 ANSI uses 'utf8', which
// can grow beyond the old ANSI decoder's fixed 80-column canvas.
bool valid_utf8(const std::string &bytes) {
    for (std::size_t i = 0; i < bytes.size();) {
        const auto lead = static_cast<unsigned char>(bytes[i]);
        if (lead < 0x80) { ++i; continue; }
        unsigned needed = 0;
        std::uint32_t cp = 0;
        if (lead >= 0xc2 && lead <= 0xdf) { needed = 1; cp = lead & 0x1f; }
        else if (lead >= 0xe0 && lead <= 0xef) { needed = 2; cp = lead & 0x0f; }
        else if (lead >= 0xf0 && lead <= 0xf4) { needed = 3; cp = lead & 0x07; }
        else return false;
        if (bytes.size() - i <= needed)
            return false;
        for (unsigned j = 1; j <= needed; ++j) {
            const auto next = static_cast<unsigned char>(bytes[i + j]);
            if ((next & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (next & 0x3f);
        }
        if ((needed == 2 && cp < 0x800) || (needed == 3 && cp < 0x10000) ||
            (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff)
            return false;
        i += needed + 1;
    }
    return true;
}

const char *import_format(const std::string &path, const std::string &bytes) {
    if (ends_with(path, ".caca")) return "caca";
    if (ends_with(path, ".asc")) return "text";
    if (ends_with(path, ".ans") && !valid_utf8(bytes)) return "ansi";
    return "utf8"; // modern ANSI sequences, plain text and UTF-8
}

int read_fps(const std::string &path) {
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0)
        return kDefaultFps;
    if (!S_ISREG(st.st_mode) || st.st_size > 64)
        return -1;
    std::ifstream file(path);
    if (!file) return -1;
    std::string line;
    std::getline(file, line);
    // Deliberately small, independent sidecar format: integer FPS, 1..60.
    std::istringstream input(line);
    int fps = 0;
    std::string trailing;
    if (!(input >> fps) || (input >> trailing) || fps < 1 || fps > 60)
        return -1;
    return fps;
}

bool add_file(const std::string &path,
              std::size_t &total_bytes, std::uint64_t &total_cells,
              std::vector<Frame> &frames, int &max_width, int &max_height,
              std::string &error) {
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size <= 0 || static_cast<std::uint64_t>(st.st_size) > kMaxFileBytes ||
        static_cast<std::uint64_t>(st.st_size) > kMaxTotalBytes - total_bytes) {
        error = "Unreadable or oversized animation file: " + path;
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "Cannot read animation file: " + path;
        return false;
    }
    std::string data(static_cast<std::size_t>(st.st_size), '\0');
    if (!input.read(&data[0], st.st_size)) {
        error = "Cannot read complete animation file: " + path;
        return false;
    }
    caca_canvas_t *raw_canvas = caca_create_canvas(0, 0);
    if (!raw_canvas) {
        error = "libcaca could not allocate the animation canvas";
        return false;
    }
    std::shared_ptr<caca_canvas_t> canvas(raw_canvas, &caca_free_canvas);
    const ssize_t parsed = caca_import_canvas_from_memory(canvas.get(), data.data(),
                                                         data.size(), import_format(path, data));
    if (parsed <= 0) {
        error = "libcaca cannot decode: " + path;
        return false;
    }
    total_bytes += data.size();
    // libcaca 0.99.beta20's native canvas importer leaves a trailing empty
    // frame on an initially empty canvas. The native file header contains the
    // authoritative frame count (big-endian bytes 14..17). Never play that
    // import artefact when it is exactly one frame beyond the file count.
    if (ends_with(path, ".caca") && data.size() >= 20 &&
        static_cast<unsigned char>(data[0]) == 0xca &&
        static_cast<unsigned char>(data[1]) == 0xca &&
        data[2] == 'C' && data[3] == 'V') {
        std::uint32_t declared_frames = 0;
        for (int i = 14; i < 18; ++i)
            declared_frames = (declared_frames << 8) |
                              static_cast<unsigned char>(data[i]);
        const int imported = caca_get_frame_count(canvas.get());
        if (declared_frames > 0 && declared_frames <= kMaxFrames &&
            imported == static_cast<int>(declared_frames + 1)) {
            if (caca_free_frame(canvas.get(), imported - 1) != 0) {
                error = "Cannot remove the extra libcaca import frame: " + path;
                return false;
            }
        }
    }
    const int count = caca_get_frame_count(canvas.get());
    if (count <= 0 || static_cast<std::size_t>(count) > kMaxFrames - frames.size()) {
        error = "Animation exceeds the 4096-frame limit";
        return false;
    }
    for (int index = 0; index < count; ++index) {
        if (caca_set_frame(canvas.get(), index) != 0) {
            error = "Invalid libcaca frame in: " + path;
            return false;
        }
        const int width = caca_get_canvas_width(canvas.get());
        const int height = caca_get_canvas_height(canvas.get());
        const auto cells = static_cast<std::uint64_t>(std::max(0, width)) *
                           static_cast<std::uint64_t>(std::max(0, height));
        if (width <= 0 || height <= 0 || cells > kMaxFrameCells ||
            cells > kMaxTotalCells - total_cells) {
            error = "Invalid or oversized libcaca frame in: " + path;
            return false;
        }
        total_cells += cells;
        frames.push_back({canvas, index, width, height});
        max_width = std::max(max_width, width);
        max_height = std::max(max_height, height);
    }
    return true;
}

} // namespace

std::string media_directory() {
    const char *override = std::getenv("RADIO_VIDEO_DIR");
    return override && *override ? override : RADIO_DEFAULT_VIDEO_DIR;
}

std::vector<Entry> discover(const std::string &directory, std::string &error) {
    error.clear();
    std::vector<Entry> entries;
    DIR *raw = opendir(directory.c_str());
    if (!raw) {
        error = "Cannot open video library " + directory + ": " + std::strerror(errno);
        return entries;
    }
    std::unique_ptr<DIR, DirectoryCloser> dir(raw);
    struct dirent *item;
    while ((item = readdir(dir.get())) != nullptr) {
        const std::string name(item->d_name);
        if (name == "." || name == "..")
            continue;
        const std::string path = join_path(directory, name);
        const bool directory_entry = is_directory(path);
        if ((directory_entry && contains_frames(path)) ||
            (!directory_entry && is_frame_file(name) && is_regular_file(path))) {
            entries.push_back({name, path, directory_entry});
            if (entries.size() > kMaxEntries) {
                error = "Video library has too many entries (limit 512)";
                return {};
            }
        }
    }
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        return a.name < b.name;
    });
    if (entries.empty())
        error = "No .caca, .ansi, .ans, .txt or .asc animations found";
    return entries;
}

std::unique_ptr<Animation> Animation::load(const Entry &entry, std::string &error) {
    error.clear();
    const auto paths = entry.directory ? frame_paths(entry.path, error)
                                       : std::vector<std::string>{entry.path};
    if (!error.empty()) return nullptr;
    if (paths.empty()) {
        error = "Animation contains no frames";
        return nullptr;
    }
    auto animation = std::unique_ptr<Animation>(new Animation());
    const std::string sidecar = entry.directory ? join_path(entry.path, "animation.fps")
                                                : entry.path + ".fps";
    animation->fps_ = read_fps(sidecar);
    if (animation->fps_ < 0) {
        error = "Invalid FPS (expected integer 1..60) in " + sidecar;
        return nullptr;
    }
    std::size_t total_bytes = 0;
    std::uint64_t total_cells = 0;
    for (const auto &path : paths) {
        if (!add_file(path, total_bytes, total_cells,
                      animation->frames_, animation->width_, animation->height_, error))
            return nullptr;
    }
    return animation;
}

Player::Player(std::string directory) : directory_(std::move(directory)) {}

void Player::browse() {
    animation_.reset();
    position_ = 0;
    const std::string previous = entries_.empty() ? "" : entries_[selection_].path;
    entries_ = discover(directory_, message_);
    selection_ = 0;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].path == previous) {
            selection_ = i;
            break;
        }
    }
}

void Player::move(int offset) {
    if (entries_.empty()) return;
    const auto last = static_cast<std::int64_t>(entries_.size() - 1);
    selection_ = static_cast<std::size_t>(std::max<std::int64_t>(0,
                    std::min<std::int64_t>(last,
                        static_cast<std::int64_t>(selection_) + offset)));
    message_.clear();
}

void Player::page(int offset) {
    move(offset);
}

void Player::select(Time now) {
    if (entries_.empty()) return;
    std::string error;
    auto animation = Animation::load(entries_[selection_], error);
    if (!animation) {
        message_ = error;
        return;
    }
    message_.clear();
    animation_ = std::move(animation);
    position_ = 0;
    next_frame_ = now + std::chrono::nanoseconds(1000000000LL / animation_->fps());
}

void Player::stop() {
    animation_.reset();
    position_ = 0;
    message_.clear();
}

bool Player::advance(Time now) {
    if (!animation_ || now < next_frame_)
        return false;
    const auto period = std::chrono::nanoseconds(1000000000LL / animation_->fps());
    const auto elapsed = now - next_frame_;
    const auto steps = static_cast<std::uint64_t>(elapsed / period) + 1;
    const std::size_t previous = position_;
    position_ = (position_ + (steps % animation_->size())) % animation_->size();
    next_frame_ += period * steps;
    return position_ != previous;
}

int Player::wait_ms(Time now) const {
    if (!animation_)
        return 100;
    if (now >= next_frame_)
        return 0;
    const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(next_frame_ - now).count();
    return static_cast<int>(std::min<std::int64_t>(100, (remaining + 999999) / 1000000));
}

} // namespace radio_video
