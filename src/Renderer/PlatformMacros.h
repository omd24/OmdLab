#pragma once

// Graphics backend selection and routing.
//
// Renderer code is split into a backend-agnostic front layer (this
// project's top-level headers) and one implementation per graphics
// backend, in its own subfolder (e.g. dx12/). Exactly one backend define
// is active per build: OMD_GFX_DX12 for now. OMD_GFX_VULKAN is reserved
// for a future backend that does not exist yet and is not being built now.
//
// A front-layer type inherits from the macro-resolved backend type of the
// same name, e.g.:
//
//     struct Device : public OMD_GFX_CLASS(Device) { ... };
//
// resolves at compile time to `struct Device : public DeviceDX12 { ... }`
// when OMD_GFX_DX12 is defined. Calls into backend code go through
// OMD_GFX_CALL so front-layer code never names a specific backend:
//
//     OMD_GFX_CALL(Device, Init(args));   // -> DeviceDX12::Init(args)
//
// This resolves entirely at compile time: no virtual dispatch, no vtables,
// no runtime cost. It also requires no upfront interface design - a
// backend method and its front-layer forwarding declaration are added
// together, at the point the backend implementation is written, rather
// than designed speculatively ahead of it.
//
// Naming convention (binding, must be followed by every type added to
// Renderer): a front-layer type named X must have its DX12 implementation
// named XDX12, declared under Renderer/dx12/. A future second backend, if
// one is ever built, must follow the same rule (XVulkan under
// Renderer/vulkan/). OMD_GFX_CLASS depends on this naming rule holding for
// every type; breaking it breaks compilation for that type.
//
// See DESIGN.md, "Graphics backend routing convention", for the full
// rationale.

#if defined(OMD_GFX_DX12)
    #define OMD_GFX_CLASS(name) name##DX12
#elif defined(OMD_GFX_VULKAN)
    #define OMD_GFX_CLASS(name) name##Vulkan
#endif

#define OMD_GFX_CALL(name, call) OMD_GFX_CLASS(name)::call
