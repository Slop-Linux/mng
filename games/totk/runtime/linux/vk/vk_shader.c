/* Shader variants: Maxwell code -> totk_xlate --service --target vulkan (Ryujinx's translator) -> SPIR-V and its resource
   list (<variant>.spv, <variant>.vk.txt) in the shader cache, then a descriptor set layout per variant. A shader is the
   SHA-256 of its control section and code (nvn_mtl.m shader_key_of). Its variants are listed in <dir>/<shader>.variants,
   one name a line, in the prepared seed (TOTK_SHADER_SEED_DIR, prepare_shader_seed.py --target vulkan, made from the
   shared shader list) and in the cache; each variant's "dep" lines are the answers its translation read (Program.cs).
   The bound state uses the first variant whose deps it answers the same way (nvn_mtl.m deps_match), and only a state no
   variant serves is translated: with the whole bound state as the key, 729 shaders had made 8,635 variants. */
#include "vk.h"
#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int totk_depth_near_minus_w(void);   /* hle_nvn.c: nvnDeviceSetDepthMode(NEAR_IS_MINUS_W) */
#define CHAIN "botw-vk-v2"

static FILE *g_in, *g_out; static char g_dir[4096], g_seed[4096];
static pthread_mutex_t g_sh_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_translated, g_translate_ns, g_loaded, g_load_ns;   /* g_load_ns: variants met in frames, read from the cache (files, set layout, c1 buffer) */

static void dirs(void) {
    if (g_dir[0]) return;
    const char *base = getenv("TOTK_SHADER_CACHE"); if (!base || !*base) base = "build/shader-cache";
    snprintf(g_dir, sizeof g_dir, "%s/%s", base, CHAIN); mkdir(base, 0755); mkdir(g_dir, 0755);
    const char *seed = getenv("TOTK_SHADER_SEED_DIR"); if (seed && *seed) snprintf(g_seed, sizeof g_seed, "%s", seed);
    VKLOG("shader variants: cache %s, seed %s", g_dir, g_seed[0] ? g_seed : "none");
}
static void service_start(void) {
    if (g_in) return;
    const char *xl = getenv("TOTK_XLATE"); if (!xl || !*xl) xl = "prebuilt/linux-arm64/totk_xlate";
    int to[2], from[2]; if (pipe(to) || pipe(from)) VKDIE("pipe");
    pid_t pid = fork();
    if (pid < 0) VKDIE("fork");
    if (!pid) {
        dup2(to[0], 0); dup2(from[1], 1); close(to[1]); close(from[0]);
        execl(xl, xl, "--service", "--target", "vulkan", "--out", g_dir, "--chain", CHAIN, (char *)NULL);
        fprintf(stderr, "[vk] exec %s: %s\n", xl, strerror(errno)); _exit(127);
    }
    close(to[0]); close(from[1]);
    g_in = fdopen(to[1], "w"); g_out = fdopen(from[0], "r");
    signal(SIGPIPE, SIG_IGN);
    VKLOG("shader translation service %s (cache %s)", xl, g_dir);
}
static void hex(const uint8_t *p, size_t n, char *out) { for (size_t i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", p[i]); }
static int read_file(const char *path, uint8_t **data, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long s = ftell(f); fseek(f, 0, SEEK_SET);
    *data = malloc(s > 0 ? s : 1); *n = fread(*data, 1, s, f); fclose(f); return *n == (size_t)s;
}

/* a variant's deps: kind t (texture slot type), i (image slot format), a (vertex input class), f (colour output class),
   w (depth mode NEAR_IS_MINUS_W, vertex) */
typedef struct { char kind; uint8_t slot; char val[24]; } vdep;
typedef struct cand { char name[96]; const char *dir; vdep *d; int nd; vshader *s; struct cand *next; } cand;
typedef struct shv { char key[65]; cand *c, **tail; struct shv *next; } shv;
static shv *g_shv[4096];
static uint64_t g_seed_used;
/* lookups (a program stage under a bind state the replay had not met): served by a variant already loaded, by one first
   loaded from the seed / the cache, or missed (translated); the misses with the shader in the seed, and the dep each
   missed the shader's first variant on */
static uint64_t g_look, g_hit_mem, g_hit_seed, g_hit_cache, g_miss, g_miss_seeded, g_miss_kind[5];
static uint64_t g_seed_shaders;   /* shaders met that the seed lists */

static int read_deps(cand *c) {
    char path[4400], line[256]; snprintf(path, sizeof path, "%s/%s.vk.txt", c->dir, c->name);
    FILE *f = fopen(path, "r"); if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        char kind[8], val[24]; int slot;
        if (strncmp(line, "dep ", 4)) continue;
        int n = sscanf(line + 4, "%7s %d %23s", kind, &slot, val);
        if (n < 2 || slot < 0 || slot > 255) continue;
        vdep d = {.kind = !strcmp(kind, "tex") ? 't' : !strcmp(kind, "img") ? 'i' : !strcmp(kind, "attr") ? 'a' : !strcmp(kind, "fout") ? 'f' : !strcmp(kind, "dmw") ? 'w' : 0, .slot = (uint8_t)slot};
        if (!d.kind || (d.kind != 'w' && n < 3)) continue;
        if (d.kind != 'w') snprintf(d.val, sizeof d.val, "%s", val);
        c->d = realloc(c->d, (c->nd + 1) * sizeof *c->d); c->d[c->nd++] = d;
    }
    fclose(f); return 1;
}
/* the answer the translator would read for a dep (Program.cs Accessor: an unanswered texture slot is 2D or the image
   slot's type, an unanswered image format R8G8B8A8Unorm, an unanswered input or output float) */
static const vdep *g_missed_on; static char g_missed_got[24];   /* serves: the dep the last refused variant missed on */
static int serves(const cand *c, const vk_answers *A) {
    for (int i = 0; i < c->nd; i++) {
        const vdep *d = &c->d[i]; const char *got;
        switch (d->kind) {
            case 't': got = d->slot < 128 && A->tex[d->slot] ? A->tex[d->slot] : d->slot < 32 && A->img_type[d->slot] ? A->img_type[d->slot] : "2d"; break;
            case 'i': got = d->slot < 32 && A->img_fmt[d->slot] ? A->img_fmt[d->slot] : "R8G8B8A8Unorm"; break;
            case 'a': got = d->slot < 16 && A->attr[d->slot] ? A->attr[d->slot] : "float"; break;
            case 'f': got = d->slot < 8 && A->fout[d->slot] ? A->fout[d->slot] : "float"; break;
            default: if (d->slot != (totk_depth_near_minus_w() ? 1 : 0)) { g_missed_on = d; snprintf(g_missed_got, sizeof g_missed_got, "%d", totk_depth_near_minus_w()); return 0; } continue;
        }
        if (strcmp(got, d->val)) { g_missed_on = d; snprintf(g_missed_got, sizeof g_missed_got, "%s", got); return 0; }
    }
    return 1;
}
static void add_cand(shv *v, const char *dir, const char *name) {
    for (cand *c = v->c; c; c = c->next) if (c->dir == dir && !strcmp(c->name, name)) return;
    cand *c = calloc(1, sizeof *c); snprintf(c->name, sizeof c->name, "%s", name); c->dir = dir;
    if (!read_deps(c)) { free(c); return; }
    *v->tail = c; v->tail = &c->next;
}
static void read_index(shv *v, const char *dir) {
    if (!dir[0]) return;
    char path[4400], line[256]; snprintf(path, sizeof path, "%s/%s.variants", dir, v->key);
    FILE *f = fopen(path, "r"); if (!f) return;
    while (fgets(line, sizeof line, f)) { line[strcspn(line, "\r\n")] = 0; if (line[0]) add_cand(v, dir, line); }
    fclose(f);
}
static shv *shader_of(const char *key) {
    unsigned h = 0; for (int i = 0; i < 64; i++) h = h * 131 + (uint8_t)key[i]; h &= 4095;
    for (shv *v = g_shv[h]; v; v = v->next) if (!memcmp(v->key, key, 64)) return v;
    shv *v = calloc(1, sizeof *v); memcpy(v->key, key, 64); v->tail = &v->c;
    read_index(v, g_seed); if (v->c) g_seed_shaders++;
    read_index(v, g_dir);   /* seed variants first: the shared list's states, made at build time */
    v->next = g_shv[h]; g_shv[h] = v; return v;
}

/* vertex inputs the SPIR-V reads: Location decorations of Input variables (builtins carry BuiltIn instead) */
static uint32_t input_locations(const uint32_t *w, size_t n) {
    uint32_t bound = n > 3 ? w[3] : 0, mask = 0;
    if (!bound || bound > (1u << 22)) return 0;
    int32_t *loc = malloc(bound * sizeof *loc); uint8_t *input = calloc(bound, 1);
    for (uint32_t i = 0; i < bound; i++) loc[i] = -1;
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; if (!len) break;
        if (op == 71 && len >= 4 && w[i + 2] == 30 && w[i + 1] < bound) loc[w[i + 1]] = (int32_t)w[i + 3];   /* OpDecorate Location */
        if (op == 59 && len >= 4 && w[i + 3] == 1 && w[i + 2] < bound) input[w[i + 2]] = 1;               /* OpVariable Input */
        i += len;
    }
    for (uint32_t i = 0; i < bound; i++) if (input[i] && loc[i] >= 0 && loc[i] < 32) mask |= 1u << loc[i];
    free(loc); free(input); return mask;
}
/* the bytes of each uniform buffer (set, binding) the shader reads: Ryujinx declares a cbuf as { vec4 data[4096] } and reads it
   through OpAccessChain %var %int_0 <element> [...]; the largest constant element + 1 vec4, or 0 (all) when an element index
   is not a constant */
static uint32_t cbuf_used(const uint32_t *w, size_t n, int set, int binding) {
    uint32_t bound = n > 3 ? w[3] : 0; if (!bound || bound > (1u << 22)) return 0;
    uint32_t *dset = calloc(bound, 4), *dbind = calloc(bound, 4), *cval = calloc(bound, 4); uint8_t *isc = calloc(bound, 1);
    for (size_t i = 5; i < n;) { uint32_t op = w[i] & 0xffff, len = w[i] >> 16; if (!len) break;
        if (op == 71 && len >= 4 && w[i + 1] < bound) { if (w[i + 2] == 34) dset[w[i + 1]] = w[i + 3] + 1; if (w[i + 2] == 33) dbind[w[i + 1]] = w[i + 3] + 1; }
        if (op == 43 && len == 4 && w[i + 2] < bound) { isc[w[i + 2]] = 1; cval[w[i + 2]] = w[i + 3]; }
        i += len; }
    uint32_t used = 0; int all = 0;
    for (size_t i = 5; i < n && !all;) { uint32_t op = w[i] & 0xffff, len = w[i] >> 16; if (!len) break;
        if ((op == 65 || op == 66) && len >= 6 && w[i + 3] < bound && dset[w[i + 3]] == (uint32_t)set + 1 && dbind[w[i + 3]] == (uint32_t)binding + 1) {   /* OpAccessChain / OpInBoundsAccessChain */
            uint32_t e = w[i + 5]; if (e >= bound || !isc[e] || cval[e] >= 4096) all = 1; else if ((cval[e] + 1) * 16 > used) used = (cval[e] + 1) * 16; }
        else if (op != 71 && op != 5) for (uint32_t q = 1; q < len; q++) { uint32_t id = w[i + q]; if (id < bound && dset[id] == (uint32_t)set + 1 && dbind[id] == (uint32_t)binding + 1 && op != 59 && op != 15) all = 1; }   /* any other use */
        i += len; }
    free(dset); free(dbind); free(cval); free(isc);
    return all ? 0 : used ? used : 16;
}
static VkDescriptorType desc_type(const vk_rbind *b) {
    switch (b->kind) {
        case RB_CBUF: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case RB_SBUF: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case RB_TEX: return (b->stype & 0xff) == 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        default: return (b->stype & 0xff) == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
}
static vshader *load(const char *dir, const char *variant, int stage, const uint8_t *c1, size_t c1_len, const uint32_t *local) {
    char path[4400]; uint8_t *spv; size_t n;
    snprintf(path, sizeof path, "%s/%s.spv", dir, variant);
    if (!read_file(path, &spv, &n) || n < 20 || n % 4) return NULL;
    snprintf(path, sizeof path, "%s/%s.vk.txt", dir, variant);
    FILE *f = fopen(path, "r"); if (!f) { free(spv); return NULL; }
    vshader *s = calloc(1, sizeof *s); snprintf(s->key, sizeof s->key, "%s", variant); s->stage = stage; s->spv = (uint32_t *)spv; s->nwords = n / 4;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        vk_rbind b = {0}; int set, binding;
        if (!strncmp(line, "cbuf ", 5) && sscanf(line + 5, "%d %d %d", &set, &binding, &b.slot) == 3) b.kind = RB_CBUF;
        else if (!strncmp(line, "sbuf ", 5) && sscanf(line + 5, "%d %d %d %d %d %d", &set, &binding, &b.slot, &b.cb_slot, &b.cb_off, &b.flags) == 6) b.kind = RB_SBUF;
        else if ((!strncmp(line, "tex ", 4) || !strncmp(line, "img ", 4)) &&
                 sscanf(line + 4, "%d %d %d %d %d %d %d %d %d", &set, &binding, &b.stype, &b.cbuf_slot, &b.handle, &b.array, &b.separate, &b.flags, &b.format) == 9) b.kind = line[0] == 't' ? RB_TEX : RB_IMG;
        else continue;
        b.set = (uint8_t)set; b.binding = (uint16_t)binding; if (b.array < 1) b.array = 1;
        s->b = realloc(s->b, (s->nb + 1) * sizeof *s->b); s->b[s->nb++] = b;
    }
    fclose(f);
    for (int i = 0; i < s->nb; i++) if (s->b[i].kind == RB_CBUF) s->b[i].used = cbuf_used(s->spv, s->nwords, s->b[i].set, s->b[i].binding);
    if (stage == 0) s->in_mask = input_locations(s->spv, s->nwords);
    if (local) memcpy(s->local, local, sizeof s->local);
    /* c1: the shader's own constant segment */
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = c1_len > 256 ? c1_len : 256, .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VKCHECK(vkCreateBuffer(V.dev, &bi, NULL, &s->c1buf)); VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, s->c1buf, &req);
    /* c1 in a shared host-visible arena (one allocation per 4 MiB instead of one per variant: ~4,000 variants a session) */
    static VkDeviceMemory arena; static uint8_t *arena_cpu; static VkDeviceSize arena_used, arena_size; static uint32_t arena_type = UINT32_MAX;
    uint32_t type = vk_memtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceSize al = req.alignment > 256 ? req.alignment : 256, off = (arena_used + al - 1) & ~(al - 1);
    if (!arena || type != arena_type || off + req.size > arena_size) {
        arena_size = req.size > (4u << 20) ? req.size : (4u << 20); arena_type = type; off = 0;
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = arena_size, .memoryTypeIndex = type};
        VKCHECK(vkAllocateMemory(V.dev, &ai, NULL, &arena)); VKCHECK(vkMapMemory(V.dev, arena, 0, VK_WHOLE_SIZE, 0, (void **)&arena_cpu));
    }
    VKCHECK(vkBindBufferMemory(V.dev, s->c1buf, arena, off)); s->c1mem = VK_NULL_HANDLE; arena_used = off + req.size;
    memset(arena_cpu + off, 0, bi.size); if (c1_len) memcpy(arena_cpu + off, c1, c1_len);
    s->c1_len = c1_len;
    /* layout of this stage's set: its bindings (vertex / compute: set 0 with Ryujinx's support buffer at binding 0) */
    VkDescriptorSetLayoutBinding lb[128]; int nl = 0; int set = stage == 1 ? 1 : 0;
    if (set == 0) lb[nl++] = (VkDescriptorSetLayoutBinding){0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, NULL};
    for (int i = 0; i < s->nb && nl < 128; i++) {
        if (s->b[i].set != set) { VKLOG("【红】%s: binding %u in set %u, expected %d", variant, s->b[i].binding, s->b[i].set, set); continue; }
        lb[nl++] = (VkDescriptorSetLayoutBinding){s->b[i].binding, desc_type(&s->b[i]), (uint32_t)s->b[i].array, VK_SHADER_STAGE_ALL, NULL};
    }
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = (uint32_t)nl, .pBindings = lb};
    VKCHECK(vkCreateDescriptorSetLayout(V.dev, &li, NULL, &s->layout));
    g_loaded++;
    return s;
}
vshader *vk_shader_get(const uint8_t *control, const uint8_t *code, int stage, const char *answers, const vk_answers *A) {
    uint32_t c1o, c1t, klen; memcpy(&c1o, control + 0x700, 4); memcpy(&c1t, control + 0x704, 4); memcpy(&klen, control + 0x20, 4);
    if (klen < 0x7cc || klen >= 0x880) klen = 0x880;
    if (!c1t || c1t > 16u << 20 || c1o > c1t) VKDIE("shader code area %#x / c1 %#x", c1t, c1o);
    uint8_t md[32]; CC_SHA256_CTX c; CC_SHA256_Init(&c); CC_SHA256_Update(&c, control, klen); CC_SHA256_Update(&c, code, c1t); CC_SHA256_Final(md, &c);
    uint8_t ma[32]; CC_SHA256((const uint8_t *)answers, (unsigned)strlen(answers), ma);
    char key[96]; hex(md, 32, key); key[64] = '-'; hex(ma, 6, key + 65);
    pthread_mutex_lock(&g_sh_mu);
    dirs();
    shv *v = shader_of(key);
    uint32_t local[3] = {0, 0, 0}; if (stage == 5) memcpy(local, control + 0x748, 12);
    g_look++;
    for (cand *c = v->c; c; c = c->next) if (serves(c, A)) {
        if (c->s) g_hit_mem++; else if (c->dir == g_seed) g_hit_seed++; else g_hit_cache++;
        if (!c->s) {
            uint64_t l0_ = clock_gettime_nsec_np(CLOCK_MONOTONIC);
            c->s = load(c->dir, c->name, stage, code + c1o, c1t - c1o, stage == 5 ? local : NULL);
            if (!c->s) VKDIE("shader variant %s/%s: no SPIR-V", c->dir, c->name);
            g_load_ns += clock_gettime_nsec_np(CLOCK_MONOTONIC) - l0_;
            if (c->dir == g_seed) g_seed_used++;
        }
        pthread_mutex_unlock(&g_sh_mu); return c->s;
    }
    /* no variant serves this state: translate it (or take a translation an index append missed) and list it */
    g_miss++;
    if (!v->c) VKLOG("shader miss %.64s (stage %d): no variant of it in the seed or the cache", key, stage);
    else {
        serves(v->c, A);   /* the first variant's refusal, for the reason */
        if (v->c->dir == g_seed) g_miss_seeded++;
        const char *kinds = "tiafw"; int k = (int)(strchr(kinds, g_missed_on->kind) - kinds); g_miss_kind[k]++;
        VKLOG("shader miss %.64s (stage %d): %d variant(s), the first (%s) has %c %u = %s, the state %s", key, stage,
              ({ int n_ = 0; for (cand *c = v->c; c; c = c->next) n_++; n_; }), v->c->dir == g_seed ? "seed" : "cache",
              g_missed_on->kind, g_missed_on->slot, g_missed_on->kind == 'w' ? (g_missed_on->slot ? "1" : "0") : g_missed_on->val, g_missed_got);
    }
    vshader *s = load(g_dir, key, stage, code + c1o, c1t - c1o, stage == 5 ? local : NULL);
    if (!s) {
        service_start();
        uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC);
        size_t skip = stage == 5 ? 0 : 0x30;   /* graphics code drops the 0x30-byte NVN header (totk_shader_rt.m variant_request) */
        char path[4400]; snprintf(path, sizeof path, "%s/%s.code", g_dir, key);
        FILE *f = fopen(path, "wb"); if (!f || fwrite(code + skip, 1, c1t - skip, f) != c1t - skip) VKDIE("write %s", path); fclose(f);
        size_t c1n = c1t - c1o; char *c1hex = malloc(2 * c1n + 1); hex(code + c1o, c1n, c1hex); c1hex[2 * c1n] = 0;
        int efr = 0; if (stage == 1 && !(klen >= 0x7cc && klen < 0x880) && (control[0x718] & 1)) efr = 1;
        fprintf(g_in, "{\"variant\": \"%s\", \"stage\": \"%s\", \"code\": \"%s\", \"c1\": \"%s\", %s, \"depth_minus_w\": %s%s", key,
                stage == 0 ? "vs" : stage == 1 ? "ps" : "cs", path, c1hex, answers, totk_depth_near_minus_w() ? "true" : "false", efr ? ", \"early_fragment_tests\": true" : "");
        if (stage == 5) fprintf(g_in, ", \"local\": [%u, %u, %u]", local[0], local[1], local[2]);
        fprintf(g_in, "}\n"); fflush(g_in); free(c1hex);
        char reply[4096];
        if (!fgets(reply, sizeof reply, g_out)) VKDIE("shader service exited while translating %s", key);
        if (!strstr(reply, "\"ok\": true")) VKDIE("shader %s (stage %d) translation failed: %s", key, stage, reply);
        unlink(path);
        s = load(g_dir, key, stage, code + c1o, c1t - c1o, stage == 5 ? local : NULL);
        if (!s) VKDIE("shader %s: translation produced no SPIR-V", key);
        g_translated++; g_translate_ns += clock_gettime_nsec_np(CLOCK_MONOTONIC) - t0;
        if (g_translated % 100 == 0) VKLOG("shaders: %llu translated (%.1f ms each), %llu loaded, %llu from the seed", (unsigned long long)g_translated, g_translate_ns / 1e6 / g_translated, (unsigned long long)g_loaded, (unsigned long long)g_seed_used);
    }
    cand *nc = calloc(1, sizeof *nc); snprintf(nc->name, sizeof nc->name, "%s", key); nc->dir = g_dir;
    if (!read_deps(nc)) VKDIE("shader %s: no vk.txt", key);
    nc->s = s; *v->tail = nc; v->tail = &nc->next;
    if (!serves(nc, A)) VKLOG("【红】%s: its deps do not answer the state it was translated for", key);
    char path[4400]; snprintf(path, sizeof path, "%s/%.64s.variants", g_dir, key);
    FILE *f = fopen(path, "a"); if (!f || fprintf(f, "%s\n", key) < 0 || fclose(f)) VKDIE("append %s", path);
    pthread_mutex_unlock(&g_sh_mu);
    return s;
}
/* the decal variant (vk_fbread.c): the same bindings plus one input attachment per colour attachment it reads, kept on the
   shader by its (binding, attachment) pairs */
vshader *vk_shader_fbread(vshader *fs, const int bind[], const int att[], int k) {
    uint32_t key = 2166136261u; for (int i = 0; i < k; i++) key = (key ^ (uint32_t)(bind[i] << 4 | att[i])) * 16777619u;
    pthread_mutex_lock(&g_sh_mu);
    vshader *v = fs->fb_next; while (v && v->fb_key != key) v = v->fb_next;
    if (!v) {
        size_t n; uint32_t *spv = vk_fbread_spv(fs->spv, fs->nwords, bind, att, k, &n);
        v = calloc(1, sizeof *v); *v = *fs; v->fb_key = key; v->fb_none = !spv; v->fb_next = fs->fb_next; fs->fb_next = v;
        snprintf(v->key, sizeof v->key, "%.80s+fb%08x", fs->key, key);
        if (spv) {
            v->spv = spv; v->nwords = n; v->b = malloc((fs->nb + 8) * sizeof *v->b); memcpy(v->b, fs->b, fs->nb * sizeof *v->b);
            unsigned have = 0; for (int i = 0; i < k; i++) if (!(have >> att[i] & 1)) { have |= 1u << att[i]; v->b[v->nb++] = (vk_rbind){.kind = RB_IA, .set = 1, .binding = (uint16_t)(VK_FB_BINDING + att[i]), .slot = att[i], .array = 1}; }
            VkDescriptorSetLayoutBinding lb[136]; int nl = 0;
            for (int i = 0; i < v->nb && nl < 136; i++) lb[nl++] = (VkDescriptorSetLayoutBinding){v->b[i].binding, v->b[i].kind == RB_IA ? VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT : desc_type(&v->b[i]), (uint32_t)v->b[i].array,
                v->b[i].kind == RB_IA ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_ALL, NULL};
            VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = (uint32_t)nl, .pBindings = lb};
            VKCHECK(vkCreateDescriptorSetLayout(V.dev, &li, NULL, &v->layout));
            if (getenv("TOTK_VK_FB_DUMP")) { char p[256]; snprintf(p, sizeof p, "/tmp/%s.spv", v->key); FILE *f = fopen(p, "wb"); if (f) { fwrite(spv, 4, n, f); fclose(f); } }
        }
    }
    pthread_mutex_unlock(&g_sh_mu);
    return v->fb_none ? NULL : v;
}
void vk_shader_report(void) {   /* unlocked: statistics */
    double n = g_look ? (double)g_look : 1;
    VKLOG("shader variant lookups %llu: hits %llu (%.1f%%: loaded %llu, first from the seed %llu, first from the cache %llu), "
          "misses %llu (%.1f%%; %llu of shaders the seed has: on texture %llu, image %llu, vertex input %llu, colour output %llu, depth mode %llu); "
          "shaders met %llu, %llu of them in the seed",
          (unsigned long long)g_look, (unsigned long long)(g_look - g_miss), 100.0 * (g_look - g_miss) / n, (unsigned long long)g_hit_mem,
          (unsigned long long)g_hit_seed, (unsigned long long)g_hit_cache, (unsigned long long)g_miss, 100.0 * g_miss / n,
          (unsigned long long)g_miss_seeded, (unsigned long long)g_miss_kind[0], (unsigned long long)g_miss_kind[1], (unsigned long long)g_miss_kind[2],
          (unsigned long long)g_miss_kind[3], (unsigned long long)g_miss_kind[4], ({ unsigned long long n_ = 0; for (int i = 0; i < 4096; i++) for (shv *v = g_shv[i]; v; v = v->next) n_++; n_; }),
          (unsigned long long)g_seed_shaders);
}
void vk_shader_stats(uint64_t s[4]) { s[0] = g_loaded; s[1] = g_load_ns; s[2] = g_translated; s[3] = g_translate_ns; }   /* unlocked: statistics */
