#pragma once
#include <cassert>
#include <stdint.h>
using TickType_t=uint32_t;
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY 0xffffffff
#define configASSERT(x) assert(x)
