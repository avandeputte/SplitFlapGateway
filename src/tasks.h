// tasks.h -- the FreeRTOS task entry points (spawned in setup()).

#ifndef SFGW_TASKS_H
#define SFGW_TASKS_H

#include "common.h"

void taskRS485(void* pv);
void taskRTC(void* pv);
void taskWeb(void* pv);
void taskNetwork(void* pv);

#endif // SFGW_TASKS_H
