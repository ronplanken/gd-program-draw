cmake_minimum_required(VERSION 3.28...3.30)

# Preserve the installed-OBS development build used by scripts/build-macos.sh.
if(DEFINED OBS_SOURCE_DIR AND NOT OBS_SOURCE_DIR STREQUAL "")
  include(cmake/local-obs.cmake)
  return()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)
project(${_name} VERSION ${_version} LANGUAGES C CXX)
option(ENABLE_FRONTEND_API "Use the OBS frontend API" ON)
option(ENABLE_QT "Use Qt for drawing controls" ON)
include(compilerconfig)
include(defaults)
include(helpers)
find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Widgets Svg)

add_library(canvas STATIC src/canvas.cpp)
set_target_properties(canvas PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(canvas PUBLIC src)
target_link_libraries(canvas PUBLIC Qt6::Core Qt6::Gui)

add_library(${CMAKE_PROJECT_NAME} MODULE src/plugin.cpp src/ink-source.cpp)
target_link_libraries(
  ${CMAKE_PROJECT_NAME}
  PRIVATE canvas OBS::libobs OBS::obs-frontend-api Qt6::Widgets Qt6::Svg
)
target_compile_options(
  ${CMAKE_PROJECT_NAME}
  PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header;-Wno-comma>
)
set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

include(CTest)
if(BUILD_TESTING)
  foreach(suite canvas tools sports)
    add_executable(${suite}-tests tests/${suite}-tests.cpp)
    target_link_libraries(${suite}-tests PRIVATE canvas)
    if(APPLE)
      # Standalone tests load the build's Qt frameworks; the plugin loads OBS's Qt.
      get_filename_component(qt_framework_dir "${Qt6_DIR}/../.." ABSOLUTE)
      target_link_options(${suite}-tests PRIVATE "LINKER:-rpath,${qt_framework_dir}")
      set_target_properties(${suite}-tests PROPERTIES XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME NO)
      foreach(config Release RelWithDebInfo MinSizeRel)
        set_target_properties(${suite}-tests PROPERTIES XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME[variant=${config}] NO)
      endforeach()
    endif()
    if(WIN32)
      add_custom_command(
        TARGET ${suite}-tests
        POST_BUILD
        COMMAND
          ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:${suite}-tests> $<TARGET_FILE_DIR:${suite}-tests>
        COMMAND_EXPAND_LISTS
      )
    endif()
  endforeach()
  add_test(NAME canvas-behavior COMMAND canvas-tests)
  add_test(NAME tools-and-layers COMMAND tools-tests)
  add_test(NAME analysis-tools COMMAND sports-tests "${CMAKE_BINARY_DIR}/analysis-proof.png")
endif()
