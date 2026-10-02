# Copyright 2026 AltSql.com
# SPDX-License-Identifier: Apache-2.0
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "../../../dist"
                       REQUIRES esp_partition esp_timer driver)
