#pragma once
inline void vTaskDelay(uint32_t){}
inline int xTaskCreatePinnedToCore(void(*)(void*),const char*,int,void*,int,void*,int){return 1;}
