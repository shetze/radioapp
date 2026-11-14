#pragma once

#include "sensors-protocol.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>

// Independent of SOME/IP, libcaca and ncurses for deterministic UI tests.
namespace sensors {

class ClientView {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;

    void availability(bool available) {
        if (!available || !online_) {
            camera_status_ = CameraStatus{};
            camera_.reset();
            ultrasonic_ = Ultrasonic{};
            have_ultrasonic_ = false;
            frame_at_ = status_at_ = ultrasonic_at_ = Time{};
            have_frame_seq_ = have_ultrasonic_seq_ = false;
        }
        online_ = available;
        ++generation_;
    }

    bool status(const CameraStatus &status, Time now) {
        if (!online_)
            return false;
        if (camera_status_.state != CameraState::Unknown &&
            ((status.epoch == camera_status_.epoch &&
              !newer(status.sequence, camera_status_.sequence)) ||
             (status.epoch != camera_status_.epoch &&
              !newer(status.epoch, camera_status_.epoch))))
            return false;
        if (camera_status_.state != CameraState::Active || status.epoch != camera_status_.epoch ||
            status.state != CameraState::Active) {
            camera_.reset();
            have_frame_seq_ = false;
            have_ultrasonic_ = false;
            have_ultrasonic_seq_ = false;
        }
        camera_status_ = status;
        status_at_ = now;
        ++generation_;
        return true;
    }

    bool frame(CameraFrame frame, Time now) {
        if (!online_ || camera_status_.state != CameraState::Active ||
            camera_status_.epoch != frame.epoch ||
            (have_frame_seq_ && !newer(frame.sequence, last_frame_sequence_)))
            return false;
        last_frame_sequence_ = frame.sequence;
        have_frame_seq_ = true;
        camera_ = std::make_shared<const CameraFrame>(std::move(frame));
        frame_at_ = now;
        ++generation_;
        return true;
    }

    bool ultrasonic(const Ultrasonic &sample, Time now) {
        if (!online_ || camera_status_.state != CameraState::Active ||
            sample.epoch != camera_status_.epoch ||
            (have_ultrasonic_seq_ && !newer(sample.sequence, ultrasonic_.sequence)))
            return false;
        ultrasonic_ = sample;
        have_ultrasonic_ = have_ultrasonic_seq_ = true;
        ultrasonic_at_ = now;
        ++generation_;
        return true;
    }

    bool online() const { return online_; }
    std::uint64_t generation() const { return generation_; }
    CameraState camera_state(Time now) const {
        if (!online_ || status_at_ == Time{} || now - status_at_ > std::chrono::seconds(3))
            return CameraState::Unknown;
        return camera_status_.state;
    }
    const CameraFrame *fresh_frame(Time now) const {
        return camera_state(now) == CameraState::Active && camera_ &&
               frame_at_ != Time{} && now - frame_at_ <= std::chrono::milliseconds(1200)
                ? camera_.get() : nullptr;
    }
    const Ultrasonic *fresh_ultrasonic(Time now) const {
        return camera_state(now) == CameraState::Active && have_ultrasonic_ &&
               ultrasonic_at_ != Time{} && now - ultrasonic_at_ <= std::chrono::seconds(2)
                ? &ultrasonic_ : nullptr;
    }

private:
    static bool newer(std::uint32_t candidate, std::uint32_t previous) {
        return static_cast<std::int32_t>(candidate - previous) > 0;
    }
    bool online_ = false;
    CameraStatus camera_status_;
    Time status_at_{};
    std::shared_ptr<const CameraFrame> camera_;
    Time frame_at_{};
    std::uint32_t last_frame_sequence_ = 0;
    bool have_frame_seq_ = false;
    Ultrasonic ultrasonic_;
    Time ultrasonic_at_{};
    bool have_ultrasonic_ = false;
    bool have_ultrasonic_seq_ = false;
    std::uint64_t generation_ = 0;
};

// Reverse is sourced from engine-service, not inferred from a sensor frame.
inline bool show_reverse_camera(bool engine_reverse, bool monitor_visible,
                                bool video_visible) {
    return engine_reverse && (monitor_visible || video_visible);
}

} // namespace sensors
