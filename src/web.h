// web.h -- HTTP server entry point (webInit). Handlers are file-private.

#ifndef SFGW_WEB_H
#define SFGW_WEB_H

#include "common.h"

// ---- owned globals (defined in globals.cpp) ----
extern WebServer server;

void webInit();

#endif // SFGW_WEB_H
