#pragma once
#include <cstdio>
#define ESP_LOGE(tag,...) do {(void)(tag);if(false)std::printf(__VA_ARGS__);}while(0)
