# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "partition_flash.cpp" "settings.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES daw_core esp_partition nvs_flash spi_flash)
