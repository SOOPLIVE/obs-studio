if(OS_WINDOWS)
  option(ENABLE_NVAFX "Enable building with NVIDIA Audio Effects SDK (requires redistributable to be installed)" OFF)
  option(ENABLE_NVVFX "Enable building with NVIDIA Video Effects SDK (requires redistributable to be installed)" ON)
endif()

if(ENABLE_NVAFX)
  target_sources(obs-filters PRIVATE noise-suppress-filter.c)
  target_compile_definitions(obs-filters PRIVATE LIBNVAFX_ENABLED HAS_NOISEREDUCTION)
endif()
