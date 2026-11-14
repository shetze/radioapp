#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

// libcaca owns the decoded image data. Keep its public types out of the
// SOME/IP and ncurses-facing parts of the application.
struct caca_canvas;
typedef struct caca_canvas caca_canvas_t;

namespace radio_video {

constexpr int kMinimumColumns = 100;
constexpr int kMinimumRows = 30;
constexpr int kDefaultFps = 10;

struct Entry {
    std::string name;
    std::string path;
    bool directory = false;
};

std::string media_directory();
std::vector<Entry> discover(const std::string &directory, std::string &error);

struct Frame {
    std::shared_ptr<caca_canvas_t> canvas;
    int index = 0;
    int width = 0;
    int height = 0;
};

class Animation {
public:
    // An entry is a multiframe .caca/.ansi file or a directory of ordered
    // frames. A single .ans/.txt file is a valid still animation.
    static std::unique_ptr<Animation> load(const Entry &entry, std::string &error);

    std::size_t size() const { return frames_.size(); }
    int width() const { return width_; }
    int height() const { return height_; }
    int fps() const { return fps_; }
    const Frame &frame(std::size_t position) const { return frames_.at(position); }

private:
    std::vector<Frame> frames_;
    int width_ = 0;
    int height_ = 0;
    int fps_ = kDefaultFps;
};

class Player {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;

    explicit Player(std::string directory);
    void browse();
    void move(int offset);
    void page(int offset);
    void select(Time now);
    void stop();
    bool advance(Time now); // true if the next frame should be drawn
    int wait_ms(Time now) const;

    const std::string &directory() const { return directory_; }
    const std::vector<Entry> &entries() const { return entries_; }
    std::size_t selection() const { return selection_; }
    const std::string &message() const { return message_; }
    bool playing() const { return static_cast<bool>(animation_); }
    const Animation *animation() const { return animation_.get(); }
    const Frame &current_frame() const { return animation_->frame(position_); }

private:
    std::string directory_;
    std::vector<Entry> entries_;
    std::size_t selection_ = 0;
    std::string message_;
    std::unique_ptr<Animation> animation_;
    std::size_t position_ = 0;
    Time next_frame_{};
};

} // namespace radio_video
