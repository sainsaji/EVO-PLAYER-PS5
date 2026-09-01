/*
 * Module: evo_data_path - see include/evo_data_path.h.
 */

#include "evo_data_path.h"

#include <stdio.h>
#include <string.h>

const char *evo_data_dir(void)
{
    return EVO_DATA_DIR;
}

const char *evo_data_path(const char *leaf)
{
    static _Thread_local char buf[512];

    if (!leaf)
        leaf = "";
    while (*leaf == '/')
        leaf++;

    int n = snprintf(buf, sizeof buf, "%s/%s", EVO_DATA_DIR, leaf);
    if (n < 0 || (size_t)n >= sizeof buf)
        return EVO_DATA_DIR;

    return buf;
}
