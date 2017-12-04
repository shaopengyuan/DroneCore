#include <iostream>
#include "integration_test_helper.h"
#include "global_include.h"
#include "dronecore.h"
#include "plugins/action/action.h"
#include "plugins/telemetry/telemetry.h"

using namespace dronecore;
using namespace std::placeholders; // for `_1`

void receive_result(Action::Result result);
void receive_health_all_ok(bool all_ok);
void receive_in_air(bool in_air);

static bool _all_ok = false;
static bool _in_air = false;

TEST_F(SitlTest, ActionAsyncHover)
{
    DroneCore dc;

    DroneCore::ConnectionResult ret = dc.add_udp_connection();
    ASSERT_EQ(ret, DroneCore::ConnectionResult::SUCCESS);

    // Wait for device to connect via heartbeat.
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // TODO: this test is pretty dumb, should be improved with more checks.
    Device &device = dc.device();

    auto telemetry = std::make_shared<Telemetry>(&device);
    telemetry->health_all_ok_async(std::bind(&receive_health_all_ok, _1));
    telemetry->in_air_async(std::bind(&receive_in_air, _1));

    auto action = std::make_shared<Action>(&device);

    while (!_all_ok) {
        std::cout << "Waiting to be ready..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    action->arm_async(std::bind(&receive_result, _1));
    std::this_thread::sleep_for(std::chrono::seconds(2));

    action->set_takeoff_altitude(5.0f);

    action->takeoff_async(std::bind(&receive_result, _1));
    std::this_thread::sleep_for(std::chrono::seconds(5));

    action->land_async(std::bind(&receive_result, _1));

    while (_in_air) {
        std::cout << "Waiting to be landed..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    action->disarm_async(std::bind(&receive_result, _1));
    std::this_thread::sleep_for(std::chrono::seconds(2));
}


void receive_result(Action::Result result)
{
    LogDebug() << "got result: " << unsigned(result);
    EXPECT_EQ(result, Action::Result::SUCCESS);
}

void receive_health_all_ok(bool all_ok)
{
    if (all_ok && !_all_ok) {
        LogDebug() << "we're ready, let's go";
        _all_ok = true;
    }
}

void receive_in_air(bool in_air)
{
    _in_air = in_air;
}
