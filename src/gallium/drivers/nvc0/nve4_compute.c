/*
 * Copyright 2012 Nouveau Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors: Christoph Bumiller
 */

#include "nvc0_context.h"
#include "nve4_compute.h"

#include "nv50/codegen/nv50_ir_driver.h"

static void nve4_compute_dump_launch_desc(const struct nve4_cp_launch_desc *);

static const uint8_t nve4_su_format_map[PIPE_FORMAT_COUNT];
static const uint8_t nve4_su_format_aux_map[PIPE_FORMAT_COUNT];
static const uint16_t nve4_suldp_lib_offset[PIPE_FORMAT_COUNT];


int
nve4_screen_compute_setup(struct nvc0_screen *screen,
                          struct nouveau_pushbuf *push)
{
   struct nouveau_device *dev = screen->base.device;
   struct nouveau_object *chan = screen->base.channel;
   int ret;
   uint32_t obj_class;
   uint32_t magic[2];

   switch (dev->chipset & 0xf0) {
   case 0xf0:
      obj_class = NVF0_COMPUTE_CLASS; /* GK110 */
      break;
   case 0xe0:
      obj_class = NVE4_COMPUTE_CLASS; /* GK104 */
      break;
   default:
      NOUVEAU_ERR("unsupported chipset: NV%02x\n", dev->chipset);
      break;
   }

   ret = nouveau_object_new(chan, 0xbeef00c0, obj_class, NULL, 0,
                            &screen->compute);
   if (ret) {
      NOUVEAU_ERR("Failed to allocate compute object: %d\n", ret);
      return ret;
   }

   ret = nouveau_bo_new(dev, NOUVEAU_BO_VRAM, 0, NVE4_CP_INPUT_SIZE_MAX, NULL,
                        &screen->parm);
   if (ret)
      return ret;

   BEGIN_NVC0(push, SUBC_COMPUTE(NV01_SUBCHAN_OBJECT), 1);
   PUSH_DATA (push, screen->compute->oclass);

   magic[0] = (obj_class >= NVF0_COMPUTE_CLASS) ? 0x468000 : 0x438000;
   magic[1] = (obj_class >= NVF0_COMPUTE_CLASS) ? 0x400 : 0x300;

   BEGIN_NVC0(push, SUBC_COMPUTE(0x02e4), 3);
   PUSH_DATA (push, 0);
   PUSH_DATA (push, magic[0]);
   PUSH_DATA (push, 0xff);
   BEGIN_NVC0(push, SUBC_COMPUTE(0x02f0), 3);
   PUSH_DATA (push, 0);
   PUSH_DATA (push, magic[0]);
   PUSH_DATA (push, 0xff);

   /* Unified address space ? Who needs that ? Certainly not OpenCL. */
   BEGIN_NVC0(push, NVE4_COMPUTE(LOCAL_BASE), 1);
   PUSH_DATA (push, 0x20000);
   BEGIN_NVC0(push, NVE4_COMPUTE(SHARED_BASE), 1);
   PUSH_DATA (push, 0xa0000);

   BEGIN_NVC0(push, SUBC_COMPUTE(0x0310), 1);
   PUSH_DATA (push, magic[1]);

   if (obj_class >= NVF0_COMPUTE_CLASS) {
      BEGIN_NVC0(push, SUBC_COMPUTE(0x0248), 1);
      PUSH_DATA (push, 0x100);
      BEGIN_NIC0(push, SUBC_COMPUTE(0x0248), 63);
      for (i = 63; i >= 1; --i)
         PUSH_DATA(push, 0x38000 | i);
      IMMED_NVC0(push, SUBC_COMPUTE(NV50_GRAPH_SERIALIZE), 0);
      IMMED_NVC0(push, SUBC_COMPUTE(0x518), 0);
   }

   /* XXX: Does this interfere with 3D ? */
   BEGIN_NVC0(push, NVE4_COMPUTE(TEX_CB_INDEX), 1);
   PUSH_DATA (push, 0);

   if (obj_class >= NVF0_COMPUTE_CLASS)
      IMMED_NVC0(push, SUBC_COMPUTE(0x02c4), 1);

   /* MS sample coordinate offsets: these do not work with _ALT modes ! */
   BEGIN_NVC0(push, NVE4_COMPUTE(UPLOAD_ADDRESS_HIGH), 2);
   PUSH_DATAh(push, screen->parm->offset + NVE4_CP_INPUT_MS_OFFSETS);
   PUSH_DATA (push, screen->parm->offset + NVE4_CP_INPUT_MS_OFFSETS);
   BEGIN_NVC0(push, NVE4_COMPUTE(UPLOAD_SIZE), 2);
   PUSH_DATA (push, 64);
   PUSH_DATA (push, NVE4_COMPUTE_UPLOAD_UNK0184_UNKVAL);
   BEGIN_1IC0(push, NVE4_COMPUTE(UPLOAD_EXEC), 17);
   PUSH_DATA (push, NVE4_COMPUTE_UPLOAD_EXEC_UNKVAL_DATA);
   PUSH_DATA (push, 0); /* 0 */
   PUSH_DATA (push, 0);
   PUSH_DATA (push, 1); /* 1 */
   PUSH_DATA (push, 0);
   PUSH_DATA (push, 0); /* 2 */
   PUSH_DATA (push, 1);
   PUSH_DATA (push, 1); /* 3 */
   PUSH_DATA (push, 1);
   PUSH_DATA (push, 2); /* 4 */
   PUSH_DATA (push, 0);
   PUSH_DATA (push, 3); /* 5 */
   PUSH_DATA (push, 0);
   PUSH_DATA (push, 2); /* 6 */
   PUSH_DATA (push, 1);
   PUSH_DATA (push, 3); /* 7 */
   PUSH_DATA (push, 1);

   return 0;
}


static void
nve4_set_surface_info(struct nouveau_pushbuf *push,
                      struct pipe_surface *psf,
                      struct nvc0_screen *screen)
{
   struct nv50_surface *sf = nv50_surface(psf);
   struct nv04_resource *res = nv04_resource(sf->base.texture);
   uint32_t *const info = push->cur;
   uint8_t log2cpp;

   info[8] = sf->width;
   info[9] = sf->height;
   info[10] = sf->depth;
   switch (res->base.target) {
   case PIPE_TEXTURE_1D_ARRAY:
      info[11] = 1;
      break;
   case PIPE_TEXTURE_2D:
   case PIPE_TEXTURE_RECT:
      info[11] = 2;
      break;
   case PIPE_TEXTURE_3D:
      info[11] = 3;
      break;
   case PIPE_TEXTURE_2D_ARRAY:
   case PIPE_TEXTURE_CUBE:
   case PIPE_TEXTURE_CUBE_ARRAY:
      info[11] = 4;
      break;
   default:
      info[11] = 0;
      break;
   }
   log2cpp = (0xf000 & nve4_su_format_aux_map[sf->base.format]) >> 12;

   info[12] = nve4_suldp_lib_offset[sf->base.format] + screen->lib_code->start;

   info[13] = (sf->width << log2cpp) - 1;

   info[0] = res->address + sf->offset;
   info[1] = nve4_su_format_map[sf->base.format];

#if 0
   switch (util_format_get_blocksizebits(sf->base.format)) {
   case  16: info[1] |= 1 << 16; break;
   case  32: info[1] |= 2 << 16; break;
   case  64: info[1] |= 3 << 16; break;
   case 128: info[1] |= 4 << 16; break;
   default:
      break;
   }
#else
   info[1] |= log2cpp << 16;
   info[1] |=  0x4000;
   info[1] |= (0x0f00 & nve4_su_format_aux_map[sf->base.format]);
#endif

   if (res->base.target == PIPE_BUFFER) {
      info[2]  = sf->width - 1;
      info[2] |= (0xff & nve4_su_format_aux_map[sf->base.format]) << 22;
      info[3]  = 0;
      info[4]  = 0;
      info[5]  = 0;
      info[6]  = 0;
      info[7]  = 0;
      info[14] = 0;
      info[15] = 0;
   } else {
      struct nv50_miptree *mt = nv50_miptree(&res->base);
      struct nv50_miptree_level *lvl = &mt->level[sf->base.u.tex.level];

      info[2]  = sf->width - 1;
      info[2] |= (0xff & nve4_su_format_aux_map[sf->base.format]) << 22;
      info[3]  = (0x88 << 24) | (lvl->pitch / 64);
      info[4]  = sf->height - 1;
      info[4] |= (lvl->tile_mode & 0x0f0) << 25;
      info[4] |= NVC0_TILE_SHIFT_Y(lvl->tile_mode) << 22;
      info[5]  = mt->layer_stride >> 8;
      info[6]  = sf->depth - 1;
      info[6] |= (lvl->tile_mode & 0xf00) << 21;
      info[6] |= NVC0_TILE_SHIFT_Z(lvl->tile_mode) << 22;
      info[7]  = 0;
      info[14] = mt->ms_x;
      info[15] = mt->ms_y;
   }

   push->cur += 16;
}

static void
nve4_compute_validate_surfaces(struct nvc0_context *nvc0)
{
   struct nvc0_screen *screen = nvc0->screen;
   struct nouveau_pushbuf *push = nvc0->base.pushbuf;
   struct nv50_surface *sf;
   unsigned i;

   while (nvc0->surfaces_dirty) {
      i = ffs(nvc0->surfaces_dirty) - 1;
      nvc0->surfaces_dirty &= ~(1 << i);

      sf = nv50_surface(nvc0->surfaces[i]);
      /*
       * NVE4's surface load/store instructions receive all the information
       * directly instead of via binding points, so we have to supply them.
       */
      BEGIN_NVC0(push, NVE4_COMPUTE(UPLOAD_ADDRESS_HIGH), 2);
      PUSH_DATAh(push, screen->parm->offset + NVE4_CP_INPUT_SUF(i));
      PUSH_DATA (push, screen->parm->offset + NVE4_CP_INPUT_SUF(i));
      BEGIN_NVC0(push, NVE4_COMPUTE(UPLOAD_SIZE), 2);
      PUSH_DATA (push, 64);
      PUSH_DATA (push, NVE4_COMPUTE_UPLOAD_UNK0184_UNKVAL);
      BEGIN_1IC0(push, NVE4_COMPUTE(UPLOAD_EXEC), 17);
      PUSH_DATA (push, NVE4_COMPUTE_UPLOAD_EXEC_UNKVAL_DATA);
      if (sf) {
         nve4_set_surface_info(push, &sf->base, screen);
      } else {
         PUSH_DATA (push, 0);
         PUSH_DATA (push, 0x80004000);
         PUSH_DATA (push, 0);
         PUSH_DATA (push, 0);
         PUSH_DATA (push, 0);
         PUSH_DATA (push, 0);
         PUSH_DATA (push, 0);
         PUSH_DATA (push, 0);
         PUSH_DATA (push, 0);
         PUSH_DATA (push, 0);
         PUSH_DATA (push, 0);
         PUSH_DATA (push, 0);
         PUSH_DATA (push, screen->lib_code->start +
                    nve4_suldp_lib_offset[PIPE_FORMAT_RGBA32_UINT]);
         PUSH_DATA (push, 0);
         PUSH_DATA (push, 0);
         PUSH_DATA (push, 0);
      }
   }
   nvc0->surfaces_dirty = 0;
}


/* Thankfully, textures with samplers follow the normal rules. */
static void
nve4_compute_validate_samplers(struct nvc0_context *nvc0)
{
   boolean need_flush = nve4_validate_tsc(nvc0, 5);
   if (need_flush) {
      BEGIN_NVC0(nvc0->base.pushbuf, SUBC_COMPUTE(0x1330), 1);
      PUSH_DATA (nvc0->base.pushbuf, 0);
   }
}
/* (Code duplicated at bottom for various non-convincing reasons.
 *  E.g. we might want to use the COMPUTE subchannel to upload TIC/TSC
 *  entries to avoid a subchannel switch.
 *  Same for texture cache flushes.
 *  Also, the bufctx differs, and more IFs in the 3D version looks ugly.)
 */
static void nve4_compute_validate_textures(struct nvc0_context *);

static void
nve4_compute_set_tex_handles(struct nvc0_context *nvc0)
{
   struct nouveau_pushbuf *push = nvc0->base.pushbuf;
   uint64_t address;
   const unsigned s = nvc0_shader_stage(PIPE_SHADER_COMPUTE);
   unsigned i, n;
   uint32_t dirty = nvc0->textures_dirty[s] | nvc0->samplers_dirty[s];

   if (!dirty)
      return;
   i = ffs(dirty) - 1;
   n = util_logbase2(dirty) + 1 - i;
   assert(n);

   address = nvc0->screen->uniform_bo->offset + NVE4_CP_INPUT_TEX(i);

   BEGIN_NVC0(push, NVE4_COMPUTE(UPLOAD_ADDRESS_HIGH), 2);
   PUSH_DATAh(push, address);
   PUSH_DATA (push, address);
   BEGIN_NVC0(push, NVE4_COMPUTE(UPLOAD_SIZE), 2);
   PUSH_DATA (push, n * 4);
   PUSH_DATA (push, 0x1);
   BEGIN_NVC0(push, NVE4_COMPUTE(UPLOAD_EXEC), 1 + n);
   PUSH_DATA (push, 0x41);
   PUSH_DATAp(push, &nvc0->tex_handles[s][i], n);

   nvc0->textures_dirty[s] = 0;
   nvc0->samplers_dirty[s] = 0;
}


static boolean
nve4_compute_validate_program(struct nvc0_context *nvc0)
{
   struct nvc0_program *prog = nvc0->compprog;

   if (prog->mem)
      return TRUE;

   if (!prog->translated) {
      prog->translated = nvc0_program_translate(
         prog, nvc0->screen->base.device->chipset);
      if (!prog->translated)
         return FALSE;
   }
   if (unlikely(!prog->code_size))
      return FALSE;

   return nvc0_program_upload_code(nvc0, prog);
}


static boolean
nve4_compute_state_validate(struct nvc0_context *nvc0)
{
   if (!nve4_compute_validate_program(nvc0))
      return FALSE;
   nve4_compute_validate_textures(nvc0);
   nve4_compute_validate_samplers(nvc0);
   nve4_compute_set_tex_handles(nvc0);
   nve4_compute_validate_surfaces(nvc0);

   nvc0_bufctx_fence(nvc0, nvc0->bufctx_cp, FALSE);

   nouveau_pushbuf_bufctx(nvc0->base.pushbuf, nvc0->bufctx_cp);
   if (unlikely(nouveau_pushbuf_validate(nvc0->base.pushbuf)))
      return FALSE;
   if (unlikely(nvc0->state.flushed))
      nvc0_bufctx_fence(nvc0, nvc0->bufctx_cp, TRUE);

   return TRUE;
}


static void
nve4_compute_upload_input(struct nvc0_context *nvc0, const void *input)
{
   struct nvc0_screen *screen = nvc0->screen;
   struct nouveau_pushbuf *push = nvc0->base.pushbuf;
   struct nvc0_program *cp = nvc0->compprog;

   if (!cp->parm_size)
      return;

   BEGIN_NVC0(push, NVE4_COMPUTE(UPLOAD_ADDRESS_HIGH), 2);
   PUSH_DATAh(push, screen->parm->offset);
   PUSH_DATA (push, screen->parm->offset);
   BEGIN_NVC0(push, NVE4_COMPUTE(UPLOAD_SIZE), 2);
   PUSH_DATA (push, cp->parm_size);
   PUSH_DATA (push, 0x1);
   BEGIN_1IC0(push, NVE4_COMPUTE(UPLOAD_EXEC), 1 + (cp->parm_size / 4));
   PUSH_DATA (push, 0x41);
   PUSH_DATAp(push, input, cp->parm_size / 4);

   BEGIN_NVC0(push, NVE4_COMPUTE(FLUSH), 1);
   PUSH_DATA (push, NVE4_COMPUTE_FLUSH_CB);
}

static INLINE uint8_t
nve4_compute_derive_cache_split(struct nvc0_context *nvc0, uint32_t shared_size)
{
   if (shared_size > (32 << 10))
      return NVC0_3D_CACHE_SPLIT_48K_SHARED_16K_L1;
   if (shared_size > (16 << 10))
      return NVE4_3D_CACHE_SPLIT_32K_SHARED_32K_L1;
   return NVC1_3D_CACHE_SPLIT_16K_SHARED_48K_L1;
}

static void
nve4_compute_setup_launch_desc(struct nvc0_context *nvc0,
                               struct nve4_cp_launch_desc *desc,
                               uint32_t label,
                               const uint *block_layout,
                               const uint *grid_layout)
{
   const struct nvc0_screen *screen = nvc0->screen;
   const struct nvc0_program *cp = nvc0->compprog;
   unsigned i;

   nve4_cp_launch_desc_init_default(desc);

   desc->entry = nvc0_program_symbol_offset(cp, label);

   desc->griddim_x = grid_layout[0];
   desc->griddim_y = grid_layout[1];
   desc->griddim_z = grid_layout[2];
   desc->blockdim_x = block_layout[0];
   desc->blockdim_y = block_layout[1];
   desc->blockdim_z = block_layout[2];

   desc->shared_size = cp->cp.smem_size;
   desc->local_size_p = cp->cp.lmem_size;
   desc->cache_split = nve4_compute_derive_cache_split(nvc0, cp->cp.smem_size);

   desc->gpr_alloc = cp->max_gpr;

   for (i = 0; i < 7; ++i) {
      const unsigned s = 5;
      if (nvc0->constbuf[s][i].u.buf)
         nve4_cp_launch_desc_set_ctx_cb(desc, i + 1, &nvc0->constbuf[s][i]);
   }
   nve4_cp_launch_desc_set_cb(desc, 0, screen->parm, 0, NVE4_CP_INPUT_SIZE_MAX);
}

void
nve4_launch_grid(struct pipe_context *pipe,
                 const uint *block_layout, const uint *grid_layout,
                 uint32_t label,
                 const void *input)
{
   struct nvc0_context *nvc0 = nvc0_context(pipe);
   struct nouveau_pushbuf *push = nvc0->base.pushbuf;
   struct nvc0_program *cp = nvc0->compprog;
   struct nve4_cp_launch_desc *desc;
   struct nouveau_bo *desc_bo;
   uint64_t addr, desc_gpuaddr;
   int ret;

   desc = nouveau_scratch_get(&nvc0->base, 512, &addr, &desc_bo);
   desc_gpuaddr = align(addr, 256);
   desc += (desc_gpuaddr - addr) / 4;

   BCTX_REFN_bo(nvc0->bufctx_cp, CP_DESC, NOUVEAU_BO_GART | NOUVEAU_BO_RD,
                desc_bo);

   ret = !nve4_compute_state_validate(nvc0);
   if (ret)
      goto out;

   nve4_compute_setup_launch_desc(nvc0, desc, label, block_layout, grid_layout);
   nve4_compute_dump_launch_desc(desc);
   nve4_compute_upload_input(nvc0, input);

   BEGIN_NVC0(push, NVE4_COMPUTE(LAUNCH_DESC_ADDRESS), 1);
   PUSH_DATA (push, desc_gpuaddr >> 8);
   BEGIN_NVC0(push, NVE4_COMPUTE(LAUNCH), 1);
   PUSH_DATA (push, 0x3);
   BEGIN_NVC0(push, SUBC_COMPUTE(NV50_GRAPH_SERIALIZE), 1);
   PUSH_DATA (push, 0);

out:
   if (ret)
      NOUVEAU_ERR("Failed to launch grid !\n");
   nouveau_scratch_done(&nvc0->base);
   nouveau_bufctx_reset(nvc0->bufctx_cp, NVC0_BIND_CP_DESC);
}


#define NVE4_TIC_ENTRY_INVALID 0x000fffff

static void
nve4_compute_validate_textures(struct nvc0_context *nvc0)
{
   struct nouveau_bo *txc = nvc0->screen->txc;
   struct nouveau_pushbuf *push = nvc0->base.pushbuf;
   const unsigned s = 5;
   unsigned i;
   uint32_t commands[2][NVE4_CP_INPUT_TEX_MAX];
   unsigned n[2] = { 0, 0 };

   for (i = 0; i < nvc0->num_textures[s]; ++i) {
      struct nv50_tic_entry *tic = nv50_tic_entry(nvc0->textures[s][i]);
      struct nv04_resource *res;
      const boolean dirty = !!(nvc0->textures_dirty[s] & (1 << i));

      if (!tic) {
         nvc0->tex_handles[s][i] |= NVE4_TIC_ENTRY_INVALID;
         continue;
      }
      res = nv04_resource(tic->pipe.texture);

      if (tic->id < 0) {
         tic->id = nvc0_screen_tic_alloc(nvc0->screen, tic);

         PUSH_SPACE(push, 16);
         BEGIN_NVC0(push, NVE4_COMPUTE(UPLOAD_ADDRESS_HIGH), 2);
         PUSH_DATAh(push, txc->offset + (tic->id * 32));
         PUSH_DATA (push, txc->offset + (tic->id * 32));
         BEGIN_NVC0(push, NVE4_COMPUTE(UPLOAD_SIZE), 2);
         PUSH_DATA (push, 32);
         PUSH_DATA (push, NVE4_COMPUTE_UPLOAD_UNK0184_UNKVAL);
         BEGIN_1IC0(push, NVE4_COMPUTE(UPLOAD_EXEC), 9);
         PUSH_DATA (push, NVE4_COMPUTE_UPLOAD_EXEC_UNKVAL_DATA);
         PUSH_DATAp(push, &tic->tic[0], 8);

         commands[0][n[0]++] = (tic->id << 4) | 1;
      } else
      if (res->status & NOUVEAU_BUFFER_STATUS_GPU_WRITING) {
         commands[1][n[1]++] = (tic->id << 4) | 1;
      }
      nvc0->screen->tic.lock[tic->id / 32] |= 1 << (tic->id % 32);

      res->status &= ~NOUVEAU_BUFFER_STATUS_GPU_WRITING;
      res->status |=  NOUVEAU_BUFFER_STATUS_GPU_READING;

      nvc0->tex_handles[s][i] &= ~NVE4_TIC_ENTRY_INVALID;
      nvc0->tex_handles[s][i] |= tic->id;
      if (dirty)
         BCTX_REFN(nvc0->bufctx_cp, CP_TEX(i), res, RD);
   }
   for (; i < nvc0->state.num_textures[s]; ++i)
      nvc0->tex_handles[s][i] |= NVE4_TIC_ENTRY_INVALID;

   if (n[0]) {
      BEGIN_NIC0(push, SUBC_COMPUTE(0x1334), n[0]);
      PUSH_DATAp(push, commands[0], n[0]);
   }
   if (n[1]) {
      BEGIN_NIC0(push, SUBC_COMPUTE(0x1338), n[1]);
      PUSH_DATAp(push, commands[1], n[1]);
   }

   nvc0->state.num_textures[s] = nvc0->num_textures[s];
}


static const char *nve4_cache_split_name(unsigned value)
{
   switch (value) {
   case NVC1_3D_CACHE_SPLIT_16K_SHARED_48K_L1: return "16K_SHARED_48K_L1";
   case NVE4_3D_CACHE_SPLIT_32K_SHARED_32K_L1: return "32K_SHARED_32K_L1";
   case NVC0_3D_CACHE_SPLIT_48K_SHARED_16K_L1: return "48K_SHARED_16K_L1";
   default:
      return "(invalid)";
   }
}

static void
nve4_compute_dump_launch_desc(const struct nve4_cp_launch_desc *desc)
{
   const uint32_t *data = (const uint32_t *)desc;
   unsigned i;
   boolean zero = FALSE;

   debug_printf("COMPUTE LAUNCH DESCRIPTOR:\n");

   for (i = 0; i < sizeof(*desc); i += 4) {
      if (data[i / 4]) {
         debug_printf("[%x]: 0x%08x\n", i, data[i / 4]);
         zero = FALSE;
      } else
      if (!zero) {
         debug_printf("...\n");
         zero = TRUE;
      }
   }

   debug_printf("entry = 0x%x\n", desc->entry);
   debug_printf("grid dimensions = %ux%ux%u\n",
                desc->griddim_x, desc->griddim_y, desc->griddim_z);
   debug_printf("block dimensions = %ux%ux%u\n",
                desc->blockdim_x, desc->blockdim_y, desc->blockdim_z);
   debug_printf("s[] size: 0x%x\n", desc->shared_size);
   debug_printf("l[] size: -0x%x / +0x%x\n", 0, desc->local_size_p);
   debug_printf("$r count: %u\n", desc->gpr_alloc);
   debug_printf("cache split: %s\n", nve4_cache_split_name(desc->cache_split));

   for (i = 0; i < 8; ++i) {
      uint64_t address;
      uint32_t size = desc->cb[i].size;
      boolean valid = !!(desc->cb_mask & (1 << i));

      address = ((uint64_t)desc->cb[i].address_h << 32) | desc->cb[i].address_l;

      if (!valid && !address && !size)
         continue;
      debug_printf("CB[%u]: address = 0x%"PRIx64", size 0x%x%s\n",
                   i, address, size, valid ? "" : "  (invalid)");
   }
}


static const uint8_t nve4_su_format_map[PIPE_FORMAT_COUNT] =
{
   [PIPE_FORMAT_R32G32B32A32_FLOAT] = NVE4_IMAGE_FORMAT_RGBA32_FLOAT,
   [PIPE_FORMAT_R32G32B32A32_SINT] = NVE4_IMAGE_FORMAT_RGBA32_SINT,
   [PIPE_FORMAT_R32G32B32A32_UINT] = NVE4_IMAGE_FORMAT_RGBA32_UINT,
   [PIPE_FORMAT_R16G16B16A16_FLOAT] = NVE4_IMAGE_FORMAT_RGBA16_FLOAT,
   [PIPE_FORMAT_R16G16B16A16_UNORM] = NVE4_IMAGE_FORMAT_RGBA16_UNORM,
   [PIPE_FORMAT_R16G16B16A16_SNORM] = NVE4_IMAGE_FORMAT_RGBA16_SNORM,
   [PIPE_FORMAT_R16G16B16A16_SINT] = NVE4_IMAGE_FORMAT_RGBA16_SINT,
   [PIPE_FORMAT_R16G16B16A16_UINT] = NVE4_IMAGE_FORMAT_RGBA16_UINT,
   [PIPE_FORMAT_R8G8B8A8_UNORM] = NVE4_IMAGE_FORMAT_RGBA8_UNORM,
   [PIPE_FORMAT_R8G8B8A8_SNORM] = NVE4_IMAGE_FORMAT_RGBA8_SNORM,
   [PIPE_FORMAT_R8G8B8A8_SINT] = NVE4_IMAGE_FORMAT_RGBA8_SINT,
   [PIPE_FORMAT_R8G8B8A8_UINT] = NVE4_IMAGE_FORMAT_RGBA8_UINT,
   [PIPE_FORMAT_R11G11B10_FLOAT] = NVE4_IMAGE_FORMAT_R11G11B10_FLOAT,
   [PIPE_FORMAT_R10G10B10A2_UNORM] = NVE4_IMAGE_FORMAT_RGB10_A2_UNORM,
/* [PIPE_FORMAT_R10G10B10A2_UINT] = NVE4_IMAGE_FORMAT_RGB10_A2_UINT, */
   [PIPE_FORMAT_R32G32_FLOAT] = NVE4_IMAGE_FORMAT_RG32_FLOAT,
   [PIPE_FORMAT_R32G32_SINT] = NVE4_IMAGE_FORMAT_RG32_SINT,
   [PIPE_FORMAT_R32G32_UINT] = NVE4_IMAGE_FORMAT_RG32_UINT,
   [PIPE_FORMAT_R16G16_FLOAT] = NVE4_IMAGE_FORMAT_RG16_FLOAT,
   [PIPE_FORMAT_R16G16_UNORM] = NVE4_IMAGE_FORMAT_RG16_UNORM,
   [PIPE_FORMAT_R16G16_SNORM] = NVE4_IMAGE_FORMAT_RG16_SNORM,
   [PIPE_FORMAT_R16G16_SINT] = NVE4_IMAGE_FORMAT_RG16_SINT,
   [PIPE_FORMAT_R16G16_UINT] = NVE4_IMAGE_FORMAT_RG16_UINT,
   [PIPE_FORMAT_R8G8_UNORM] = NVE4_IMAGE_FORMAT_RG8_UNORM,
   [PIPE_FORMAT_R8G8_SNORM] = NVE4_IMAGE_FORMAT_RG8_SNORM,
   [PIPE_FORMAT_R8G8_SINT] = NVE4_IMAGE_FORMAT_RG8_SINT,
   [PIPE_FORMAT_R8G8_UINT] = NVE4_IMAGE_FORMAT_RG8_UINT,
   [PIPE_FORMAT_R32_FLOAT] = NVE4_IMAGE_FORMAT_R32_FLOAT,
   [PIPE_FORMAT_R32_SINT] = NVE4_IMAGE_FORMAT_R32_SINT,
   [PIPE_FORMAT_R32_UINT] = NVE4_IMAGE_FORMAT_R32_UINT,
   [PIPE_FORMAT_R16_FLOAT] = NVE4_IMAGE_FORMAT_R16_FLOAT,
   [PIPE_FORMAT_R16_UNORM] = NVE4_IMAGE_FORMAT_R16_UNORM,
   [PIPE_FORMAT_R16_SNORM] = NVE4_IMAGE_FORMAT_R16_SNORM,
   [PIPE_FORMAT_R16_SINT] = NVE4_IMAGE_FORMAT_R16_SINT,
   [PIPE_FORMAT_R16_UINT] = NVE4_IMAGE_FORMAT_R16_UINT,
   [PIPE_FORMAT_R8_UNORM] = NVE4_IMAGE_FORMAT_R8_UNORM,
   [PIPE_FORMAT_R8_SNORM] = NVE4_IMAGE_FORMAT_R8_SNORM,
   [PIPE_FORMAT_R8_SINT] = NVE4_IMAGE_FORMAT_R8_SINT,
   [PIPE_FORMAT_R8_UINT] = NVE4_IMAGE_FORMAT_R8_UINT,
};

/* Auxiliary format description values for surface instructions.
 * (log2(bytes per pixel) << 12) | (unk8 << 8) | unk22
 */
static const uint16_t nve4_su_format_aux_map[PIPE_FORMAT_COUNT] =
{
   [PIPE_FORMAT_R32G32B32A32_FLOAT] = 0x4802,
   [PIPE_FORMAT_R32G32B32A32_SINT] = 0x4802,
   [PIPE_FORMAT_R32G32B32A32_UINT] = 0x4802,

   [PIPE_FORMAT_R16G16B16A16_UNORM] = 0x3933,
   [PIPE_FORMAT_R16G16B16A16_SNORM] = 0x3933,
   [PIPE_FORMAT_R16G16B16A16_SINT] = 0x3933,
   [PIPE_FORMAT_R16G16B16A16_UINT] = 0x3933,
   [PIPE_FORMAT_R16G16B16A16_FLOAT] = 0x3933,

   [PIPE_FORMAT_R32G32_FLOAT] = 0x3433,
   [PIPE_FORMAT_R32G32_SINT] = 0x3433,
   [PIPE_FORMAT_R32G32_UINT] = 0x3433,

   [PIPE_FORMAT_R10G10B10A2_UNORM] = 0x2a24,
/* [PIPE_FORMAT_R10G10B10A2_UINT] = 0x2a24, */
   [PIPE_FORMAT_R8G8B8A8_UNORM] = 0x2a24,
   [PIPE_FORMAT_R8G8B8A8_SNORM] = 0x2a24,
   [PIPE_FORMAT_R8G8B8A8_SINT] = 0x2a24,
   [PIPE_FORMAT_R8G8B8A8_UINT] = 0x2a24,
   [PIPE_FORMAT_R11G11B10_FLOAT] = 0x2a24,

   [PIPE_FORMAT_R16G16_UNORM] = 0x2524,
   [PIPE_FORMAT_R16G16_SNORM] = 0x2524,
   [PIPE_FORMAT_R16G16_SINT] = 0x2524,
   [PIPE_FORMAT_R16G16_UINT] = 0x2524,
   [PIPE_FORMAT_R16G16_FLOAT] = 0x2524,

   [PIPE_FORMAT_R32_SINT] = 0x2024,
   [PIPE_FORMAT_R32_UINT] = 0x2024,
   [PIPE_FORMAT_R32_FLOAT] = 0x2024,

   [PIPE_FORMAT_R8G8_UNORM] = 0x1615,
   [PIPE_FORMAT_R8G8_SNORM] = 0x1615,
   [PIPE_FORMAT_R8G8_SINT] = 0x1615,
   [PIPE_FORMAT_R8G8_UINT] = 0x1615,

   [PIPE_FORMAT_R16_UNORM] = 0x1115,
   [PIPE_FORMAT_R16_SNORM] = 0x1115,
   [PIPE_FORMAT_R16_SINT] = 0x1115,
   [PIPE_FORMAT_R16_UINT] = 0x1115,
   [PIPE_FORMAT_R16_FLOAT] = 0x1115,

   [PIPE_FORMAT_R8_UNORM] = 0x0206,
   [PIPE_FORMAT_R8_SNORM] = 0x0206,
   [PIPE_FORMAT_R8_SINT] = 0x0206,
   [PIPE_FORMAT_R8_UINT] = 0x0206
};

/* NOTE: These are hardcoded offsets for the shader library.
 * TODO: Automate them.
 */
static const uint16_t nve4_suldp_lib_offset[PIPE_FORMAT_COUNT]
{
   [PIPE_FORMAT_R32G32B32A32_FLOAT] = 0x218,
   [PIPE_FORMAT_R32G32B32A32_SINT]  = 0x218,
   [PIPE_FORMAT_R32G32B32A32_UINT]  = 0x218,
   [PIPE_FORMAT_R16G16B16A16_UNORM] = 0x248,
   [PIPE_FORMAT_R16G16B16A16_SNORM] = 0x2b8,
   [PIPE_FORMAT_R16G16B16A16_SINT]  = 0x330,
   [PIPE_FORMAT_R16G16B16A16_UINT]  = 0x388,
   [PIPE_FORMAT_R16G16B16A16_FLOAT] = 0x3d8,
   [PIPE_FORMAT_R32G32_FLOAT]       = 0x428,
   [PIPE_FORMAT_R32G32_SINT]        = 0x468,
   [PIPE_FORMAT_R32G32_UINT]        = 0x468,
   [PIPE_FORMAT_R10G10B10A2_UNORM]  = 0x4a8,
/* [PIPE_FORMAT_R10G10B10A2_UINT]   = 0x530, */
   [PIPE_FORMAT_R8G8B8A8_UNORM]     = 0x588,
   [PIPE_FORMAT_R8G8B8A8_SNORM]     = 0x5f8,
   [PIPE_FORMAT_R8G8B8A8_SINT]      = 0x670,
   [PIPE_FORMAT_R8G8B8A8_UINT]      = 0x6c8,
   [PIPE_FORMAT_R5G6B5_UNORM]       = 0x718,
   [PIPE_FORMAT_R5G5B5X1_UNORM]     = 0x7a0,
   [PIPE_FORMAT_R16G16_UNORM]       = 0x828,
   [PIPE_FORMAT_R16G16_SNORM]       = 0x890,
   [PIPE_FORMAT_R16G16_SINT]        = 0x8f0,
   [PIPE_FORMAT_R16G16_UINT]        = 0x948,
   [PIPE_FORMAT_R16G16_FLOAT]       = 0x998,
   [PIPE_FORMAT_R32_FLOAT]          = 0x9e8,
   [PIPE_FORMAT_R32_SINT]           = 0xa30,
   [PIPE_FORMAT_R32_UINT]           = 0xa30,
   [PIPE_FORMAT_R8G8_UNORM]         = 0xa78,
   [PIPE_FORMAT_R8G8_SNORM]         = 0xae0,
   [PIPE_FORMAT_R8G8_UINT]          = 0xb48,
   [PIPE_FORMAT_R8G8_SINT]          = 0xb98,
   [PIPE_FORMAT_R16_UNORM]          = 0xbe8,
   [PIPE_FORMAT_R16_SNORM]          = 0xc48,
   [PIPE_FORMAT_R16_SINT]           = 0xca0,
   [PIPE_FORMAT_R16_UINT]           = 0xce8,
   [PIPE_FORMAT_R16_FLOAT]          = 0xd30,
   [PIPE_FORMAT_R8_UNORM]           = 0xd88,
   [PIPE_FORMAT_R8_SNORM]           = 0xde0,
   [PIPE_FORMAT_R8_SINT]            = 0xe38,
   [PIPE_FORMAT_R8_UINT]            = 0xe88
   [PIPE_FORMAT_R11G11B10_FLOAT]    = 0xed0,
};