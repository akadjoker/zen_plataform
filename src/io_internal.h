#ifndef IO_INTERNAL_H
#define IO_INTERNAL_H

#include "platform.h"

typedef struct
{
    int64_t (*size)(IoStream *s);
    int64_t (*seek)(IoStream *s, int64_t offset, IoWhence whence);
    size_t (*read)(IoStream *s, void *dst, size_t n);
    size_t (*write)(IoStream *s, const void *src, size_t n);
    bool (*flush)(IoStream *s);
    bool (*close)(IoStream *s);
} IoVTable;

struct IoStream
{
    const IoVTable *vt;
    bool eof;
    union
    {
        int fd;
        void *handle;
        struct
        {
            const uint8_t *base;
            size_t size;
            size_t pos;
            void (*release)(void *mem);
        } mem;
    } u;
};

IoStream *io_stream_alloc(const IoVTable *vt);

typedef enum
{
    IO_MODE_READ = 1,
    IO_MODE_WRITE = 2,
    IO_MODE_APPEND = 4,
    IO_MODE_TRUNCATE = 8,
    IO_MODE_CREATE = 16
} IoModeFlags;

int io_parse_mode(const char *mode);

/* A number no other call in this process, and no other process on the machine, returns at the same time: for the name
   of a temporary file. */
unsigned long long io_platform_unique(void);

IoStream *io_platform_open_file(const char *path, int flags);
/* The file a save has to replace: `path` itself, or, when it is a symbolic link, the file the link leads to (written to
   buf), so that saving through a link does not turn the link into a regular file. NULL, with the error set, for a loop of links. */
const char *io_platform_save_target(const char *path, char *buf, size_t cap);
/* Gives the open file s the permissions the file at `like` has (nothing when there is none, or where permissions are not
   a file's own). For the temporary file of a save, so that replacing a file does not widen who can read it. */
void io_platform_copy_mode(IoStream *s, const char *like);
bool io_platform_replace_file(const char *from, const char *to);
bool io_platform_remove_file(const char *path);

#endif /* IO_INTERNAL_H */
