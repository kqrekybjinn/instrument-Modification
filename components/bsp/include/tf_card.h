#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t tf_card_mount(void);
esp_err_t tf_card_unmount(void);

bool tf_card_is_mounted(void);
const char *tf_card_mount_path(void);

esp_err_t tf_card_write_file(const char *rel_path, const void *data, size_t len, bool append);
esp_err_t tf_card_read_file(const char *rel_path, void *out, size_t out_size, size_t *out_len);

#ifdef __cplusplus
}
#endif
