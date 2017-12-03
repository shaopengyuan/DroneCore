#include "follow_me_impl.h"
#include "device.h"
#include "global_include.h"
#include "px4_custom_mode.h"
#include <functional>
#include <cmath>

namespace dronecore {

using namespace std::placeholders; // for `_1`

FollowMeImpl::FollowMeImpl() :
    PluginImplBase()
{
    // (Lat, Lon, Alt) => double, (vx, vy, vz) => float
    _last_location =  _curr_target_location = \
                                              FollowMe::TargetLocation { double(NAN), double(NAN), double(NAN),
                                                                         NAN, NAN, NAN };
}

FollowMeImpl::~FollowMeImpl() {}

void FollowMeImpl::init()
{
    _parent->register_mavlink_message_handler(
        MAVLINK_MSG_ID_HEARTBEAT,
        std::bind(&FollowMeImpl::process_heartbeat, this, _1), static_cast<void *>(this));
    set_default_config();
}

void FollowMeImpl::deinit()
{
    _parent->unregister_all_mavlink_message_handlers(this);
}

void FollowMeImpl::enable() {}

void FollowMeImpl::disable()
{
    stop_sending_target_location();
}

const FollowMe::Config &FollowMeImpl::get_config() const
{
    return _config;
}

bool FollowMeImpl::set_config(const FollowMe::Config &config)
{
    // Valdidate configuration
    if (!is_config_ok(config)) {
        LogErr() << "set_config() failed. Last configuration is preserved.";
        return false;
    }

    auto height = config.min_height_m;
    auto distance = config.follow_dist_m;
    int32_t direction = static_cast<int32_t>(config.follow_direction);
    auto responsiveness = config.responsiveness;

    // Send configuration to Vehicle
    _parent->set_param_float_async("NAV_MIN_FT_HT", height,
                                   std::bind(&FollowMeImpl::receive_param_min_height,
                                             this, _1, height));
    _parent->set_param_float_async("NAV_FT_DST", distance,
                                   std::bind(&FollowMeImpl::receive_param_follow_distance,
                                             this, _1, distance));
    _parent->set_param_int_async("NAV_FT_FS", direction,
                                 std::bind(&FollowMeImpl::receive_param_follow_direction,
                                           this, _1, direction));
    _parent->set_param_float_async("NAV_FT_RS", responsiveness,
                                   std::bind(&FollowMeImpl::receive_param_responsiveness,
                                             this, _1, responsiveness));

    // FIXME: We've sent valid configuration to Vehicle.
    // But that doesn't mean configuration is applied, untill we receive confirmation.
    // For now we're hoping that it is applied successfully.
    return true;
}

void FollowMeImpl::set_curr_target_location(const FollowMe::TargetLocation &location)
{
    _mutex.lock();
    _curr_target_location = location;
    // We're sending only lat, long & alt to the vehicle.
    _estimatation_capabilities |= (1 << static_cast<int>(EstimationCapabilites::POS));

    if (_mode != Mode::ACTIVE) {
        _mutex.unlock();
        return;
    }
    // If set already, reschedule it.
    if (_curr_target_location_cookie) {
        _parent->reset_call_every(_curr_target_location_cookie);
        _curr_target_location_cookie = nullptr;
    } else {
        // Regiter now for sending in the next cycle.
        _parent->add_call_every([this]() { send_curr_target_location(); },
        SENDER_RATE,
        &_curr_target_location_cookie);
    }
    _mutex.unlock();

    // Send it immediately for now.
    send_curr_target_location();
}

void FollowMeImpl::get_last_location(FollowMe::TargetLocation &last_location)
{
    std::lock_guard<std::mutex> lock(_mutex);
    last_location = _curr_target_location;
}

bool FollowMeImpl::is_active() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _mode == Mode::ACTIVE;
}

FollowMe::Result FollowMeImpl::start()
{
    // Note: the safety flag is not needed in future versions of the PX4 Firmware
    //       but want to be rather safe than sorry.
    uint8_t flag_safety_armed = _parent->is_armed() ? MAV_MODE_FLAG_SAFETY_ARMED : 0;

    uint8_t mode = MAV_MODE_FLAG_CUSTOM_MODE_ENABLED | flag_safety_armed;
    uint8_t custom_mode = px4::PX4_CUSTOM_MAIN_MODE_AUTO;
    uint8_t custom_sub_mode = px4::PX4_CUSTOM_SUB_MODE_AUTO_FOLLOW_TARGET;


    FollowMe::Result result = to_follow_me_result(
                                  _parent->send_command_with_ack(
                                      MAV_CMD_DO_SET_MODE,
                                      MavlinkCommands::Params {float(mode),
                                                               float(custom_mode),
                                                               float(custom_sub_mode),
                                                               NAN, NAN, NAN, NAN},
                                      MavlinkCommands::DEFAULT_COMPONENT_ID_AUTOPILOT));

    if (result == FollowMe::Result::SUCCESS) {
        // If location was set before, lets send it to vehicle
        std::lock_guard<std::mutex> lock(
            _mutex); // locking is not necessary here but lets do it for integrity
        if (is_current_location_set()) {
            _parent->add_call_every([this]() { send_curr_target_location(); },
            SENDER_RATE,
            &_curr_target_location_cookie);
        }
    }
    return result;
}

FollowMe::Result FollowMeImpl::stop()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_mode == Mode::ACTIVE) {
            stop_sending_target_location();
        }
    }
    // Note: the safety flag is not needed in future versions of the PX4 Firmware
    //       but want to be rather safe than sorry.
    uint8_t flag_safety_armed = _parent->is_armed() ? MAV_MODE_FLAG_SAFETY_ARMED : 0;

    uint8_t mode = MAV_MODE_FLAG_CUSTOM_MODE_ENABLED | flag_safety_armed;
    uint8_t custom_mode = px4::PX4_CUSTOM_MAIN_MODE_AUTO;
    uint8_t custom_sub_mode = px4::PX4_CUSTOM_SUB_MODE_AUTO_LOITER;

    return to_follow_me_result(
               _parent->send_command_with_ack(
                   MAV_CMD_DO_SET_MODE,
                   MavlinkCommands::Params {float(mode),
                                            float(custom_mode),
                                            float(custom_sub_mode),
                                            NAN, NAN, NAN, NAN},
                   MavlinkCommands::DEFAULT_COMPONENT_ID_AUTOPILOT));
}

// Applies default FollowMe configuration to the device
void FollowMeImpl::set_default_config()
{
    LogInfo() << "Applying default FollowMe configuration FollowMe to the device...";
    FollowMe::Config default_config {};

    auto height = default_config.min_height_m;
    auto distance = default_config.follow_dist_m;
    int32_t direction = static_cast<int32_t>(default_config.follow_direction);
    auto responsiveness = default_config.responsiveness;

    // Send configuration to Vehicle
    _parent->set_param_float_async("NAV_MIN_FT_HT", height,
                                   std::bind(&FollowMeImpl::receive_param_min_height,
                                             this, _1, height));
    _parent->set_param_float_async("NAV_FT_DST", distance,
                                   std::bind(&FollowMeImpl::receive_param_follow_distance,
                                             this, _1, distance));
    _parent->set_param_int_async("NAV_FT_FS", direction,
                                 std::bind(&FollowMeImpl::receive_param_follow_direction,
                                           this, _1, direction));
    _parent->set_param_float_async("NAV_FT_RS", responsiveness,
                                   std::bind(&FollowMeImpl::receive_param_responsiveness,
                                             this, _1, responsiveness));
}

bool FollowMeImpl::is_config_ok(const FollowMe::Config &config) const
{
    auto config_ok = false;

    if (config.min_height_m < FollowMe::Config::MIN_HEIGHT_M) {
        LogErr() << "Err: Min height must be atleast 8.0 meters";
    } else if (config.follow_dist_m < FollowMe::Config::MIN_FOLLOW_DIST_M) {
        LogErr() << "Err: Min Follow distance must be atleast 1.0 meter";
    } else if (config.follow_direction < FollowMe::Config::FollowDirection::FRONT_RIGHT ||
               config.follow_direction > FollowMe::Config::FollowDirection::NONE) {
        LogErr() << "Err: Invalid Follow direction";
    } else if (config.responsiveness < FollowMe::Config::MIN_RESPONSIVENESS ||
               config.responsiveness > FollowMe::Config::MAX_RESPONSIVENESS) {
        LogErr() << "Err: Responsiveness must be in range (0.0 to 1.0)";
    } else { // Config is OK
        config_ok = true;
    }

    return config_ok;
}

void FollowMeImpl::receive_param_min_height(bool success, float min_height_m)
{
    if (success) {
        _config.min_height_m = min_height_m;
    } else {
        LogErr() << "Failed to set NAV_MIN_FT_HT: " << min_height_m << "m";
    }
}

void FollowMeImpl::receive_param_follow_distance(bool success, float follow_dist_m)
{
    if (success) {
        _config.follow_dist_m = follow_dist_m;
    } else {
        LogErr() << "Failed to set NAV_FT_DST: " << follow_dist_m << "m";
    }
}

void FollowMeImpl::receive_param_follow_direction(bool success, int32_t direction)
{
    auto new_direction = FollowMe::Config::FollowDirection::NONE;
    switch (direction) {
        case 0: new_direction = FollowMe::Config::FollowDirection::FRONT_RIGHT; break;
        case 1: new_direction = FollowMe::Config::FollowDirection::BEHIND; break;
        case 2: new_direction = FollowMe::Config::FollowDirection::FRONT; break;
        case 3: new_direction = FollowMe::Config::FollowDirection::FRONT_LEFT; break;
        default: break;
    }
    auto curr_direction_s = FollowMe::Config::to_str(_config.follow_direction);
    auto new_direction_s = FollowMe::Config::to_str(new_direction);
    if (success) {
        if (new_direction != FollowMe::Config::FollowDirection::NONE) {
            _config.follow_direction = new_direction;
        }
    } else {
        LogErr() << "Failed to set NAV_FT_FS: " <<  FollowMe::Config::to_str(new_direction);
    }
}

void FollowMeImpl::receive_param_responsiveness(bool success, float responsiveness)
{
    if (success) {
        _config.responsiveness = responsiveness;
    } else {
        LogErr() << "Failed to set NAV_FT_RS: " << responsiveness;
    }
}

FollowMe::Result
FollowMeImpl::to_follow_me_result(MavlinkCommands::Result result) const
{
    switch (result) {
        case MavlinkCommands::Result::SUCCESS:
            return FollowMe::Result::SUCCESS;
        case MavlinkCommands::Result::NO_DEVICE:
            return FollowMe::Result::NO_DEVICE;
        case MavlinkCommands::Result::CONNECTION_ERROR:
            return FollowMe::Result::CONNECTION_ERROR;
        case MavlinkCommands::Result::BUSY:
            return FollowMe::Result::BUSY;
        case MavlinkCommands::Result::COMMAND_DENIED:
            return FollowMe::Result::COMMAND_DENIED;
        case MavlinkCommands::Result::TIMEOUT:
            return FollowMe::Result::TIMEOUT;
        default:
            return FollowMe::Result::UNKNOWN;
    }
}

bool FollowMeImpl::is_current_location_set() const
{
    // If the target's latitude is NAN, we assume that location is not set.
    // We assume that mutex was acquired by the caller
    return std::isfinite(_curr_target_location.latitude_deg);
}

void FollowMeImpl::send_curr_target_location()
{
    // Don't send if we're not in FollowMe mode.
    if (!is_active()) {
        return;
    }

    dl_time_t now = _time.steady_time();
    // needed by http://mavlink.org/messages/common#FOLLOW_TARGET
    uint64_t elapsed_msec = static_cast<uint64_t>(_time.elapsed_since_s(now) * 1000); // milliseconds

    _mutex.lock();
//    LogDebug() << "Lat: " << _curr_target_location.latitude_deg << " Lon: " << _curr_target_location.longitude_deg <<
//	" Alt: " << _curr_target_location.absolute_altitude_m;
    const int32_t lat_int = static_cast<int32_t>(_curr_target_location.latitude_deg * 1e7);
    const int32_t lon_int = static_cast<int32_t>(_curr_target_location.longitude_deg * 1e7);
    const float alt = static_cast<float>(_curr_target_location.absolute_altitude_m);
    _mutex.unlock();

    const float pos_std_dev[] = { NAN, NAN, NAN };
    const float vel[] = { NAN, NAN, NAN };
    const float accel_unknown[] = { NAN, NAN, NAN };
    const float attitude_q_unknown[] = { 1.f, NAN, NAN, NAN };
    const float rates_unknown[] = { NAN, NAN, NAN };
    uint64_t custom_state = 0;

    mavlink_message_t msg {};
    mavlink_msg_follow_target_pack(_parent->get_own_system_id(),
                                   _parent->get_own_component_id(),
                                   &msg,
                                   elapsed_msec,
                                   _estimatation_capabilities,
                                   lat_int,
                                   lon_int,
                                   alt,
                                   vel,
                                   accel_unknown,
                                   attitude_q_unknown,
                                   rates_unknown,
                                   pos_std_dev,
                                   custom_state);

    if (!_parent->send_message(msg)) {
        LogErr() << "send_curr_target_location() failed..";
    } else {
        std::lock_guard<std::mutex> lock(_mutex);
        _last_location = _curr_target_location;
    }
}

void FollowMeImpl::stop_sending_target_location()
{
    // We assume that mutex was acquired by the caller
    if (_curr_target_location_cookie) {
        _parent->remove_call_every(_curr_target_location_cookie);
        _curr_target_location_cookie = nullptr;
    }
    _mode = Mode::NOT_ACTIVE;
}

void FollowMeImpl::process_heartbeat(const mavlink_message_t &message)
{
    mavlink_heartbeat_t heartbeat;
    mavlink_msg_heartbeat_decode(&message, &heartbeat);

    bool follow_me_active = false; // tells whether we're in FollowMe mode right now
    if (heartbeat.base_mode & MAV_MODE_FLAG_CUSTOM_MODE_ENABLED) {

        px4::px4_custom_mode px4_custom_mode;
        px4_custom_mode.data = heartbeat.custom_mode;

        if (px4_custom_mode.main_mode == px4::PX4_CUSTOM_MAIN_MODE_AUTO &&
            px4_custom_mode.sub_mode == px4::PX4_CUSTOM_SUB_MODE_AUTO_FOLLOW_TARGET) {
            follow_me_active = true; // we're in FollowMe mode
        }
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!follow_me_active && _mode == Mode::ACTIVE) {
            // We're NOT in FollowMe mode anymore.
            // Lets stop sending target location updates
            stop_sending_target_location();
        } else if (follow_me_active && _mode == Mode::NOT_ACTIVE) {
            // We're in FollowMe mode now
            _mode = Mode::ACTIVE;
            _mutex.unlock(); // we must unlock to avoid deadlock in send_curr_target_location()
            return;
        }
    }
}

} // namespace dronecore
