#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

void msc_driver_task(void *arg);

bool msc_is_mounted(void);
const char *msc_mount_path(void);
esp_err_t msc_write_file(const char *rel_path, const void *data, size_t len, bool append);
esp_err_t msc_read_file(const char *rel_path, void *out, size_t out_size, size_t *out_len);
