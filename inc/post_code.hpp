/*
// Copyright (c) 2019 Intel Corporation
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
#pragma once
#include <config.h>
#include <fcntl.h>
#include <unistd.h>

#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/elog.hpp>
#include <sdbusplus/timer.hpp>
#include <xyz/openbmc_project/Collection/DeleteAll/server.hpp>
#include <xyz/openbmc_project/Common/error.hpp>
#include <xyz/openbmc_project/State/Boot/PostCode/server.hpp>
#include <xyz/openbmc_project/State/Host/server.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

const static constexpr char* CurrentBootCycleCountName =
    "CurrentBootCycleCount";
const static constexpr char* CurrentBootCycleIndexName =
    "CurrentBootCycleIndex";

const static constexpr char* PostCodePath =
    "/xyz/openbmc_project/state/boot/raw";
const static constexpr char* PostCodeListPathPrefix =
    "/var/log/phosphor-post-code-manager/host";
const static constexpr char* HostStatePathPrefix =
    "/xyz/openbmc_project/state/host";

struct EventDeleter
{
    void operator()(sd_event* event) const
    {
        sd_event_unref(event);
    }
};
using EventPtr = std::unique_ptr<sd_event, EventDeleter>;
using primarycode_t = std::vector<uint8_t>;
using secondarycode_t = std::vector<uint8_t>;
using postcode_t = std::tuple<primarycode_t, secondarycode_t>;
using ::phosphor::logging::elog;
using ::sdbusplus::xyz::openbmc_project::Common::Error::InvalidArgument;
using Argument =
    ::phosphor::logging::xyz::openbmc_project::Common::InvalidArgument;

namespace fs = std::filesystem;
namespace StateServer = sdbusplus::xyz::openbmc_project::State::server;

using post_code =
    sdbusplus::xyz::openbmc_project::State::Boot::server::PostCode;
using delete_all =
    sdbusplus::xyz::openbmc_project::Collection::server::DeleteAll;

struct PostCode : sdbusplus::server::object_t<post_code, delete_all>
{
    PostCode(sdbusplus::bus_t& bus, const char* path, EventPtr& event,
             int nodeIndex) :
        sdbusplus::server::object_t<post_code, delete_all>(bus, path), bus(bus),
        event(event), node(nodeIndex),
        postCodeListPath(PostCodeListPathPrefix + std::to_string(node)),
        propertiesChangedSignalRaw(
            bus,
            sdbusplus::bus::match::rules::propertiesChanged(
                PostCodePath + std::to_string(node),
                "xyz.openbmc_project.State.Boot.Raw"),
            [this](sdbusplus::message_t& msg) {
        std::string intfName;
        std::map<std::string, std::variant<postcode_t>> msgData;
        msg.read(intfName, msgData);
        // Check if it was the Value property that changed.
        auto valPropMap = msgData.find("Value");
        if (valPropMap != msgData.end())
        {
            if (this->shutdownRequested)
            {
                phosphor::logging::log<phosphor::logging::level::INFO>(
                    "Ignoring post code, shutdown requested");
                return;
            }
            this->savePostCodes(std::get<postcode_t>(valPropMap->second));
        }
            }),
        propertiesChangedSignalCurrentHostState(
            bus,
            sdbusplus::bus::match::rules::propertiesChanged(
                HostStatePathPrefix + std::to_string(node),
                "xyz.openbmc_project.State.Host"),
            [this](sdbusplus::message_t& msg) {
        std::string intfName;
        std::map<std::string, std::variant<std::string>> msgData;
        msg.read(intfName, msgData);
        auto valPropMap = msgData.find("CurrentHostState");
        if (valPropMap != msgData.end())
        {
            if (!std::holds_alternative<std::string>(valPropMap->second))
            {
                return;
            }

            StateServer::Host::HostState currentHostState =
                StateServer::Host::convertHostStateFromString(
                    std::get<std::string>(valPropMap->second));
            if (currentHostState == StateServer::Host::HostState::Off)
            {
                this->hostOff = true;
                this->shutdownRequested = true;
                if (this->timer && this->timer->isRunning())
                {
                    this->timer->stop();
                }
                if (!this->postCodes.empty())
                {
                    if (this->bootInProgress)
                    {
                        // Valid boot codes - persist them
                        this->serialize(this->postCodeListPath);
                    }
                    else
                    {
                        // Discard shutdown-time codes and remove
                        // any file the timer may have written
                        fs::path codeFile =
                            this->postCodeListPath /
                            std::to_string(this->currentBootCycleIndex);
                        this->postCodes.clear();
                        if (fs::exists(codeFile))
                        {
                            fs::remove(codeFile);
                            if (this->currentBootCycleIndex > 0)
                            {
                                this->currentBootCycleIndex--;
                            }
                            uint16_t count = this->currentBootCycleCount();
                            if (count > 0)
                            {
                                this->currentBootCycleCount(count - 1);
                            }
                        }
                    }
                    this->bootInProgress = false;
                }
                else if (currentHostState ==
                         StateServer::Host::HostState::Running)
                {
                    this->hostOff = false;
                    this->shutdownRequested = false;
                    this->bootInProgress = true;
                }
            }

            // Check if RequestedHostTransition changed.
            auto requestedTransitionProp =
                msgData.find("RequestedHostTransition");
            if (requestedTransitionProp != msgData.end())
            {
                if (!std::holds_alternative<std::string>(
                        requestedTransitionProp->second))
                {
                    return;
                }
                auto requestedTransition =
                    StateServer::Host::convertTransitionFromString(
                        std::get<std::string>(requestedTransitionProp->second));
                if (requestedTransition ==
                    StateServer::Host::Transition::ForceWarmReboot)
                {
                    this->postCodes.clear();
                    this->shutdownRequested = false;
                }
                else if (requestedTransition ==
                         StateServer::Host::Transition::Off)
                {
                    this->shutdownRequested = true;
                }
                else
                {
                    this->shutdownRequested = false;
                }
            }
        })
        {
            phosphor::logging::log<phosphor::logging::level::INFO>(
                "PostCode is created");
            // current host state to sync hostOff on daemon restart
            try
            {
                auto method = bus.new_method_call(
                    "xyz.openbmc_project.State.Host",
                    (HostStatePathPrefix + std::to_string(node)).c_str(),
                    "org.freedesktop.DBus.Properties", "Get");
                method.append("xyz.openbmc_project.State.Host",
                              "CurrentHostState");
                auto reply = bus.call(method);
                std::variant<std::string> currentState;
                reply.read(currentState);
                auto stateStr = std::get<std::string>(currentState);
                auto state =
                    StateServer::Host::convertHostStateFromString(stateStr);
                if (state == StateServer::Host::HostState::Running)
                {
                    hostOff = false;
                    bootInProgress = true;
                }
            }
            catch (const std::exception& e)
            {
                phosphor::logging::log<phosphor::logging::level::WARNING>(
                    "Failed to query initial host state, assuming host off");
            }
            fs::create_directories(postCodeListPath);
            deserialize(postCodeListPath / CurrentBootCycleIndexName,
                        currentBootCycleIndex);
            uint16_t count = 0;
            deserialize(postCodeListPath / CurrentBootCycleCountName, count);
            currentBootCycleCount(count);
            maxBootCycleNum(MAX_BOOT_CYCLE_COUNT);
        }
        ~PostCode() {}

        std::vector<postcode_t> getPostCodes(uint16_t index) override;
        std::map<uint64_t, postcode_t> getPostCodesWithTimeStamp(uint16_t index)
            override;
        void deleteAll() override;

      private:
        void incrBootCycle();
        uint16_t getBootNum(const uint16_t index) const;

        std::unique_ptr<sdbusplus::Timer> timer;
        sdbusplus::bus_t& bus;
        EventPtr& event;
        int node;
        std::chrono::time_point<std::chrono::steady_clock>
            firstPostCodeTimeSteady;
        uint64_t firstPostCodeUsSinceEpoch;
        std::map<uint64_t, postcode_t> postCodes;
        fs::path postCodeListPath;
        uint16_t currentBootCycleIndex = 0;
        bool hostOff = true;
        bool shutdownRequested = false;
        bool bootInProgress = false;
        sdbusplus::bus::match_t propertiesChangedSignalRaw;
        sdbusplus::bus::match_t propertiesChangedSignalCurrentHostState;

        void savePostCodes(postcode_t code);
        fs::path serialize(const fs::path& path);
        bool deserialize(const fs::path& path, uint16_t& index);
        bool deserializePostCodes(const fs::path& path,
                                  std::map<uint64_t, postcode_t>& codes);
};
