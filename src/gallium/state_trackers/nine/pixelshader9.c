/*
 * Copyright 2011 Joakim Sindholt <opensource@zhasha.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE. */

#include "nine_helpers.h"
#include "nine_shader.h"

#include "pixelshader9.h"

#include "device9.h"
#include "pipe/p_context.h"

#define DBG_CHANNEL DBG_PIXELSHADER

HRESULT
NinePixelShader9_ctor( struct NinePixelShader9 *This,
                       struct NineUnknownParams *pParams,
                       struct NineDevice9 *pDevice,
                       const DWORD *pFunction )
{
    struct nine_shader_info info;
    HRESULT hr;

    hr = NineUnknown_ctor(&This->base, pParams);
    if (FAILED(hr))
        return hr;

    This->device = pDevice;

    info.type = PIPE_SHADER_FRAGMENT;
    info.byte_code = pFunction;

    hr = nine_translate_shader(pDevice, &info);
    if (FAILED(hr))
        return hr;

    This->byte_code.tokens = mem_dup(pFunction, info.byte_size);
    This->byte_code.size = info.byte_size;
    This->cso = info.cso;

    return D3D_OK;
}

void
NinePixelShader9_dtor( struct NinePixelShader9 *This )
{
    struct pipe_context *pipe = This->device->pipe;
    if (This->cso)
        pipe->delete_fs_state(pipe, This->cso);

    if (This->byte_code.tokens)
        FREE((void *)This->byte_code.tokens); /* const_cast */

    NineUnknown_dtor(&This->base);
}

HRESULT WINAPI
NinePixelShader9_GetDevice( struct NinePixelShader9 *This,
                            IDirect3DDevice9 **ppDevice )
{
    user_assert(ppDevice, E_POINTER);
    NineUnknown_AddRef(NineUnknown(This->device));
    *ppDevice = (IDirect3DDevice9 *)This->device;
    return D3D_OK;
}

HRESULT WINAPI
NinePixelShader9_GetFunction( struct NinePixelShader9 *This,
                              void *pData,
                              UINT *pSizeOfData )
{
    user_assert(pSizeOfData, D3DERR_INVALIDCALL);

    if (!pData) {
        *pSizeOfData = This->byte_code.size;
        return D3D_OK;
    }
    user_assert(*pSizeOfData >= This->byte_code.size, D3DERR_INVALIDCALL);

    memcpy(pData, This->byte_code.tokens, This->byte_code.size);

    return D3D_OK;
}

IDirect3DPixelShader9Vtbl NinePixelShader9_vtable = {
    (void *)NineUnknown_QueryInterface,
    (void *)NineUnknown_AddRef,
    (void *)NineUnknown_Release,
    (void *)NinePixelShader9_GetDevice,
    (void *)NinePixelShader9_GetFunction
};

static const GUID *NinePixelShader9_IIDs[] = {
    &IID_IDirect3DPixelShader9,
    &IID_IUnknown,
    NULL
};

HRESULT
NinePixelShader9_new( struct NineDevice9 *pDevice,
                      struct NinePixelShader9 **ppOut,
                      const DWORD *pFunction )
{
    NINE_NEW(NinePixelShader9, ppOut, pDevice, pFunction);
}