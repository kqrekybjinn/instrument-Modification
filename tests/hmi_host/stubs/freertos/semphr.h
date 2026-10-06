#pragma once
typedef void* SemaphoreHandle_t;
static inline int xSemaphoreTakeRecursive(SemaphoreHandle_t,int){return 1;}
static inline int xSemaphoreGiveRecursive(SemaphoreHandle_t){return 1;}
