/* _DEFAULT_SOURCE must be defined before any system header is pulled in
 * (including transitively, through U3IOLinux.h -> U3LegacyTypes.h) so that
 * glibc declares strdup(), mkstemp() and O_DIRECTORY. Guarded so this still
 * compiles cleanly in a build that already defines it on the command line. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "U3IOLinux.h"

/* Only for the Handle/DisposeHandle declarations used by
 * U3IOReleaseLegacyResourceHandle() below -- see the comment there. This is
 * a read-only dependency on an already-implemented, stable API; nothing in
 * this file edits or assumes anything else about U3LegacyTypes.h. */
#include "U3LegacyTypes.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---------------------------------------------------------------------
 * On-disk save container format ("U3LS", version 1)
 *
 * This format is new to the Linux port: the original Mac build kept
 * mutable resources in a binary property list (see U3WriteResources() in
 * U3IOLegacy.m), an opaque Apple-specific encoding with no portable
 * byte-for-byte format to preserve. There is therefore no legacy on-disk
 * *container* format to match here.
 *
 * What must be preserved exactly is the meaning of each resource's
 * *payload* bytes: game code (see e.g. LoadUltimaMap() and GetRoster() in
 * UltimaMisc.c) only ever indexes resource bytes one at a time
 * (`buffer.bytes[i]`) -- it never reinterprets a resource as a native
 * multi-byte struct. So this file never needs to byte-swap a resource's
 * payload; payloads are carried through as opaque blobs, unchanged, in
 * both directions.
 *
 * This file's own container metadata (the magic/version/count header and
 * each record's kind/id/size fields) is new data with no Mac precedent,
 * but both hosts we build for are little-endian, so an incidental native
 * store would make the format host-dependent for no reason. All multi-byte
 * container fields are therefore written big-endian ("network order"),
 * explicitly, through the readBE16/readBE32/writeBE16/writeBE32 helpers
 * below.
 *
 * File layout:
 *
 *   offset  size  field
 *   0       4     magic       0x55334C53 ("U3LS", big-endian)
 *   4       4     version     1
 *   8       4     recordCount
 *   12      ...   recordCount records, back to back:
 *
 *     record offset  size  field
 *     0              1     kind        (U3ResourceKind, 0-12)
 *     1              2     resourceID  (int16_t, big-endian, two's complement)
 *     3              4     dataSize    (uint32_t, big-endian)
 *     7              dataSize  resource bytes, verbatim, no swapping
 *
 * Nothing follows the last record -- any trailing byte is corruption.
 * recordCount is capped at U3_MAX_RECORD_COUNT and dataSize at
 * U3_MAX_RESOURCE_BYTES/U3_MAX_CONTAINER_BYTES so a corrupt header can't
 * send the reader off trying to allocate or read gigabytes from a short
 * file.
 * ------------------------------------------------------------------- */

#define U3_CONTAINER_MAGIC    0x55334C53u /* "U3LS" */
#define U3_CONTAINER_VERSION  1u
#define U3_CONTAINER_HEADER_SIZE 12u
#define U3_RECORD_HEADER_SIZE 7u /* kind(1) + resourceID(2) + dataSize(4) */

#define U3_MAX_RESOURCE_BYTES  ((size_t)16u * 1024u * 1024u)
#define U3_MAX_CONTAINER_BYTES ((size_t)128u * 1024u * 1024u)
#define U3_MAX_RECORD_COUNT    4096u

#define U3_CONTAINER_FILE_NAME "Roster-v1.u3s"
#define U3_TEMP_FILE_SUFFIX    ".tmpXXXXXX"
#define U3_DEFAULT_SAVE_LEAF   "ultima3"

/* One resource living in the save container, either freshly loaded from
 * disk or pending its next flush. */
typedef struct U3LinuxResource {
    uint8_t kind;
    int16_t resourceID;
    uint8_t *data;
    size_t size;
} U3LinuxResource;

/* Process-wide save container state. U3IOLinuxConfigure() is documented as
 * a one-per-process setup call, so a single global (rather than a handle
 * threaded through every call) matches how callers actually use this API. */
typedef struct U3LinuxContainer {
    U3LinuxResource *resources;
    size_t count;
    size_t capacity;
    char *saveDirectory;
    char *containerPath;
    U3LinuxAssets *assets;
} U3LinuxContainer;

static U3LinuxContainer gContainer;
static int32_t gLastError;

/* A resource opened for writing via U3IOCreateMutableResource() /
 * U3IOOpenMutableResource(). This is what U3MutableDataBuffer.owner points
 * at; it is freed by U3IOCloseMutableResource(). */
typedef struct U3LinuxMutableResource {
    uint8_t kind;
    int16_t resourceID;
    uint8_t *data;
    size_t size;
} U3LinuxMutableResource;

static void set_error(int errnoValue) {
    gLastError = errnoValue ? errnoValue : EIO;
}

static bool kind_is_valid(U3ResourceKind kind) {
    return (unsigned)kind <= (unsigned)U3ResourceKindStringTable;
}

/* Four-character resource type codes, verified against
 * U3LegacyResourceTypeForKind() in U3IOLegacy.m and cross-checked against
 * the "type_hex" fields actually present in the exported asset manifest
 * (build/linux-game/assets/manifest.json). U3ResourceKindPreferences has no
 * bundled counterpart -- preferences are created fresh by OpenRstr(), never
 * read from the asset bundle -- which is why 'PREF' does not appear in the
 * manifest; that is expected, not a bug. */
static uint32_t resource_type_fourcc(U3ResourceKind kind) {
    switch (kind) {
        case U3ResourceKindMisc:          return 0x4D495343u; /* 'MISC' */
        case U3ResourceKindMap:           return 0x4D415053u; /* 'MAPS' */
        case U3ResourceKindParty:         return 0x50525459u; /* 'PRTY' */
        case U3ResourceKindRoster:        return 0x524F5354u; /* 'ROST' */
        case U3ResourceKindMonster:       return 0x4D4F4E53u; /* 'MONS' */
        case U3ResourceKindTalk:          return 0x544C4B53u; /* 'TLKS' */
        case U3ResourceKindDemo:          return 0x44454D4Fu; /* 'DEMO' */
        case U3ResourceKindPreferences:   return 0x50524546u; /* 'PREF' */
        case U3ResourceKindSoundResource: return 0x736E6420u; /* 'snd ' */
        case U3ResourceKindSignature:     return 0x53474E54u; /* 'SGNT' */
        case U3ResourceKindConsoleScreen: return 0x434F4E53u; /* 'CONS' */
        case U3ResourceKindImage:         return 0x50494354u; /* 'PICT' */
        case U3ResourceKindStringTable:   return 0x53545223u; /* 'STR#' */
    }
    return 0;
}

static uint32_t read_be32(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static uint16_t read_be16(const uint8_t *bytes) {
    return (uint16_t)(((uint16_t)bytes[0] << 8) | (uint16_t)bytes[1]);
}

static void write_be32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static void write_be16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void free_all_resources(void) {
    for (size_t i = 0; i < gContainer.count; ++i) {
        free(gContainer.resources[i].data);
    }
    free(gContainer.resources);
    gContainer.resources = NULL;
    gContainer.count = 0;
    gContainer.capacity = 0;
}

static ssize_t find_resource(uint8_t kind, int16_t resourceID) {
    for (size_t i = 0; i < gContainer.count; ++i) {
        if (gContainer.resources[i].kind == kind &&
            gContainer.resources[i].resourceID == resourceID) {
            return (ssize_t)i;
        }
    }
    return -1;
}

/* Copies (kind, resourceID) -> data into the in-memory resource table,
 * replacing any existing entry for that key. Used both while committing a
 * mutable resource and while loading records from disk. Does not touch the
 * container file; call flush_container() afterwards to persist. */
static bool put_resource(uint8_t kind, int16_t resourceID, const uint8_t *data, size_t size) {
    if (size > U3_MAX_RESOURCE_BYTES) {
        set_error(EFBIG);
        return false;
    }
    uint8_t *copy = malloc(size ? size : 1);
    if (!copy) {
        set_error(ENOMEM);
        return false;
    }
    if (size) memcpy(copy, data, size);

    ssize_t index = find_resource(kind, resourceID);
    if (index < 0) {
        if (gContainer.count == gContainer.capacity) {
            size_t newCapacity = gContainer.capacity ? gContainer.capacity * 2 : 32;
            U3LinuxResource *grown = realloc(gContainer.resources, newCapacity * sizeof(*grown));
            if (!grown) {
                free(copy);
                set_error(ENOMEM);
                return false;
            }
            gContainer.resources = grown;
            gContainer.capacity = newCapacity;
        }
        index = (ssize_t)gContainer.count;
        gContainer.resources[index] = (U3LinuxResource){0};
        gContainer.count++;
    }

    free(gContainer.resources[index].data);
    gContainer.resources[index].kind = kind;
    gContainer.resources[index].resourceID = resourceID;
    gContainer.resources[index].data = copy;
    gContainer.resources[index].size = size;
    return true;
}

/* mkdir -p, Linux style: create every missing component of `path`.
 * On failure, errno is left as set by the failing mkdir() call. */
static bool make_directory_recursive(const char *path) {
    char buffer[PATH_MAX];
    if (strlen(path) >= sizeof(buffer)) {
        errno = ENAMETOOLONG;
        return false;
    }
    strcpy(buffer, path);
    for (char *slash = buffer + 1; *slash; ++slash) {
        if (*slash != '/') continue;
        *slash = '\0';
        if (mkdir(buffer, 0700) != 0 && errno != EEXIST) return false;
        *slash = '/';
    }
    if (mkdir(buffer, 0700) != 0 && errno != EEXIST) return false;
    return true;
}

/* XDG-based default save directory, used only when U3IOOpenSaveContainer()
 * is called before any explicit U3IOLinuxConfigure() and U3_SAVE_DIRECTORY
 * is unset: $XDG_DATA_HOME/ultima3, or $HOME/.local/share/ultima3 if
 * XDG_DATA_HOME is unset. Returns NULL (caller treats as ENOENT) if neither
 * variable is available. Caller owns the returned string. */
static char *default_save_directory(void) {
    const char *xdgDataHome = getenv("XDG_DATA_HOME");
    char homeDataBuffer[PATH_MAX];
    const char *base = (xdgDataHome && *xdgDataHome) ? xdgDataHome : NULL;
    if (!base) {
        const char *home = getenv("HOME");
        if (!home || !*home) return NULL;
        if (snprintf(homeDataBuffer, sizeof(homeDataBuffer), "%s/.local/share", home) >=
            (int)sizeof(homeDataBuffer)) {
            return NULL;
        }
        base = homeDataBuffer;
    }
    size_t length = strlen(base) + strlen("/" U3_DEFAULT_SAVE_LEAF) + 1;
    char *result = malloc(length);
    if (result) snprintf(result, length, "%s/" U3_DEFAULT_SAVE_LEAF, base);
    return result;
}

/* Sets the active save directory and the container file path derived from
 * it, discarding whatever resources were loaded from the previous
 * directory (if any). Does not touch gContainer.assets. */
static bool set_save_directory(const char *directory) {
    if (!directory || !*directory) {
        set_error(EINVAL);
        return false;
    }
    free_all_resources();
    free(gContainer.saveDirectory);
    free(gContainer.containerPath);
    gContainer.containerPath = NULL;

    gContainer.saveDirectory = strdup(directory);
    if (!gContainer.saveDirectory) {
        set_error(ENOMEM);
        return false;
    }
    size_t pathLength = strlen(directory) + strlen("/" U3_CONTAINER_FILE_NAME) + 1;
    gContainer.containerPath = malloc(pathLength);
    if (!gContainer.containerPath) {
        set_error(ENOMEM);
        return false;
    }
    snprintf(gContainer.containerPath, pathLength, "%s/" U3_CONTAINER_FILE_NAME, directory);
    return true;
}

/* Resolves and applies a save directory if U3IOLinuxConfigure() was never
 * called: U3_SAVE_DIRECTORY if set (this is also how the isolated test
 * suite points the adapter at a scratch directory), otherwise the XDG
 * default above. */
static bool ensure_save_directory_configured(void) {
    if (gContainer.saveDirectory) return true;
    const char *override = getenv("U3_SAVE_DIRECTORY");
    char *resolved = (override && *override) ? strdup(override) : default_save_directory();
    if (!resolved) {
        set_error((override && *override) ? ENOMEM : ENOENT);
        return false;
    }
    bool configured = set_save_directory(resolved);
    free(resolved);
    return configured;
}

void U3IOLinuxReset(void) {
    free_all_resources();
    free(gContainer.saveDirectory);
    free(gContainer.containerPath);
    gContainer.saveDirectory = NULL;
    gContainer.containerPath = NULL;
    gContainer.assets = NULL;
    gLastError = 0;
}

bool U3IOLinuxConfigure(const char *saveDirectory, U3LinuxAssets *assets) {
    if (!set_save_directory(saveDirectory)) return false;
    gContainer.assets = assets;
    gLastError = 0;
    return true;
}

int32_t U3IOLastError(void) {
    return gLastError;
}

static bool write_all(int fd, const void *data, size_t length) {
    const uint8_t *bytes = data;
    while (length) {
        ssize_t written = write(fd, bytes, length);
        if (written <= 0) return false;
        bytes += written;
        length -= (size_t)written;
    }
    return true;
}

/* Atomically rewrites the container file from gContainer.resources:
 * write a temp file in the same directory, fsync it, close it, rename it
 * over the real path, then fsync the directory so the rename itself is
 * durable. Every failure path records the errno of the call that actually
 * failed (not of whatever ran afterwards) and removes the temp file. */
static bool flush_container(void) {
    if (!gContainer.containerPath || !gContainer.saveDirectory) {
        set_error(EINVAL);
        return false;
    }

    size_t total = U3_CONTAINER_HEADER_SIZE;
    for (size_t i = 0; i < gContainer.count; ++i) {
        size_t recordSize = U3_RECORD_HEADER_SIZE + gContainer.resources[i].size;
        if (gContainer.resources[i].size > U3_MAX_RESOURCE_BYTES ||
            total > U3_MAX_CONTAINER_BYTES - recordSize) {
            set_error(EFBIG);
            return false;
        }
        total += recordSize;
    }

    size_t tempPathLength = strlen(gContainer.containerPath) + strlen(U3_TEMP_FILE_SUFFIX) + 1;
    char *tempPath = malloc(tempPathLength);
    if (!tempPath) {
        set_error(ENOMEM);
        return false;
    }
    snprintf(tempPath, tempPathLength, "%s" U3_TEMP_FILE_SUFFIX, gContainer.containerPath);

    int fd = mkstemp(tempPath);
    if (fd < 0) {
        set_error(errno);
        free(tempPath);
        return false;
    }

    bool ok = true;
    int failureErrno = 0;

    uint8_t header[U3_CONTAINER_HEADER_SIZE];
    write_be32(header, U3_CONTAINER_MAGIC);
    write_be32(header + 4, U3_CONTAINER_VERSION);
    write_be32(header + 8, (uint32_t)gContainer.count);
    if (!write_all(fd, header, sizeof(header))) {
        failureErrno = errno;
        ok = false;
    }

    for (size_t i = 0; ok && i < gContainer.count; ++i) {
        uint8_t recordHeader[U3_RECORD_HEADER_SIZE];
        recordHeader[0] = gContainer.resources[i].kind;
        write_be16(recordHeader + 1, (uint16_t)gContainer.resources[i].resourceID);
        write_be32(recordHeader + 3, (uint32_t)gContainer.resources[i].size);
        if (!write_all(fd, recordHeader, sizeof(recordHeader)) ||
            !write_all(fd, gContainer.resources[i].data, gContainer.resources[i].size)) {
            failureErrno = errno;
            ok = false;
        }
    }

    if (ok && fsync(fd) != 0) {
        failureErrno = errno;
        ok = false;
    }
    int closeResult = close(fd);
    if (ok && closeResult != 0) {
        failureErrno = errno;
        ok = false;
    }

    if (ok && rename(tempPath, gContainer.containerPath) != 0) {
        failureErrno = errno;
        ok = false;
    }

    if (ok) {
        int dirfd = open(gContainer.saveDirectory, O_RDONLY | O_DIRECTORY);
        if (dirfd < 0) {
            failureErrno = errno;
            ok = false;
        } else {
            if (fsync(dirfd) != 0) {
                failureErrno = errno;
                ok = false;
            }
            int dirCloseResult = close(dirfd);
            if (ok && dirCloseResult != 0) {
                failureErrno = errno;
                ok = false;
            }
        }
    }

    if (!ok) {
        unlink(tempPath);
        set_error(failureErrno);
    } else {
        gLastError = 0;
    }
    free(tempPath);
    return ok;
}

void U3IOFlushSaveContainer(void) {
    flush_container();
}

/* Reads and fully validates the container file already open on `fd`,
 * staging records into a freshly allocated array. Nothing is written to
 * gContainer until the whole file has been validated -- a corrupt file
 * must never leave the in-memory state partially overwritten. */
static bool parse_container(int fd, U3LinuxResource **outResources, size_t *outCount) {
    uint8_t header[U3_CONTAINER_HEADER_SIZE];
    if (read(fd, header, sizeof(header)) != (ssize_t)sizeof(header) ||
        read_be32(header) != U3_CONTAINER_MAGIC ||
        read_be32(header + 4) != U3_CONTAINER_VERSION) {
        set_error(EINVAL);
        return false;
    }
    uint32_t recordCount = read_be32(header + 8);
    if (recordCount > U3_MAX_RECORD_COUNT) {
        set_error(EINVAL);
        return false;
    }

    U3LinuxResource *staged = NULL;
    if (recordCount) {
        staged = calloc(recordCount, sizeof(*staged));
        if (!staged) {
            set_error(ENOMEM);
            return false;
        }
    }

    size_t stagedCount = 0;
    size_t total = U3_CONTAINER_HEADER_SIZE;
    bool ok = true;

    for (uint32_t i = 0; i < recordCount; ++i) {
        uint8_t recordHeader[U3_RECORD_HEADER_SIZE];
        if (read(fd, recordHeader, sizeof(recordHeader)) != (ssize_t)sizeof(recordHeader)) {
            ok = false;
            break;
        }
        uint32_t size = read_be32(recordHeader + 3);
        if (size > U3_MAX_RESOURCE_BYTES ||
            total > U3_MAX_CONTAINER_BYTES - U3_RECORD_HEADER_SIZE - size) {
            ok = false;
            break;
        }
        uint8_t *data = malloc(size ? size : 1);
        if (!data) {
            ok = false;
            break;
        }
        if (size && read(fd, data, size) != (ssize_t)size) {
            free(data);
            ok = false;
            break;
        }
        staged[i].kind = recordHeader[0];
        staged[i].resourceID = (int16_t)read_be16(recordHeader + 1);
        staged[i].data = data;
        staged[i].size = size;
        stagedCount = i + 1;
        total += U3_RECORD_HEADER_SIZE + size;
    }

    if (ok) {
        uint8_t trailingByte;
        if (read(fd, &trailingByte, 1) != 0) ok = false; /* reject trailing garbage */
    }

    if (!ok) {
        for (size_t i = 0; i < stagedCount; ++i) free(staged[i].data);
        free(staged);
        set_error(EINVAL);
        return false;
    }

    *outResources = staged;
    *outCount = recordCount;
    return true;
}

U3SaveContainerOpenResult U3IOOpenSaveContainer(void) {
    if (!ensure_save_directory_configured()) return U3SaveContainerOpenResultFailed;

    struct stat directoryInfo;
    if (stat(gContainer.saveDirectory, &directoryInfo) != 0) {
        if (errno != ENOENT) {
            set_error(errno);
            return U3SaveContainerOpenResultFailed;
        }
        if (!make_directory_recursive(gContainer.saveDirectory)) {
            set_error(errno);
            return U3SaveContainerOpenResultFailed;
        }
    }

    int fd = open(gContainer.containerPath, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            free_all_resources();
            gLastError = 0;
            return U3SaveContainerOpenResultCreatedEmpty;
        }
        set_error(errno);
        return U3SaveContainerOpenResultFailed;
    }

    U3LinuxResource *staged = NULL;
    size_t stagedCount = 0;
    bool parsed = parse_container(fd, &staged, &stagedCount);
    close(fd);
    if (!parsed) return U3SaveContainerOpenResultFailed;

    free_all_resources();
    gContainer.resources = staged;
    gContainer.count = stagedCount;
    gContainer.capacity = stagedCount;
    gLastError = 0;
    return U3SaveContainerOpenResultOpened;
}

bool U3IOLoadResource(U3ResourceKind kind, int16_t resourceID, U3DataBuffer *outBuffer) {
    if (!outBuffer) {
        set_error(EINVAL);
        return false;
    }
    *outBuffer = (U3DataBuffer){0};
    if (!kind_is_valid(kind)) {
        set_error(EINVAL);
        return false;
    }

    uint8_t *bytes = NULL;
    size_t size = 0;

    ssize_t index = find_resource((uint8_t)kind, resourceID);
    if (index >= 0) {
        size = gContainer.resources[index].size;
        bytes = malloc(size ? size : 1);
        if (!bytes) {
            set_error(ENOMEM);
            return false;
        }
        if (size) memcpy(bytes, gContainer.resources[index].data, size);
    } else if (gContainer.assets) {
        /* Bundled asset bytes come straight from U3LinuxAssets, which
         * already owns the allocation; nothing further to copy here. */
        U3LinuxAssetResource(gContainer.assets, resource_type_fourcc(kind), resourceID, &bytes, &size);
    }

    if (!bytes) {
        set_error(ENOENT);
        return false;
    }
    outBuffer->bytes = bytes;
    outBuffer->size = size;
    outBuffer->owner = bytes;
    gLastError = 0;
    return true;
}

void U3IOReleaseResource(U3DataBuffer *buffer) {
    if (!buffer) return;
    free(buffer->owner);
    *buffer = (U3DataBuffer){0};
}

bool U3IOCreateMutableResource(U3ResourceKind kind, int16_t resourceID, size_t size,
                                const uint8_t *name, U3MutableDataBuffer *outBuffer) {
    (void)name; /* Mac resource names have no Linux equivalent; kept only for call-site parity. */
    if (!outBuffer) {
        set_error(EINVAL);
        return false;
    }
    *outBuffer = (U3MutableDataBuffer){0};
    if (!kind_is_valid(kind) || size > U3_MAX_RESOURCE_BYTES) {
        set_error(EINVAL);
        return false;
    }

    U3LinuxMutableResource *handle = calloc(1, sizeof(*handle));
    if (!handle) {
        set_error(ENOMEM);
        return false;
    }
    handle->data = calloc(1, size ? size : 1);
    if (!handle->data) {
        free(handle);
        set_error(ENOMEM);
        return false;
    }
    handle->kind = (uint8_t)kind;
    handle->resourceID = resourceID;
    handle->size = size;

    outBuffer->bytes = handle->data;
    outBuffer->size = size;
    outBuffer->owner = handle;
    gLastError = 0;
    return true;
}

bool U3IOOpenMutableResource(U3ResourceKind kind, int16_t resourceID, U3MutableDataBuffer *outBuffer) {
    U3DataBuffer source;
    if (!U3IOLoadResource(kind, resourceID, &source)) return false;
    bool ok = U3IOCreateMutableResource(kind, resourceID, source.size, NULL, outBuffer);
    if (ok) memcpy(outBuffer->bytes, source.bytes, source.size);
    U3IOReleaseResource(&source);
    return ok;
}

bool U3IOResizeMutableResource(U3MutableDataBuffer *buffer, size_t size) {
    if (!buffer || !buffer->owner || size > U3_MAX_RESOURCE_BYTES) {
        set_error(EINVAL);
        return false;
    }
    U3LinuxMutableResource *handle = buffer->owner;
    uint8_t *grown = realloc(handle->data, size ? size : 1);
    if (!grown) {
        set_error(ENOMEM);
        return false;
    }
    if (size > handle->size) memset(grown + handle->size, 0, size - handle->size);
    handle->data = grown;
    handle->size = size;
    buffer->bytes = grown;
    buffer->size = size;
    gLastError = 0;
    return true;
}

void U3IOCloseMutableResource(U3MutableDataBuffer *buffer, bool commit) {
    if (!buffer || !buffer->owner) return;
    U3LinuxMutableResource *handle = buffer->owner;
    if (commit) {
        /* put_resource()/flush_container() record their own errors; a
         * discarded (commit == false) close deliberately leaves
         * U3IOLastError() untouched, matching U3IOCloseMutableResource()
         * in U3IOLegacy.m. */
        if (put_resource(handle->kind, handle->resourceID, handle->data, handle->size)) {
            flush_container();
        }
    }
    free(handle->data);
    free(handle);
    *buffer = (U3MutableDataBuffer){0};
}

bool U3IOCopyResource(U3ResourceKind kind, int16_t sourceResourceID, int16_t destinationResourceID,
                       const uint8_t *name) {
    U3DataBuffer source;
    if (!U3IOLoadResource(kind, sourceResourceID, &source)) return false;

    U3MutableDataBuffer destination;
    if (!U3IOCreateMutableResource(kind, destinationResourceID, source.size, name, &destination)) {
        U3IOReleaseResource(&source);
        return false;
    }
    memcpy(destination.bytes, source.bytes, source.size);
    U3IOCloseMutableResource(&destination, true);
    U3IOReleaseResource(&source);
    return gLastError == 0;
}

void U3IOReleaseLegacyResourceHandle(void *resourceHandle) {
    /* The Mac build's U3IOReleaseLegacyResourceHandle() calls
     * ReleaseResource() on a Resource Manager Handle (e.g. the PICT handle
     * GetPicture() returns in UltimaNewMap.c). On Linux, Handle is the
     * double-indirected `char **` defined in U3LegacyTypes.h, allocated by
     * NewHandle()/NewHandleClear() in U3LegacyMemory.c as a small struct
     * whose first member is the payload pointer; DisposeHandle() is the
     * matching release and frees both the payload and the struct. A plain
     * free(resourceHandle) would only free the struct and leak the
     * payload, so this must go through DisposeHandle(), not free(). */
    DisposeHandle((Handle)resourceHandle);
}
