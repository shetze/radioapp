#define _XOPEN_SOURCE_EXTENDED 1

#include <ncursesw/curses.h>
#include <caca.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <climits>
#include <map>
#include <utility>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>

#include <vsomeip/vsomeip.hpp>

#include "radio-client-state.hpp"
#include "radio-video.hpp"
#include "services.hpp"
#include "demo-health.hpp"

namespace {

using radio_ui::Availability;
using radio_ui::ClientState;
using radio_ui::Communication;
using radio_ui::Direction;
using radio_ui::Evidence;
using radio_ui::Health;
using radio_ui::ServiceStatus;
using radio_ui::StyledText;
using radio_ui::TextColor;

constexpr int kRefreshMs = 100;
constexpr int kMonitorMinRows = 16;

std::shared_ptr<vsomeip::application> app;
std::mutex state_mutex;
ClientState state;
std::atomic<bool> running{true};

// SOME/IP callbacks never call ncurses: the UI thread owns the terminal.
template <typename F>
void update_state(F update) {
    std::lock_guard<std::mutex> lock(state_mutex);
    update(state);
}

void on_volume_message(const std::shared_ptr<vsomeip::message> &event) {
    const std::uint32_t value = get_arg0(event);
    update_state([&](ClientState &s) { s.volume_event(value); });
}

void on_song_message(const std::shared_ptr<vsomeip::message> &event) {
    const std::string value = get_arg0_string(event);
    update_state([&](ClientState &s) { s.title_event(value); });
}

void on_artist_message(const std::shared_ptr<vsomeip::message> &event) {
    const std::string value = get_arg0_string(event);
    update_state([&](ClientState &s) { s.artist_event(value); });
}

void on_station_message(const std::shared_ptr<vsomeip::message> &event) {
    const std::string value = get_arg0_string(event);
    update_state([&](ClientState &s) {
        s.station_event(value, RADIO_RAM_FAILURE_MARKER);
    });
}

void on_radio_availability(vsomeip::service_t, vsomeip::instance_t, bool available) {
    update_state([&](ClientState &s) { s.radio_availability(available); });
}

void on_engine_availability(vsomeip::service_t, vsomeip::instance_t, bool available) {
    update_state([&](ClientState &s) { s.engine_availability(available); });
}

void on_engine_reverse_event(const std::shared_ptr<vsomeip::message> &event) {
    const bool reversed = get_arg0(event) != 0;
    update_state([&](ClientState &s) { s.reverse_event(reversed); });
}

void on_radio_health_event(const std::shared_ptr<vsomeip::message> &event) {
    const auto payload = event->get_payload();
    if (!payload)
        return;
    demo_health::Message message;
    if (!demo_health::decode(payload->get_data(), payload->get_length(), message))
        return;
    update_state([&](ClientState &s) { s.radio_health_event(message); });
}

void on_engine_health_event(const std::shared_ptr<vsomeip::message> &event) {
    const auto payload = event->get_payload();
    if (!payload)
        return;
    demo_health::Message message;
    if (!demo_health::decode(payload->get_data(), payload->get_length(), message))
        return;
    update_state([&](ClientState &s) { s.engine_health_event(message); });
}

void request_radio_events() {
    app->register_message_handler(RADIO_SERVICE_ID, RADIO_INSTANCE_ID,
                                  RADIO_VOLUME_EVENT_ID, on_volume_message);
    app->register_message_handler(RADIO_SERVICE_ID, RADIO_INSTANCE_ID,
                                  RADIO_STATION_EVENT_ID, on_station_message);
    app->register_message_handler(RADIO_SERVICE_ID, RADIO_INSTANCE_ID,
                                  RADIO_SONG_EVENT_ID, on_song_message);
    app->register_message_handler(RADIO_SERVICE_ID, RADIO_INSTANCE_ID,
                                  RADIO_ARTIST_EVENT_ID, on_artist_message);

    const std::set<vsomeip::eventgroup_t> groups{RADIO_EVENTGROUP_ID};
    for (vsomeip::event_t id : {RADIO_VOLUME_EVENT_ID, RADIO_STATION_EVENT_ID,
                                RADIO_SONG_EVENT_ID, RADIO_ARTIST_EVENT_ID}) {
        app->request_event(RADIO_SERVICE_ID, RADIO_INSTANCE_ID, id, groups,
                           vsomeip::event_type_e::ET_FIELD);
    }
    app->subscribe(RADIO_SERVICE_ID, RADIO_INSTANCE_ID, RADIO_EVENTGROUP_ID);

    app->register_message_handler(RADIO_SERVICE_ID, RADIO_INSTANCE_ID,
                                  RADIO_HEALTH_EVENT_ID, on_radio_health_event);
    const std::set<vsomeip::eventgroup_t> health_group{RADIO_HEALTH_EVENTGROUP_ID};
    app->request_event(RADIO_SERVICE_ID, RADIO_INSTANCE_ID,
                       RADIO_HEALTH_EVENT_ID, health_group,
                       vsomeip::event_type_e::ET_FIELD);
    app->subscribe(RADIO_SERVICE_ID, RADIO_INSTANCE_ID, RADIO_HEALTH_EVENTGROUP_ID);
}

void request_engine_events() {
    app->register_message_handler(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID,
                                  ENGINE_REVERSE_EVENT_ID, on_engine_reverse_event);
    app->register_message_handler(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID,
                                  ENGINE_HEALTH_EVENT_ID, on_engine_health_event);
    const std::set<vsomeip::eventgroup_t> reverse_group{ENGINE_EVENTGROUP_ID};
    app->request_event(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID,
                       ENGINE_REVERSE_EVENT_ID, reverse_group,
                       vsomeip::event_type_e::ET_FIELD);
    const std::set<vsomeip::eventgroup_t> health_group{ENGINE_HEALTH_EVENTGROUP_ID};
    app->request_event(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID,
                       ENGINE_HEALTH_EVENT_ID, health_group,
                       vsomeip::event_type_e::ET_FIELD);
    app->subscribe(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID, ENGINE_EVENTGROUP_ID);
    app->subscribe(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID, ENGINE_HEALTH_EVENTGROUP_ID);
}

bool setup_vsomeip() {
    app = vsomeip::runtime::get()->create_application("Radio client");
    if (!app || !app->init())
        return false;
    app->register_availability_handler(RADIO_SERVICE_ID, RADIO_INSTANCE_ID,
                                       on_radio_availability);
    app->register_availability_handler(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID,
                                       on_engine_availability);
    request_radio_events();
    request_engine_events();
    app->request_service(RADIO_SERVICE_ID, RADIO_INSTANCE_ID);
    app->request_service(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID);
    return true;
}

enum ColorPair {
    PairNormal = 0,
    PairGood = 1,
    PairWarning = 2,
    PairError = 3,
    PairMagenta = 4,
    PairMuted = 5,
    PairHeading = 6,
    PairVideoFirst = 7
};

int palette_color(TextColor color) {
    switch (color) {
    case TextColor::Red: return PairError;
    case TextColor::Yellow: return PairWarning;
    case TextColor::Magenta: return PairMagenta;
    case TextColor::Default: return PairNormal;
    }
    return PairNormal;
}

class Terminal {
public:
    bool initialize() {
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
            std::cerr << "radio-client requires an interactive terminal (TTY)\n";
            return false;
        }
        std::setlocale(LC_ALL, "");
        if (MB_CUR_MAX == 1)
            std::setlocale(LC_CTYPE, "C.UTF-8");
        if (!std::getenv("TERM"))
            setenv("TERM", "xterm", 0);

        // Use a dedicated copy of stdout for ncurses. Library messages can
        // subsequently go to a log file without corrupting the display.
        const int screen_fd = dup(STDOUT_FILENO);
        if (screen_fd < 0)
            return false;
        screen_output_ = fdopen(screen_fd, "w");
        if (!screen_output_) {
            close(screen_fd);
            return false;
        }
        setvbuf(screen_output_, nullptr, _IONBF, 0);
        screen_ = newterm(nullptr, screen_output_, stdin);
        if (!screen_)
            return false;
        set_term(screen_);
        cbreak();
        noecho();
        keypad(stdscr, TRUE);
        wtimeout(stdscr, kRefreshMs);
        curs_set(0);
        set_escdelay(25);
        if (has_colors()) {
            start_color();
            default_colors_ = use_default_colors() == OK;
            const short background = default_colors_ ? -1 : COLOR_BLACK;
            init_pair(PairGood, COLOR_GREEN, background);
            init_pair(PairWarning, COLOR_YELLOW, background);
            init_pair(PairError, COLOR_RED, background);
            init_pair(PairMagenta, COLOR_MAGENTA, background);
            init_pair(PairMuted, COLOR_CYAN, background);
            init_pair(PairHeading, COLOR_WHITE, background);
            colors_ = true;
        }
        redirect_console_logs();
        return true;
    }

    ~Terminal() {
        if (video_window_)
            delwin(video_window_);
        if (screen_) {
            set_term(screen_);
            endwin();
            delscreen(screen_);
        }
        if (screen_output_)
            fclose(screen_output_);
        if (saved_stdout_ >= 0) {
            fflush(stdout);
            dup2(saved_stdout_, STDOUT_FILENO);
            close(saved_stdout_);
        }
        if (saved_stderr_ >= 0) {
            fflush(stderr);
            dup2(saved_stderr_, STDERR_FILENO);
            close(saved_stderr_);
        }
    }

    int read_key(wint_t &key) const {
        return get_wch(&key);
    }

    void set_input_timeout(int milliseconds) const {
        wtimeout(stdscr, milliseconds);
    }

    void render(const ClientState &s, bool monitor) const {
        erase();
        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        if (rows < 6 || cols < 28) {
            draw_text(0, 0, cols - 1, "Resize terminal (min 28x6)", PairWarning, true);
            refresh();
            return;
        }
        const int content_width = cols - 2;
        if (s.radio.availability == Availability::Unavailable) {
            draw_text(0, 1, content_width, "RADIO UNAVAILABLE", PairError, true);
            draw_text(1, 1, content_width, "Waiting for radio service", PairMuted);
            draw_text(2, 1, content_width, "Last title: " + s.last_good_title, PairMuted);
        } else if (s.radio.availability == Availability::Unknown) {
            draw_text(0, 1, content_width, "RADIO", PairHeading, true);
            draw_text(1, 1, content_width, "Connecting to radio service...", PairMuted);
        } else if (s.ram_warning) {
            draw_text(0, 1, content_width, "RADIO / MEMORY WARNING", PairError, true);
            draw_text(1, 1, content_width, "!  R A M  !", PairError, true, true);
            draw_text(2, 1, content_width, "Memory allocation failed", PairError);
        } else {
            draw_text(0, 1, content_width,
                      s.station_received ? s.station : "Radio station: --", PairHeading, true);
            draw_text(1, 1, content_width,
                      s.title_received ? s.title : "Waiting for song...", PairNormal, true);
            draw_text(2, 1, content_width,
                      s.artist_received ? s.artist : "", PairMuted);
        }
        draw_volume(3, cols, s);
        draw_status(4, cols, s);
        draw_text(5, 1, content_width,
                  "+/- Volume   SPACE Station   ESC Power   M Monitor   V Video   Q Quit", PairMuted);
        if (monitor)
            draw_monitor(s, rows, cols);
        refresh();
    }

    // Radio/monitor and video never draw into each other's windows. Video
    // owns a single full-screen ncurses WINDOW, including the browser UI.
    void render_video(const radio_video::Player &player) {
        int rows = 0, cols = 0;
        getmaxyx(stdscr, rows, cols);
        if (rows <= 0 || cols <= 0)
            return;
        if (!video_window_ || rows != video_rows_ || cols != video_cols_) {
            if (video_window_)
                delwin(video_window_);
            video_window_ = newwin(rows, cols, 0, 0);
            video_rows_ = rows;
            video_cols_ = cols;
        }
        if (!video_window_)
            return;
        werase(video_window_);
        if (cols < radio_video::kMinimumColumns ||
            rows < radio_video::kMinimumRows) {
            video_line(0, "VIDEO  (X/V to return)", PairHeading, true);
            video_line(2, "Resize terminal to at least 100x30 characters", PairWarning);
        } else if (player.playing()) {
            draw_video_frame(player, rows, cols);
        } else {
            draw_video_library(player, rows, cols);
        }
        wnoutrefresh(video_window_);
        doupdate();
    }

private:
    static std::string printable(const std::string &text) {
        std::string result = text;
        for (char &character : result) {
            if (static_cast<unsigned char>(character) < 32 || character == 127)
                character = '?';
        }
        return result;
    }

    void video_line(int row, const std::string &text, int color = PairNormal,
                    bool bold = false, bool reverse = false, int column = 1) const {
        if (!video_window_ || row < 0 || row >= video_rows_ ||
            column < 0 || column >= video_cols_)
            return;
        int attrs = attribute(color, bold, reverse);
        wattron(video_window_, attrs);
        const std::string clean = printable(text);
        mvwaddnstr(video_window_, row, column, clean.c_str(), video_cols_ - column);
        wattroff(video_window_, attrs);
    }

    void draw_video_library(const radio_video::Player &player, int rows, int cols) const {
        video_line(1, "VIDEO LIBRARY", PairHeading, true);
        video_line(2, player.directory(), PairMuted);
        video_line(4, "Select an animation (X stops playback):", PairNormal);
        const auto &entries = player.entries();
        const int slots = std::max(0, rows - 9);
        const std::size_t visible = static_cast<std::size_t>(slots);
        const std::size_t first = player.selection() >= visible && visible > 0
                ? player.selection() - visible + 1 : 0;
        for (std::size_t index = first;
             index < entries.size() && index - first < visible; ++index) {
            const bool chosen = index == player.selection();
            std::string name = entries[index].name;
            if (entries[index].directory)
                name += "  [frames/]";
            video_line(static_cast<int>(index - first) + 6,
                       (chosen ? "> " : "  ") + name,
                       chosen ? PairHeading : PairNormal, chosen, chosen, 2);
        }
        if (entries.empty() && player.message().empty())
            video_line(6, "No animations found", PairWarning);
        if (!player.message().empty())
            video_line(rows - 3, player.message(), PairWarning);
        video_line(rows - 1,
                   "UP/DOWN Select   ENTER Play   R Reload   V/ESC/X Radio   Q Quit",
                   PairMuted);
        (void)cols;
    }

    // libcaca uses the 16 DOS ANSI colour IDs (BGR), whereas curses uses
    // the conventional RGB numbering. Allocate pairs lazily for the media.
    int video_color(unsigned c, bool foreground) const {
        static const short caca_to_curses[] = {
            COLOR_BLACK, COLOR_BLUE, COLOR_GREEN, COLOR_CYAN,
            COLOR_RED, COLOR_MAGENTA, COLOR_YELLOW, COLOR_WHITE
        };
        if (c >= 16)
            return default_colors_ ? -1 : (foreground ? COLOR_WHITE : COLOR_BLACK);
        int mapped = caca_to_curses[c % 8];
        if (c >= 8 && COLORS >= 16)
            mapped += 8;
        return mapped;
    }

    int video_attribute(std::uint32_t caca_attr) {
        if (!colors_)
            return 0;
        const unsigned fg = caca_attr_to_ansi_fg(caca_attr);
        const unsigned bg = caca_attr_to_ansi_bg(caca_attr);
        const int foreground = video_color(fg, true);
        const int background = video_color(bg, false);
        const auto key = std::make_pair(foreground, background);
        auto found = video_pairs_.find(key);
        short pair = 0;
        if (found != video_pairs_.end()) {
            pair = found->second;
        } else if (next_video_pair_ < COLOR_PAIRS && next_video_pair_ < SHRT_MAX) {
            const short candidate = static_cast<short>(next_video_pair_);
            if (init_pair(candidate, static_cast<short>(foreground),
                          static_cast<short>(background)) == OK) {
                pair = candidate;
                video_pairs_.emplace(key, pair);
                ++next_video_pair_;
            }
        }
        return COLOR_PAIR(pair) | (fg >= 8 && fg < 16 && COLORS < 16 ? A_BOLD : 0);
    }

    void draw_video_frame(const radio_video::Player &player, int rows, int cols) {
        const auto *animation = player.animation();
        const int need_cols = std::max(radio_video::kMinimumColumns, animation->width());
        const int need_rows = std::max(radio_video::kMinimumRows, animation->height());
        if (cols < need_cols || rows < need_rows) {
            video_line(1, "VIDEO: " + player.entries()[player.selection()].name,
                       PairHeading, true);
            video_line(3, "This animation requires a terminal of at least " +
                       std::to_string(need_cols) + "x" + std::to_string(need_rows),
                       PairWarning);
            video_line(5, "Resize the terminal, or press X to select another video.");
            return;
        }
        const auto &frame = player.current_frame();
        caca_set_frame(frame.canvas.get(), frame.index);
        const int offset_x = (cols - animation->width()) / 2;
        const int offset_y = (rows - animation->height()) / 2;
        int last_attr = -1;
        for (int y = 0; y < frame.height; ++y) {
            for (int x = 0; x < frame.width; ++x) {
                const std::uint32_t cp = caca_get_char(frame.canvas.get(), x, y);
                // libcaca marks the second cell of a wide character with
                // an internal sentinel. ncursesw draws it automatically.
                if (cp == CACA_MAGIC_FULLWIDTH || cp < 32 || cp > 0x10ffff)
                    continue;
                const wchar_t wc = static_cast<wchar_t>(cp);
                const int width = wcwidth(wc);
                if (width <= 0 || x + width > frame.width ||
                    offset_x + x + width > cols)
                    continue;
                const int attr = video_attribute(caca_get_attr(frame.canvas.get(), x, y));
                if (attr != last_attr) {
                    wattrset(video_window_, attr);
                    last_attr = attr;
                }
                const wchar_t text[2] = {wc, 0};
                mvwaddnwstr(video_window_, offset_y + y, offset_x + x, text, 1);
            }
        }
        wattrset(video_window_, A_NORMAL);
        if (offset_y > 0) {
            video_line(0, "VIDEO: " + player.entries()[player.selection()].name +
                       "   X Stop   V Radio", PairMuted);
        }
    }

    void redirect_console_logs() {
        const char *path = std::getenv("RADIO_CLIENT_LOG_FILE");
        if (!path)
            path = "/tmp/radio-client-vsomeip.log";
        if (std::strcmp(path, "-") == 0)
            return;
        const int log_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (log_fd < 0)
            return;
        saved_stdout_ = dup(STDOUT_FILENO);
        saved_stderr_ = dup(STDERR_FILENO);
        fflush(stdout);
        fflush(stderr);
        dup2(log_fd, STDOUT_FILENO);
        dup2(log_fd, STDERR_FILENO);
        close(log_fd);
    }

    int attribute(int pair, bool bold = false, bool reverse = false) const {
        int attr = 0;
        if (colors_ && pair)
            attr |= COLOR_PAIR(pair);
        if (bold || (pair == PairError && !colors_))
            attr |= A_BOLD;
        if (reverse)
            attr |= A_REVERSE;
        return attr;
    }

    void draw_text(int row, int col, int width, const std::string &text,
                   int pair = PairNormal, bool bold = false, bool reverse = false) const {
        if (width <= 0)
            return;
        draw_styled(row, col, width, radio_ui::parse_styled_text(text),
                    pair, bold, reverse);
    }

    void draw_styled(int row, int col, int width, const StyledText &styled,
                     int base_pair, bool bold, bool reverse) const {
        int screen_rows, screen_cols;
        getmaxyx(stdscr, screen_rows, screen_cols);
        if (row < 0 || row >= screen_rows || col >= screen_cols - 1)
            return;
        const int limit = std::min(screen_cols - 1, col + width);
        int x = col;
        for (const auto &glyph : styled.glyphs) {
            const int char_width = wcwidth(glyph.value);
            if (char_width <= 0)
                continue;
            if (x + char_width > limit)
                break;
            const int color = glyph.color == TextColor::Default
                    ? base_pair : palette_color(glyph.color);
            const int attr = attribute(color, bold || glyph.color != TextColor::Default, reverse);
            wchar_t character[2] = {glyph.value, L'\0'};
            attron(attr);
            mvaddnwstr(row, x, character, 1);
            attroff(attr);
            x += char_width;
        }
    }

    void draw_volume(int row, int cols, const ClientState &s) const {
        const std::string label = "VOL ";
        draw_text(row, 1, cols - 2, label, PairMuted);
        if (!s.volume_received || s.radio.availability != Availability::Available) {
            draw_text(row, 5, cols - 6, "--", PairMuted);
            return;
        }
        const unsigned volume = std::min(100u, s.volume);
        const int bar_size = std::min(20, std::max(0, cols - 17));
        if (bar_size < 4) {
            draw_text(row, 5, cols - 6, std::to_string(volume) + "%");
            return;
        }
        const int filled = static_cast<int>((volume * bar_size + 50) / 100);
        const std::string meter = "[" + std::string(filled, '#') +
                                  std::string(bar_size - filled, '-') + "] ";
        draw_text(row, 5, cols - 6, meter + std::to_string(volume) + "%",
                  PairNormal);
    }

    void draw_status(int row, int cols, const ClientState &s) const {
        int x = 1;
        const int limit = cols - 1;
        const bool narrow = cols < 44;
        auto token = [&](const std::string &value, int pair, bool bold = false) {
            if (x >= limit)
                return;
            draw_text(row, x, limit - x, value, pair, bold);
            x += static_cast<int>(value.size());
        };
        auto sep = [&]() { token(narrow ? " " : " | ", PairMuted); };
        const char *cpu_label = narrow ? "C!" : " CPU!";
        const char *ram_label = narrow ? "M!" : " RAM!";

        if (s.engine.availability != Availability::Available ||
            s.direction == Direction::Unknown) {
            token("? DIR", PairWarning, true);
        } else if (s.direction == Direction::Reverse) {
            token("< REV", PairWarning, true);
        } else {
            token("> FWD", PairGood, true);
        }
        sep();
        draw_service_status(token, "R", s.radio);
        if (s.radio.cpu.state == Health::Degraded || s.radio.cpu.state == Health::Failed)
            token(cpu_label, PairWarning, true);
        if (s.radio.ram.state == Health::Degraded || s.radio.ram.state == Health::Failed)
            token(ram_label, PairError, true);
        sep();
        draw_service_status(token, "E", s.engine);
        if (s.engine.cpu.state == Health::Degraded || s.engine.cpu.state == Health::Failed)
            token(cpu_label, PairWarning, true);
        if (s.engine.ram.state == Health::Degraded || s.engine.ram.state == Health::Failed)
            token(ram_label, PairError, true);
    }

    template <typename Add>
    static void draw_service_status(Add add, const char *name, const ServiceStatus &service) {
        std::string label(name);
        if (service.availability == Availability::Available &&
            service.communication == Communication::Stale)
            add(label + ":STALE", PairWarning, true);
        else if (service.availability == Availability::Available)
            add(label + ":+", PairGood, true);
        else if (service.availability == Availability::Unavailable)
            add(label + ":OFF", PairError, true);
        else
            add(label + ":?", PairWarning, true);
    }

    static const char *direction_text(Direction value) {
        switch (value) {
        case Direction::Forward: return "FWD";
        case Direction::Reverse: return "REV";
        case Direction::Unknown: return "UNKNOWN";
        }
        return "UNKNOWN";
    }

    static const char *availability_text(Availability value) {
        switch (value) {
        case Availability::Available: return "AVAILABLE";
        case Availability::Unavailable: return "UNAVAILABLE";
        case Availability::Unknown: return "UNKNOWN";
        }
        return "UNKNOWN";
    }

    static const char *communication_text(Communication value) {
        switch (value) {
        case Communication::Unknown: return "UNKNOWN";
        case Communication::Receiving: return "EVENTS SEEN";
        case Communication::Unavailable: return "UNAVAILABLE";
        case Communication::Stale: return "STALE";
        }
        return "UNKNOWN";
    }

    static std::string health_text(const radio_ui::HealthAxis &axis) {
        const char *status = "UNKNOWN";
        switch (axis.state) {
        case Health::Normal: status = "NORMAL"; break;
        case Health::Degraded: status = "DEGRADED"; break;
        case Health::Failed: status = "FAILED"; break;
        case Health::Unknown: break;
        }
        if (axis.source == Evidence::DemoIndicator)
            return std::string(status) + " (demo)";
        return status;
    }

    static std::string age_text(const ServiceStatus &service) {
        if (!service.has_message)
            return "--";
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - service.last_message).count();
        std::ostringstream out;
        out << std::fixed << std::setprecision(1)
            << std::max<std::int64_t>(0, age) / 1000.0 << "s ago";
        return out.str();
    }

    static std::string health_age_text(bool seen,
                    std::chrono::steady_clock::time_point time) {
        if (!seen)
            return "--";
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - time).count();
        std::ostringstream out;
        out << std::fixed << std::setprecision(1)
            << std::max<std::int64_t>(0, age) / 1000.0 << "s ago";
        return out.str();
    }

    static std::string cpu_text(const ServiceStatus &service, bool share) {
        if (!service.cpu_metrics.available)
            return "--";
        std::ostringstream out;
        out << std::fixed << std::setprecision(1);
        if (share)
            out << service.cpu_metrics.cpu_share_percent << "%";
        else
            out << service.cpu_metrics.deadline_overrun_ms << "ms";
        return out.str();
    }

    static std::string ram_text(const ServiceStatus &service) {
        if (!service.ram_metrics.available)
            return "--";
        std::ostringstream out;
        out << std::fixed << std::setprecision(0)
            << service.ram_metrics.allocated_bytes / (1024.0 * 1024.0) << " MiB";
        return out.str();
    }

    void draw_monitor(const ClientState &s, int rows, int cols) const {
        if (rows < kMonitorMinRows || cols < 54) {
            draw_text(7, 1, cols - 2, "Monitor: resize to at least 54x16", PairWarning);
            return;
        }
        draw_text(7, 1, cols - 2, "INTERFERENCE MONITOR  (M to close)", PairHeading, true);
        const int col_radio = std::max(20, cols / 3);
        const int col_engine = std::max(37, 2 * cols / 3);
        draw_text(8, col_radio, col_engine - col_radio - 2, "RADIO", PairHeading, true);
        draw_text(8, col_engine, cols - col_engine - 1, "ENGINE", PairHeading, true);
        auto line = [&](int row, const std::string &name,
                        const std::string &radio_value, const std::string &engine_value) {
            draw_text(row, 1, col_radio - 2, name, PairMuted);
            draw_text(row, col_radio, col_engine - col_radio - 2, radio_value);
            draw_text(row, col_engine, cols - col_engine - 1, engine_value);
        };
        line(9,  "Service", availability_text(s.radio.availability),
               availability_text(s.engine.availability));
        line(10, "CPU", health_text(s.radio.cpu), health_text(s.engine.cpu));
        line(11, "RAM", health_text(s.radio.ram), health_text(s.engine.ram));
        line(12, "Data link", communication_text(s.radio.communication),
             communication_text(s.engine.communication));
        line(13, "Last event", age_text(s.radio), age_text(s.engine));
        line(14, "CPU share", cpu_text(s.radio, true), cpu_text(s.engine, true));
        line(15, "Deadline overrun", cpu_text(s.radio, false), cpu_text(s.engine, false));
        if (rows >= 17)
            line(16, "Allocated RAM", ram_text(s.radio), ram_text(s.engine));
        if (rows >= 18) {
            std::string direction = direction_text(s.direction);
            if (s.direction == Direction::Unknown &&
                s.last_known_direction != Direction::Unknown)
                direction = std::string(direction_text(s.last_known_direction)) + " (last)";
            line(17, "Direction", "--", direction);
        }
        if (rows >= 19)
            line(18, "Health age", health_age_text(s.radio_health_seen, s.radio_health_at),
                 health_age_text(s.engine_health_seen, s.engine_health_at));
        if (rows >= 20)
            line(19, "Health sequence",
                 s.radio_health_seen ? std::to_string(s.radio_health_sequence) : "--",
                 s.engine_health_seen ? std::to_string(s.engine_health_sequence) : "--");
        if (rows >= 22) {
            draw_text(21, 1, cols - 2,
                      "CPU/RAM from SOME/IP health; (demo) = metadata fallback; -- = no measurement",
                      PairMuted);
        }
    }

    WINDOW *video_window_ = nullptr;
    int video_rows_ = 0;
    int video_cols_ = 0;
    int next_video_pair_ = PairVideoFirst;
    std::map<std::pair<int, int>, short> video_pairs_;
    bool default_colors_ = false;
    SCREEN *screen_ = nullptr;
    FILE *screen_output_ = nullptr;
    int saved_stdout_ = -1;
    int saved_stderr_ = -1;
    bool colors_ = false;
};

void handle_key(wint_t key, bool &monitor_visible, bool &off_requested) {
    std::uint32_t method = 0;
    std::uint32_t argument = 0;
    switch (key) {
    case L'q': case L'Q':
        running.store(false);
        return;
    case L'm': case L'M':
        monitor_visible = !monitor_visible;
        return;
    case L'+':
        method = RADIO_CHANGE_VOLUME_METHOD_ID;
        argument = 10;
        break;
    case L'-':
        method = RADIO_CHANGE_VOLUME_METHOD_ID;
        argument = static_cast<std::uint32_t>(-10);
        break;
    case L' ':
        method = RADIO_SWITCH_STATION_METHOD_ID;
        break;
    case 27:
        method = RADIO_SET_PLAYING_METHOD_ID;
        argument = off_requested ? 1 : 0;
        off_requested = !off_requested;
        break;
    default:
        return;
    }
    app->send(new_request(method, argument));
}

} // namespace

int main() {
    if (!setup_vsomeip()) {
        std::cerr << "Couldn't initialize the radio client\n";
        return EXIT_FAILURE;
    }

    {
        Terminal terminal;
        if (!terminal.initialize()) {
            std::cerr << "Couldn't initialize ncursesw\n";
            return EXIT_FAILURE;
        }

        std::thread vsomeip_thread([] {
            app->start();
            running.store(false);
        });

        bool monitor_visible = false;
        bool video_visible = false;
        bool off_requested = false;
        radio_video::Player video(radio_video::media_directory());
        std::uint64_t rendered_generation = static_cast<std::uint64_t>(-1);
        bool redraw = true;
        auto last_rendered = std::chrono::steady_clock::time_point{};
        while (running.load()) {
            terminal.set_input_timeout(video_visible
                    ? video.wait_ms(std::chrono::steady_clock::now()) : kRefreshMs);
            wint_t key = 0;
            const int result = terminal.read_key(key);
            if (result == KEY_CODE_YES && key == KEY_RESIZE) {
                redraw = true;
            } else if (result == OK && (key == L'q' || key == L'Q')) {
                running.store(false);
            } else if (video_visible) {
                if (result == OK && (key == L'v' || key == L'V')) {
                    video.stop();
                    video_visible = false;
                    redraw = true;
                } else if (result == OK && (key == L'x' || key == L'X' || key == 27)) {
                    if (video.playing())
                        video.stop();
                    else
                        video_visible = false;
                    redraw = true;
                } else if (!video.playing()) {
                    if (result == KEY_CODE_YES && key == KEY_UP) {
                        video.move(-1); redraw = true;
                    } else if (result == KEY_CODE_YES && key == KEY_DOWN) {
                        video.move(1); redraw = true;
                    } else if (result == KEY_CODE_YES && key == KEY_PPAGE) {
                        video.page(-10); redraw = true;
                    } else if (result == KEY_CODE_YES && key == KEY_NPAGE) {
                        video.page(10); redraw = true;
                    } else if ((result == KEY_CODE_YES && key == KEY_ENTER) ||
                               (result == OK && (key == L'\n' || key == L'\r'))) {
                        video.select(std::chrono::steady_clock::now());
                        redraw = true;
                    } else if (result == OK && (key == L'r' || key == L'R')) {
                        video.browse(); redraw = true;
                    }
                }
            } else if (result == OK) {
                if (key == L'v' || key == L'V') {
                    video.browse();
                    video_visible = true;
                } else {
                    handle_key(key, monitor_visible, off_requested);
                }
                redraw = true;
            }

            // Continue receiving and aging telemetry while the video overlay
            // is visible. No video operation sends radio control messages.
            ClientState snapshot;
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                const auto now = std::chrono::steady_clock::now();
                state.refresh_radio_health(now);
                state.refresh_engine_health(now);
                snapshot = state;
            }
            const auto now = std::chrono::steady_clock::now();
            if (video_visible) {
                const bool frame_due = video.advance(now);
                if (redraw || frame_due) {
                    terminal.render_video(video);
                    redraw = false;
                }
            } else if (redraw || snapshot.generation != rendered_generation ||
                       (monitor_visible && now - last_rendered >= std::chrono::seconds(1))) {
                terminal.render(snapshot, monitor_visible);
                rendered_generation = snapshot.generation;
                last_rendered = now;
                redraw = false;
            }
        }

        app->stop();
        vsomeip_thread.join();
        app->clear_all_handler();
        app->unsubscribe(RADIO_SERVICE_ID, RADIO_INSTANCE_ID, RADIO_HEALTH_EVENTGROUP_ID);
        app->unsubscribe(RADIO_SERVICE_ID, RADIO_INSTANCE_ID, RADIO_EVENTGROUP_ID);
        app->release_event(RADIO_SERVICE_ID, RADIO_INSTANCE_ID, RADIO_HEALTH_EVENT_ID);
        for (vsomeip::event_t id : {RADIO_VOLUME_EVENT_ID, RADIO_STATION_EVENT_ID,
                                    RADIO_SONG_EVENT_ID, RADIO_ARTIST_EVENT_ID})
            app->release_event(RADIO_SERVICE_ID, RADIO_INSTANCE_ID, id);
        app->release_service(RADIO_SERVICE_ID, RADIO_INSTANCE_ID);
        app->unsubscribe(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID, ENGINE_HEALTH_EVENTGROUP_ID);
        app->unsubscribe(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID, ENGINE_EVENTGROUP_ID);
        app->release_event(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID, ENGINE_HEALTH_EVENT_ID);
        app->release_event(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID, ENGINE_REVERSE_EVENT_ID);
        app->release_service(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID);

    } // Restore the terminal before the original DLT shutdown workaround.
    std::fflush(nullptr);
    _exit(EXIT_SUCCESS);
}
