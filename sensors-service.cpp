#include "sensors-activation.hpp"
#include "sensors-simulation.hpp"
#include "services.hpp"

#include <vsomeip/vsomeip.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace {

int positive_env(const char *name, int fallback, int minimum, int maximum) {
    const char *raw = std::getenv(name);
    if (!raw || !*raw)
        return fallback;
    char *end = nullptr;
    errno = 0;
    const long parsed = std::strtol(raw, &end, 10);
    if (errno || end == raw || *end || parsed < minimum || parsed > maximum) {
        std::cerr << "SENSORS: Invalid " << name << " (expected " << minimum
                  << ".." << maximum << "); using " << fallback << '\n';
        return fallback;
    }
    return static_cast<int>(parsed);
}

const char *status_name(sensors::CameraState state) {
    switch (state) {
    case sensors::CameraState::Active: return "ACTIVE";
    case sensors::CameraState::Inactive: return "INACTIVE";
    case sensors::CameraState::Fault: return "UNAVAILABLE (no engine direction)";
    case sensors::CameraState::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

class SensorsService {
public:
    SensorsService()
        : app_(vsomeip::runtime::get()->create_application("Sensors")) {
        config_.width = positive_env("SENSORS_CAMERA_WIDTH", 100,
                                     sensors::kMinCameraWidth, sensors::kMaxCameraWidth);
        config_.height = positive_env("SENSORS_CAMERA_HEIGHT", 30,
                                      sensors::kMinCameraHeight, sensors::kMaxCameraHeight);
        config_.fps = static_cast<unsigned>(positive_env("SENSORS_CAMERA_FPS", 10, 1, 30));
    }

    bool init() {
        if (!app_ || !app_->init())
            return false;
        app_->register_state_handler([this](vsomeip::state_type_e state) {
            std::lock_guard<std::mutex> lock(mutex_);
            registered_ = state == vsomeip::state_type_e::ST_REGISTERED;
            changed_ = true;
            cv_.notify_one();
        });
        app_->register_availability_handler(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID,
                [this](vsomeip::service_t, vsomeip::instance_t, bool available) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    activation_.engine_availability(available);
                    changed_ = true;
                    cv_.notify_one();
                });
        app_->register_message_handler(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID,
                ENGINE_REVERSE_EVENT_ID,
                [this](const std::shared_ptr<vsomeip::message> &message) {
                    const auto payload = message->get_payload();
                    if (!payload || payload->get_length() != 4)
                        return;
                    const bool reversed = payload_get_arg(payload) != 0;
                    std::lock_guard<std::mutex> lock(mutex_);
                    activation_.engine_reverse(reversed);
                    changed_ = true;
                    cv_.notify_one();
                });
        app_->request_event(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID,
                ENGINE_REVERSE_EVENT_ID,
                std::set<vsomeip::eventgroup_t>{ENGINE_EVENTGROUP_ID},
                vsomeip::event_type_e::ET_FIELD);
        app_->subscribe(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID, ENGINE_EVENTGROUP_ID);
        app_->request_service(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID);

        // TCP / RT_RELIABLE for the ~6 KiB 100x30 camera frames, rather than
        // relying on IP fragmentation of large UDP events. Ultrasonic/status
        // use independent groups so consumers can subscribe separately.
        offer_sensor_event(SENSORS_CAMERA_FRAME_EVENT_ID, SENSORS_CAMERA_EVENTGROUP_ID);
        offer_sensor_event(SENSORS_ULTRASONIC_EVENT_ID, SENSORS_ULTRASONIC_EVENTGROUP_ID);
        offer_sensor_event(SENSORS_CAMERA_STATUS_EVENT_ID, SENSORS_CAMERA_STATUS_EVENTGROUP_ID);
        return true;
    }

    void start() {
        worker_ = std::thread([this] { publish_loop(); });
        app_->start();
        running_.store(false);
        cv_.notify_all();
        if (worker_.joinable())
            worker_.join();
    }

    void stop() {
        if (!running_.exchange(false))
            return;
        cv_.notify_all();
        app_->stop();
    }

private:
    void offer_sensor_event(vsomeip::event_t event, vsomeip::eventgroup_t group) {
        app_->offer_event(SENSORS_SERVICE_ID, SENSORS_INSTANCE_ID, event,
                std::set<vsomeip::eventgroup_t>{group},
                vsomeip::event_type_e::ET_FIELD, std::chrono::milliseconds::zero(),
                false, true, nullptr, vsomeip::reliability_type_e::RT_RELIABLE);
    }

    void notify(vsomeip::event_t event, const sensors::Bytes &bytes) {
        if (bytes.empty() || !running_.load())
            return;
        auto payload = vsomeip::runtime::get()->create_payload();
        payload->set_data(bytes);
        app_->notify(SENSORS_SERVICE_ID, SENSORS_INSTANCE_ID, event, payload);
    }

    void publish_loop() {
        using Clock = std::chrono::steady_clock;
        using namespace std::chrono;
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return registered_ || !running_.load(); });
        if (!running_.load())
            return;
        lock.unlock();
        app_->offer_service(SENSORS_SERVICE_ID, SENSORS_INSTANCE_ID);
        std::cout << "SENSORS: Simulated camera " << config_.width << 'x' << config_.height
                  << " @ " << config_.fps << " fps; 4 rear ultrasonic sensors\n";
        lock.lock();

        std::uint32_t last_epoch = ~std::uint32_t{0};
        std::uint32_t status_sequence = 0;
        std::uint32_t frame_sequence = 0;
        std::uint32_t ultrasonic_sequence = 0;
        std::uint32_t step = 0;
        Clock::time_point next_status{};
        Clock::time_point next_frame{};
        Clock::time_point next_ultrasonic{};
        const auto frame_period = nanoseconds(1000000000LL / config_.fps);

        while (running_.load()) {
            const auto now = Clock::now();
            const auto epoch = activation_.epoch();
            const auto state = activation_.state();
            if (last_epoch != epoch) {
                last_epoch = epoch;
                next_status = next_frame = next_ultrasonic = now;
                frame_sequence = ultrasonic_sequence = step = 0;
                std::cout << "SENSORS: Camera " << status_name(state)
                          << " (epoch " << epoch << ")\n";
            }
            sensors::Bytes status_bytes, frame_bytes, ultrasonic_bytes;
            if (now >= next_status) {
                status_bytes = sensors::encode(sensors::CameraStatus{
                        state, epoch, ++status_sequence});
                next_status = now + seconds(1); // explicit field heartbeat
            }
            if (state == sensors::CameraState::Active) {
                if (now >= next_frame) {
                    frame_bytes = sensors::encode(sensors::simulate_camera(
                            config_, step++, epoch, ++frame_sequence));
                    next_frame = now + frame_period;
                }
                if (now >= next_ultrasonic) {
                    ultrasonic_bytes = sensors::encode(sensors::simulate_ultrasonic(
                            step ? step - 1 : 0, epoch, ++ultrasonic_sequence));
                    next_ultrasonic = now + milliseconds(200); // 5 Hz
                }
            }
            auto deadline = next_status;
            if (state == sensors::CameraState::Active)
                deadline = std::min(deadline, std::min(next_frame, next_ultrasonic));
            changed_ = false;
            lock.unlock();
            // Publish the state first so clients know the epoch before frames.
            notify(SENSORS_CAMERA_STATUS_EVENT_ID, status_bytes);
            notify(SENSORS_CAMERA_FRAME_EVENT_ID, frame_bytes);
            notify(SENSORS_ULTRASONIC_EVENT_ID, ultrasonic_bytes);
            lock.lock();
            cv_.wait_until(lock, deadline,
                    [this] { return changed_ || !running_.load(); });
        }
        lock.unlock();
        app_->stop_offer_service(SENSORS_SERVICE_ID, SENSORS_INSTANCE_ID);
        app_->unsubscribe(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID, ENGINE_EVENTGROUP_ID);
        app_->release_event(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID, ENGINE_REVERSE_EVENT_ID);
        app_->release_service(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID);
    }

    std::shared_ptr<vsomeip::application> app_;
    sensors::SimulationConfig config_;
    sensors::Activation activation_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{true};
    std::thread worker_;
    bool registered_ = false;
    bool changed_ = false;
};

std::atomic<bool> stop_requested{false};
void handle_signal(int) {
    stop_requested.store(true);
}

} // namespace

int main() {
    SensorsService service;
    if (!service.init()) {
        std::cerr << "SENSORS: Failed to initialize vsomeip\n";
        return EXIT_FAILURE;
    }
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    std::thread signal_watcher([&] {
        while (!stop_requested.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        service.stop();
    });
    service.start();
    stop_requested.store(true);
    signal_watcher.join();
    return EXIT_SUCCESS;
}
