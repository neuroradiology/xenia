/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/d3d12/d3d12_provider.h"

#include <malloc.h>
#include <cstdlib>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/ui/d3d12/d3d12_context.h"

DEFINE_bool(d3d12_debug, false, "Enable Direct3D 12 and DXGI debug layer.",
            "D3D12");
DEFINE_bool(d3d12_break_on_error, false,
            "Break on Direct3D 12 validation errors.", "D3D12");
DEFINE_int32(d3d12_adapter, -1,
             "Index of the DXGI adapter to use. "
             "-1 for any physical adapter, -2 for WARP software rendering.",
             "D3D12");
DEFINE_int32(
    d3d12_queue_priority, 1,
    "Graphics (direct) command queue scheduling priority, 0 - normal, 1 - "
    "high, 2 - global realtime (requires administrator privileges, may impact "
    "system responsibility)",
    "D3D12");

namespace xe {
namespace ui {
namespace d3d12 {

bool D3D12Provider::IsD3D12APIAvailable() {
  HMODULE library_d3d12 = LoadLibraryW(L"D3D12.dll");
  if (!library_d3d12) {
    return false;
  }
  FreeLibrary(library_d3d12);
  return true;
}

std::unique_ptr<D3D12Provider> D3D12Provider::Create(Window* main_window) {
  std::unique_ptr<D3D12Provider> provider(new D3D12Provider(main_window));
  if (!provider->Initialize()) {
    xe::FatalError(
        "Unable to initialize Direct3D 12 graphics subsystem.\n"
        "\n"
        "Ensure that you have the latest drivers for your GPU and it supports "
        "Direct3D 12 feature level 11_0.\n"
        "\n"
        "See https://xenia.jp/faq/ for more information and a list of "
        "supported GPUs.");
    return nullptr;
  }
  return provider;
}

D3D12Provider::D3D12Provider(Window* main_window)
    : GraphicsProvider(main_window) {}

D3D12Provider::~D3D12Provider() {
  if (graphics_analysis_ != nullptr) {
    graphics_analysis_->Release();
  }
  if (direct_queue_ != nullptr) {
    direct_queue_->Release();
  }
  if (device_ != nullptr) {
    device_->Release();
  }
  if (dxgi_factory_ != nullptr) {
    dxgi_factory_->Release();
  }

  if (library_dxcompiler_ != nullptr) {
    FreeLibrary(library_dxcompiler_);
  }
  if (library_dxilconv_ != nullptr) {
    FreeLibrary(library_dxilconv_);
  }
  if (library_d3dcompiler_ != nullptr) {
    FreeLibrary(library_d3dcompiler_);
  }
  if (library_d3d12_ != nullptr) {
    FreeLibrary(library_d3d12_);
  }
  if (library_dxgi_ != nullptr) {
    FreeLibrary(library_dxgi_);
  }
}

bool D3D12Provider::EnableIncreaseBasePriorityPrivilege() {
  TOKEN_PRIVILEGES privileges;
  privileges.PrivilegeCount = 1;
  privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  if (!LookupPrivilegeValue(nullptr, SE_INC_BASE_PRIORITY_NAME,
                            &privileges.Privileges[0].Luid)) {
    return false;
  }
  HANDLE token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &token)) {
    return false;
  }
  bool enabled = AdjustTokenPrivileges(token, FALSE, &privileges,
                                       sizeof(privileges), nullptr, nullptr) &&
                 GetLastError() != ERROR_NOT_ALL_ASSIGNED;
  CloseHandle(token);
  return enabled;
}

bool D3D12Provider::Initialize() {
  // Load the core libraries.
  library_dxgi_ = LoadLibraryW(L"dxgi.dll");
  library_d3d12_ = LoadLibraryW(L"D3D12.dll");
  if (library_dxgi_ == nullptr || library_d3d12_ == nullptr) {
    XELOGE("Failed to load dxgi.dll or D3D12.dll");
    return false;
  }
  bool libraries_loaded = true;
  libraries_loaded &=
      (pfn_create_dxgi_factory2_ = PFNCreateDXGIFactory2(
           GetProcAddress(library_dxgi_, "CreateDXGIFactory2"))) != nullptr;
  libraries_loaded &=
      (pfn_dxgi_get_debug_interface1_ = PFNDXGIGetDebugInterface1(
           GetProcAddress(library_dxgi_, "DXGIGetDebugInterface1"))) != nullptr;
  libraries_loaded &=
      (pfn_d3d12_get_debug_interface_ = PFN_D3D12_GET_DEBUG_INTERFACE(
           GetProcAddress(library_d3d12_, "D3D12GetDebugInterface"))) !=
      nullptr;
  libraries_loaded &=
      (pfn_d3d12_create_device_ = PFN_D3D12_CREATE_DEVICE(
           GetProcAddress(library_d3d12_, "D3D12CreateDevice"))) != nullptr;
  libraries_loaded &=
      (pfn_d3d12_serialize_root_signature_ = PFN_D3D12_SERIALIZE_ROOT_SIGNATURE(
           GetProcAddress(library_d3d12_, "D3D12SerializeRootSignature"))) !=
      nullptr;
  if (!libraries_loaded) {
    XELOGE("Failed to get DXGI or Direct3D 12 functions");
    return false;
  }

  // Load optional D3DCompiler_47.dll.
  pfn_d3d_disassemble_ = nullptr;
  library_d3dcompiler_ = LoadLibraryW(L"D3DCompiler_47.dll");
  if (library_d3dcompiler_) {
    pfn_d3d_disassemble_ =
        pD3DDisassemble(GetProcAddress(library_d3dcompiler_, "D3DDisassemble"));
    if (pfn_d3d_disassemble_ == nullptr) {
      XELOGW(
          "Failed to get D3DDisassemble from D3DCompiler_47.dll, DXBC "
          "disassembly for debugging will be unavailable");
    }
  } else {
    XELOGW(
        "Failed to load D3DCompiler_47.dll, DXBC disassembly for debugging "
        "will be unavailable");
  }

  // Load optional dxilconv.dll.
  pfn_dxilconv_dxc_create_instance_ = nullptr;
  library_dxilconv_ = LoadLibraryW(L"dxilconv.dll");
  if (library_dxilconv_) {
    pfn_dxilconv_dxc_create_instance_ = DxcCreateInstanceProc(
        GetProcAddress(library_dxilconv_, "DxcCreateInstance"));
    if (pfn_dxilconv_dxc_create_instance_ == nullptr) {
      XELOGW(
          "Failed to get DxcCreateInstance from dxilconv.dll, converted DXIL "
          "disassembly for debugging will be unavailable");
    }
  } else {
    XELOGW(
        "Failed to load dxilconv.dll, converted DXIL disassembly for debugging "
        "will be unavailable - DXIL may be unsupported by your OS version");
  }

  // Load optional dxcompiler.dll.
  pfn_dxcompiler_dxc_create_instance_ = nullptr;
  library_dxcompiler_ = LoadLibraryW(L"dxcompiler.dll");
  if (library_dxcompiler_) {
    pfn_dxcompiler_dxc_create_instance_ = DxcCreateInstanceProc(
        GetProcAddress(library_dxcompiler_, "DxcCreateInstance"));
    if (pfn_dxcompiler_dxc_create_instance_ == nullptr) {
      XELOGW(
          "Failed to get DxcCreateInstance from dxcompiler.dll, converted DXIL "
          "disassembly for debugging will be unavailable");
    }
  } else {
    XELOGW(
        "Failed to load dxcompiler.dll, converted DXIL disassembly for "
        "debugging will be unavailable - if needed, download the DirectX "
        "Shader Compiler from "
        "https://github.com/microsoft/DirectXShaderCompiler/releases and place "
        "the DLL in the Xenia directory");
  }

  // Configure the DXGI debug info queue.
  if (cvars::d3d12_break_on_error) {
    IDXGIInfoQueue* dxgi_info_queue;
    if (SUCCEEDED(pfn_dxgi_get_debug_interface1_(
            0, IID_PPV_ARGS(&dxgi_info_queue)))) {
      dxgi_info_queue->SetBreakOnSeverity(
          DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION, TRUE);
      dxgi_info_queue->SetBreakOnSeverity(
          DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR, TRUE);
      dxgi_info_queue->Release();
    }
  }

  // Enable the debug layer.
  bool debug = cvars::d3d12_debug;
  if (debug) {
    ID3D12Debug* debug_interface;
    if (SUCCEEDED(
            pfn_d3d12_get_debug_interface_(IID_PPV_ARGS(&debug_interface)))) {
      debug_interface->EnableDebugLayer();
      debug_interface->Release();
    } else {
      XELOGW("Failed to enable the Direct3D 12 debug layer");
      debug = false;
    }
  }

  // Create the DXGI factory.
  IDXGIFactory2* dxgi_factory;
  if (FAILED(pfn_create_dxgi_factory2_(debug ? DXGI_CREATE_FACTORY_DEBUG : 0,
                                       IID_PPV_ARGS(&dxgi_factory)))) {
    XELOGE("Failed to create a DXGI factory");
    return false;
  }

  // Choose the adapter.
  uint32_t adapter_index = 0;
  IDXGIAdapter1* adapter = nullptr;
  while (dxgi_factory->EnumAdapters1(adapter_index, &adapter) == S_OK) {
    DXGI_ADAPTER_DESC1 adapter_desc;
    if (SUCCEEDED(adapter->GetDesc1(&adapter_desc))) {
      if (SUCCEEDED(pfn_d3d12_create_device_(adapter, D3D_FEATURE_LEVEL_11_0,
                                             _uuidof(ID3D12Device), nullptr))) {
        if (cvars::d3d12_adapter >= 0) {
          if (adapter_index == cvars::d3d12_adapter) {
            break;
          }
        } else if (cvars::d3d12_adapter == -2) {
          if (adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            break;
          }
        } else {
          if (!(adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            break;
          }
        }
      }
    }
    adapter->Release();
    adapter = nullptr;
    ++adapter_index;
  }
  if (adapter == nullptr) {
    XELOGE("Failed to get an adapter supporting Direct3D feature level 11_0");
    dxgi_factory->Release();
    return false;
  }
  DXGI_ADAPTER_DESC adapter_desc;
  if (FAILED(adapter->GetDesc(&adapter_desc))) {
    XELOGE("Failed to get the DXGI adapter description");
    adapter->Release();
    dxgi_factory->Release();
    return false;
  }
  adapter_vendor_id_ = adapter_desc.VendorId;
  int adapter_name_mb_size = WideCharToMultiByte(
      CP_UTF8, 0, adapter_desc.Description, -1, nullptr, 0, nullptr, nullptr);
  if (adapter_name_mb_size != 0) {
    char* adapter_name_mb =
        reinterpret_cast<char*>(alloca(adapter_name_mb_size));
    if (WideCharToMultiByte(CP_UTF8, 0, adapter_desc.Description, -1,
                            adapter_name_mb, adapter_name_mb_size, nullptr,
                            nullptr) != 0) {
      XELOGD3D("DXGI adapter: {} (vendor {:04X}, device {:04X})",
               adapter_name_mb, adapter_desc.VendorId, adapter_desc.DeviceId);
    }
  }

  // Create the Direct3D 12 device.
  ID3D12Device* device;
  if (FAILED(pfn_d3d12_create_device_(adapter, D3D_FEATURE_LEVEL_11_0,
                                      IID_PPV_ARGS(&device)))) {
    XELOGE("Failed to create a Direct3D 12 feature level 11_0 device");
    adapter->Release();
    dxgi_factory->Release();
    return false;
  }
  adapter->Release();

  // Configure the Direct3D 12 debug info queue.
  ID3D12InfoQueue* d3d12_info_queue;
  if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d3d12_info_queue)))) {
    D3D12_MESSAGE_SEVERITY d3d12_info_queue_denied_severities[] = {
        D3D12_MESSAGE_SEVERITY_INFO,
    };
    D3D12_MESSAGE_ID d3d12_info_queue_denied_messages[] = {
        // Xbox 360 vertex fetch is explicit in shaders.
        D3D12_MESSAGE_ID_CREATEINPUTLAYOUT_EMPTY_LAYOUT,
        // Render targets and shader exports don't have to match on the Xbox
        // 360.
        D3D12_MESSAGE_ID_CREATEGRAPHICSPIPELINESTATE_RENDERTARGETVIEW_NOT_SET,
    };
    D3D12_INFO_QUEUE_FILTER d3d12_info_queue_filter = {};
    d3d12_info_queue_filter.DenyList.NumSeverities =
        UINT(xe::countof(d3d12_info_queue_denied_severities));
    d3d12_info_queue_filter.DenyList.pSeverityList =
        d3d12_info_queue_denied_severities;
    d3d12_info_queue_filter.DenyList.NumIDs =
        UINT(xe::countof(d3d12_info_queue_denied_messages));
    d3d12_info_queue_filter.DenyList.pIDList = d3d12_info_queue_denied_messages;
    d3d12_info_queue->PushStorageFilter(&d3d12_info_queue_filter);
    if (cvars::d3d12_break_on_error) {
      d3d12_info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION,
                                           TRUE);
      d3d12_info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
    }
    d3d12_info_queue->Release();
  }

  // Create the command queue for graphics.
  D3D12_COMMAND_QUEUE_DESC queue_desc;
  queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  if (cvars::d3d12_queue_priority >= 2) {
    queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME;
    if (!EnableIncreaseBasePriorityPrivilege()) {
      XELOGD3D(
          "Failed to enable SeIncreaseBasePriorityPrivilege for global "
          "realtime Direct3D 12 command queue priority, falling back to high "
          "priority, try launching Xenia as administrator");
      queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    }
  } else if (cvars::d3d12_queue_priority >= 1) {
    queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
  } else {
    queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
  }
  queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
  queue_desc.NodeMask = 0;
  ID3D12CommandQueue* direct_queue;
  if (FAILED(device->CreateCommandQueue(&queue_desc,
                                        IID_PPV_ARGS(&direct_queue)))) {
    bool queue_created = false;
    if (queue_desc.Priority == D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME) {
      XELOGD3D(
          "Failed to create a Direct3D 12 direct command queue with global "
          "realtime priority, falling back to high priority, try launching "
          "Xenia as administrator");
      queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
      queue_created = SUCCEEDED(
          device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&direct_queue)));
    }
    if (!queue_created) {
      XELOGE("Failed to create a Direct3D 12 direct command queue");
      device->Release();
      dxgi_factory->Release();
      return false;
    }
  }

  dxgi_factory_ = dxgi_factory;
  device_ = device;
  direct_queue_ = direct_queue;

  // Get descriptor sizes for each type.
  descriptor_size_view_ = device->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  descriptor_size_sampler_ = device->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  descriptor_size_rtv_ =
      device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  descriptor_size_dsv_ =
      device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

  // Check if optional features are supported.
  // D3D12_HEAP_FLAG_CREATE_NOT_ZEROED requires Windows 10 2004 (indicated by
  // the availability of ID3D12Device8 or D3D12_FEATURE_D3D12_OPTIONS7).
  heap_flag_create_not_zeroed_ = D3D12_HEAP_FLAG_NONE;
  D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7;
  if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7,
                                            &options7, sizeof(options7)))) {
    heap_flag_create_not_zeroed_ = D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;
  }
  rasterizer_ordered_views_supported_ = false;
  resource_binding_tier_ = D3D12_RESOURCE_BINDING_TIER_1;
  tiled_resources_tier_ = D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED;
  D3D12_FEATURE_DATA_D3D12_OPTIONS options;
  if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS,
                                            &options, sizeof(options)))) {
    rasterizer_ordered_views_supported_ = options.ROVsSupported ? true : false;
    resource_binding_tier_ = options.ResourceBindingTier;
    tiled_resources_tier_ = options.TiledResourcesTier;
  }
  programmable_sample_positions_tier_ =
      D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED;
  D3D12_FEATURE_DATA_D3D12_OPTIONS2 options2;
  if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2,
                                            &options2, sizeof(options2)))) {
    programmable_sample_positions_tier_ =
        options2.ProgrammableSamplePositionsTier;
  }
  virtual_address_bits_per_resource_ = 0;
  D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT virtual_address_support;
  if (SUCCEEDED(device->CheckFeatureSupport(
          D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT, &virtual_address_support,
          sizeof(virtual_address_support)))) {
    virtual_address_bits_per_resource_ =
        virtual_address_support.MaxGPUVirtualAddressBitsPerResource;
  }
  XELOGD3D(
      "Direct3D 12 device and OS features:\n"
      "* Max GPU virtual address bits per resource: {}\n"
      "* Non-zeroed heap creation: {}\n"
      "* Programmable sample positions: tier {}\n"
      "* Rasterizer-ordered views: {}\n"
      "* Resource binding: tier {}\n"
      "* Tiled resources: tier {}\n",
      virtual_address_bits_per_resource_,
      (heap_flag_create_not_zeroed_ & D3D12_HEAP_FLAG_CREATE_NOT_ZEROED) ? "yes"
                                                                         : "no",
      uint32_t(programmable_sample_positions_tier_),
      rasterizer_ordered_views_supported_ ? "yes" : "no",
      uint32_t(resource_binding_tier_), uint32_t(tiled_resources_tier_));

  // Get the graphics analysis interface, will silently fail if PIX is not
  // attached.
  pfn_dxgi_get_debug_interface1_(0, IID_PPV_ARGS(&graphics_analysis_));

  return true;
}

std::unique_ptr<GraphicsContext> D3D12Provider::CreateContext(
    Window* target_window) {
  auto new_context =
      std::unique_ptr<D3D12Context>(new D3D12Context(this, target_window));
  if (!new_context->Initialize()) {
    return nullptr;
  }
  return std::unique_ptr<GraphicsContext>(new_context.release());
}

std::unique_ptr<GraphicsContext> D3D12Provider::CreateOffscreenContext() {
  auto new_context =
      std::unique_ptr<D3D12Context>(new D3D12Context(this, nullptr));
  if (!new_context->Initialize()) {
    return nullptr;
  }
  return std::unique_ptr<GraphicsContext>(new_context.release());
}

}  // namespace d3d12
}  // namespace ui
}  // namespace xe
