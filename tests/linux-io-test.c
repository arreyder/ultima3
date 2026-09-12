/* Exercises Sources/Linux/U3IOLinux.c against the real exported asset
 * bundle and a scratch save directory.
 *
 * Usage: linux-io-test <assets-directory>
 * Requires U3_SAVE_DIRECTORY set to an existing, empty, writable directory
 * that this test owns for its duration (it will write real files there).
 *
 * This does not use U3IOLinuxReset()'s internals directly -- it only calls
 * the public U3IOLinux.h / U3IO.h API, the same surface the rest of the
 * game links against.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "Linux/U3IOLinux.h"
#include "Linux/U3LinuxAssets.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Must match U3_CONTAINER_FILE_NAME in Sources/Linux/U3IOLinux.c -- there is
 * no public accessor for the container path, and this test deliberately
 * pokes at the file directly to manufacture corruption scenarios. */
#define CONTAINER_FILE_NAME "Roster-v1.u3s"

/* ---- tiny growable byte buffer, for hand-building raw container files -- */

typedef struct ByteBuffer {
    uint8_t *bytes;
    size_t size;
    size_t capacity;
} ByteBuffer;

static void buffer_append(ByteBuffer *buffer, const void *data, size_t length) {
    if (buffer->size + length > buffer->capacity) {
        buffer->capacity = (buffer->size + length) * 2 + 16;
        buffer->bytes = realloc(buffer->bytes, buffer->capacity);
        assert(buffer->bytes);
    }
    memcpy(buffer->bytes + buffer->size, data, length);
    buffer->size += length;
}

static void buffer_append_be32(ByteBuffer *buffer, uint32_t value) {
    uint8_t bytes[4] = {(uint8_t)(value >> 24), (uint8_t)(value >> 16),
                         (uint8_t)(value >> 8), (uint8_t)value};
    buffer_append(buffer, bytes, sizeof(bytes));
}

static void buffer_append_be16(ByteBuffer *buffer, uint16_t value) {
    uint8_t bytes[2] = {(uint8_t)(value >> 8), (uint8_t)value};
    buffer_append(buffer, bytes, sizeof(bytes));
}

static void buffer_free(ByteBuffer *buffer) {
    free(buffer->bytes);
    *buffer = (ByteBuffer){0};
}

static void write_file_bytes(const char *path, const uint8_t *data, size_t size) {
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(data, 1, size, file) == size);
    assert(fclose(file) == 0);
}

/* One valid record (kind=Misc, id=777, 2 bytes of payload) used as the
 * basis for every corruption scenario below. */
static ByteBuffer build_valid_single_record_container(void) {
    ByteBuffer buffer = {0};
    buffer_append_be32(&buffer, 0x55334C53u); /* magic */
    buffer_append_be32(&buffer, 1u);          /* version */
    buffer_append_be32(&buffer, 1u);          /* record count */
    uint8_t kind = U3ResourceKindMisc;
    buffer_append(&buffer, &kind, 1);
    buffer_append_be16(&buffer, (uint16_t)777);
    buffer_append_be32(&buffer, 2u);
    uint8_t payload[2] = {0xAA, 0xBB};
    buffer_append(&buffer, payload, sizeof(payload));
    return buffer;
}

#include <sys/stat.h>

int main(int argc, char **argv) {
    assert(argc == 2);
    const char *assetsDir = argv[1];
    const char *saveDir = getenv("U3_SAVE_DIRECTORY");
    if (!saveDir || !*saveDir) {
        saveDir = "/tmp/u3-io-test-dir";
        setenv("U3_SAVE_DIRECTORY", saveDir, 1);
    }
    mkdir(saveDir, 0755);

    char containerPath[PATH_MAX];
    snprintf(containerPath, sizeof(containerPath), "%s/%s", saveDir, CONTAINER_FILE_NAME);
    unlink(containerPath);

    U3LinuxAssets *assets = U3LinuxAssetsOpen(assetsDir);
    assert(assets);

    /* --- configure + first open: fresh directory, nothing saved yet ---- */
    assert(U3IOLinuxConfigure(saveDir, assets));
    assert(U3IOOpenSaveContainer() == U3SaveContainerOpenResultCreatedEmpty);
    puts("open on empty save directory reports CreatedEmpty");

    /* --- bundled resource reads of real exported game data ------------- */
    U3DataBuffer bundledMap = {0};
    assert(U3IOLoadResource(U3ResourceKindMap, 420, &bundledMap));
    assert(bundledMap.size == 4101 && bundledMap.bytes[0] == 64);
    U3IOReleaseResource(&bundledMap);
    puts("bundled MAPS 420 reads real asset bytes");

    /* --- the 420 -> 419 template-world copy ----------------------------- */
    assert(U3IOCopyResource(U3ResourceKindMap, 420, 419, (const uint8_t *)"\x10Sosaria Current"));
    U3DataBuffer copiedMap = {0};
    assert(U3IOLoadResource(U3ResourceKindMap, 419, &copiedMap));
    assert(copiedMap.size == 4101 && copiedMap.bytes[0] == 64);
    U3IOReleaseResource(&copiedMap);
    /* Bundled 419 does not exist (confirmed against the manifest), so this
     * can only have come from the copy landing in the mutable container. */
    puts("MAPS 420 -> 419 template copy lands in the save container");

    /* --- round trip: create, write, commit, reopen, read back identical - */
    U3MutableDataBuffer created = {0};
    assert(U3IOCreateMutableResource(U3ResourceKindMisc, 999, 4, NULL, &created));
    for (int i = 0; i < 4; ++i) created.bytes[i] = (uint8_t)(0x10 + i);
    U3IOCloseMutableResource(&created, true);
    assert(U3IOLastError() == 0);

    assert(U3IOOpenSaveContainer() == U3SaveContainerOpenResultOpened);
    U3DataBuffer roundTrip = {0};
    assert(U3IOLoadResource(U3ResourceKindMisc, 999, &roundTrip));
    assert(roundTrip.size == 4);
    for (int i = 0; i < 4; ++i) assert(roundTrip.bytes[i] == (uint8_t)(0x10 + i));
    U3IOReleaseResource(&roundTrip);
    puts("create/commit/reopen/read round trip preserves bytes exactly");

    /* --- shadowing: a saved resource hides the bundled one of same id -- */
    U3DataBuffer bundledMonsters = {0};
    assert(U3IOLoadResource(U3ResourceKindMonster, 420, &bundledMonsters));
    assert(bundledMonsters.size == 256);
    uint8_t originalFirstByte = bundledMonsters.bytes[0];
    U3IOReleaseResource(&bundledMonsters);

    U3MutableDataBuffer shadow = {0};
    assert(U3IOOpenMutableResource(U3ResourceKindMonster, 420, &shadow));
    assert(shadow.size == 256);
    shadow.bytes[0] = (uint8_t)(originalFirstByte ^ 0xFF);
    U3IOCloseMutableResource(&shadow, true);
    assert(U3IOLastError() == 0);

    U3DataBuffer shadowed = {0};
    assert(U3IOLoadResource(U3ResourceKindMonster, 420, &shadowed));
    assert(shadowed.bytes[0] == (uint8_t)(originalFirstByte ^ 0xFF));
    assert(shadowed.size == 256);
    U3IOReleaseResource(&shadowed);
    puts("a saved resource shadows the bundled resource of the same kind/id");

    /* --- resize semantics: grow zero-fills, shrink truncates ------------ */
    U3MutableDataBuffer resized = {0};
    assert(U3IOCreateMutableResource(U3ResourceKindMisc, 998, 4, NULL, &resized));
    resized.bytes[0] = 42;
    assert(U3IOResizeMutableResource(&resized, 16));
    assert(resized.size == 16 && resized.bytes[0] == 42);
    for (size_t i = 4; i < 16; ++i) assert(resized.bytes[i] == 0);
    assert(U3IOResizeMutableResource(&resized, 1));
    assert(resized.size == 1 && resized.bytes[0] == 42);
    U3IOCloseMutableResource(&resized, true);
    assert(U3IOLastError() == 0);
    puts("resize grows with zero-fill and shrinks with truncation");

    /* --- close without commit discards the change ----------------------- */
    U3MutableDataBuffer discarded = {0};
    assert(U3IOOpenMutableResource(U3ResourceKindMisc, 999, &discarded));
    assert(discarded.size == 4);
    uint8_t savedFirstByte = discarded.bytes[0];
    discarded.bytes[0] = (uint8_t)(savedFirstByte ^ 0xFF);
    U3IOCloseMutableResource(&discarded, false);

    U3DataBuffer unchanged = {0};
    assert(U3IOLoadResource(U3ResourceKindMisc, 999, &unchanged));
    assert(unchanged.bytes[0] == savedFirstByte);
    U3IOReleaseResource(&unchanged);
    puts("close without commit discards the in-memory change");

    /* --- corruption rejection: each case must fail without touching the -
     * already-loaded good state, and must report a non-zero error. ------ */
    ByteBuffer validContainer = build_valid_single_record_container();

    /* Bad magic. */
    {
        ByteBuffer bad = {0};
        buffer_append(&bad, validContainer.bytes, validContainer.size);
        bad.bytes[0] = 0x00;
        write_file_bytes(containerPath, bad.bytes, bad.size);
        buffer_free(&bad);
        assert(U3IOOpenSaveContainer() == U3SaveContainerOpenResultFailed);
        assert(U3IOLastError() == EINVAL);
    }

    /* Bad version. */
    {
        ByteBuffer bad = {0};
        buffer_append(&bad, validContainer.bytes, validContainer.size);
        bad.bytes[7] = 0x02; /* version field's low byte: 1 -> 2 */
        write_file_bytes(containerPath, bad.bytes, bad.size);
        buffer_free(&bad);
        assert(U3IOOpenSaveContainer() == U3SaveContainerOpenResultFailed);
        assert(U3IOLastError() == EINVAL);
    }

    /* Truncated record: header claims one record, file has none. */
    {
        write_file_bytes(containerPath, validContainer.bytes, 12);
        assert(U3IOOpenSaveContainer() == U3SaveContainerOpenResultFailed);
        assert(U3IOLastError() == EINVAL);
    }

    /* Trailing garbage after an otherwise-valid record set. */
    {
        ByteBuffer bad = {0};
        buffer_append(&bad, validContainer.bytes, validContainer.size);
        uint8_t extra = 0xFF;
        buffer_append(&bad, &extra, 1);
        write_file_bytes(containerPath, bad.bytes, bad.size);
        buffer_free(&bad);
        assert(U3IOOpenSaveContainer() == U3SaveContainerOpenResultFailed);
        assert(U3IOLastError() == EINVAL);
    }

    buffer_free(&validContainer);
    puts("bad magic, bad version, truncated record, and trailing garbage are all rejected");

    /* A corrupt container on disk must not have clobbered the previously
     * loaded good in-memory state (Misc 999 was loaded further above, long
     * before any of the corrupt files existed). */
    U3DataBuffer stillGood = {0};
    assert(U3IOLoadResource(U3ResourceKindMisc, 999, &stillGood));
    assert(stillGood.size == 4 && stillGood.bytes[0] == savedFirstByte);
    U3IOReleaseResource(&stillGood);
    puts("a failed reopen leaves previously loaded in-memory state untouched");

    /* Restore a good container so the env-var fallback path below has
     * something real to open (the last write above was the trailing-
     * garbage file, which is still sitting on disk at this point). */
    U3MutableDataBuffer restore = {0};
    assert(U3IOCreateMutableResource(U3ResourceKindMisc, 999, 1, NULL, &restore));
    restore.bytes[0] = savedFirstByte;
    U3IOCloseMutableResource(&restore, true);
    assert(U3IOLastError() == 0);

    /* --- U3_SAVE_DIRECTORY fallback: no explicit Configure() this time --
     * The harness runs this whole test with U3_SAVE_DIRECTORY already set
     * to saveDir (checked at the top of main), so U3IOLinuxReset() followed
     * directly by U3IOOpenSaveContainer() exercises the lazy env-var
     * resolution path in ensure_save_directory_configured() instead of the
     * explicit U3IOLinuxConfigure() path used everywhere above. */
    U3IOLinuxReset();
    assert(U3IOOpenSaveContainer() == U3SaveContainerOpenResultOpened);
    U3DataBuffer viaEnvFallback = {0};
    assert(U3IOLoadResource(U3ResourceKindMisc, 999, &viaEnvFallback));
    assert(viaEnvFallback.size == 1 && viaEnvFallback.bytes[0] == savedFirstByte);
    U3IOReleaseResource(&viaEnvFallback);
    puts("U3IOOpenSaveContainer() resolves U3_SAVE_DIRECTORY without an explicit Configure()");

    U3IOLinuxReset();
    U3LinuxAssetsClose(assets);
    puts("all linux-io-test scenarios passed");
    return 0;
}
