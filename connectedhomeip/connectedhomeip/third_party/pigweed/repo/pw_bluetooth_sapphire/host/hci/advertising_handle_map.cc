// Copyright 2023 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

#include "pw_bluetooth_sapphire/internal/host/hci/advertising_handle_map.h"

#include <pw_assert/check.h>

namespace bt::hci {

std::optional<hci_spec::AdvertisingHandle> AdvertisingHandleMap::MapHandle(
    const DeviceAddress& address, bool extended_pdu) {
  auto handle = map_.get({address, extended_pdu});
  if (handle) {
    return handle;
  }

  if (Size() >= capacity_) {
    return std::nullopt;
  }

  auto next_handle = NextHandle();
  PW_CHECK(next_handle);

  map_.insert(next_handle.value(), {address, extended_pdu});
  return next_handle;
}

std::optional<DeviceAddress> AdvertisingHandleMap::GetAddress(
    hci_spec::AdvertisingHandle handle) const {
  if (map_.contains(handle)) {
    const auto& [address, extended] = map_.get(handle).value().get();
    return address;
  }

  return std::nullopt;
}

std::optional<hci_spec::AdvertisingHandle>
AdvertisingHandleMap::LastUsedHandleForTesting() const {
  if (last_handle_ > hci_spec::kMaxAdvertisingHandle) {
    return std::nullopt;
  }

  return last_handle_;
}

std::optional<hci_spec::AdvertisingHandle> AdvertisingHandleMap::NextHandle() {
  if (Size() >= capacity_) {
    return std::nullopt;
  }

  hci_spec::AdvertisingHandle handle = last_handle_;
  do {
    handle = static_cast<uint8_t>(handle + 1) % capacity_;
  } while (map_.contains(handle));

  last_handle_ = handle;
  return handle;
}

}  // namespace bt::hci
