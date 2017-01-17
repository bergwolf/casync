#include <sys/stat.h>
#include <fcntl.h>

#include "caencoder.h"
#include "calocation.h"
#include "caseed.h"
#include "chunker.h"
#include "realloc-buffer.h"
#include "util.h"

struct CaSeed {
        CaEncoder *encoder;
        int base_fd;
        int cache_fd;
        char *cache_path;

        CaChunker chunker;
        gcry_md_hd_t object_digest;

        bool ready;
        bool remove_cache;

        ReallocBuffer buffer;
        CaLocation *buffer_location;
};

CaSeed *ca_seed_new(void) {
        CaSeed *s;

        s = new0(CaSeed, 1);
        if (!s)
                return NULL;

        s->cache_fd = -1;
        s->base_fd = -1;
        return s;
}

static void ca_seed_remove_and_close_cache(CaSeed *s) {
        DIR *d;
        int r;

        assert(s);

        if (!s->remove_cache)
                return;

        d = fdopendir(s->cache_fd);
        if (!d)
                return;
        s->cache_fd = -1;

        for (;;) {
                DIR *sub;
                struct dirent *de;

                errno = 0;
                de = readdir(d);
                if (!de)
                        break;

                if (dot_or_dot_dot(de->d_name))
                        continue;

                if (!IN_SET(de->d_type, DT_DIR, DT_UNKNOWN))
                        continue;

                r = xopendirat(dirfd(d), de->d_name, 0, &sub);
                if (r < 0)
                        continue;

                for (;;) {
                        struct dirent *sde;

                        errno = 0;
                        sde = readdir(sub);
                        if (!sde)
                                break;

                        if (dot_or_dot_dot(sde->d_name))
                                continue;

                        (void) unlinkat(dirfd(sub), sde->d_name, 0);
                }

                closedir(sub);
                (void) unlinkat(dirfd(d), de->d_name, AT_REMOVEDIR);
        }

        closedir(d);

        if (s->cache_path) {
                (void) rmdir(s->cache_path);
                s->cache_path = mfree(s->cache_path);
        }
}

CaSeed *ca_seed_unref(CaSeed *s) {
        if (!s)
                return NULL;

        ca_seed_remove_and_close_cache(s);

        ca_encoder_unref(s->encoder);

        safe_close(s->base_fd);
        safe_close(s->cache_fd);
        free(s->cache_path);

        gcry_md_close(s->object_digest);

        realloc_buffer_free(&s->buffer);
        ca_location_unref(s->buffer_location);

        free(s);

        return NULL;
}

int ca_seed_set_base_fd(CaSeed *s, int fd) {
        if (!s)
                return -EINVAL;
        if (fd < 0)
                return -EINVAL;

        if (s->base_fd >= 0)
                return -EBUSY;

        s->base_fd = fd;
        return 0;
}

int ca_seed_set_base_path(CaSeed *s, const char *path) {
        if (!s)
                return -EINVAL;
        if (!path)
                return -EINVAL;

        if (s->base_fd >= 0)
                return -EBUSY;

        s->base_fd = open(path, O_CLOEXEC|O_NOCTTY|O_RDONLY);
        if (s->base_fd < 0)
                return -errno;

        return 0;
}

int ca_seed_set_cache_fd(CaSeed *s, int fd) {
        if (!s)
                return -EINVAL;
        if (fd < 0)
                return -EINVAL;

        if (s->cache_fd >= 0)
                return -EBUSY;
        if (s->cache_path)
                return -EBUSY;

        s->cache_fd = fd;
        return 0;
}

int ca_seed_set_cache_path(CaSeed *s, const char *path) {
        if (!s)
                return -EINVAL;
        if (!path)
                return -EINVAL;

        if (s->cache_fd >= 0)
                return -EBUSY;
        if (s->cache_path)
                return -EBUSY;

        s->cache_path = strdup(path);
        if (!s->cache_path)
                return -ENOMEM;

        return 0;
}

static int ca_seed_open(CaSeed *s) {
        int r;

        if (!s)
                return -EINVAL;

        if (!s->encoder) {
                if (s->base_fd < 0)
                        return -EUNATCH;

                s->encoder = ca_encoder_new();
                if (!s->encoder)
                        return -ENOMEM;

                r = ca_encoder_set_base_fd(s->encoder, s->base_fd);
                if (r < 0)
                        return r;

                s->base_fd = -1;
        }

        if (s->cache_fd < 0) {
                if (!s->cache_path) {
                        if (asprintf(&s->cache_path, "/var/tmp/casync-cache.%" PRIu64, random_u64()) < 0)
                                return r;

                        s->remove_cache = true;
                }

                (void) mkdir(s->cache_path, 0777);

                s->cache_fd = open(s->cache_path, O_RDONLY|O_CLOEXEC|O_DIRECTORY|O_NOCTTY);
                if (s->cache_fd < 0)
                        return -errno;
        }

        return 0;
}

static int ca_seed_write_cache_entry(CaSeed *s, CaLocation *location, const void *data, size_t l) {
        const char *t, *four, *combined;
        char ids[CA_OBJECT_ID_FORMAT_MAX];
        CaObjectID id;
        int r;

        assert(s);
        assert(location);
        assert(data);
        assert(l > 0);

        r = ca_location_patch_size(&location, l);
        if (r < 0)
                return r;

        t = ca_location_format(location);
        if (!t)
                return -ENOMEM;

        r = ca_object_id_make(&s->object_digest, data, l, &id);
        if (r < 0)
                return r;

        if (!ca_object_id_format(&id, ids))
                return -EINVAL;

        four = strndupa(ids, 4);
        combined = strjoina(four, "/", ids);

        (void) mkdirat(s->cache_fd, four, 0777);

        if (symlinkat(t, s->cache_fd, combined) < 0) {
                if (errno != EEXIST)
                        return -errno;
        }

        return 0;
}

static int ca_seed_cache_chunks(CaSeed *s) {
        uint64_t offset = 0;
        const void *p;
        size_t l;
        int r;

        assert(s);

        r = ca_encoder_get_data(s->encoder, &p, &l);
        if (r < 0)
                return r;

        while (l > 0) {
                const void *object;
                size_t object_size, k;

                if (!s->buffer_location) {
                        r = ca_encoder_current_location(s->encoder, offset, &s->buffer_location);
                        if (r < 0)
                                return r;
                }

                k = ca_chunker_scan(&s->chunker, p, l);
                if (k == (size_t) -1) {
                        if (!realloc_buffer_append(&s->buffer, p, l))
                                return -ENOMEM;

                        return 0;
                }

                if (s->buffer.size == 0) {
                        object = p;
                        object_size = k;
                } else {
                        if (!realloc_buffer_append(&s->buffer, p, k))
                                return -ENOMEM;

                        object = s->buffer.data;
                        object_size = s->buffer.size;
                }

                r = ca_seed_write_cache_entry(s, s->buffer_location, object, object_size);
                if (r < 0)
                        return r;

                realloc_buffer_empty(&s->buffer);
                s->buffer_location = ca_location_unref(s->buffer_location);

                p = (const uint8_t*) p + k;
                l -= k;

                offset += k;
        }

        return 0;
}

static int ca_seed_cache_final_chunk(CaSeed *s) {
        int r;

        assert(s);

        if (s->buffer.size == 0)
                return 0;

        if (!s->buffer_location)
                return 0;

        r = ca_seed_write_cache_entry(s, s->buffer_location, s->buffer.data, s->buffer.size);
        if (r < 0)
                return 0;

        realloc_buffer_empty(&s->buffer);
        s->buffer_location = ca_location_unref(s->buffer_location);

        return 0;
}

int ca_seed_step(CaSeed *s) {
        int r;

        if (!s)
                return -EINVAL;

        if (s->ready)
                return -EALREADY;

        r = ca_seed_open(s);
        if (r < 0)
                return r;

        for (;;) {
                int step;

                step = ca_encoder_step(s->encoder);
                if (step < 0)
                        return step;

                switch (step) {

                case CA_ENCODER_FINISHED:

                        r = ca_seed_cache_final_chunk(s);
                        if (r < 0)
                                return r;

                        s->ready = true;

                        return CA_SEED_READY;

                case CA_ENCODER_NEXT_FILE:
                case CA_ENCODER_DATA:
                        r = ca_seed_cache_chunks(s);
                        if (r < 0)
                                return r;

                        return step == CA_ENCODER_NEXT_FILE ? CA_SEED_NEXT_FILE : CA_SEED_STEP;

                default:
                        assert(false);
                }
        }
}

int ca_seed_get(CaSeed *s, const CaObjectID *object_id, void **ret, size_t *ret_size) {
        char id[CA_OBJECT_ID_FORMAT_MAX], *target = NULL;
        const char *four, *combined;
        CaLocation *l = NULL;
        uint64_t size, n = 0;
        void *p = NULL;
        int r, step;

        if (!s)
                return -EINVAL;
        if (!object_id)
                return -EINVAL;
        if (!ret)
                return -EINVAL;
        if (!ret_size)
                return -EINVAL;

        if (!ca_object_id_format(object_id, id))
                return -EINVAL;

        four = strndupa(id, 4);
        combined = strjoina(four, "/", id);

        r = readlinkat_malloc(s->cache_fd, combined, &target);
        if (r < 0)
                return r;

        /* fprintf(stderr, "GOT %s → %s\n", combined, target); */

        r = ca_location_parse(target, &l);
        free(target);
        if (r < 0)
                return r;

        if (l->size == UINT64_MAX || l->size > CHUNK_MAX) {
                ca_location_unref(l);
                return -EINVAL;
        }
        size = l->size;

        step = ca_encoder_seek(s->encoder, l);
        ca_location_unref(l);
        if (step < 0)
                return step;

        p = realloc_buffer_acquire(&s->buffer, size);
        if (!p)
                return -ENOMEM;

        for (;;) {
                switch (step) {

                case CA_ENCODER_FINISHED:
                        return -EPIPE;

                case CA_ENCODER_NEXT_FILE:
                case CA_ENCODER_DATA: {
                        const void *q;
                        size_t w;
                        uint64_t m;

                        r = ca_encoder_get_data(s->encoder, &q, &w);
                        if (r < 0)
                                return r;

                        m = MIN(w, size - n);

                        memcpy((uint8_t*) p + n, q, m);
                        n += m;

                        if (n >= size) {
                                CaObjectID test_id;

                                r = ca_object_id_make(&s->object_digest, p, size, &test_id);
                                if (r < 0)
                                        return r;

                                if (!ca_object_id_equal(object_id, &test_id)) {

                                        /* fprintf(stderr, "FROM SEED (%" PRIu64 ":\n", size); */
                                        /* hexdump(stderr, p, MIN(1024U, size)); */

                                        return -ESTALE;
                                }

                                *ret = p;
                                *ret_size = size;
                                return 0;
                        }
                        break;
                }

                default:
                        assert(false);
                }

                step = ca_encoder_step(s->encoder);
                if (step < 0)
                        return step;
        }
}

int ca_seed_current_path(CaSeed *seed, char **ret) {
        if (!seed)
                return -EINVAL;
        if (!ret)
                return -EINVAL;

        if (seed->ready)
                return -EALREADY;

        return ca_encoder_current_path(seed->encoder, ret);
}

int ca_seed_current_mode(CaSeed *seed, mode_t *ret) {
        if (!seed)
                return -EINVAL;
        if (!ret)
                return -EINVAL;

        if (seed->ready)
                return -EALREADY;

        return ca_encoder_current_mode(seed->encoder, ret);
}
