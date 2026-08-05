#ifndef CELLMGR_SYSINFO_H
#define CELLMGR_SYSINFO_H

#include "common.h"

int sysinfo_json(cellmgr_buf *out, const char *wan_iface);
int sysinfo_drop_caches(void);

#endif
