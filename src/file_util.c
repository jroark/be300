#include "file_util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

int be300_write_file_atomic(const char *path, const void *data, size_t len)
{
    size_t n = strlen(path);
    char *tmp = malloc(n + sizeof(".tmp"));
    FILE *f;
    int saved;

    if (!tmp)
        return -1;
    memcpy(tmp, path, n);
    memcpy(tmp + n, ".tmp", sizeof(".tmp"));

    f = fopen(tmp, "wb");
    if (!f)
        goto fail;
    if (fwrite(data, 1, len, f) != len || fflush(f) != 0) {
        saved = errno;
        fclose(f);
        errno = saved;
        goto fail_unlink;
    }
#ifdef _WIN32
    _commit(_fileno(f));
#else
    fsync(fileno(f));
#endif
    if (fclose(f) != 0)
        goto fail_unlink;
#ifdef _WIN32
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        errno = EIO;
        goto fail_unlink;
    }
#else
    if (rename(tmp, path) != 0)
        goto fail_unlink;
#endif
    free(tmp);
    return 0;

fail_unlink:
    saved = errno;
    remove(tmp);
    errno = saved;
fail:
    free(tmp);
    return -1;
}
