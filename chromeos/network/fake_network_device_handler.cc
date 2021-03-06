// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos/network/fake_network_device_handler.h"

namespace chromeos {

FakeNetworkDeviceHandler::FakeNetworkDeviceHandler() {}

FakeNetworkDeviceHandler::~FakeNetworkDeviceHandler() {}

void FakeNetworkDeviceHandler::GetDeviceProperties(
    const std::string& device_path,
    const network_handler::DictionaryResultCallback& callback,
    const network_handler::ErrorCallback& error_callback) const {}

void FakeNetworkDeviceHandler::SetDeviceProperty(
    const std::string& device_path,
    const std::string& property_name,
    const base::Value& value,
    const base::Closure& callback,
    const network_handler::ErrorCallback& error_callback) {}

void FakeNetworkDeviceHandler::RequestRefreshIPConfigs(
    const std::string& device_path,
    const base::Closure& callback,
    const network_handler::ErrorCallback& error_callback) {}

void FakeNetworkDeviceHandler::ProposeScan(
    const std::string& device_path,
    const base::Closure& callback,
    const network_handler::ErrorCallback& error_callback) {}

void FakeNetworkDeviceHandler::RegisterCellularNetwork(
    const std::string& device_path,
    const std::string& network_id,
    const base::Closure& callback,
    const network_handler::ErrorCallback& error_callback) {}

void FakeNetworkDeviceHandler::SetCarrier(
    const std::string& device_path,
    const std::string& carrier,
    const base::Closure& callback,
    const network_handler::ErrorCallback& error_callback) {}

void FakeNetworkDeviceHandler::RequirePin(
    const std::string& device_path,
    bool require_pin,
    const std::string& pin,
    const base::Closure& callback,
    const network_handler::ErrorCallback& error_callback) {}

void FakeNetworkDeviceHandler::EnterPin(
    const std::string& device_path,
    const std::string& pin,
    const base::Closure& callback,
    const network_handler::ErrorCallback& error_callback) {}

void FakeNetworkDeviceHandler::UnblockPin(
    const std::string& device_path,
    const std::string& puk,
    const std::string& new_pin,
    const base::Closure& callback,
    const network_handler::ErrorCallback& error_callback) {}

void FakeNetworkDeviceHandler::ChangePin(
    const std::string& device_path,
    const std::string& old_pin,
    const std::string& new_pin,
    const base::Closure& callback,
    const network_handler::ErrorCallback& error_callback) {}

void FakeNetworkDeviceHandler::SetCellularAllowRoaming(bool allow_roaming) {}

}  // namespace chromeos
