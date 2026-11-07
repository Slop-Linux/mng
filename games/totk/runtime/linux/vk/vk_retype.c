/* Fragment outputs retyped to the bound render targets (nvn_mtl.m fn_retype_outputs on Metal). NVN's shader writes untyped
   32-bit registers and the colour unit takes their bits as integers for a pure integer format and as floats otherwise;
   Ryujinx types each output from the target it was told of (the fout answer) and writes the register bits through
   OpBitcast. A variant translated for one target type and the one for another differ only there (2026-11-07: the uint
   variant of a Great Plateau fragment shader against its float variant: the output variable's vec4 type, its component
   pointer type and the four OpBitcast result types). So the runtime rewrites those instead of translating.
   want[loc]: 'f' / 'u' / 's' for the output at that location, 0 to leave it. NULL (vk_retype_why) when an output to
   change is written other than by storing an OpBitcast used only there, or used otherwise. */
#include "vk.h"

enum { OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypePointer = 32, OpFunction = 54, OpVariable = 59, OpLoad = 61,
       OpStore = 62, OpAccessChain = 65, OpInBoundsAccessChain = 66, OpDecorate = 71, OpCopyObject = 83, OpBitcast = 124 };
enum { SC_OUTPUT = 3, DEC_LOCATION = 30, MORE = 64 };

const char *vk_retype_why;

typedef struct {
    uint32_t nb, *ptr_sc, *ptr_to, *vec_comp, *vec_n; char *cls;
    uint32_t add[MORE * 5]; size_t na;   /* declarations to insert before the first function */
} types_t;
static uint32_t scalar(types_t *T, char c) {
    for (uint32_t k = 1; k < T->nb; k++) if (T->cls[k] == c) return k;
    uint32_t id = T->nb++; T->cls[id] = c;
    if (c == 'f') { T->add[T->na++] = 3 << 16 | OpTypeFloat; T->add[T->na++] = id; T->add[T->na++] = 32; }
    else { T->add[T->na++] = 4 << 16 | OpTypeInt; T->add[T->na++] = id; T->add[T->na++] = 32; T->add[T->na++] = c == 's'; }
    return id;
}
static uint32_t vector(types_t *T, uint32_t s, uint32_t cn) {
    for (uint32_t k = 1; k < T->nb; k++) if (T->vec_comp[k] == s && T->vec_n[k] == cn) return k;
    uint32_t id = T->nb++; T->vec_comp[id] = s; T->vec_n[id] = cn;
    T->add[T->na++] = 4 << 16 | OpTypeVector; T->add[T->na++] = id; T->add[T->na++] = s; T->add[T->na++] = cn;
    return id;
}
static uint32_t out_ptr(types_t *T, uint32_t t) {
    for (uint32_t k = 1; k < T->nb; k++) if (T->ptr_sc[k] == SC_OUTPUT && T->ptr_to[k] == t) return k;
    uint32_t id = T->nb++; T->ptr_sc[id] = SC_OUTPUT; T->ptr_to[id] = t;
    T->add[T->na++] = 4 << 16 | OpTypePointer; T->add[T->na++] = id; T->add[T->na++] = SC_OUTPUT; T->add[T->na++] = t;
    return id;
}

uint32_t *vk_retype_spv(const uint32_t *w, size_t n, const char want[8], size_t *out_n) {
    if (n < 5 || w[0] != 0x07230203) { vk_retype_why = "not SPIR-V"; return NULL; }
    uint32_t bound = w[3], cap = bound + MORE;
    types_t T = {.nb = bound, .ptr_sc = calloc(cap, 4), .ptr_to = calloc(cap, 4), .vec_comp = calloc(cap, 4), .vec_n = calloc(cap, 4), .cls = calloc(cap, 1)};
    uint32_t *loc = calloc(bound, 4), *var_type = calloc(bound, 4), *load_type = calloc(bound, 4), *ac_var = calloc(bound, 4),
             *uses = calloc(bound, 4), *new_var_ptr = calloc(bound, 4), *new_ac_ptr = calloc(bound, 4), *new_val = calloc(bound, 4),
             *store_type = calloc(bound, 4), *def_pos = calloc(bound, 4);
    uint8_t *bitcast = calloc(bound, 1);
    uint32_t *res = NULL; size_t first_fn = 0, insert_at = SIZE_MAX; int changed = 0;
    vk_retype_why = NULL;
    for (uint32_t i = 0; i < bound; i++) loc[i] = UINT32_MAX;
    /* pass 1: types, output variables and their access chains, the types of loads, every id's occurrences in functions */
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; if (!len || i + len > n) { vk_retype_why = "malformed"; goto out; }
        const uint32_t *a = w + i + 1;
        if (op >= OpTypeInt && op <= OpTypePointer && a[0] < bound) def_pos[a[0]] = (uint32_t)i;
        switch (op) {
            case OpTypeInt: if (a[1] == 32) T.cls[a[0]] = a[2] ? 's' : 'u'; break;
            case OpTypeFloat: if (a[1] == 32) T.cls[a[0]] = 'f'; break;
            case OpTypeVector: T.vec_comp[a[0]] = a[1]; T.vec_n[a[0]] = a[2]; break;
            case OpTypePointer: T.ptr_sc[a[0]] = a[1]; T.ptr_to[a[0]] = a[2]; break;
            case OpDecorate: if (len >= 4 && a[1] == DEC_LOCATION && a[0] < bound) loc[a[0]] = a[2]; break;
            case OpVariable: if (a[2] == SC_OUTPUT) { var_type[a[1]] = a[0]; def_pos[a[1]] = (uint32_t)i; } break;
            case OpFunction: if (!first_fn) first_fn = i; break;
            case OpLoad: load_type[a[1]] = a[0]; break;
            case OpAccessChain: case OpInBoundsAccessChain: if (a[2] < bound && var_type[a[2]]) ac_var[a[1]] = a[2]; break;
            case OpBitcast: bitcast[a[1]] = 1; break;
        }
        if (first_fn) for (uint32_t k = 1; k < len; k++) if (w[i + k] < bound) uses[w[i + k]]++;
        i += len;
    }
    if (!first_fn) { vk_retype_why = "no function"; goto out; }
    /* the outputs to change and the types they take */
    for (uint32_t id = 1; id < bound; id++) if (var_type[id] && loc[id] < 8 && want[loc[id]]) {
        uint32_t pointee = T.ptr_to[var_type[id]], cn = T.vec_n[pointee], comp = cn ? T.vec_comp[pointee] : pointee;
        if (!T.cls[comp]) { vk_retype_why = "an output of another type"; goto out; }
        if (T.cls[comp] == want[loc[id]]) continue;
        uint32_t s = scalar(&T, want[loc[id]]), v = cn ? vector(&T, s, cn) : s;
        if (def_pos[id] < insert_at) insert_at = def_pos[id];   /* new declarations go before the first changed output */
        new_var_ptr[id] = out_ptr(&T, v); new_ac_ptr[id] = out_ptr(&T, s); new_val[id] = v; store_type[id] = s;
        changed = 1;
    }
    if (!changed) { vk_retype_why = "nothing to retype"; goto out; }
    /* the existing types the new declarations name are declared before that point */
    for (size_t j = 0; j < T.na;) {
        uint32_t op = T.add[j] & 0xffff, len = T.add[j] >> 16;
        uint32_t ref = op == OpTypeVector ? T.add[j + 2] : op == OpTypePointer ? T.add[j + 3] : 0;
        if (ref && ref < bound && def_pos[ref] >= insert_at) { vk_retype_why = "a type declared after the output"; goto out; }
        j += len;
    }
    /* every use of a changed output: an access chain of it, or a store into it (or its chain) of an OpBitcast used there only */
    for (size_t i = first_fn; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; const uint32_t *a = w + i + 1;
        for (uint32_t k = 1; k < len; k++) {
            uint32_t x = w[i + k]; if (x >= bound) continue;
            uint32_t var = new_var_ptr[x] ? x : ac_var[x] && new_var_ptr[ac_var[x]] ? ac_var[x] : 0;
            if (!var) continue;
            if ((op == OpAccessChain || op == OpInBoundsAccessChain) && (k == 2 || k == 3)) continue;   /* the chain itself */
            if (op != OpStore || k != 1) { vk_retype_why = "an output used other than by a store"; goto out; }
            uint32_t obj = a[1];
            if (obj >= bound || !bitcast[obj] || uses[obj] != 2) { vk_retype_why = "an output stored other than from a bitcast used once"; goto out; }
            new_val[obj] = x == var ? new_val[var] : store_type[var];
        }
        i += len;
    }
    /* pass 2: the module with the new declarations before the first function and the changed types */
    res = malloc((n + T.na) * 4); size_t o = 5;
    memcpy(res, w, 5 * 4); res[3] = T.nb;
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (i == insert_at) { memcpy(res + o, T.add, T.na * 4); o += T.na; }
        uint32_t *r = res + o; memcpy(r, w + i, len * 4); o += len;
        if (op == OpVariable && r[2] < bound && new_var_ptr[r[2]]) r[1] = new_var_ptr[r[2]];
        if ((op == OpAccessChain || op == OpInBoundsAccessChain) && r[3] < bound && new_var_ptr[r[3]]) r[1] = new_ac_ptr[r[3]];
        if (op == OpBitcast && new_val[r[2]] && !var_type[r[2]]) {
            r[1] = new_val[r[2]];
            if (load_type[r[3]] == r[1]) r[0] = 4 << 16 | OpCopyObject;   /* the register is of that class already */
        }
        i += len;
    }
    *out_n = o;
out:
    free(T.ptr_sc); free(T.ptr_to); free(T.vec_comp); free(T.vec_n); free(T.cls);
    free(loc); free(var_type); free(load_type); free(ac_var); free(uses); free(new_var_ptr); free(new_ac_ptr); free(new_val); free(store_type); free(bitcast); free(def_pos);
    if (vk_retype_why) { free(res); return NULL; }
    return res;
}
