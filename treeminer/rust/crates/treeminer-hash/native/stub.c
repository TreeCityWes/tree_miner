/* Deterministic hash-batch stub implementing treeminer_hash.h.
 * Used by cargo tests (no CUDA). Production is treeminer_hash.cpp → CudaHashBackend
 * → kernelrunner.cu. Validation error strings match HashApiValidation.cpp. */

#include "treeminer_hash.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KEY_LEN 64
#define MAX_PATTERN 128
#define MAX_CPU_BATCH 10000
#define MAX_MATCHES 32

static const char* or_empty(const char* s) { return s ? s : ""; }

static const char* or_default(const char* s, const char* fallback) {
    return s ? s : fallback;
}

static char* dup_str(const char* s) {
    if (!s) {
        s = "";
    }
    size_t n = strlen(s);
    char* p = (char*)malloc(n + 1);
    if (!p) {
        return NULL;
    }
    memcpy(p, s, n + 1);
    return p;
}

static int is_hex(const char* s) {
    for (; *s; ++s) {
        if (!isxdigit((unsigned char)*s)) {
            return 0;
        }
    }
    return 1;
}

static void normalize_hex(const char* in, char* out, size_t out_sz) {
    if (!in) {
        out[0] = '\0';
        return;
    }
    if (in[0] == '0' && (in[1] == 'x' || in[1] == 'X')) {
        in += 2;
    }
    size_t i = 0;
    for (; in[i] && i + 1 < out_sz; ++i) {
        out[i] = (char)tolower((unsigned char)in[i]);
    }
    out[i] = '\0';
}

static int supported_algo(const char* a) { return strcmp(a, "argon2id-xen") == 0; }

static int supported_backend(const char* b) {
    return strcmp(b, "cpu") == 0 || strcmp(b, "reference") == 0 || strcmp(b, "cuda") == 0;
}

static void append_err(char* buf, size_t buf_sz, const char* msg) {
    size_t n = strlen(buf);
    if (n && n + 2 < buf_sz) {
        memcpy(buf + n, "; ", 2);
        n += 2;
    }
    size_t m = strlen(msg);
    if (n + m < buf_sz) {
        memcpy(buf + n, msg, m + 1);
    }
}

static void validate(const TreeminerHashRequest* r, char* errors, size_t errors_sz) {
    errors[0] = '\0';
    const char* algo = or_default(r->algorithm, "argon2id-xen");
    const char* backend = or_default(r->backend, "cpu");
    const char* pattern = or_default(r->target_pattern, "XEN11");
    char salt[256];
    char prefix[256];
    char key[256];
    normalize_hex(or_empty(r->salt_hex), salt, sizeof(salt));
    normalize_hex(or_empty(r->key_prefix), prefix, sizeof(prefix));
    normalize_hex(or_empty(r->key), key, sizeof(key));

    if (!supported_algo(algo)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "unsupported algorithm: %s", algo);
        append_err(errors, errors_sz, msg);
    }
    if (!supported_backend(backend)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "unsupported backend: %s", backend);
        append_err(errors, errors_sz, msg);
    }
    if (salt[0] == '\0') {
        append_err(errors, errors_sz, "salt_hex is required");
    } else {
        if (strlen(salt) % 2 != 0) {
            append_err(errors, errors_sz, "salt_hex must contain an even number of hex characters");
        }
        if (strlen(salt) < 16) {
            append_err(errors, errors_sz, "salt_hex must be at least 16 hex characters");
        }
        if (!is_hex(salt)) {
            append_err(errors, errors_sz, "salt_hex must contain only hex characters");
        }
    }
    if (prefix[0]) {
        if (strlen(prefix) > KEY_LEN) {
            append_err(errors, errors_sz, "key_prefix cannot exceed 64 hex characters");
        }
        if (!is_hex(prefix)) {
            append_err(errors, errors_sz, "key_prefix must contain only hex characters");
        }
    }
    if (key[0]) {
        if (strlen(key) != KEY_LEN) {
            append_err(errors, errors_sz, "key must contain exactly 64 hex characters");
        }
        if (!is_hex(key)) {
            append_err(errors, errors_sz, "key must contain only hex characters");
        }
        if (prefix[0] && strncmp(key, prefix, strlen(prefix)) != 0) {
            append_err(errors, errors_sz, "key must start with key_prefix when both are provided");
        }
    }
    if (pattern[0] == '\0') {
        append_err(errors, errors_sz, "target_pattern is required");
    }
    if (strlen(pattern) > MAX_PATTERN) {
        append_err(errors, errors_sz, "target_pattern is too long");
    }
    if (r->difficulty == 0) {
        append_err(errors, errors_sz, "difficulty must be greater than zero");
    }
    if (r->batch_size == 0) {
        append_err(errors, errors_sz, "batch_size must be greater than zero");
    }
    if ((strcmp(backend, "cpu") == 0 || strcmp(backend, "reference") == 0) &&
        r->batch_size > MAX_CPU_BATCH) {
        append_err(errors, errors_sz, "cpu batch_size exceeds safe limit");
    }
    if (r->device_id < 0) {
        append_err(errors, errors_sz, "device_id must be non-negative");
    }
    if (r->gpu_first_blocks && strcmp(backend, "cuda") != 0) {
        append_err(errors, errors_sz, "gpu_first_blocks requires backend=cuda");
    }
}

static void make_key(const char* prefix, size_t index, char* out) {
    size_t plen = strlen(prefix);
    if (plen >= KEY_LEN) {
        memcpy(out, prefix, KEY_LEN);
        out[KEY_LEN] = '\0';
        return;
    }
    size_t suffix_len = KEY_LEN - plen;
    char suffix[KEY_LEN + 32];
    snprintf(suffix, sizeof(suffix), "%0*zx", (int)suffix_len, index);
    size_t slen = strlen(suffix);
    if (slen > suffix_len) {
        memmove(suffix, suffix + (slen - suffix_len), suffix_len + 1);
    }
    memcpy(out, prefix, plen);
    memcpy(out + plen, suffix, suffix_len);
    out[KEY_LEN] = '\0';
}

static void stub_hash(const char* salt, const char* key, uint32_t difficulty, char* out,
                      size_t out_sz) {
    snprintf(out, out_sz, "stub$argon2id-xen$%s$%s$%u", salt, key, difficulty);
}

static int has_xuni(const char* hash) {
    const char* p = hash;
    while ((p = strstr(p, "XUNI")) != NULL) {
        if (isdigit((unsigned char)p[4])) {
            return 1;
        }
        p += 4;
    }
    return 0;
}

static int is_superblock(const char* hash) {
    int upper = 0;
    for (; *hash; ++hash) {
        if (isupper((unsigned char)*hash)) {
            ++upper;
        }
    }
    return upper >= 50;
}

int treeminer_hash_run_batch(const TreeminerHashRequest* request, TreeminerHashResult* out) {
    if (!request || !out) {
        return TREEMINER_HASH_ERR_NULL;
    }
    memset(out, 0, sizeof(*out));

    const char* algo = or_default(request->algorithm, "argon2id-xen");
    const char* backend = or_default(request->backend, "cpu");
    const char* pattern = or_default(request->target_pattern, "XEN11");
    out->request_id = dup_str(or_empty(request->request_id));
    out->algorithm = dup_str(algo);
    out->device_id = request->device_id;
    out->batch_size = request->batch_size;

    char errors[1024];
    validate(request, errors, sizeof(errors));
    if (errors[0]) {
        out->backend = dup_str(strcmp(backend, "reference") == 0 ? "reference-stub" : "cpu-stub");
        out->error = dup_str(errors);
        out->ok = 0;
        return TREEMINER_HASH_OK;
    }
    if (strcmp(backend, "cuda") == 0) {
        out->backend = dup_str("cuda");
        out->error = dup_str("cuda backend is not available in the stub backend");
        out->ok = 0;
        return TREEMINER_HASH_OK;
    }
    out->backend = dup_str(strcmp(backend, "reference") == 0 ? "reference-stub" : "cpu-stub");

    char salt[256];
    char prefix[256];
    char fixed_key[256];
    normalize_hex(or_empty(request->salt_hex), salt, sizeof(salt));
    normalize_hex(or_empty(request->key_prefix), prefix, sizeof(prefix));
    normalize_hex(or_empty(request->key), fixed_key, sizeof(fixed_key));
    int single = fixed_key[0] != '\0';
    size_t attempts = single ? 1 : request->batch_size;

    TreeminerHashMatch matches[MAX_MATCHES];
    size_t match_n = 0;
    char last_hash[512] = "";

    for (size_t i = 0; i < attempts; ++i) {
        char generated[KEY_LEN + 1];
        const char* key = generated;
        if (single) {
            key = fixed_key;
        } else {
            make_key(prefix, i, generated);
        }
        char hash[512];
        stub_hash(salt, key, request->difficulty, hash, sizeof(hash));
        if (single) {
            snprintf(last_hash, sizeof(last_hash), "%s", hash);
        }
        if (strstr(hash, pattern) && match_n < MAX_MATCHES) {
            matches[match_n].key = dup_str(key);
            matches[match_n].hash = dup_str(hash);
            matches[match_n].matched_pattern = dup_str(pattern);
            matches[match_n].attempt_index = i;
            matches[match_n].is_superblock = is_superblock(hash);
            match_n++;
        }
        if (request->allow_xuni && has_xuni(hash) && match_n < MAX_MATCHES) {
            matches[match_n].key = dup_str(key);
            matches[match_n].hash = dup_str(hash);
            matches[match_n].matched_pattern = dup_str("XUNI");
            matches[match_n].attempt_index = i;
            matches[match_n].is_superblock = 0;
            match_n++;
        }
    }

    out->ok = 1;
    out->error = dup_str("");
    out->attempts = attempts;
    out->batch_size = attempts;
    out->batch_size_min = attempts;
    out->batch_size_max = attempts;
    out->hash = dup_str(last_hash);
    out->match_count = match_n;
    if (match_n) {
        out->matches = (TreeminerHashMatch*)calloc(match_n, sizeof(TreeminerHashMatch));
        if (out->matches) {
            memcpy(out->matches, matches, match_n * sizeof(TreeminerHashMatch));
        } else {
            out->match_count = 0;
        }
    }
    return TREEMINER_HASH_OK;
}

void treeminer_hash_result_free(TreeminerHashResult* out) {
    if (!out) {
        return;
    }
    free(out->request_id);
    free(out->error);
    free(out->algorithm);
    free(out->backend);
    free(out->hash);
    if (out->matches) {
        for (size_t i = 0; i < out->match_count; ++i) {
            free(out->matches[i].key);
            free(out->matches[i].hash);
            free(out->matches[i].matched_pattern);
        }
        free(out->matches);
    }
    memset(out, 0, sizeof(*out));
}
