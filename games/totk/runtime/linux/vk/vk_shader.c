/* Shader variants: Maxwell code -> totk_xlate --service --target vulkan (Ryujinx's translator) -> SPIR-V and its resource
   list (<variant>.spv, <variant>.vk.txt) in the shader cache, then a descriptor set layout per variant. A variant is the
   shader (SHA-256 of its control section and code, as nvn_mtl.m shader_key_of) plus the translator's answers from the
   bound state (texture types, image formats, vertex input and colour output classes). */
#include "vk.h"
#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int totk_depth_near_minus_w(void);   /* hle_nvn.c: nvnDeviceSetDepthMode(NEAR_IS_MINUS_W) */
#define CHAIN "botw-vk-v1"

static FILE *g_in, *g_out; static char g_dir[4096];
static pthread_mutex_t g_sh_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_translated, g_translate_ns, g_loaded, g_load_ns;   /* g_load_ns: variants met in frames, read from the cache (files, set layout, c1 buffer) */

static void service_start(void) {
    if (g_in) return;
    const char *base = getenv("TOTK_SHADER_CACHE"); if (!base || !*base) base = "build/shader-cache";
    snprintf(g_dir, sizeof g_dir, "%s/%s", base, CHAIN); mkdir(base, 0755); mkdir(g_dir, 0755);
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

typedef struct sh_ent { char key[96]; vshader *s; struct sh_ent *next; } sh_ent;
static sh_ent *g_tab[4096];

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
static vshader *load(const char *variant, int stage, const uint8_t *c1, size_t c1_len, const uint32_t *local) {
    char path[4400]; uint8_t *spv; size_t n;
    snprintf(path, sizeof path, "%s/%s.spv", g_dir, variant);
    if (!read_file(path, &spv, &n) || n < 20 || n % 4) return NULL;
    snprintf(path, sizeof path, "%s/%s.vk.txt", g_dir, variant);
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
vshader *vk_shader_get(const uint8_t *control, const uint8_t *code, int stage, const char *answers) {
    uint32_t c1o, c1t, klen; memcpy(&c1o, control + 0x700, 4); memcpy(&c1t, control + 0x704, 4); memcpy(&klen, control + 0x20, 4);
    if (klen < 0x7cc || klen >= 0x880) klen = 0x880;
    if (!c1t || c1t > 16u << 20 || c1o > c1t) VKDIE("shader code area %#x / c1 %#x", c1t, c1o);
    uint8_t md[32]; CC_SHA256_CTX c; CC_SHA256_Init(&c); CC_SHA256_Update(&c, control, klen); CC_SHA256_Update(&c, code, c1t); CC_SHA256_Final(md, &c);
    uint8_t ma[32]; CC_SHA256((const uint8_t *)answers, (unsigned)strlen(answers), ma);
    char key[96]; hex(md, 32, key); key[64] = '-'; hex(ma, 6, key + 65);
    unsigned h = 0; for (const char *p = key; *p; p++) h = h * 131 + (uint8_t)*p; h &= 4095;
    pthread_mutex_lock(&g_sh_mu);
    for (sh_ent *e = g_tab[h]; e; e = e->next) if (!strcmp(e->key, key)) { pthread_mutex_unlock(&g_sh_mu); return e->s; }
    service_start();
    uint32_t local[3] = {0, 0, 0}; if (stage == 5) memcpy(local, control + 0x748, 12);
    uint64_t l0_ = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    vshader *s = load(key, stage, code + c1o, c1t - c1o, stage == 5 ? local : NULL);
    if (s) g_load_ns += clock_gettime_nsec_np(CLOCK_MONOTONIC) - l0_;
    if (!s) {
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
        s = load(key, stage, code + c1o, c1t - c1o, stage == 5 ? local : NULL);
        if (!s) VKDIE("shader %s: translation produced no SPIR-V", key);
        g_translated++; g_translate_ns += clock_gettime_nsec_np(CLOCK_MONOTONIC) - t0;
        if (g_translated % 100 == 0) VKLOG("shaders: %llu translated (%.1f ms each), %llu loaded", (unsigned long long)g_translated, g_translate_ns / 1e6 / g_translated, (unsigned long long)g_loaded);
    }
    sh_ent *e = calloc(1, sizeof *e); snprintf(e->key, sizeof e->key, "%s", key); e->s = s; e->next = g_tab[h]; g_tab[h] = e;
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

/* ---------- shader-object warm-up (nvn_mtl.m warm_start, its pipeline archive): the variant pairs (vertex, fragment) and
   compute variants met in earlier runs are listed in <cache>/objects.txt; at start a thread creates their shader objects, so
   the ICD compiles them before the first draw needs them (in a frame that met ~150 new pairs, 300-460 objects, the game
   waited 100-400 ms on the GPU while the submit thread waited for the compiles). A pair met in a frame takes the ready
   objects; a new one is appended. TOTK_VK_WARM=0: off ---------- */
typedef struct warm_ent { char vs[96], fs[96]; VkShaderEXT sh[2]; int ready; struct warm_ent *next; } warm_ent;
static warm_ent *g_warm[4096]; static pthread_mutex_t g_warm_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_warm_cv = PTHREAD_COND_INITIALIZER;
static FILE *g_warm_list; static int g_warm_on = -1, g_warm_running; static uint64_t g_warm_made, g_warm_taken, g_warm_noted;
static char g_warm_dir[4096];
static unsigned warm_hash(const char *a, const char *b) { unsigned h = 0; for (; *a; a++) h = h * 131 + (uint8_t)*a; for (; *b; b++) h = h * 131 + (uint8_t)*b; return h & 4095; }
static warm_ent *warm_find(const char *vs, const char *fs) {   /* g_warm_mu held */
    for (warm_ent *e = g_warm[warm_hash(vs, fs)]; e; e = e->next) if (!strcmp(e->vs, vs) && !strcmp(e->fs, fs)) return e;
    return NULL;
}
/* the set layout load() makes for a variant, from its resource list alone */
static VkDescriptorSetLayout warm_layout(const char *key, int stage) {
    char path[4400]; snprintf(path, sizeof path, "%s/%s.vk.txt", g_warm_dir, key);
    FILE *f = fopen(path, "r"); if (!f) return VK_NULL_HANDLE;
    VkDescriptorSetLayoutBinding lb[128]; int nl = 0, want = stage == 1 ? 1 : 0; char line[256];
    if (want == 0) lb[nl++] = (VkDescriptorSetLayoutBinding){0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, NULL};
    while (fgets(line, sizeof line, f) && nl < 128) {
        vk_rbind b = {0}; int set, binding;
        if (!strncmp(line, "cbuf ", 5) && sscanf(line + 5, "%d %d %d", &set, &binding, &b.slot) == 3) b.kind = RB_CBUF;
        else if (!strncmp(line, "sbuf ", 5) && sscanf(line + 5, "%d %d %d %d %d %d", &set, &binding, &b.slot, &b.cb_slot, &b.cb_off, &b.flags) == 6) b.kind = RB_SBUF;
        else if ((!strncmp(line, "tex ", 4) || !strncmp(line, "img ", 4)) &&
                 sscanf(line + 4, "%d %d %d %d %d %d %d %d %d", &set, &binding, &b.stype, &b.cbuf_slot, &b.handle, &b.array, &b.separate, &b.flags, &b.format) == 9) b.kind = line[0] == 't' ? RB_TEX : RB_IMG;
        else continue;
        if (set != want) continue;
        lb[nl++] = (VkDescriptorSetLayoutBinding){(uint32_t)binding, desc_type(&b), (uint32_t)(b.array < 1 ? 1 : b.array), VK_SHADER_STAGE_ALL, NULL};
    }
    fclose(f);
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = (uint32_t)nl, .pBindings = lb};
    VkDescriptorSetLayout l; return vkCreateDescriptorSetLayout(V.dev, &li, NULL, &l) == VK_SUCCESS ? l : VK_NULL_HANDLE;
}
static VkShaderEXT warm_object(const char *key, VkShaderStageFlagBits stage, VkShaderStageFlags next, VkDescriptorSetLayout *l, int nl) {
    char path[4400]; uint8_t *spv; size_t n; snprintf(path, sizeof path, "%s/%s.spv", g_warm_dir, key);
    if (!read_file(path, &spv, &n) || n < 20 || n % 4) return VK_NULL_HANDLE;
    VkShaderCreateInfoEXT ci = {.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT, .stage = stage, .nextStage = next, .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
        .codeSize = n, .pCode = spv, .pName = "main", .setLayoutCount = (uint32_t)nl, .pSetLayouts = l};
    VkShaderEXT o; VkResult r = V.CreateShadersEXT(V.dev, 1, &ci, NULL, &o); free(spv);
    return r == VK_SUCCESS ? o : VK_NULL_HANDLE;
}
static void *warm_thread(void *arg) {
    (void)arg; pthread_setname_np("vk-warm");
    char path[4400]; snprintf(path, sizeof path, "%s/objects.txt", g_warm_dir);
    FILE *f = fopen(path, "r"); uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC), n = 0;
    char line[256];
    while (f && fgets(line, sizeof line, f)) {
        char vs[96], fs[96]; if (sscanf(line, "%95s %95s", vs, fs) != 2) continue;
        pthread_mutex_lock(&g_warm_mu); int have = warm_find(vs, fs) != NULL; pthread_mutex_unlock(&g_warm_mu); if (have) continue;
        warm_ent *e = calloc(1, sizeof *e); snprintf(e->vs, sizeof e->vs, "%s", vs); snprintf(e->fs, sizeof e->fs, "%s", fs);
        if (!strcmp(fs, "compute")) { VkDescriptorSetLayout l = warm_layout(vs, 5); if (l) e->sh[0] = warm_object(vs, VK_SHADER_STAGE_COMPUTE_BIT, 0, &l, 1); }
        else { int hf = strcmp(fs, "-") != 0; VkDescriptorSetLayout l[2] = {warm_layout(vs, 0), hf ? warm_layout(fs, 1) : VK_NULL_HANDLE};
               if (l[0] && (!hf || l[1])) { if (!hf) { VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; vkCreateDescriptorSetLayout(V.dev, &li, NULL, &l[1]); }
                   e->sh[0] = warm_object(vs, VK_SHADER_STAGE_VERTEX_BIT, hf ? VK_SHADER_STAGE_FRAGMENT_BIT : 0, l, 2);
                   if (hf) e->sh[1] = warm_object(fs, VK_SHADER_STAGE_FRAGMENT_BIT, 0, l, 2); } }
        e->ready = e->sh[0] && (!strcmp(fs, "-") || !strcmp(fs, "compute") || e->sh[1]);
        pthread_mutex_lock(&g_warm_mu); unsigned h = warm_hash(vs, fs); e->next = g_warm[h]; g_warm[h] = e; if (e->ready) g_warm_made++; pthread_mutex_unlock(&g_warm_mu);
        n++;
    }
    if (f) fclose(f);
    pthread_mutex_lock(&g_warm_mu); g_warm_running = 0; pthread_cond_broadcast(&g_warm_cv); pthread_mutex_unlock(&g_warm_mu);
    VKLOG("shader-object warm-up: %llu listed variants, %llu ready, %.1f s", (unsigned long long)n, (unsigned long long)g_warm_made, (clock_gettime_nsec_np(CLOCK_MONOTONIC) - t0) / 1e9);
    return NULL;
}
void vk_warm_start(void) {
    if (g_warm_on >= 0) return;
    const char *e = getenv("TOTK_VK_WARM"); g_warm_on = !(e && !strcmp(e, "0"));
    const char *base = getenv("TOTK_SHADER_CACHE"); if (!base || !*base) base = "build/shader-cache";
    snprintf(g_warm_dir, sizeof g_warm_dir, "%s/%s", base, CHAIN); mkdir(base, 0755); mkdir(g_warm_dir, 0755);
    char path[4400]; snprintf(path, sizeof path, "%s/objects.txt", g_warm_dir);
    g_warm_list = fopen(path, "a");
    if (!g_warm_on) return;
    g_warm_running = 1; pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 4u << 20);
    if (pthread_create(&t, &a, warm_thread, NULL)) { g_warm_running = 0; return; }
    pthread_detach(t);
}
/* a pair the replay needs: the warm objects when ready (taken once), else 0; a pair not listed yet is appended */
int vk_warm_take(const char *vs, const char *fs, VkShaderEXT out[2]) {
    if (strchr(vs, '+') || strchr(fs, '+')) return 0;   /* decal variants are made at run time (vk_shader_fbread) */
    pthread_mutex_lock(&g_warm_mu);
    warm_ent *e = warm_find(vs, fs);
    if (!e) {   /* not listed (or not reached by the warm-up yet): listed for the next run */
        e = calloc(1, sizeof *e); snprintf(e->vs, sizeof e->vs, "%s", vs); snprintf(e->fs, sizeof e->fs, "%s", fs);
        unsigned h = warm_hash(vs, fs); e->next = g_warm[h]; g_warm[h] = e;
        if (g_warm_list) { fprintf(g_warm_list, "%s %s\n", vs, fs); fflush(g_warm_list); g_warm_noted++; }
        pthread_mutex_unlock(&g_warm_mu); return 0;
    }
    int ok = e->ready && e->sh[0]; if (ok) { out[0] = e->sh[0]; out[1] = e->sh[1]; e->ready = 0; e->sh[0] = e->sh[1] = VK_NULL_HANDLE; g_warm_taken++; }
    pthread_mutex_unlock(&g_warm_mu); return ok;
}
/* mtl_warm_wait: hle_nvn.c holds the first world frame until the warm-up is done (timeout_ms < 0: as long as it takes) */
int vk_warm_wait(int timeout_ms) {
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    pthread_mutex_lock(&g_warm_mu);
    while (g_warm_running) {
        if (timeout_ms >= 0) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 50000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&g_warm_cv, &g_warm_mu, &ts); if ((clock_gettime_nsec_np(CLOCK_MONOTONIC) - t0) / 1000000 >= (uint64_t)timeout_ms) break; }
        else pthread_cond_wait(&g_warm_cv, &g_warm_mu);
    }
    pthread_mutex_unlock(&g_warm_mu);
    return (int)((clock_gettime_nsec_np(CLOCK_MONOTONIC) - t0) / 1000000);
}
void vk_warm_stats(uint64_t s[3]) { pthread_mutex_lock(&g_warm_mu); s[0] = g_warm_made; s[1] = g_warm_taken; s[2] = g_warm_noted; pthread_mutex_unlock(&g_warm_mu); }
void vk_shader_stats(uint64_t s[4]) { s[0] = g_loaded; s[1] = g_load_ns; s[2] = g_translated; s[3] = g_translate_ns; }   /* unlocked: statistics */
