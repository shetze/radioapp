#include "sensors-activation.hpp"
#include "sensors-client-state.hpp"
#include "sensors-simulation.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <utility>

int main() {
    using namespace sensors;
    const auto t = ClientView::Clock::now();
    Activation activation;
    assert(activation.state() == CameraState::Fault);
    assert(!activation.active());
    activation.engine_reverse(true); // ignored before engine discovery
    assert(!activation.active());
    activation.engine_availability(true);
    assert(activation.state() == CameraState::Fault); // no direction yet
    activation.engine_reverse(false);
    assert(activation.state() == CameraState::Inactive);
    activation.engine_reverse(true);
    assert(activation.state() == CameraState::Active);
    const auto epoch = activation.epoch();

    SimulationConfig config;
    auto frame = simulate_camera(config, 16, epoch, 1);
    auto ranges = simulate_ultrasonic(16, epoch, 1);
    assert(frame.width == 100 && frame.height == 30 && frame.cells.size() == 3000);
    assert(ranges.valid_mask == 0x0f);
    for (auto distance : ranges.millimeters)
        assert(distance > 0 && distance <= 3000);
    assert(frame.at(2, 1).glyph == 'R');
    assert(frame.at(0, 0).glyph == ' ');
    const auto encoded = encode(frame);
    assert(encoded.size() == 16 + 3000 * 2);
    CameraFrame decoded;
    assert(decode(encoded.data(), encoded.size(), decoded));
    assert(decoded.width == 100 && decoded.height == 30);
    assert(decoded.epoch == epoch && decoded.sequence == 1);
    assert(decoded.cells[200].glyph == frame.cells[200].glyph);
    auto truncated = encoded;
    truncated.pop_back();
    assert(!decode(truncated.data(), truncated.size(), decoded));
    truncated = encoded;
    truncated[0] = 'X';
    assert(!decode(truncated.data(), truncated.size(), decoded));
    truncated = encoded;
    truncated[4] = 0;
    truncated[5] = 0;
    assert(!decode(truncated.data(), truncated.size(), decoded));
    truncated = encoded;
    truncated[16] = 0x1b; // no terminal escape / control bytes permitted
    assert(!decode(truncated.data(), truncated.size(), decoded));
    auto bad_frame = frame;
    bad_frame.cells[0].glyph = '\n';
    assert(encode(bad_frame).empty());
    assert(!decode(nullptr, encoded.size(), decoded));

    auto raw_status = encode(CameraStatus{CameraState::Active, epoch, 1});
    CameraStatus status;
    assert(decode(raw_status.data(), raw_status.size(), status));
    assert(status.state == CameraState::Active && status.epoch == epoch);
    raw_status[4] = 27;
    assert(!decode(raw_status.data(), raw_status.size(), status));
    const auto raw_ranges = encode(ranges);
    Ultrasonic parsed_ranges;
    assert(raw_ranges.size() == kUltrasonicWireSize);
    assert(decode(raw_ranges.data(), raw_ranges.size(), parsed_ranges));
    assert(parsed_ranges.millimeters == ranges.millimeters);
    auto invalid_ranges = raw_ranges;
    invalid_ranges[12] = 0xff;
    assert(!decode(invalid_ranges.data(), invalid_ranges.size(), parsed_ranges));

    ClientView view;
    assert(!view.online());
    assert(!view.frame(frame, t));
    view.availability(true);
    assert(!view.frame(frame, t)); // status/epoch required
    assert(view.status(CameraStatus{CameraState::Active, epoch, 10}, t));
    assert(view.camera_state(t) == CameraState::Active);
    assert(view.frame(frame, t));
    assert(!view.frame(frame, t)); // same sequence not accepted again
    assert(view.fresh_frame(t) && view.fresh_frame(t)->width == 100);
    assert(view.ultrasonic(ranges, t));
    assert(view.fresh_ultrasonic(t));
    assert(!view.ultrasonic(ranges, t));
    assert(!view.fresh_frame(t + std::chrono::milliseconds(1300)));
    assert(!view.fresh_ultrasonic(t + std::chrono::seconds(3)));
    assert(view.camera_state(t + std::chrono::seconds(4)) == CameraState::Unknown);
    assert(view.status(CameraStatus{CameraState::Active, epoch, 11},
                       t + std::chrono::milliseconds(1500)));
    assert(!view.fresh_frame(t + std::chrono::milliseconds(1500))); // no resurrected old frame
    assert(view.status(CameraStatus{CameraState::Inactive, epoch + 1, 12},
                       t + std::chrono::milliseconds(1600)));
    assert(!view.fresh_frame(t + std::chrono::milliseconds(1600)));
    assert(!view.frame(frame, t + std::chrono::milliseconds(1600)));
    assert(!view.status(CameraStatus{CameraState::Active, epoch, 100},
                        t + std::chrono::milliseconds(1700))); // old epoch replay
    assert(view.status(CameraStatus{CameraState::Active, epoch + 2, 1},
                       t + std::chrono::milliseconds(1800)));
    auto new_frame = frame;
    new_frame.epoch = epoch + 2;
    new_frame.sequence = 1;
    assert(view.frame(std::move(new_frame), t + std::chrono::milliseconds(1801)));
    view.availability(false);
    assert(!view.fresh_frame(t + std::chrono::milliseconds(1802)));
    assert(view.camera_state(t + std::chrono::milliseconds(1802)) == CameraState::Unknown);
    activation.engine_availability(false);
    assert(activation.state() == CameraState::Fault);
    assert(!activation.active());

    config.width = 160;
    config.height = 50;
    frame = simulate_camera(config, 38, epoch, 44);
    assert(frame.cells.size() == 160u * 50u);
    const auto big_encoded = encode(frame);
    assert(decode(big_encoded.data(), big_encoded.size(), decoded));
    assert(decoded.width == 160 && decoded.height == 50);
    config.width = 300;
    assert(simulate_camera(config, 0, epoch, 1).cells.empty());

    assert(!show_reverse_camera(false, false, false));
    assert(!show_reverse_camera(true, false, false)); // radio mode: no camera
    assert(show_reverse_camera(true, true, false)); // monitor unfolded
    assert(show_reverse_camera(true, false, true)); // local video open
    assert(show_reverse_camera(true, true, true));
    assert(!show_reverse_camera(false, true, true));
    std::puts("sensors-test OK");
}
