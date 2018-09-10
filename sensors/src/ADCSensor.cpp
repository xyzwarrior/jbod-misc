/*
// Copyright (c) 2017 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include <unistd.h>

#include <ADCSensor.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <iostream>
#include <limits>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <string>

static constexpr unsigned int SENSOR_POLL_MS = 500;
static constexpr size_t WARN_AFTER_ERROR_COUNT = 10;

// scaling factor from hwmon
static constexpr unsigned int SENSOR_SCALE_FACTOR = 1000;

ADCSensor::ADCSensor(const std::string &path,
                     sdbusplus::asio::object_server &objectServer,
                     std::shared_ptr<sdbusplus::asio::connection> &conn,
                     boost::asio::io_service &io,
                     const std::string &sensor_name,
                     std::vector<thresholds::Threshold> &&_thresholds,
                     const double scale_factor,
                     const std::string &sensorConfiguration) :
    path(path),
    objServer(objectServer), configuration(sensorConfiguration),
    name(boost::replace_all_copy(sensor_name, " ", "_")),
    thresholds(std::move(_thresholds)), scale_factor(scale_factor),
    sensor_interface(objectServer.add_interface(
        "/xyz/openbmc_project/sensors/voltage/" + name,
        "xyz.openbmc_project.Sensor.Value")),
    input_dev(io, open(path.c_str(), O_RDONLY)), wait_timer(io),
    value(std::numeric_limits<double>::quiet_NaN()), err_count(0),
    // todo, get these from config
    max_value(20), min_value(0)
{
    if (thresholds::HasWarningInterface(thresholds))
    {
        threshold_interface_warning = objectServer.add_interface(
            "/xyz/openbmc_project/sensors/voltage/" + name,
            "xyz.openbmc_project.Sensor.Threshold.Warning");
    }
    if (thresholds::HasCriticalInterface(thresholds))
    {
        threshold_interface_critical = objectServer.add_interface(
            "/xyz/openbmc_project/sensors/voltage/" + name,
            "xyz.openbmc_project.Sensor.Threshold.Critical");
    }
    set_initial_properties(conn);
    setup_read();
}

ADCSensor::~ADCSensor()
{
    // close the input dev to cancel async operations
    input_dev.close();
    wait_timer.cancel();
    objServer.remove_interface(threshold_interface_warning);
    objServer.remove_interface(threshold_interface_critical);
    objServer.remove_interface(sensor_interface);
}

void ADCSensor::setup_read(void)
{
    boost::asio::async_read_until(
        input_dev, read_buf, '\n',
        [&](const boost::system::error_code &ec,
            std::size_t /*bytes_transfered*/) { handle_response(ec); });
}

void ADCSensor::handle_response(const boost::system::error_code &err)
{
    if (err == boost::system::errc::bad_file_descriptor)
    {
        return; // we're being destroyed
    }
    std::istream response_stream(&read_buf);

    if (!err)
    {
        std::string response;
        std::getline(response_stream, response);

        // todo read scaling factors from configuration
        try
        {
            float nvalue = std::stof(response);

            nvalue = (nvalue / SENSOR_SCALE_FACTOR) / scale_factor;

            if (nvalue != value)
            {
                update_value(nvalue);
            }
            err_count = 0;
        }
        catch (std::invalid_argument)
        {
            err_count++;
        }
    }
    else
    {
        std::cerr << "Failure to read sensor " << name << " at " << path
                  << " ec:" << err << "\n";

        err_count++;
    }

    // only send value update once
    if (err_count == WARN_AFTER_ERROR_COUNT)
    {
        update_value(0);
    }

    response_stream.clear();
    input_dev.close();
    int fd = open(path.c_str(), O_RDONLY);
    if (fd <= 0)
    {
        return; // we're no longer valid
    }
    input_dev.assign(fd);
    wait_timer.expires_from_now(
        boost::posix_time::milliseconds(SENSOR_POLL_MS));
    wait_timer.async_wait([&](const boost::system::error_code &ec) {
        if (ec == boost::asio::error::operation_aborted)
        {
            return; // we're being canceled
        }
        setup_read();
    });
}

void ADCSensor::check_thresholds(void)
{
    if (thresholds.empty())
        return;
    for (auto threshold : thresholds)
    {
        if (threshold.direction == thresholds::Direction::HIGH)
        {
            if (value > threshold.value)
            {
                assert_thresholds(threshold.level, threshold.direction, true);
            }
            else
            {
                assert_thresholds(threshold.level, threshold.direction, false);
            }
        }
        else
        {
            if (value < threshold.value)
            {
                assert_thresholds(threshold.level, threshold.direction, true);
            }
            else
            {
                assert_thresholds(threshold.level, threshold.direction, false);
            }
        }
    }
}

void ADCSensor::update_value(const double &new_value)
{
    bool ret = sensor_interface->set_property("Value", new_value);
    value = new_value;
    check_thresholds();
}

void ADCSensor::assert_thresholds(thresholds::Level level,
                                  thresholds::Direction direction, bool assert)
{
    std::string property;
    std::shared_ptr<sdbusplus::asio::dbus_interface> interface;
    if (level == thresholds::Level::WARNING &&
        direction == thresholds::Direction::HIGH)
    {
        property = "WarningAlarmHigh";
        interface = threshold_interface_warning;
    }
    else if (level == thresholds::Level::WARNING &&
             direction == thresholds::Direction::LOW)
    {
        property = "WarningAlarmLow";
        interface = threshold_interface_warning;
    }
    else if (level == thresholds::Level::CRITICAL &&
             direction == thresholds::Direction::HIGH)
    {
        property = "CriticalAlarmHigh";
        interface = threshold_interface_critical;
    }
    else if (level == thresholds::Level::CRITICAL &&
             direction == thresholds::Direction::LOW)
    {
        property = "CriticalAlarmLow";
        interface = threshold_interface_critical;
    }
    else
    {
        std::cerr << "Unknown threshold, level " << level << "direction "
                  << direction << "\n";
        return;
    }
    if (!interface)
    {
        std::cout << "trying to set uninitialized interface\n";
        return;
    }
    interface->set_property(property, assert);
}

void ADCSensor::set_initial_properties(
    std::shared_ptr<sdbusplus::asio::connection> &conn)
{
    // todo, get max and min from configuration
    sensor_interface->register_property("MaxValue", max_value);
    sensor_interface->register_property("MinValue", min_value);
    sensor_interface->register_property("Value", value);

    for (auto &threshold : thresholds)
    {
        std::shared_ptr<sdbusplus::asio::dbus_interface> iface;
        std::string level;
        std::string alarm;
        if (threshold.level == thresholds::Level::CRITICAL)
        {
            iface = threshold_interface_critical;
            if (threshold.direction == thresholds::Direction::HIGH)
            {
                level = "CriticalHigh";
                alarm = "CriticalAlarmHigh";
            }
            else
            {
                level = "CriticalLow";
                alarm = "CriticalAlarmLow";
            }
        }
        else if (threshold.level == thresholds::Level::WARNING)
        {
            iface = threshold_interface_warning;
            if (threshold.direction == thresholds::Direction::HIGH)
            {
                level = "WarningHigh";
                alarm = "WarningAlarmHigh";
            }
            else
            {
                level = "WarningLow";
                alarm = "WarningAlarmLow";
            }
        }
        else
        {
            std::cerr << "Unknown threshold level" << threshold.level << "\n";
            continue;
        }
        if (!iface)
        {
            std::cout << "trying to set uninitialized interface\n";
            continue;
        }
        iface->register_property(
            level, threshold.value,
            [&](const double &request, double &oldValue) {
                oldValue = request; // todo, just let the config do this?
                threshold.value = request;
                thresholds::persistThreshold(
                    configuration, "xyz.openbmc_project.Configuration.ADC",
                    threshold, conn);
                return 1;
            });
        iface->register_property(alarm, false);
    }
    if (!sensor_interface->initialize())
    {
        std::cerr << "error initializing value interface\n";
    }
    if (threshold_interface_warning &&
        !threshold_interface_warning->initialize())
    {
        std::cerr << "error initializing warning threshold interface\n";
    }

    if (threshold_interface_critical &&
        !threshold_interface_critical->initialize())
    {
        std::cerr << "error initializing critical threshold interface\n";
    }
}