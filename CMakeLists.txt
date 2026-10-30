cmake_minimum_required(VERSION 3.28...3.30)

# the local macOS build lives only in the development branch
option(VAKI_LOCAL_MACOS "Local macOS build without Xcode against installed OBS.app" OFF)
if(VAKI_LOCAL_MACOS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/local-macos.cmake")
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/local-macos.cmake")
  return()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

# Two OBS modules share the sources and ship apart; each has its own name, version (buildspec.json "plugins")
# and data/<name> folder.
find_package(libobs REQUIRED)
if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
endif()
if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
endif()

# the update check asks GitHub for new releases (update-check.c)
find_package(CURL REQUIRED)

function(vf_plugin name)
  string(JSON _plugin_version GET ${buildspec} plugins ${name} version)

  # The helpers and templates read the module's name and version from these.
  set(CMAKE_PROJECT_NAME ${name})
  set(PROJECT_NAME ${name})
  set(CMAKE_PROJECT_VERSION ${_plugin_version})
  set(PROJECT_VERSION ${_plugin_version})
  set(PLUGIN_VERSION ${_plugin_version})
  string(REPLACE "." ";" _parts "${_plugin_version}")
  list(GET _parts 0 PROJECT_VERSION_MAJOR)
  list(GET _parts 1 PROJECT_VERSION_MINOR)
  list(GET _parts 2 PROJECT_VERSION_PATCH)
  set(MACOS_BUNDLEID "com.vakovsky.${name}")

  add_library(${name} MODULE ${ARGN})
  # Own PLUGIN_NAME and PLUGIN_VERSION: the shared plugin-support library carries the repository's.
  configure_file(src/plugin-support.c.in ${name}/plugin-support.c @ONLY)
  target_sources(${name} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/${name}/plugin-support.c")
  target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

  target_link_libraries(${name} PRIVATE OBS::libobs CURL::libcurl)
  if(ENABLE_FRONTEND_API)
    target_link_libraries(${name} PRIVATE OBS::obs-frontend-api)
  endif()
  if(ENABLE_QT)
    target_link_libraries(${name} PRIVATE Qt6::Core Qt6::Widgets)
    target_compile_options(
      ${name}
      PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
    )
    set_target_properties(
      ${name}
      PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
    )
  endif()

  set_target_properties_plugin(${name} PROPERTIES OUTPUT_NAME ${name})
  # the license travels in every archive, in the module's data folder
  target_add_resource(${name} "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
endfunction()

vf_plugin(
  ultimate-frame
  src/frame-main.c
  src/frame-filter.c
  src/frame-render.c
  src/fade-transition.c
  src/copy-registry.c
  src/scene-anim.c
  src/vf-params.c
  src/vf-store.c
  src/vf-select.c
  src/vf-match.c
  src/vf-preview.c
  src/vf-fade.c
  src/scene-scope.c
  src/profile-prune.c
  src/vf-about.c
  src/vf-update.c
  src/update-check.c
)

vf_plugin(
  browser-resize
  src/resize-main.c
  src/size-filter.c
  src/scene-scope.c
  src/copy-registry.c
  src/scene-anim.c
  src/vf-fade.c
  src/vf-size.c
  src/vf-resize.c
  src/vf-css.c
  src/vf-params.c
  src/vf-select.c
  src/profile-prune.c
  src/vf-about.c
  src/vf-update.c
  src/update-check.c
)
