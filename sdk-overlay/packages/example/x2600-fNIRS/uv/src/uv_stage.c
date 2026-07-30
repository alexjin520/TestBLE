#include "fnirs_uv_stage.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define UV_STAGE_LOG "/app_data/golgi/uv-stage.log"

void fnirs_uv_stage(const char *stage)
{
    char line[160];
    int fd;
    int len;

    if (!stage)
        return;

    len = snprintf(line, sizeof(line), "pid=%d uv-stage: %s\n",
                   (int)getpid(), stage);
    if (len <= 0)
        return;
    if ((size_t)len >= sizeof(line))
        len = (int)sizeof(line) - 1;

    (void)write(STDERR_FILENO, line, (size_t)len);

    fd = open(UV_STAGE_LOG,
              O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_SYNC, 0644);
    if (fd < 0)
        return;
    (void)write(fd, line, (size_t)len);
    (void)fsync(fd);
    (void)close(fd);
}
