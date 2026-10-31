/* Decals that sample the colour attachments of their own pass (nvn_fbread.h on Metal): the fragment SPIR-V is rewritten so a
   sample of such a texture returns the attachment's value through an input attachment (dynamic rendering local read)
   when the sampled texel is the fragment's own pixel, and the sample as before otherwise:
       r = all(floor(uv * size(t)) == floor(gl_FragCoord.xy)) ? subpassLoad(ia_j) : texture(t, uv, ...)
   Only plain 2D samples (OpImageSample{Implicit,Explicit}Lod with a vec2 coordinate, vec4 float result) of the texture are
   allowed; any other use of it leaves the shader alone (NULL). Input attachment j reads colour attachment j (the default
   mapping), descriptor set 1, binding VK_FB_BINDING + j. */
#include "vk.h"

enum { OpExtInstImport = 11, OpExtInst = 12, OpMemoryModel = 14, OpEntryPoint = 15, OpCapability = 17, OpTypeBool = 20, OpTypeInt = 21,
       OpTypeFloat = 22, OpTypeVector = 23, OpTypeImage = 25, OpTypeSampledImage = 27, OpTypePointer = 32, OpConstant = 43,
       OpConstantComposite = 44, OpFunction = 54, OpVariable = 59, OpLoad = 61, OpDecorate = 71, OpVectorShuffle = 79,
       OpImageSampleImplicitLod = 87, OpImageSampleExplicitLod = 88, OpImageRead = 98, OpImage = 100, OpImageQuerySizeLod = 103,
       OpConvertSToF = 111, OpFMul = 133, OpAll = 155, OpSelect = 169, OpFOrdEqual = 180 };

typedef struct { uint32_t *w; size_t n, cap; } buf_t;
static void put(buf_t *b, uint32_t v) { if (b->n == b->cap) { b->cap = b->cap ? 2 * b->cap : 4096; b->w = realloc(b->w, b->cap * 4); } b->w[b->n++] = v; }
static void ins(buf_t *b, uint32_t op, int n, const uint32_t *a) { put(b, (uint32_t)(n + 1) << 16 | op); for (int i = 0; i < n; i++) put(b, a[i]); }
#define INS(b, op, ...) do { const uint32_t a_[] = {__VA_ARGS__}; ins(b, op, (int)(sizeof a_ / 4), a_); } while (0)

const char *vk_fbread_why;   /* why the last shader was left alone */
static int is_decl(uint32_t op) { return op == 1 || (op >= 19 && op <= 39) || (op >= 41 && op <= 52) || op == OpVariable || op == 322 || op == 4456; }

uint32_t *vk_fbread_spv(const uint32_t *w, size_t n, const int bind[], const int att[], int k, size_t *out_n) {
    if (n < 5 || w[0] != 0x07230203 || k <= 0 || k > 8) return NULL;
    uint32_t bound = w[3], version = w[1];
    uint32_t *dset = calloc(bound, 4), *dbind = calloc(bound, 4), *type_of = calloc(bound, 4), *load_var = calloc(bound, 4);
    uint8_t *is_target = calloc(bound, 1); int *att_of = calloc(bound, sizeof(int));
    uint32_t t_float = 0, t_int = 0, t_bool = 0, t_v2f = 0, t_v4f = 0, t_v2i = 0, t_v2b = 0, t_v4b = 0, c_int0 = 0, glsl = 0, fragcoord = 0;
    int cap_ia = 0, cap_query = 0; uint32_t *vec_c = calloc(bound, 4), *vec_n = calloc(bound, 4), *img_of_si = calloc(bound, 4);
    uint32_t *img_dim = calloc(bound, 4), *img_arr = calloc(bound, 4), *builtin = calloc(bound, 4);
    for (uint32_t i = 0; i < bound; i++) builtin[i] = UINT32_MAX;
    uint32_t *res = NULL; int bad = 0;
    /* pass 1: what the module declares */
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; if (!len || i + len > n) { bad = 1; break; }
        const uint32_t *a = w + i + 1;
        switch (op) {
            case OpCapability: if (a[0] == 40) cap_ia = 1; if (a[0] == 50) cap_query = 1; break;
            case OpExtInstImport: if (!strcmp((const char *)(a + 1), "GLSL.std.450")) glsl = a[0]; break;
            case OpDecorate: if (a[0] < bound) { if (a[1] == 34) dset[a[0]] = a[2] + 1; if (a[1] == 33) dbind[a[0]] = a[2] + 1; if (a[1] == 11) builtin[a[0]] = a[2]; } break;
            case OpTypeFloat: if (a[1] == 32) t_float = a[0]; break;
            case OpTypeInt: if (a[1] == 32 && a[2] == 1) t_int = a[0]; break;
            case OpTypeBool: t_bool = a[0]; break;
            case OpTypeVector: if (a[0] < bound) { vec_c[a[0]] = a[1]; vec_n[a[0]] = a[2]; } break;
            case OpTypeImage: if (a[0] < bound) { img_dim[a[0]] = a[2] + 1; img_arr[a[0]] = a[4]; } break;
            case OpTypeSampledImage: if (a[0] < bound) img_of_si[a[0]] = a[1]; break;
            case OpConstant: if (len == 4 && a[2] == 0 && a[0] == t_int && t_int) c_int0 = a[1]; break;
            case OpVariable: if (a[1] < bound) type_of[a[1]] = a[0]; break;
            case OpLoad: if (a[1] < bound) { type_of[a[1]] = a[0]; load_var[a[1]] = a[2]; } break;
            default: if (len >= 3 && !is_decl(op) && op != OpEntryPoint && a[1] < bound && op != OpDecorate) type_of[a[1]] = a[0]; break;
        }
        i += len;
    }
    for (uint32_t v = 0; v < bound && !bad; v++) if (builtin[v] == 15) fragcoord = v;   /* BuiltIn FragCoord */
    for (uint32_t t = 0; t < bound; t++) if (vec_c[t]) {
        if (vec_c[t] == t_float && vec_n[t] == 2) t_v2f = t; if (vec_c[t] == t_float && vec_n[t] == 4) t_v4f = t;
        if (t_int && vec_c[t] == t_int && vec_n[t] == 2) t_v2i = t; if (t_bool && vec_c[t] == t_bool && vec_n[t] == 2) t_v2b = t; if (t_bool && vec_c[t] == t_bool && vec_n[t] == 4) t_v4b = t; }
    for (uint32_t v = 0; v < bound; v++) for (int j = 0; j < k; j++) if (dset[v] == 2 && dbind[v] == (uint32_t)bind[j] + 1) { is_target[v] = 1; att_of[v] = att[j]; }
    if (bad || !t_float || !t_v4f) { vk_fbread_why = "no float vec4 type"; goto out; }
    /* every use of a target's loads must be the sampled image of a plain 2D sample with a vec2 coordinate: the operands of
       the function bodies that are ids (result ids and the literal operands of the common instructions skipped) */
    int body = 0;
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; const uint32_t *a = w + i + 1;
        if (op == OpFunction) body = 1;
        for (uint32_t q = 1; q < len && body; q++) {
            uint32_t id = a[q - 1]; if (id >= bound) continue;
            if (q == 2 && op != 246 && op != 247 && op != 249 && op != 250 && op != 251 && op != 62 && op != 99) continue;   /* a result id (not OpLoopMerge, OpSelectionMerge, OpBranch(Conditional), OpSwitch, OpStore, OpImageWrite) */
            if (op == OpLoad && q == 3 && is_target[id]) continue;   /* the load itself */
            if ((op == OpVectorShuffle && q >= 5) || (op == 81 && q >= 4) || (op == 82 && q >= 5) || (op == OpExtInst && q == 4) || (op == 246 && q >= 3) || (op == 247 && q == 2) ||
                ((op >= 87 && op <= 97) && q == 5) || (op == 251 && q >= 3)) continue;   /* literals: components, indices, instruction, controls, image operand mask, switch values */
            int uses = (load_var[id] && is_target[load_var[id]]) || (is_target[id] && op != OpVariable);
            if (!uses) continue;
            uint32_t si = type_of[id], img = si < bound ? img_of_si[si] : 0;
            if (!((op == OpImageSampleImplicitLod || op == OpImageSampleExplicitLod) && q == 3 && a[0] == t_v4f && a[3] < bound && type_of[a[3]] == t_v2f && t_v2f &&
                  img && img_dim[img] == 2 && !img_arr[img])) { static char why[64]; snprintf(why, sizeof why, "use by opcode %u operand %u", op, q); vk_fbread_why = why; goto out; }
        }
        i += len;
    }
    /* pass 2: the module with the input attachments */
    uint32_t nb = bound;
#define NEW() (nb++)
    uint32_t n_int = t_int ? t_int : NEW(), n_bool = t_bool ? t_bool : NEW(), n_v2f = t_v2f ? t_v2f : NEW(), n_v2i = t_v2i ? t_v2i : NEW(), n_v2b = t_v2b ? t_v2b : NEW(), n_v4b = t_v4b ? t_v4b : NEW();
    uint32_t n_c0 = c_int0 ? c_int0 : NEW(), n_cv0 = NEW(), n_glsl = glsl ? glsl : NEW(), t_sub = NEW(), t_psub = NEW(), ia[8] = {0};
    for (int j = 0; j < k; j++) { int dup = 0; for (int q = 0; q < j; q++) dup |= att[q] == att[j]; if (!dup) ia[att[j]] = NEW(); }
    uint32_t t_pin = 0, n_frag = fragcoord; if (!fragcoord) { t_pin = NEW(); n_frag = NEW(); }
    buf_t o = {0};
    for (int i = 0; i < 5; i++) put(&o, w[i]);
    int caps_done = 0, glsl_done = !!glsl, decor_done = 0, types_done = 0;
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; const uint32_t *a = w + i + 1;
        if (!caps_done && op != OpCapability) { if (!cap_ia) INS(&o, OpCapability, 40); if (!cap_query) INS(&o, OpCapability, 50); caps_done = 1; }
        if (!glsl_done && op == OpMemoryModel) { put(&o, 6u << 16 | OpExtInstImport); put(&o, n_glsl); const char s[16] = "GLSL.std.450"; uint32_t sw[4]; memcpy(sw, s, 16); for (int q = 0; q < 4; q++) put(&o, sw[q]); glsl_done = 1; }
        if (!decor_done && is_decl(op)) {
            for (int j = 0; j < 8; j++) if (ia[j]) { INS(&o, OpDecorate, ia[j], 34, 1); INS(&o, OpDecorate, ia[j], 33, VK_FB_BINDING + (uint32_t)j); INS(&o, OpDecorate, ia[j], 43, (uint32_t)j); }
            if (!fragcoord) INS(&o, OpDecorate, n_frag, 11, 15);
            decor_done = 1;
        }
        if (!types_done && op == OpFunction) {
            if (!t_int) INS(&o, OpTypeInt, n_int, 32, 1);
            if (!t_bool) INS(&o, OpTypeBool, n_bool);
            if (!t_v2f) INS(&o, OpTypeVector, n_v2f, t_float, 2);
            if (!t_v2i) INS(&o, OpTypeVector, n_v2i, n_int, 2);
            if (!t_v2b) INS(&o, OpTypeVector, n_v2b, n_bool, 2);
            if (!t_v4b) INS(&o, OpTypeVector, n_v4b, n_bool, 4);
            if (!c_int0) INS(&o, OpConstant, n_int, n_c0, 0);
            INS(&o, OpConstantComposite, n_v2i, n_cv0, n_c0, n_c0);
            INS(&o, OpTypeImage, t_sub, t_float, 6, 0, 0, 0, 2, 0);
            INS(&o, OpTypePointer, t_psub, 0, t_sub);
            for (int j = 0; j < 8; j++) if (ia[j]) INS(&o, OpVariable, t_psub, ia[j], 0);
            if (!fragcoord) { INS(&o, OpTypePointer, t_pin, 1, t_v4f); INS(&o, OpVariable, t_pin, n_frag, 1); }
            types_done = 1;
        }
        if (op == OpEntryPoint) {   /* the new FragCoord input (and, from SPIR-V 1.4, the input attachments) join the interface */
            int extra = (!fragcoord) + (version >= 0x10400 ? (ia[0] != 0) + (ia[1] != 0) + (ia[2] != 0) + (ia[3] != 0) + (ia[4] != 0) + (ia[5] != 0) + (ia[6] != 0) + (ia[7] != 0) : 0);
            put(&o, (len + extra) << 16 | op); for (uint32_t q = 0; q + 1 < len; q++) put(&o, a[q]);
            if (!fragcoord) put(&o, n_frag);
            if (version >= 0x10400) for (int j = 0; j < 8; j++) if (ia[j]) put(&o, ia[j]);
            i += len; continue;
        }
        if ((op == OpImageSampleImplicitLod || op == OpImageSampleExplicitLod) && a[2] < bound && load_var[a[2]] && is_target[load_var[a[2]]]) {
            uint32_t r = a[1], si = a[2], uv = a[3], s = NEW(), img_t = img_of_si[type_of[si]], j = (uint32_t)att_of[load_var[si]];
            put(&o, w[i]); put(&o, a[0]); put(&o, s); for (uint32_t q = 2; q + 1 < len; q++) put(&o, a[q]);
            uint32_t l = NEW(), fb = NEW(), im = NEW(), sz = NEW(), szf = NEW(), t = NEW(), fl = NEW(), fc = NEW(), xy = NEW(), fxy = NEW(), eq = NEW(), all = NEW();
            INS(&o, OpLoad, t_sub, l, ia[j]);
            INS(&o, OpImageRead, t_v4f, fb, l, n_cv0);
            INS(&o, OpImage, img_t, im, si);
            INS(&o, OpImageQuerySizeLod, n_v2i, sz, im, n_c0);
            INS(&o, OpConvertSToF, n_v2f, szf, sz);
            INS(&o, OpFMul, n_v2f, t, uv, szf);
            INS(&o, OpExtInst, n_v2f, fl, n_glsl, 8, t);
            INS(&o, OpLoad, t_v4f, fc, n_frag);
            INS(&o, OpVectorShuffle, n_v2f, xy, fc, fc, 0, 1);
            INS(&o, OpExtInst, n_v2f, fxy, n_glsl, 8, xy);
            INS(&o, OpFOrdEqual, n_v2b, eq, fl, fxy);
            INS(&o, OpAll, n_bool, all, eq);
            uint32_t c4 = NEW(); INS(&o, 80 /* OpCompositeConstruct */, n_v4b, c4, all, all, all, all);   /* SPIR-V before 1.4: a vector condition */
            INS(&o, OpSelect, t_v4f, r, c4, fb, s);
            i += len; continue;
        }
        for (uint32_t q = 0; q < len; q++) put(&o, w[i + q]);
        i += len;
    }
#undef NEW
    o.w[3] = nb; *out_n = o.n; res = o.w;
out:
    free(dset); free(dbind); free(type_of); free(load_var); free(is_target); free(att_of); free(vec_c); free(vec_n); free(img_of_si); free(img_dim); free(img_arr); free(builtin);
    return res;
}
