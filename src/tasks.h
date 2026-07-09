// tasks.h -- the FreeRTOS task entry points (spawned in setup()).

#ifndef SFGW_TASKS_H
#define SFGW_TASKS_H

#include "common.h"

void taskRS485(void* pv);
void taskRTC(void* pv);
void taskWeb(void* pv);
void taskNetwork(void* pv);

// True if the quiet schedule is enabled AND the current user-local time is inside
// its window. Shared by the schedule tick, the /api/quiet/schedule readout, and
// the guards that let the schedule win over an external quiet-OFF (MQTT/manual).
bool quietSchedInWindow();

#endif // SFGW_TASKS_H
