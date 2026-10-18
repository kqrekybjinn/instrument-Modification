#pragma once
#include <stddef.h>
#include "esp_err.h"
bool msc_is_mounted(void);
esp_err_t msc_write_file(const char*,const void*,size_t,bool);
esp_err_t msc_read_file(const char*,void*,size_t,size_t*);
