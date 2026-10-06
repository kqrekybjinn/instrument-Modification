#pragma once
typedef void* SemaphoreHandle_t;
inline void* xSemaphoreCreateMutex(){return (void*)1;}
inline int xSemaphoreTake(void*,uint32_t){return 1;}
inline int xSemaphoreGive(void*){return 1;}
