# CMake build for dxvk-st (static D3D8/D3D9 over Vulkan, non-Windows only).
# Transliterated from meson.build / src/*/meson.build; meson stays the upstream
# build. Only the targets vkdawn needs are expressed here (no dxgi/d3d10/d3d11).
cmake_minimum_required(VERSION 3.24)
project(dxvk VERSION 2.7.1 LANGUAGES C CXX)

include(CheckCXXCompilerFlag)
include(CheckCCompilerFlag)

option(DXVK_STATIC_DLLS     "Build d3d8/d3d9 as static libraries" ON)
option(DXVK_ENABLE_D3D8     "Build D3D8" ON)
option(DXVK_ENABLE_D3D9     "Build D3D9" ON)
option(DXVK_NATIVE_HEADLESS "Enable headless WSI (VK_EXT_headless_surface)" ON)
option(DXVK_SINGLE_THREADED "Run CS, submission and pipeline compilation inline" OFF)
option(DXVK_WEBGPU_TARGET   "Target a WebGPU-backed Vulkan device" OFF)
set(DXVK_GIPA_SYMBOL "" CACHE STRING "vkGetInstanceProcAddr-compatible symbol to use instead of loading libvulkan")
set(DXVK_GIPA_LIBRARIES "" CACHE STRING "Libraries/targets that define DXVK_GIPA_SYMBOL; put inside the consumer link group")
set(DXVK_TARGET_SUFFIX "" CACHE STRING "Suffix appended to every target name (lets one tree hold two configurations)")

if(WIN32)
  message(FATAL_ERROR "dxvk-st CMake build supports non-Windows targets only; use meson on Windows")
endif()
if(NOT DXVK_STATIC_DLLS)
  message(FATAL_ERROR "dxvk-st CMake build only produces static libraries; set DXVK_STATIC_DLLS=ON or use meson")
endif()
if(DXVK_ENABLE_D3D8 AND NOT DXVK_ENABLE_D3D9)
  message(FATAL_ERROR "DXVK_ENABLE_D3D8 requires DXVK_ENABLE_D3D9")
endif()
if(NOT DXVK_NATIVE_HEADLESS)
  message(FATAL_ERROR "headless WSI is the only WSI this CMake build wires up; set DXVK_NATIVE_HEADLESS=ON")
endif()

set(S "${DXVK_TARGET_SUFFIX}")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---- project-wide flags (meson: compiler_args via get_supported_arguments) ----
set(_dxvk_flags
  -msse -msse2 -msse3 -mfpmath=sse
  -Wall -Wextra -Wimplicit-fallthrough
  -Wno-missing-field-initializers -Wno-unused-parameter -Wno-misleading-indentation
  -Wno-cast-function-type -Wno-unused-private-field -Wno-microsoft-exception-spec
  -Wno-extern-c-compat -Wno-unused-const-variable -Wno-missing-braces)
add_library(dxvk_flags${S} INTERFACE)
foreach(flag IN LISTS _dxvk_flags)
  string(MAKE_C_IDENTIFIER "DXVK_HAS${flag}" var)
  check_cxx_compiler_flag("${flag}" ${var})
  if(${var})
    target_compile_options(dxvk_flags${S} INTERFACE $<$<COMPILE_LANGUAGE:CXX>:${flag}>)
  endif()
  check_c_compiler_flag("${flag}" ${var}_C)
  if(${var}_C)
    target_compile_options(dxvk_flags${S} INTERFACE $<$<COMPILE_LANGUAGE:C>:${flag}>)
  endif()
endforeach()
target_compile_definitions(dxvk_flags${S} INTERFACE DXVK_WSI_HEADLESS)
if(DXVK_GIPA_SYMBOL)
  target_compile_definitions(dxvk_flags${S} INTERFACE DXVK_VULKAN_GIPA=${DXVK_GIPA_SYMBOL})
endif()

# Consumer link list. vkcommon resolves DXVK_GIPA_SYMBOL from DXVK_GIPA_LIBRARIES, so
# they go inside the group. wasm-ld rescans archives by itself and CMake has no RESCAN
# feature for Emscripten, so there the plain list is enough.
function(dxvk_link_group iface)
  if(EMSCRIPTEN)
    target_link_libraries(${iface} INTERFACE ${ARGN})
  else()
    string(REPLACE ";" "," _libs "${ARGN}")
    target_link_libraries(${iface} INTERFACE "$<LINK_GROUP:RESCAN,${_libs}>")
  endif()
endfunction()
if(DXVK_SINGLE_THREADED)
  target_compile_definitions(dxvk_flags${S} INTERFACE DXVK_SINGLE_THREADED)
endif()
if(DXVK_WEBGPU_TARGET)
  target_compile_definitions(dxvk_flags${S} INTERFACE DXVK_WEBGPU_TARGET)
endif()
target_include_directories(dxvk_flags${S} INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/include/vulkan/include
  ${CMAKE_CURRENT_SOURCE_DIR}/include/spirv/include
  ${CMAKE_CURRENT_SOURCE_DIR}/include/native
  ${CMAKE_CURRENT_SOURCE_DIR}/include/native/windows
  ${CMAKE_CURRENT_SOURCE_DIR}/include/native/directx
  ${CMAKE_CURRENT_BINARY_DIR}/gen)

# ---- version.h (meson: vcs_tag, re-run every build) and buildenv.h ----
set(_gen ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${_gen})
add_custom_target(dxvk_version${S}
  COMMAND ${CMAKE_COMMAND} -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR} -DDST=${_gen}/version.h
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/dxvk_version.cmake
  BYPRODUCTS ${_gen}/version.h
  VERBATIM)
set(BUILD_TARGET "${CMAKE_SYSTEM_PROCESSOR}")
set(BUILD_COMPILER "${CMAKE_CXX_COMPILER_ID}")
set(BUILD_COMPILER_VERSION "${CMAKE_CXX_COMPILER_VERSION}")
configure_file(buildenv.h.in ${_gen}/buildenv.h @ONLY)

# ---- GLSL -> C headers (meson: glsl_generator) ----
find_program(DXVK_GLSLANG NAMES glslang glslangValidator REQUIRED)
set(_glsl_args --quiet --target-env vulkan1.3)
if(DXVK_WEBGPU_TARGET)
  list(APPEND _glsl_args --target-env spirv1.3)  # Tint reads SPIR-V <= 1.3
endif()
# dxvk_glsl_headers(<out-var> <shader>...) appends generated header paths to <out-var>.
function(dxvk_glsl_headers out)
  set(headers)
  foreach(src IN LISTS ARGN)
    get_filename_component(name ${src} NAME_WE)
    set(h ${_gen}/${name}.h)
    add_custom_command(OUTPUT ${h}
      COMMAND ${DXVK_GLSLANG} ${_glsl_args} --vn ${name} --depfile ${h}.d
              ${CMAKE_CURRENT_SOURCE_DIR}/${src} -o ${h}
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${src}
      DEPFILE ${h}.d
      COMMENT "glslang ${src}"
      VERBATIM)
    list(APPEND headers ${h})
  endforeach()
  set(${out} ${${out}} ${headers} PARENT_SCOPE)
endfunction()

if(NOT EMSCRIPTEN)
  find_package(Threads REQUIRED)
endif()

# dxvk_static(<target> <srcdir> <sources>...): static lib named like meson's,
# with its own source dir on the include path (meson's implicit include dir).
function(dxvk_static name dir)
  add_library(${name}${S} STATIC ${ARGN})
  set_target_properties(${name}${S} PROPERTIES OUTPUT_NAME ${name})
  target_include_directories(${name}${S} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/${dir})
  target_link_libraries(${name}${S} PUBLIC dxvk_flags${S})
endfunction()

# ---- libdisplay-info (subproject) ----
set(_di subprojects/libdisplay-info)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(OUTPUT ${_gen}/pnp-id-table.c
  COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/${_di}/tool/gen-search-table.py
          ${CMAKE_CURRENT_SOURCE_DIR}/${_di}/pnp.ids ${_gen}/pnp-id-table.c pnp_id_table
  DEPENDS ${_di}/tool/gen-search-table.py ${_di}/pnp.ids
  VERBATIM)
add_library(display-info${S} STATIC
  ${_di}/cta.c ${_di}/displayid.c ${_di}/dmt-table.c ${_di}/edid.c
  ${_di}/gtf.c ${_di}/info.c ${_di}/log.c ${_di}/memory-stream.c
  ${_gen}/pnp-id-table.c)
set_target_properties(display-info${S} PROPERTIES OUTPUT_NAME display-info C_STANDARD 11)
target_include_directories(display-info${S} PUBLIC ${_di}/include)
target_compile_definitions(display-info${S} PRIVATE _POSIX_C_SOURCE=200809L static_array=static)
target_compile_options(display-info${S} PRIVATE -Wno-unused-parameter -Wno-missing-field-initializers)

# ---- util ----
dxvk_static(util src/util
  src/util/util_env.cpp src/util/util_string.cpp src/util/util_fps_limiter.cpp
  src/util/util_flush.cpp src/util/util_gdi.cpp src/util/util_luid.cpp
  src/util/util_matrix.cpp src/util/util_shared_res.cpp src/util/util_sleep.cpp
  src/util/thread.cpp
  src/util/com/com_destruction_notifier.cpp src/util/com/com_guid.cpp
  src/util/com/com_private_data.cpp
  src/util/config/config.cpp
  src/util/log/log.cpp src/util/log/log_debug.cpp
  src/util/sha1/sha1.c src/util/sha1/sha1_util.cpp
  src/util/sync/sync_recursive.cpp)

# ---- vulkan (vkcommon) ----
dxvk_static(vkcommon src/vulkan src/vulkan/vulkan_loader.cpp src/vulkan/vulkan_names.cpp)
if(NOT EMSCRIPTEN)
  target_link_libraries(vkcommon${S} PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()

# ---- spirv ----
dxvk_static(spirv src/spirv
  src/spirv/spirv_code_buffer.cpp src/spirv/spirv_compression.cpp src/spirv/spirv_module.cpp)

# ---- wsi ----
dxvk_static(wsi src/wsi
  src/wsi/wsi_edid.cpp src/wsi/wsi_platform.cpp
  src/wsi/win32/wsi_monitor_win32.cpp src/wsi/win32/wsi_platform_win32.cpp src/wsi/win32/wsi_window_win32.cpp
  src/wsi/sdl3/wsi_monitor_sdl3.cpp src/wsi/sdl3/wsi_platform_sdl3.cpp src/wsi/sdl3/wsi_window_sdl3.cpp
  src/wsi/sdl2/wsi_monitor_sdl2.cpp src/wsi/sdl2/wsi_platform_sdl2.cpp src/wsi/sdl2/wsi_window_sdl2.cpp
  src/wsi/glfw/wsi_monitor_glfw.cpp src/wsi/glfw/wsi_platform_glfw.cpp src/wsi/glfw/wsi_window_glfw.cpp
  src/wsi/headless/wsi_headless.cpp)
target_link_libraries(wsi${S} PUBLIC dxvk_flags${S} display-info${S})

# ---- dxvk ----
set(_dxvk_shaders
  src/dxvk/shaders/dxvk_blit_frag_1d.frag src/dxvk/shaders/dxvk_blit_frag_2d.frag
  src/dxvk/shaders/dxvk_blit_frag_3d.frag src/dxvk/shaders/dxvk_blit_frag_2d_ms.frag
  src/dxvk/shaders/dxvk_buffer_to_image_d.frag src/dxvk/shaders/dxvk_buffer_to_image_ds_export.frag
  src/dxvk/shaders/dxvk_buffer_to_image_s_discard.frag
  src/dxvk/shaders/dxvk_clear_buffer_u.comp src/dxvk/shaders/dxvk_clear_buffer_f.comp
  src/dxvk/shaders/dxvk_clear_image1d_u.comp src/dxvk/shaders/dxvk_clear_image1d_f.comp
  src/dxvk/shaders/dxvk_clear_image1darr_u.comp src/dxvk/shaders/dxvk_clear_image1darr_f.comp
  src/dxvk/shaders/dxvk_clear_image2d_u.comp src/dxvk/shaders/dxvk_clear_image2d_f.comp
  src/dxvk/shaders/dxvk_clear_image2darr_u.comp src/dxvk/shaders/dxvk_clear_image2darr_f.comp
  src/dxvk/shaders/dxvk_clear_image3d_u.comp src/dxvk/shaders/dxvk_clear_image3d_f.comp
  src/dxvk/shaders/dxvk_copy_buffer_image.comp
  src/dxvk/shaders/dxvk_copy_color_1d.frag src/dxvk/shaders/dxvk_copy_color_2d.frag
  src/dxvk/shaders/dxvk_copy_color_ms.frag
  src/dxvk/shaders/dxvk_copy_depth_stencil_1d.frag src/dxvk/shaders/dxvk_copy_depth_stencil_2d.frag
  src/dxvk/shaders/dxvk_copy_depth_stencil_ms.frag
  src/dxvk/shaders/dxvk_cursor_vert.vert src/dxvk/shaders/dxvk_cursor_frag.frag
  src/dxvk/shaders/dxvk_dummy_frag.frag
  src/dxvk/shaders/dxvk_fullscreen_geom.geom src/dxvk/shaders/dxvk_fullscreen_vert.vert
  src/dxvk/shaders/dxvk_fullscreen_layer_vert.vert
  src/dxvk/shaders/dxvk_image_to_buffer_ds.comp src/dxvk/shaders/dxvk_image_to_buffer_f.comp
  src/dxvk/shaders/dxvk_present_frag.frag src/dxvk/shaders/dxvk_present_frag_blit.frag
  src/dxvk/shaders/dxvk_present_frag_ms.frag src/dxvk/shaders/dxvk_present_frag_ms_blit.frag
  src/dxvk/shaders/dxvk_present_vert.vert
  src/dxvk/shaders/dxvk_resolve_frag_d.frag src/dxvk/shaders/dxvk_resolve_frag_ds.frag
  src/dxvk/shaders/dxvk_resolve_frag_f.frag src/dxvk/shaders/dxvk_resolve_frag_i.frag
  src/dxvk/shaders/dxvk_resolve_frag_u.frag
  src/dxvk/hud/shaders/hud_chunk_frag_background.frag src/dxvk/hud/shaders/hud_chunk_frag_visualize.frag
  src/dxvk/hud/shaders/hud_chunk_vert_background.vert src/dxvk/hud/shaders/hud_chunk_vert_visualize.vert
  src/dxvk/hud/shaders/hud_frame_time_eval.comp
  src/dxvk/hud/shaders/hud_graph_frag.frag src/dxvk/hud/shaders/hud_graph_vert.vert
  src/dxvk/hud/shaders/hud_text_frag.frag src/dxvk/hud/shaders/hud_text_vert.vert)
set(_dxvk_gen)
dxvk_glsl_headers(_dxvk_gen ${_dxvk_shaders})

set(_dxvk_src
  dxvk_access dxvk_adapter dxvk_allocator dxvk_barrier dxvk_buffer dxvk_cmdlist dxvk_compute
  dxvk_constant_state dxvk_context dxvk_cs dxvk_descriptor_heap dxvk_descriptor_info
  dxvk_descriptor_pool dxvk_descriptor_worker dxvk_device dxvk_device_filter dxvk_device_info
  dxvk_fence dxvk_format dxvk_framebuffer dxvk_gpu_event dxvk_gpu_query dxvk_graphics dxvk_image
  dxvk_implicit_resolve dxvk_instance dxvk_latency_builtin dxvk_latency_reflex dxvk_memory
  dxvk_meta_blit dxvk_meta_clear dxvk_meta_copy dxvk_meta_mipgen dxvk_meta_resolve dxvk_options
  dxvk_pipelayout dxvk_pipemanager dxvk_platform_exts dxvk_presenter dxvk_queue dxvk_sampler
  dxvk_shader dxvk_shader_key dxvk_signal dxvk_sparse dxvk_staging dxvk_stats
  dxvk_swapchain_blitter dxvk_unbound dxvk_util
  hud/dxvk_hud hud/dxvk_hud_font hud/dxvk_hud_item hud/dxvk_hud_renderer)
list(TRANSFORM _dxvk_src PREPEND src/dxvk/)
list(TRANSFORM _dxvk_src APPEND .cpp)
dxvk_static(dxvk src/dxvk ${_dxvk_src} ${_dxvk_gen})
add_dependencies(dxvk${S} dxvk_version${S})
target_link_libraries(dxvk${S} PUBLIC util${S} spirv${S} wsi${S} vkcommon${S})

# ---- d3d9 (needs dxso) ----
if(DXVK_ENABLE_D3D9)
  set(_dxso_src
    dxso_common dxso_options dxso_module dxso_reader dxso_header dxso_ctab dxso_util dxso_code
    dxso_tables dxso_decoder dxso_analysis dxso_compiler dxso_enums)
  list(TRANSFORM _dxso_src PREPEND src/dxso/)
  list(TRANSFORM _dxso_src APPEND .cpp)
  dxvk_static(dxso src/dxso ${_dxso_src})
  target_link_libraries(dxso${S} PUBLIC dxvk${S})

  set(_d3d9_gen)
  dxvk_glsl_headers(_d3d9_gen
    src/d3d9/shaders/d3d9_convert_yuy2_uyvy.comp src/d3d9/shaders/d3d9_convert_l6v5u5.comp
    src/d3d9/shaders/d3d9_convert_x8l8v8u8.comp src/d3d9/shaders/d3d9_convert_a2w10v10u10.comp
    src/d3d9/shaders/d3d9_convert_w11v11u10.comp src/d3d9/shaders/d3d9_convert_nv12.comp
    src/d3d9/shaders/d3d9_convert_yv12.comp)
  set(_d3d9_src
    d3d9_main d3d9_interface d3d9_adapter d3d9_monitor d3d9_device d3d9_state d3d9_cursor
    d3d9_swapchain d3d9_format d3d9_common_texture d3d9_constant_buffer d3d9_texture d3d9_surface
    d3d9_volume d3d9_common_buffer d3d9_buffer d3d9_shader d3d9_vertex_declaration d3d9_query
    d3d9_shader_validator d3d9_multithread d3d9_options d3d9_stateblock d3d9_util d3d9_initializer
    d3d9_fixed_function d3d9_names d3d9_swvp_emu d3d9_format_helpers d3d9_hud d3d9_annotation
    d3d9_mem d3d9_window d3d9_interop d3d9_on_12 d3d9_bridge)
  list(TRANSFORM _d3d9_src PREPEND src/d3d9/)
  list(TRANSFORM _d3d9_src APPEND .cpp)
  dxvk_static(dxvk_d3d9 src/d3d9 ${_d3d9_src} ${_d3d9_gen})
  target_link_libraries(dxvk_d3d9${S} PUBLIC dxso${S} dxvk${S})

  # Consumer-facing: link group so static archives resolve regardless of order.
  add_library(dxvk_d3d9_link${S} INTERFACE)
  dxvk_link_group(dxvk_d3d9_link${S}
    dxvk_d3d9${S} dxvk${S} dxso${S} spirv${S} wsi${S} vkcommon${S} util${S} display-info${S} ${DXVK_GIPA_LIBRARIES})
  add_library(dxvk::d3d9${S} ALIAS dxvk_d3d9_link${S})
endif()

# ---- d3d8 (on top of d3d9) ----
if(DXVK_ENABLE_D3D8)
  set(_d3d8_src
    d3d8_buffer d3d8_device d3d8_interface d3d8_main d3d8_multithread d3d8_options d3d8_shader
    d3d8_state_block d3d8_surface d3d8_swapchain d3d8_texture d3d8_volume)
  list(TRANSFORM _d3d8_src PREPEND src/d3d8/)
  list(TRANSFORM _d3d8_src APPEND .cpp)
  dxvk_static(dxvk_d3d8 src/d3d8 ${_d3d8_src})
  target_compile_definitions(dxvk_d3d8${S} PRIVATE DXVK_STATIC_DLLS)
  target_link_libraries(dxvk_d3d8${S} PUBLIC dxvk_d3d9${S} util${S} dxso${S} dxvk${S})

  add_library(dxvk_d3d8_link${S} INTERFACE)
  dxvk_link_group(dxvk_d3d8_link${S}
    dxvk_d3d8${S} dxvk_d3d9${S} dxvk${S} dxso${S} spirv${S} wsi${S} vkcommon${S} util${S} display-info${S} ${DXVK_GIPA_LIBRARIES})
  add_library(dxvk::d3d8${S} ALIAS dxvk_d3d8_link${S})
endif()
