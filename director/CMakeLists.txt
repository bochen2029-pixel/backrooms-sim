# director — L4 intelligence. Contract-only link to core; uses telemetry.
# M11: WandererSummary -> KEEL sidecar (OpenAI HTTP) -> Directive, schema-validated
# (ADR-038 — no embedded llama.cpp; the JSON reader is dependency-free).
backrooms_module(director DEPS core telemetry)
target_sources(director PRIVATE src/json.cpp src/keel_client.cpp src/host.cpp)
target_link_libraries(director PRIVATE winhttp)  # system import lib (no vcpkg dep)
