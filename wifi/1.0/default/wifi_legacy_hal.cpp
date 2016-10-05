/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <array>

#include "failure_reason_util.h"
#include "wifi_legacy_hal.h"

#include <android-base/logging.h>
#include <cutils/properties.h>

namespace {
std::string getWlanInterfaceName() {
  char buffer[PROPERTY_VALUE_MAX];
  property_get("wifi.interface", buffer, "wlan0");
  return buffer;
}

// Legacy HAL functions accept "C" style function pointers, so use global
// functions to pass to the legacy HAL function and store the corresponding
// std::function methods to be invoked.
// Callback to be invoked once |stop| is complete.
std::function<void(wifi_handle handle)> on_stop_complete_internal_callback;
void onStopComplete(wifi_handle handle) {
  if (on_stop_complete_internal_callback) {
    on_stop_complete_internal_callback(handle);
  }
}
}

namespace android {
namespace hardware {
namespace wifi {
namespace V1_0 {
namespace implementation {

WifiLegacyHal::WifiLegacyHal()
    : global_handle_(nullptr),
      wlan_interface_handle_(nullptr),
      awaiting_event_loop_termination_(false) {
  CHECK_EQ(init_wifi_vendor_hal_func_table(&global_func_table_), WIFI_SUCCESS)
      << "Failed to initialize legacy hal function table";
}

wifi_error WifiLegacyHal::start() {
  // Ensure that we're starting in a good state.
  CHECK(!global_handle_ && !wlan_interface_handle_ &&
        !awaiting_event_loop_termination_);

  LOG(INFO) << "Starting legacy HAL";
  wifi_error status = global_func_table_.wifi_initialize(&global_handle_);
  if (status != WIFI_SUCCESS || !global_handle_) {
    LOG(ERROR) << "Failed to retrieve global handle";
    return status;
  }
  event_loop_thread_ = std::thread(&WifiLegacyHal::runEventLoop, this);
  status = retrieveWlanInterfaceHandle();
  if (status != WIFI_SUCCESS || !wlan_interface_handle_) {
    LOG(ERROR) << "Failed to retrieve wlan interface handle";
    return status;
  }
  LOG(VERBOSE) << "Legacy HAL start complete";
  return WIFI_SUCCESS;
}

wifi_error WifiLegacyHal::stop(
    const std::function<void()>& on_stop_complete_user_callback) {
  LOG(INFO) << "Stopping legacy HAL";
  on_stop_complete_internal_callback = [&](wifi_handle handle) {
    CHECK_EQ(global_handle_, handle) << "Handle mismatch";
    on_stop_complete_user_callback();
    global_handle_ = nullptr;
    wlan_interface_handle_ = nullptr;
    on_stop_complete_internal_callback = nullptr;
  };
  awaiting_event_loop_termination_ = true;
  global_func_table_.wifi_cleanup(global_handle_, onStopComplete);
  LOG(VERBOSE) << "Legacy HAL stop initiated";
  return WIFI_SUCCESS;
}

wifi_error WifiLegacyHal::retrieveWlanInterfaceHandle() {
  const std::string& ifname_to_find = getWlanInterfaceName();

  wifi_interface_handle* iface_handles = nullptr;
  int num_iface_handles = 0;
  wifi_error status = global_func_table_.wifi_get_ifaces(
      global_handle_, &num_iface_handles, &iface_handles);
  if (status != WIFI_SUCCESS) {
    LOG(ERROR) << "Failed to enumerate interface handles: "
               << LegacyErrorToString(status);
    return status;
  }
  for (int i = 0; i < num_iface_handles; ++i) {
    std::array<char, IFNAMSIZ> current_ifname;
    current_ifname.fill(0);
    status = global_func_table_.wifi_get_iface_name(
        iface_handles[i], current_ifname.data(), current_ifname.size());
    if (status != WIFI_SUCCESS) {
      LOG(WARNING) << "Failed to get interface handle name: "
                   << LegacyErrorToString(status);
      continue;
    }
    if (ifname_to_find == current_ifname.data()) {
      wlan_interface_handle_ = iface_handles[i];
      return WIFI_SUCCESS;
    }
  }
  return WIFI_ERROR_UNKNOWN;
}

void WifiLegacyHal::runEventLoop() {
  LOG(VERBOSE) << "Starting legacy HAL event loop";
  global_func_table_.wifi_event_loop(global_handle_);
  if (!awaiting_event_loop_termination_) {
    LOG(FATAL) << "Legacy HAL event loop terminated, but HAL was not stopping";
  }
  LOG(VERBOSE) << "Legacy HAL event loop terminated";
  awaiting_event_loop_termination_ = false;
}

}  // namespace implementation
}  // namespace V1_0
}  // namespace wifi
}  // namespace hardware
}  // namespace android
