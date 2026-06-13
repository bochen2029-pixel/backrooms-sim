# render_d3d12 — L2 raster renderer. Reads core (view) + stream (events).
# D3D12/DXGI link libraries are introduced in M1, not at M0.
backrooms_module(render_d3d12 DEPS core stream)
