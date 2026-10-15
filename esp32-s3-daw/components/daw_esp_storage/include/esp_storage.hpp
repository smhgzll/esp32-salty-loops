// SPDX-License-Identifier: GPL-3.0-or-later
// Flash partitions as daw::FlashDevice, and persistent device settings (NVS).
#pragma once

#include <string>

#include "daw/store.hpp"
#include "esp_partition.h"

namespace daw {

class PartitionFlash : public FlashDevice {
public:
    // data partition by label ("projects", "stage", ...); ok() false when it does not exist
    explicit PartitionFlash(const char *label);
    bool ok() const { return part_ != nullptr; }
    uint32_t size() const override;
    bool read(uint32_t off, void *dst, uint32_t len) override;
    bool write(uint32_t off, const void *src, uint32_t len) override;
    bool erase(uint32_t off, uint32_t len) override;
    const esp_partition_t *partition() const { return part_; }

private:
    const esp_partition_t *part_;
};

// device settings changed from the browser (override the build's Kconfig values)
struct DeviceSettings {
    std::string wifiMode, ssid, password, hostname, adminPassword;
    bool fromNvs = false;
};
DeviceSettings settingsLoad();
bool settingsSave(const DeviceSettings &s);
bool settingsErase();
uint32_t bootCounter();
// one-shot flags in NVS (test hooks): set, and read-and-clear
bool settingsSetFlag(const char *key);
bool settingsTakeFlag(const char *key);  // incremented at every boot (tells clients a restart happened)

}  // namespace daw
