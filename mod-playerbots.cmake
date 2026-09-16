# Reuse the core's common script headers for the static Playerbots build.
# NOPCH / USE_SCRIPTPCH remain available for diagnostic builds.
if (USE_SCRIPTPCH AND MODULE_MOD-PLAYERBOTS STREQUAL "static")
  target_precompile_headers(modules PRIVATE "${CMAKE_SOURCE_DIR}/modules/ModulesPCH.h")
endif()
