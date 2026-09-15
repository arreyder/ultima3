#include "U3LinuxAssets.h"
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

struct U3LinuxAssets {
    char *directory;
    struct json_object *manifest;
};

static struct json_object *member(struct json_object *object, const char *name,
                                  enum json_type type) {
    struct json_object *value = NULL;
    return json_object_object_get_ex(object, name, &value) &&
           json_object_is_type(value, type) ? value : NULL;
}

static const char *string_member(struct json_object *object, const char *name) {
    struct json_object *value = member(object, name, json_type_string);
    return value ? json_object_get_string(value) : NULL;
}

static int64_t integer_member(struct json_object *object, const char *name) {
    struct json_object *value = member(object, name, json_type_int);
    return value ? json_object_get_int64(value) : -1;
}

static char *asset_path(const char *directory, const char *relative) {
    if (!directory || !relative || !*relative || *relative == '/') return NULL;
    for (const char *part = relative; *part;) {
        const char *end = strchr(part, '/');
        size_t length = end ? (size_t)(end - part) : strlen(part);
        if (!length || (length == 2 && !memcmp(part, "..", 2))) return NULL;
        part += length;
        if (*part) ++part;
    }
    size_t size = strlen(directory) + strlen(relative) + 2;
    char *path = malloc(size);
    if (path) snprintf(path, size, "%s/%s", directory, relative);
    return path;
}

static uint8_t *read_record(U3LinuxAssets *assets, struct json_object *record, size_t *size) {
    int64_t length = integer_member(record, "size");
    if (length < 0 || length > 256 * 1024 * 1024) return NULL;
    char *path = asset_path(assets->directory, string_member(record, "path"));
    if (!path) return NULL;
    FILE *file = fopen(path, "rb");
    free(path);
    if (!file) return NULL;
    uint8_t *bytes = malloc(length ? (size_t)length : 1);
    if (!bytes || fread(bytes, 1, (size_t)length, file) != (size_t)length ||
        fgetc(file) != EOF || ferror(file)) {
        free(bytes);
        bytes = NULL;
    }
    fclose(file);
    if (bytes) *size = (size_t)length;
    return bytes;
}

static struct json_object *find_named(U3LinuxAssets *assets, const char *category,
                                     const char *key, const char *value) {
    if (!assets || !value) return NULL;
    struct json_object *array = member(assets->manifest, category, json_type_array);
    if (!array) return NULL;
    for (size_t i = 0; i < json_object_array_length(array); ++i) {
        struct json_object *record = json_object_array_get_idx(array, i);
        const char *name = string_member(record, key);
        if (name && !strcmp(name, value)) return record;
    }
    return NULL;
}

U3LinuxAssets *U3LinuxAssetsOpen(const char *directory) {
    char *path = asset_path(directory, "manifest.json");
    if (!path) return NULL;
    struct json_object *manifest = json_object_from_file(path);
    free(path);
    if (!manifest) return NULL;
    if (integer_member(manifest, "version") != 1 ||
        !member(manifest, "resources", json_type_array) ||
        !member(manifest, "images", json_type_array) ||
        !member(manifest, "strings", json_type_array)) {
        json_object_put(manifest);
        return NULL;
    }
    U3LinuxAssets *assets = calloc(1, sizeof(*assets));
    if (!assets) { json_object_put(manifest); return NULL; }
    assets->directory = strdup(directory);
    assets->manifest = manifest;
    if (!assets->directory) { U3LinuxAssetsClose(assets); return NULL; }
    return assets;
}

void U3LinuxAssetsClose(U3LinuxAssets *assets) {
    if (!assets) return;
    free(assets->directory);
    json_object_put(assets->manifest);
    free(assets);
}

bool U3LinuxAssetResource(U3LinuxAssets *assets, uint32_t type, int16_t id,
                         uint8_t **bytes, size_t *size) {
    if (!bytes || !size) return false;
    *bytes = NULL; *size = 0;
    if (!assets) return false;
    char hex[9];
    snprintf(hex, sizeof(hex), "%08x", (unsigned)type);
    struct json_object *array = member(assets->manifest, "resources", json_type_array);
    for (size_t i = 0; i < json_object_array_length(array); ++i) {
        struct json_object *record = json_object_array_get_idx(array, i);
        const char *kind = string_member(record, "type_hex");
        if (kind && !strcmp(kind, hex) && integer_member(record, "id") == id) {
            *bytes = read_record(assets, record, size);
            return *bytes != NULL;
        }
    }
    return false;
}

char *U3LinuxAssetString(U3LinuxAssets *assets, const char *table, size_t index) {
    struct json_object *record = find_named(assets, "strings", "name", table);
    if (!record) return NULL;
    size_t size = 0;
    uint8_t *bytes = read_record(assets, record, &size);
    if (!bytes || size > INT_MAX) { free(bytes); return NULL; }
    struct json_tokener *tokener = json_tokener_new();
    if (!tokener) { free(bytes); return NULL; }
    struct json_object *array = json_tokener_parse_ex(tokener, (char *)bytes, (int)size);
    free(bytes);
    char *result = NULL;
    if (json_tokener_get_error(tokener) == json_tokener_success &&
        array && json_object_is_type(array, json_type_array) &&
        index < json_object_array_length(array)) {
        struct json_object *value = json_object_array_get_idx(array, index);
        if (json_object_is_type(value, json_type_string)) result = strdup(json_object_get_string(value));
    }
    json_object_put(array);
    json_tokener_free(tokener);
    return result;
}

bool U3LinuxAssetImage(U3LinuxAssets *assets, const char *source, U3Bitmap *bitmap,
                      int width, int height, int columns, int rows) {
    if (!bitmap || width <= 0 || height <= 0 || width > 8191 || height > 32767 ||
        columns <= 0 || rows <= 0 || width % columns || height % rows) return false;
    struct json_object *record = find_named(assets, "images", "source", source);
    if (!record) return false;
    int64_t sw = integer_member(record, "width"), sh = integer_member(record, "height");
    const char *format = string_member(record, "format");
    if (!format || strcmp(format, "RGBA8") || sw < columns || sh < rows ||
        sw > 32767 || sh > 32767 || integer_member(record, "stride") != sw * 4) return false;
    size_t size = 0;
    uint8_t *bytes = read_record(assets, record, &size);
    if (!bytes || size != (size_t)(sw * sh * 4)) { free(bytes); return false; }
    U3Bitmap decoded = {0}, scaled = {0};
    if (!U3BitmapAllocate(&decoded, (int)sw, (int)sh) ||
        !U3BitmapAllocate(&scaled, width, height)) {
        free(bytes); U3BitmapDispose(&decoded); U3BitmapDispose(&scaled); return false;
    }
    for (size_t i = 0; i < size; i += 4) {
        for (int channel = 0; channel < 3; ++channel)
            decoded.pixels[i + channel] = (uint8_t)((bytes[i + channel] * bytes[i + 3] + 127) / 255);
    }
    free(bytes);
    bool success = true;
    for (int row = 0; row < rows && success; ++row) {
        for (int column = 0; column < columns && success; ++column) {
            int x = (int)(column * sw / columns), y = (int)(row * sh / rows);
            U3BitmapRect src = {x, y, (int)((column + 1) * sw / columns) - x,
                               (int)((row + 1) * sh / rows) - y};
            U3BitmapRect dst = {column * (width / columns), row * (height / rows),
                               width / columns, height / rows};
            success = U3BitmapCopy(&scaled, dst, &decoded, src);
        }
    }
    U3BitmapDispose(&decoded);
    if (!success) { U3BitmapDispose(&scaled); return false; }
    U3BitmapDispose(bitmap);
    *bitmap = scaled;
    return true;
}
