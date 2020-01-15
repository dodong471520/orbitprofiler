//-----------------------------------
// Copyright Pierric Gimmig 2013-2017
//-----------------------------------
#pragma once

//#define USE_LOCAL_PERF_EVENT_HEADER

#ifdef USE_LOCAL_PERF_EVENT_HEADER
#include "../external/linux/perf_event.h"
#else
#include <linux/perf_event.h>
#endif
