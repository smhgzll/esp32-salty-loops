// SPDX-License-Identifier: GPL-3.0-or-later
#include <string.h>

#include "esp_storage.hpp"

namespace daw {

PartitionFlash::PartitionFlash(const char *label)
    : part_(esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label))
{
}

uint32_t PartitionFlash::size() const { return part_ ? (uint32_t)part_->size : 0; }

bool PartitionFlash::read(uint32_t off, void *dst, uint32_t len)
{
    return part_ && esp_partition_read(part_, off, dst, len) == ESP_OK;
}

bool PartitionFlash::write(uint32_t off, const void *src, uint32_t len)
{
    if (!part_)
        return false;
    // the flash driver bounces non-internal sources in small pieces: copy PSRAM data through an
    // internal 4 KiB buffer (much faster; KNOWN_ISSUES KI-6)
    static uint8_t bounce[4096];
    const uint8_t *s = static_cast<const uint8_t *>(src);
    while (len) {
        const uint32_t n = len < sizeof bounce ? len : (uint32_t)sizeof bounce;
        memcpy(bounce, s, n);
        if (esp_partition_write(part_, off, bounce, n) != ESP_OK)
            return false;
        off += n;
        s += n;
        len -= n;
    }
    return true;
}

bool PartitionFlash::erase(uint32_t off, uint32_t len) { return part_ && esp_partition_erase_range(part_, off, len) == ESP_OK; }

}  // namespace daw
