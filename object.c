#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>
#include <stdint.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────────

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    const char *type_str;
    switch (type) {
        case OBJ_BLOB:   type_str = "blob";   break;
        case OBJ_TREE:   type_str = "tree";   break;
        case OBJ_COMMIT: type_str = "commit"; break;
        default: return -1;
    }

    char header[64];
    int header_len = sprintf(header, "%s %zu", type_str, len) + 1;

    size_t total_len = header_len + len;
    uint8_t *full_object = malloc(total_len);
    if (!full_object) return -1;
    memcpy(full_object, header, header_len);
    memcpy(full_object + header_len, data, len);

    compute_hash(full_object, total_len, id_out);

    if (object_exists(id_out)) {
        free(full_object);
        return 0;
    }

    char path[512];
    object_path(id_out, path, sizeof(path));

    char shard_dir[512];
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id_out, hex);
    snprintf(shard_dir, sizeof(shard_dir), "%s/%.2s", OBJECTS_DIR, hex);
    mkdir(shard_dir, 0755);

    char tmp_path[520];
    snprintf(tmp_path, sizeof(tmp_path), "%s/tmp_XXXXXX", shard_dir);
    int fd = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) { free(full_object); return -1; }

    if (write(fd, full_object, total_len) != (ssize_t)total_len) {
        close(fd);
        unlink(tmp_path);
        free(full_object);
        return -1;
    }

    fsync(fd);
    close(fd);
    free(full_object);

    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return -1;
    }

    int dir_fd = open(shard_dir, O_RDONLY);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }

    return 0;
}

int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    char path[512];
    object_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *buf = malloc(file_size);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, file_size, f) != (size_t)file_size) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    uint8_t *null_sep = memchr(buf, '\0', file_size);
    if (!null_sep) { free(buf); return -1; }

    if (strncmp((char *)buf, "blob ", 5) == 0) *type_out = OBJ_BLOB;
    else if (strncmp((char *)buf, "tree ", 5) == 0) *type_out = OBJ_TREE;
    else if (strncmp((char *)buf, "commit ", 7) == 0) *type_out = OBJ_COMMIT;
    else { free(buf); return -1; }

    ObjectID computed;
    compute_hash(buf, file_size, &computed);
    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) {
        fprintf(stderr, "error: object integrity check failed\n");
        free(buf);
        return -1;
    }

    size_t header_size = (null_sep - buf) + 1;
    size_t data_len = file_size - header_size;
    *data_out = malloc(data_len + 1);
    if (!*data_out) { free(buf); return -1; }
    memcpy(*data_out, null_sep + 1, data_len);
    ((char *)*data_out)[data_len] = '\0';
    *len_out = data_len;

    free(buf);
    return 0;
}


