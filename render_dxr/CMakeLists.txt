# render_dxr — L2 path-traced mode (M9). Self-contained DXR device + DXC runtime
# shader compiler; consumes the same ResidentChunk geometry as render_d3d12.
backrooms_module(render_dxr DEPS render_d3d12 stream)

target_sources(render_dxr PRIVATE
  src/dxc.cpp
  src/dxr.cpp)
# DXR uses the same D3D12/DXGI stack; DXC is loaded at runtime (LoadLibrary), so
# only the import libs for device/AS creation are needed here.
target_link_libraries(render_dxr PRIVATE d3d12 dxgi dxguid)
