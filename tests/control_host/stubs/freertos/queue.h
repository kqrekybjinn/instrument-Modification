#pragma once
#include <deque>
#include <vector>
#include <cstring>
struct MockQueue{unsigned capacity,item_size;std::deque<std::vector<char>> items;};
using QueueHandle_t=MockQueue*;
inline auto xQueueCreate(unsigned capacity,unsigned size){return new MockQueue{capacity,size,{}};}
inline unsigned uxQueueSpacesAvailable(MockQueue*q){return q->capacity-q->items.size();}
inline int xQueueReset(MockQueue*q){q->items.clear();return 1;}
inline int xQueueSend(MockQueue*q,const void*data,int){if(!uxQueueSpacesAvailable(q))return 0;auto b=(const char*)data;q->items.emplace_back(b,b+q->item_size);return 1;}
inline int xQueueSendToFront(MockQueue*q,const void*data,int){if(!uxQueueSpacesAvailable(q))return 0;auto b=(const char*)data;q->items.emplace_front(b,b+q->item_size);return 1;}
inline int xQueueReceive(MockQueue*q,void*data,uint32_t){if(q->items.empty())return 0;memcpy(data,q->items.front().data(),q->item_size);q->items.pop_front();return 1;}
