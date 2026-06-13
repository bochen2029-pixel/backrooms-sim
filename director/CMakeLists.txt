# director — L4 intelligence. Contract-only link to core; uses telemetry.
# llama.cpp dependency is introduced in M11 (future ADR per Iron Rule 8).
backrooms_module(director DEPS core telemetry)
