/* Shader interfaces retyped to what is bound, as nvn_mtl.m does on Metal: fragment outputs to the render targets
   (fn_retype_outputs, vk_retype_spv) and vertex inputs to the vertex formats (vs_vin, vk_retype_inputs_spv).

   Fragment outputs (fn_retype_outputs). NVN's shader writes untyped
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
enum { SC_INPUT = 1, SC_OUTPUT = 3, DEC_LOCATION = 30, MORE = 64 };

const char *vk_retype_why;

/* whether word k of a function body instruction is a literal, not an id (2026-11-07: GLSL.std.450 Fma, instruction 50,
   read as a use of the input variable %50) */
static int literal(uint32_t op, uint32_t k) {
    switch (op) {
        case 8: return k >= 2;                     /* OpLine */
        case 12: return k == 4;                    /* OpExtInst: the instruction number */
        case 61: return k >= 4;                    /* OpLoad: memory operands */
        case 62: return k >= 3;                    /* OpStore: memory operands */
        case 79: return k >= 5;                    /* OpVectorShuffle: components */
        case 81: return k >= 4;                    /* OpCompositeExtract: indices */
        case 82: return k >= 5;                    /* OpCompositeInsert: indices */
        case 246: return k >= 3;                   /* OpLoopMerge: control */
        case 247: return k == 2;                   /* OpSelectionMerge: control */
        case 250: return k >= 4;                   /* OpBranchConditional: weights */
        case 251: return k >= 3 && (k - 3) % 2 == 0;   /* OpSwitch: case values */
        default: return 0;
    }
}

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
static uint32_t ptr_of(types_t *T, uint32_t sc, uint32_t t) {
    for (uint32_t k = 1; k < T->nb; k++) if (T->ptr_sc[k] == sc && T->ptr_to[k] == t) return k;
    uint32_t id = T->nb++; T->ptr_sc[id] = sc; T->ptr_to[id] = t;
    T->add[T->na++] = 4 << 16 | OpTypePointer; T->add[T->na++] = id; T->add[T->na++] = sc; T->add[T->na++] = t;
    return id;
}
static uint32_t out_ptr(types_t *T, uint32_t t) { return ptr_of(T, SC_OUTPUT, t); }
/* the new declarations name only types declared before the insertion point */
static int decls_before(const types_t *T, const uint32_t *def_pos, uint32_t bound, size_t at) {
    for (size_t j = 0; j < T->na;) {
        uint32_t op = T->add[j] & 0xffff, len = T->add[j] >> 16;
        uint32_t ref = op == OpTypeVector ? T->add[j + 2] : op == OpTypePointer ? T->add[j + 3] : 0;
        if (ref && ref < bound && def_pos[ref] >= at) return 0;
        j += len;
    }
    return 1;
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
        if (first_fn) for (uint32_t k = 1; k < len; k++) if (w[i + k] < bound && !literal(op, k)) uses[w[i + k]]++;
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
    if (!decls_before(&T, def_pos, bound, insert_at)) { vk_retype_why = "a type declared after the output"; goto out; }
    /* every use of a changed output: an access chain of it, or a store into it (or its chain) of an OpBitcast used there only */
    for (size_t i = first_fn; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; const uint32_t *a = w + i + 1;
        for (uint32_t k = 1; k < len; k++) {
            uint32_t x = w[i + k]; if (x >= bound || literal(op, k)) continue;
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

/* Vertex inputs (vs_vin). The bound vertex format decides whether the attribute's bits arrive as integers or floats;
   Ryujinx declares the input with the class it was told of (the attr answer) and, for an integer one, loads it as such
   and bitcasts it to float at once (2026-11-07: the attr 6 uint variant of a Great Plateau vertex shader against its float
   variant: the input's vec4 type, its component pointer type, and an OpBitcast after each of its loads). So each load
   of a changed input becomes a load of the new class into a fresh id and an OpBitcast back to the type and id the load
   had, and the code after it stays as it is. want[loc]: 'f' / 'u' / 's', 0 to leave. NULL (vk_retype_why) when a changed
   input is used other than through access chains and loads. */
uint32_t *vk_retype_inputs_spv(const uint32_t *w, size_t n, const char want[16], size_t *out_n) {
    if (n < 5 || w[0] != 0x07230203) { vk_retype_why = "not SPIR-V"; return NULL; }
    uint32_t bound = w[3], cap = bound + MORE;
    types_t T = {.nb = bound, .ptr_sc = calloc(cap, 4), .ptr_to = calloc(cap, 4), .vec_comp = calloc(cap, 4), .vec_n = calloc(cap, 4), .cls = calloc(cap, 1)};
    uint32_t *loc = calloc(bound, 4), *var_type = calloc(bound, 4), *ac_var = calloc(bound, 4), *def_pos = calloc(bound, 4),
             *new_var_ptr = calloc(bound, 4), *new_ac_ptr = calloc(bound, 4), *new_val = calloc(bound, 4), *new_comp = calloc(bound, 4);
    uint32_t *res = NULL; size_t first_fn = 0, insert_at = SIZE_MAX, loads = 0; int changed = 0;
    vk_retype_why = NULL;
    for (uint32_t i = 0; i < bound; i++) loc[i] = UINT32_MAX;
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
            case OpVariable: if (a[2] == SC_INPUT) { var_type[a[1]] = a[0]; def_pos[a[1]] = (uint32_t)i; } break;
            case OpFunction: if (!first_fn) first_fn = i; break;
            case OpAccessChain: case OpInBoundsAccessChain: if (a[2] < bound && var_type[a[2]]) ac_var[a[1]] = a[2]; break;
        }
        i += len;
    }
    if (!first_fn) { vk_retype_why = "no function"; goto out; }
    for (uint32_t id = 1; id < bound; id++) if (var_type[id] && loc[id] < 16 && want[loc[id]]) {
        uint32_t pointee = T.ptr_to[var_type[id]], cn = T.vec_n[pointee], comp = cn ? T.vec_comp[pointee] : pointee;
        if (!T.cls[comp]) { vk_retype_why = "an input of another type"; goto out; }
        if (T.cls[comp] == want[loc[id]]) continue;
        uint32_t s = scalar(&T, want[loc[id]]), v = cn ? vector(&T, s, cn) : s;
        new_var_ptr[id] = ptr_of(&T, SC_INPUT, v); new_ac_ptr[id] = ptr_of(&T, SC_INPUT, s); new_val[id] = v; new_comp[id] = s;
        if (def_pos[id] < insert_at) insert_at = def_pos[id];
        changed = 1;
    }
    if (!changed) { vk_retype_why = "nothing to retype"; goto out; }
    if (!decls_before(&T, def_pos, bound, insert_at)) { vk_retype_why = "a type declared after the input"; goto out; }
    /* a changed input (or a chain of it) is only the base of an access chain or the pointer of a load */
    for (size_t i = first_fn; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        for (uint32_t k = 1; k < len; k++) {
            uint32_t x = w[i + k]; if (x >= bound || literal(op, k)) continue;
            uint32_t var = new_var_ptr[x] ? x : ac_var[x] && new_var_ptr[ac_var[x]] ? ac_var[x] : 0;
            if (!var) continue;
            if ((op == OpAccessChain || op == OpInBoundsAccessChain) && (k == 2 || k == 3)) continue;
            if (op == OpLoad && k == 3) { loads++; continue; }
            vk_retype_why = "an input used other than by a load"; goto out;
        }
        i += len;
    }
    /* pass 2: declarations before the first changed input, the changed types, each load split into load + bitcast */
    res = malloc((n + T.na + loads * 4) * 4); size_t o = 5;
    memcpy(res, w, 5 * 4);
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (i == insert_at) { memcpy(res + o, T.add, T.na * 4); o += T.na; }
        uint32_t *r = res + o; memcpy(r, w + i, len * 4); o += len;
        if (op == OpVariable && r[2] < bound && new_var_ptr[r[2]]) r[1] = new_var_ptr[r[2]];
        if ((op == OpAccessChain || op == OpInBoundsAccessChain) && r[3] < bound && new_var_ptr[r[3]]) r[1] = new_ac_ptr[r[3]];
        if (op == OpLoad && r[3] < bound) {
            uint32_t p = r[3], var = new_var_ptr[p] ? p : ac_var[p] && new_var_ptr[ac_var[p]] ? ac_var[p] : 0;
            if (var) {
                uint32_t had_type = r[1], had_id = r[2], fresh = T.nb++;
                r[1] = p == var ? new_val[var] : new_comp[var]; r[2] = fresh;
                res[o++] = 4 << 16 | OpBitcast; res[o++] = had_type; res[o++] = had_id; res[o++] = fresh;
            }
        }
        i += len;
    }
    res[3] = T.nb;
    *out_n = o;
out:
    free(T.ptr_sc); free(T.ptr_to); free(T.vec_comp); free(T.vec_n); free(T.cls);
    free(loc); free(var_type); free(ac_var); free(def_pos); free(new_var_ptr); free(new_ac_ptr); free(new_val); free(new_comp);
    if (vk_retype_why) { free(res); return NULL; }
    return res;
}
