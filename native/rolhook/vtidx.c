/* vtable slot indices taken from the SDK's C declarations (no hand-counted magic numbers). */
#define CINTERFACE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stddef.h>

#define IDX(iface, m) (int)(offsetof(struct iface##Vtbl, m) / sizeof(void*))

int rol_vt_d3d9_CreateDevice(void) { return IDX(IDirect3D9, CreateDevice); }
int rol_vt_dev_Reset(void) { return IDX(IDirect3DDevice9, Reset); }
int rol_vt_dev_Present(void) { return IDX(IDirect3DDevice9, Present); }
int rol_vt_dev_CreateTexture(void) { return IDX(IDirect3DDevice9, CreateTexture); }
int rol_vt_dev_BeginScene(void) { return IDX(IDirect3DDevice9, BeginScene); }
int rol_vt_dev_SetRenderTarget(void) { return IDX(IDirect3DDevice9, SetRenderTarget); }
int rol_vt_dev_SetTexture(void) { return IDX(IDirect3DDevice9, SetTexture); }
int rol_vt_dev_SetSamplerState(void) { return IDX(IDirect3DDevice9, SetSamplerState); }
int rol_vt_dev_DrawPrimitive(void) { return IDX(IDirect3DDevice9, DrawPrimitive); }
int rol_vt_dev_DrawIndexedPrimitive(void) { return IDX(IDirect3DDevice9, DrawIndexedPrimitive); }
int rol_vt_dev_DrawPrimitiveUP(void) { return IDX(IDirect3DDevice9, DrawPrimitiveUP); }
int rol_vt_dev_DrawIndexedPrimitiveUP(void) { return IDX(IDirect3DDevice9, DrawIndexedPrimitiveUP); }
int rol_vt_dev_CreateVertexShader(void) { return IDX(IDirect3DDevice9, CreateVertexShader); }
int rol_vt_dev_CreatePixelShader(void) { return IDX(IDirect3DDevice9, CreatePixelShader); }
int rol_vt_dev_SetPixelShader(void) { return IDX(IDirect3DDevice9, SetPixelShader); }
int rol_vt_dev_Clear(void) { return IDX(IDirect3DDevice9, Clear); }
int rol_vt_dev_SetViewport(void) { return IDX(IDirect3DDevice9, SetViewport); }
int rol_vt_dev_SetScissorRect(void) { return IDX(IDirect3DDevice9, SetScissorRect); }
int rol_vt_dev_StretchRect(void) { return IDX(IDirect3DDevice9, StretchRect); }
int rol_vt_dev_SetVertexShader(void) { return IDX(IDirect3DDevice9, SetVertexShader); }
int rol_vt_tex_LockRect(void) { return IDX(IDirect3DTexture9, LockRect); }
int rol_vt_vb_Lock(void) { return IDX(IDirect3DVertexBuffer9, Lock); }
int rol_vt_vb_Unlock(void) { return IDX(IDirect3DVertexBuffer9, Unlock); }
int rol_vt_dev_CreateVertexBuffer(void) { return IDX(IDirect3DDevice9, CreateVertexBuffer); }
