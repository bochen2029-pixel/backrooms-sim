# render_d3d12 — L2 raster renderer. Reads core (view) + stream (events).
# M1 introduces the D3D12 device, debug layer + DRED, and the clear-color frame
# (headless offscreen readback + windowed swapchain). D3D12/DXGI link libs and
# the renderer source enter here, not at M0.
add_library(render_d3d12 STATIC
  src/render_d3d12.cpp
  src/renderer.cpp)
target_include_directories(render_d3d12 PUBLIC include)
target_link_libraries(render_d3d12
  PUBLIC core stream
  PRIVATE backrooms_flags d3d12 dxgi dxguid Psapi)
add_library(backrooms::render_d3d12 ALIAS render_d3d12)
