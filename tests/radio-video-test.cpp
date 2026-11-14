#include "radio-video.hpp"

#include <caca.h>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

void write_file(const std::string &path, const std::string &contents) {
    std::ofstream file(path, std::ios::binary);
    assert(file.good());
    file << contents;
    assert(file.good());
}

radio_video::Entry find(const std::vector<radio_video::Entry> &entries,
                        const std::string &name) {
    for (const auto &entry : entries)
        if (entry.name == name) return entry;
    assert(false && "expected entry not found");
    return {};
}

} // namespace

int main() {
    char template_path[] = "/tmp/radio-video-test-XXXXXX";
    char *directory = mkdtemp(template_path);
    assert(directory);
    const std::string root(directory);
    const std::string clip = root + "/clip.ansi";
    // Native libcaca ANSI/UTF-8 frame delimiter: form-feed + newline.
    write_file(clip, "\033[31mA\033[0m\f\n\033[32mB\033[0m");
    write_file(clip + ".fps", "10\n");
    assert(mkdir((root + "/seq").c_str(), 0700) == 0);
    write_file(root + "/seq/02.ansi", "B\n");
    write_file(root + "/seq/01.ansi", "A\n");
    write_file(root + "/seq/animation.fps", "20\n");
    write_file(root + "/text.txt", "Example!\n");
    write_file(root + "/ignored.png", "not an animation");
    assert(symlink(clip.c_str(), (root + "/linked.ansi").c_str()) == 0);

    std::string error;
    const auto entries = radio_video::discover(root, error);
    assert(error.empty());
    assert(entries.size() == 3);  // seq, clip, text; symlink ignored
    assert(entries[0].name == "clip.ansi");
    assert(entries[1].name == "seq" && entries[1].directory);
    assert(entries[2].name == "text.txt");

    auto animation = radio_video::Animation::load(find(entries, "clip.ansi"), error);
    assert(animation && error.empty());
    assert(animation->fps() == 10);
    assert(animation->size() == 2);
    assert(animation->width() == 1 && animation->height() == 1);
    const auto &frame_a = animation->frame(0);
    const auto &frame_b = animation->frame(1);
    assert(caca_set_frame(frame_a.canvas.get(), frame_a.index) == 0);
    assert(caca_get_char(frame_a.canvas.get(), 0, 0) == 'A');
    const auto color_a = caca_attr_to_ansi_fg(caca_get_attr(frame_a.canvas.get(), 0, 0));
    assert(caca_set_frame(frame_b.canvas.get(), frame_b.index) == 0);
    assert(caca_get_char(frame_b.canvas.get(), 0, 0) == 'B');
    const auto color_b = caca_attr_to_ansi_fg(caca_get_attr(frame_b.canvas.get(), 0, 0));
    assert(color_a != color_b);

    animation = radio_video::Animation::load(find(entries, "seq"), error);
    assert(animation && animation->size() == 2 && animation->fps() == 20);
    const auto &first = animation->frame(0);
    assert(caca_set_frame(first.canvas.get(), first.index) == 0);
    assert(caca_get_char(first.canvas.get(), 0, 0) == 'A');
    const auto &second = animation->frame(1);
    assert(caca_set_frame(second.canvas.get(), second.index) == 0);
    assert(caca_get_char(second.canvas.get(), 0, 0) == 'B');

    radio_video::Player player(root);
    player.browse();
    assert(player.entries().size() == 3);
    assert(player.selection() == 0);
    const auto start = radio_video::Player::Clock::now();
    player.select(start);
    assert(player.playing());
    assert(player.current_frame().index == 0);
    assert(player.wait_ms(start) == 100);
    assert(!player.advance(start + std::chrono::milliseconds(99)));
    assert(player.wait_ms(start + std::chrono::milliseconds(99)) == 1);
    assert(player.advance(start + std::chrono::milliseconds(100)));
    assert(player.current_frame().index == 1);
    assert(player.advance(start + std::chrono::milliseconds(200)));
    assert(player.current_frame().index == 0); // loop automatically
    assert(player.advance(start + std::chrono::milliseconds(550)));
    assert(player.current_frame().index == 1); // skip frames if UI is delayed
    assert(!player.advance(start + std::chrono::milliseconds(551)));
    player.stop();
    assert(!player.playing());
    player.move(500);
    assert(player.selection() == 2);
    player.move(-500);
    assert(player.selection() == 0);

    write_file(clip + ".fps", "500\n");
    assert(!radio_video::Animation::load(find(entries, "clip.ansi"), error));
    assert(error.find("Invalid FPS") != std::string::npos);
    assert(unlink((clip + ".fps").c_str()) == 0);
    write_file(root + "/invalid.caca", "garbage");
    const auto invalid = radio_video::discover(root, error);
    assert(invalid.size() == 4);
    assert(!radio_video::Animation::load(find(invalid, "invalid.caca"), error));
    assert(error.find("decode") != std::string::npos);

    // Native libcaca .caca files retain multiple differently sized frames.
    caca_canvas_t *native = caca_create_canvas(120, 40);
    assert(native);
    assert(caca_put_char(native, 0, 0, 'A') == 1);
    assert(caca_create_frame(native, 1) == 0);
    assert(caca_set_frame(native, 1) == 0);
    assert(caca_put_char(native, 0, 0, 'B') == 1);
    std::size_t caca_bytes = 0;
    void *caca_data = caca_export_canvas_to_memory(native, "caca", &caca_bytes);
    assert(caca_data && caca_bytes > 0);
    const std::string native_path = root + "/native.caca";
    write_file(native_path, std::string(static_cast<char *>(caca_data), caca_bytes));
    std::free(caca_data);
    assert(caca_free_canvas(native) == 0);
    const radio_video::Entry native_entry{"native.caca", native_path, false};
    animation = radio_video::Animation::load(native_entry, error);
    assert(animation && error.empty());
    assert(animation->size() == 2);
    assert(animation->width() == 120 && animation->height() == 40);
    assert(caca_set_frame(animation->frame(0).canvas.get(), 0) == 0);
    assert(caca_get_char(animation->frame(0).canvas.get(), 0, 0) == 'A');
    assert(caca_set_frame(animation->frame(1).canvas.get(), 1) == 0);
    assert(caca_get_char(animation->frame(1).canvas.get(), 0, 0) == 'B');

    // The shipped scene exercises 100 frame imports, exactly 100x30 cells,
    // and a true full-length loop (not only the two-frame unit fixture).
    const radio_video::Entry demo{"city-traffic.ansi", VIDEO_DEMO_PATH, false};
    animation = radio_video::Animation::load(demo, error);
    assert(animation && error.empty());
    assert(animation->size() == 100);
    assert(animation->fps() == 10);
    assert(animation->width() == 100 && animation->height() == 30);
    assert(animation->frame(50).width == 100);

    assert(unlink((root + "/invalid.caca").c_str()) == 0);
    assert(unlink((root + "/native.caca").c_str()) == 0);
    assert(unlink((root + "/linked.ansi").c_str()) == 0);
    assert(unlink((root + "/text.txt").c_str()) == 0);
    assert(unlink((root + "/seq/02.ansi").c_str()) == 0);
    assert(unlink((root + "/seq/01.ansi").c_str()) == 0);
    assert(unlink((root + "/seq/animation.fps").c_str()) == 0);
    assert(rmdir((root + "/seq").c_str()) == 0);
    assert(unlink(clip.c_str()) == 0);
    assert(unlink((root + "/ignored.png").c_str()) == 0);
    assert(rmdir(root.c_str()) == 0);
    std::puts("radio-video-test OK");
}
