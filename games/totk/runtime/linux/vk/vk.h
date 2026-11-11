/* Linux Vulkan backend of the NVN layer (the nvn_mtl.h interface). Files: vk_dev.c (device, window, memory, submissions,
   present), vk_tex.c (formats, textures, samplers), vk_shader.c (translation service, shader variants), vk_replay.c (the
   command stream). */
#pragma once
#include "d2r_gfx/gfx/nvn/nvn_obj.h"
#include "d2r_gfx/gfx/nvn/nvn_mtl.h"
#include "d2r_gfx/host/dm.h"
#include <vulkan/vulkan.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VKDIE(...) do { fprintf(stderr, "[vk] FATAL: " __VA_ARGS__); fputc('\n', stderr); abort(); } while (0)
#define VKCHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) VKDIE("%s = %d (%s:%d)", #x, r_, __FILE__, __LINE__); } while (0)
#define VKLOG(...) do { fprintf(stderr, "[vk] " __VA_ARGS__); fputc('\n', stderr); } while (0)

/* ---------- vk_dev.c ---------- */
typedef struct {
    VkInstance inst; VkPhysicalDevice phys; VkDevice dev; VkQueue queue, queue2; uint32_t qfam;   /* queue2: VK_NULL_HANDLE when the family has one queue */
    VkPhysicalDeviceProperties props; VkPhysicalDeviceMemoryProperties mem;
    VkSemaphore timeline; VkCommandPool cmdpool;
    VkDescriptorSetLayout empty_layout;
    uint64_t host_align;
    PFN_vkCmdSetVertexInputEXT CmdSetVertexInputEXT;
    PFN_vkCreateShadersEXT CreateShadersEXT; PFN_vkDestroyShaderEXT DestroyShaderEXT; PFN_vkCmdBindShadersEXT CmdBindShadersEXT;
    PFN_vkCmdSetColorBlendEnableEXT CmdSetColorBlendEnableEXT; PFN_vkCmdSetColorBlendEquationEXT CmdSetColorBlendEquationEXT;
    PFN_vkCmdSetColorWriteMaskEXT CmdSetColorWriteMaskEXT; PFN_vkCmdSetPolygonModeEXT CmdSetPolygonModeEXT;
    PFN_vkCmdSetRasterizationSamplesEXT CmdSetRasterizationSamplesEXT; PFN_vkCmdSetSampleMaskEXT CmdSetSampleMaskEXT;
    PFN_vkCmdSetAlphaToCoverageEnableEXT CmdSetAlphaToCoverageEnableEXT; PFN_vkCmdSetDepthClampEnableEXT CmdSetDepthClampEnableEXT;
    PFN_vkCmdSetLogicOpEnableEXT CmdSetLogicOpEnableEXT; PFN_vkCmdSetLogicOpEXT CmdSetLogicOpEXT;
    PFN_vkCmdSetTessellationDomainOriginEXT CmdSetTessellationDomainOriginEXT;
    PFN_vkCmdSetConservativeRasterizationModeEXT CmdSetConservativeRasterizationModeEXT;
    PFN_vkCmdSetLineRasterizationModeEXT CmdSetLineRasterizationModeEXT;
    PFN_vkCmdSetDepthClipEnableEXT CmdSetDepthClipEnableEXT;
    PFN_vkCmdSetProvokingVertexModeEXT CmdSetProvokingVertexModeEXT;
    PFN_vkCmdPushDescriptorSetKHR CmdPushDescriptorSetKHR;
    PFN_vkGetMemoryHostPointerPropertiesEXT GetMemoryHostPointerPropertiesEXT;
} vk_ctx;
extern vk_ctx V;
void vk_init(void);
int runtime_core_away(void); void runtime_core_back(int *away);   /* GPU waits on a guest thread give its virtual core to other workers (runtime/threads.c) */
uint32_t vk_memtype(uint32_t bits, VkMemoryPropertyFlags want);

/* image memory: suballocated from 64 MiB device-local blocks (maxMemoryAllocationCount is 4096) */
typedef struct { VkDeviceMemory mem; VkDeviceSize off, size; int block; } vk_alloc;
int vk_alloc_mem(VkMemoryRequirements req, vk_alloc *out);
void vk_free_mem(vk_alloc *a);

/* guest memory pools: imported host memory (VK_EXT_external_memory_host), one VkBuffer each */
typedef struct { uintptr_t lo, hi; VkBuffer buf; VkDeviceMemory mem; uintptr_t base; } vk_pool;
int vk_pool_find(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off);

/* per-submission ring of host-visible memory (driver banks, uniform copies, upload staging) */
typedef struct { VkBuffer buf; uint8_t *cpu; VkDeviceSize off; } vk_span;
vk_span vk_ring(VkDeviceSize size, VkDeviceSize align);

/* the submission being recorded: prologue (uploads), main command buffer */
typedef struct sub sub_t;
VkCommandBuffer vk_cb(void);          /* main command buffer of the open submission */
VkCommandBuffer vk_upload_cb(void);   /* prologue: executed before the main one */
uint64_t vk_next_fence(void);         /* the fence value the open submission will signal */
uint64_t vk_done(void);               /* completed fence value */
void vk_sub_post(int kind, volatile uint32_t *val, uint32_t mode, uint32_t v, uint64_t addr);
uint64_t vk_open_fence(void);
void vk_defer_free(void (*fn)(void *), void *p);   /* run when the open submission completes */
/* occlusion queries: slots of one query pool taken in recording order; a submission's slots are read and reset when it
   completes, a report (vk_sub_post_samples) sums the slots counted since the last reset */
typedef struct { int n, skipped, draws; uint32_t slot[]; } vk_qlist;   /* draws: counted since the reset (TOTK_VK_QUERY_LOG) */
VkQueryPool vk_query_pool(void);
uint32_t vk_query_alloc(void);
void vk_sub_post_samples(uint64_t addr, vk_qlist *l);
void vk_query_stats(uint64_t r[2]);   /* samples-passed reports of 0 / not 0 so far */

/* window and present */
void vk_window_open(void);
void vk_present_image(VkImage img, int32_t x, int32_t y, uint32_t w, uint32_t h, VkFormat fmt);   /* the (x, y, w, h) part of the image */

/* ---------- vk_tex.c ---------- */
typedef struct {
    VkFormat vk; uint8_t depth, stencil, compressed, bw, bh, bpb, integer, expand3, swz_bgr;
} vk_fmt;
const vk_fmt *vk_format(int nvn_format);
typedef struct vtex vtex;   /* a VkImage made for one texture object (by serial) */
vtex *vk_tex_get(const tex_t *t);                      /* made on first use */
void vk_tex_upload_if_stale(vtex *v, const tex_t *t);  /* from guest memory when the CPU wrote it since */
void vk_tex_gpu_wrote(vtex *v);                         /* rendered / copied into: the image holds the newest content */
VkImage vk_tex_image(vtex *v);
VkImageView vk_tex_view(vtex *v, const tex_t *t, const view_t *view, int has_view, VkImageViewType type, int attach);
void vk_tex_dims(vtex *v, uint32_t *w, uint32_t *h, VkFormat *f);
int vk_tex_is3d(vtex *v);
int vk_tex_state(vtex *v);
int vk_tex_aspect(vtex *v);   /* VkImageAspectFlags of the whole format */
VkSampler vk_sampler(const samp_par *p);
VkSampler vk_sampler_id(int id);   /* by sampler pool id, cached */
void vk_tex_registry_set(int id, const tex_t *t, const view_t *v);
int vk_tex_registry_get(int id, tex_t *t, view_t *v, int *has_view);
void vk_samp_registry_set(int id, const samp_par *p);
int vk_samp_registry_get(int id, samp_par *p);
void vk_tex_release_serial(uint64_t serial);
void vk_tex_readback(vtex *v, const tex_t *t);   /* write the image back into guest memory (CPU reads) */
VkBufferView vk_tex_bufview(const tex_t *t, int format);   /* a buffer texture's view over its pool memory */
void vk_tex_mark_read(vtex *v); int vk_tex_store_needed(vtex *v); void vk_tex_frame(void); void vk_tex_prune_stats(uint64_t *stores, uint64_t *reads);   /* store pruning */

/* ---------- vk_shader.c ---------- */
enum { RB_CBUF, RB_SBUF, RB_TEX, RB_IMG, RB_IA };   /* RB_IA: an input attachment of a decal variant (vk_fbread.c), slot = colour attachment */
typedef struct { uint8_t kind, set, type_buffer; uint16_t binding; int32_t slot, cb_slot, cb_off, flags, stype, handle, cbuf_slot, array, separate, format; uint32_t used; } vk_rbind;   /* used: bytes of a uniform buffer the shader reads (cbuf_used), 0 = all */
typedef struct vshader {
    char key[96]; int stage;   /* 0 vertex, 1 fragment, 5 compute */
    uint32_t *spv; size_t nwords;
    vk_rbind *b; int nb;
    uint32_t in_mask;          /* vertex: input locations used */
    uint32_t local[3];
    uint8_t *c1; size_t c1_len;
    VkBuffer c1buf; VkDeviceMemory c1mem;
    VkDescriptorSetLayout layout;
    struct vshader *fb_next; uint32_t fb_key; int fb_none;   /* decal variants of a fragment shader by binding / attachment pairs (vk_shader_fbread) */
    char fout[8], attr[16];    /* each fragment output's / vertex input's class as translated ('f' / 'u' / 's', 0 none) */
    struct vshader *rt_next; uint32_t rt_key;   /* the variant retyped to other targets / vertex formats (vk_retype.c), by their classes */
} vshader;
/* the bound state's answers, as the translation request names them (NULL: not answered) */
typedef struct { const char *tex[128], *img_type[32], *img_fmt[32], *attr[16], *fout[8]; } vk_answers;
vshader *vk_shader_get(const uint8_t *control, const uint8_t *code, int stage, const char *answers, const vk_answers *ans);
/* the decal variant of a fragment shader: texture bindings bind[i] read colour attachment att[i] at the fragment's own pixel
   through input attachments (vk_fbread.c); NULL when the shader uses them otherwise */
#define VK_FB_BINDING 120
uint32_t *vk_fbread_spv(const uint32_t *w, size_t n, const int bind[], const int att[], int k, size_t *out_n);
vshader *vk_shader_fbread(vshader *fs, const int bind[], const int att[], int k);
/* the fragment SPIR-V with the outputs want[loc] names ('f' / 'u' / 's') retyped (vk_retype.c); NULL, vk_retype_why */
uint32_t *vk_retype_spv(const uint32_t *w, size_t n, const char want[8], size_t *out_n);
uint32_t *vk_retype_inputs_spv(const uint32_t *w, size_t n, const char want[16], size_t *out_n);   /* vertex inputs, the same way */
extern const char *vk_retype_why;
void vk_shader_stats(uint64_t s[4]);   /* variants loaded, ns, translated, ns */
void vk_shader_report(void);   /* variant lookups: hits by source, misses by reason */

