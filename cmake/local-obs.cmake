cmake_minimum_required(VERSION 3.24)
project(obs-program-draw VERSION 0.5.6 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OBS_SOURCE_DIR "" CACHE PATH "OBS Studio 32.2.2 source checkout (headers only)")
set(OBS_APP "/Applications/OBS.app" CACHE PATH "Installed OBS application")
set(QT_HEADERS_PREFIX "/opt/homebrew/opt/qtbase" CACHE PATH "Matching Qt 6.11 header installation")
set(QT_SVG_HEADERS_PREFIX "/opt/homebrew/opt/qtsvg" CACHE PATH "Matching Qt 6.11 SVG header installation")
set(SIMDE_DIR "" CACHE PATH "SIMDe checkout containing simde/x86/sse2.h")
if(NOT APPLE)
  message(
    FATAL_ERROR
    "This POC build package targets macOS. The source uses portable Qt/libobs APIs, but other platforms are untested."
  )
endif()
if(NOT EXISTS "${OBS_SOURCE_DIR}/libobs/obs.h")
  message(FATAL_ERROR "Pass -DOBS_SOURCE_DIR=/path/to/obs-studio-32.2.2")
endif()
set(OBS_RELEASE_CANDIDATE 0)
set(OBS_BETA 0)
configure_file("${OBS_SOURCE_DIR}/libobs/obsconfig.h.in" "${CMAKE_CURRENT_BINARY_DIR}/generated/obsconfig.h")

add_library(deps INTERFACE)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/qt-headers")
target_include_directories(deps SYSTEM INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/qt-headers")
target_include_directories(
  deps
  SYSTEM
  INTERFACE
    "${OBS_SOURCE_DIR}/libobs"
    "${OBS_SOURCE_DIR}/frontend/api"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    "${SIMDE_DIR}"
)
# Compile against public Qt headers, link to OBS's own Qt frameworks. Do not
# load Homebrew's second copy of Qt inside the OBS process.
foreach(component Core Gui Widgets)
  file(
    CREATE_LINK
      "${QT_HEADERS_PREFIX}/lib/Qt${component}.framework/Headers"
      "${CMAKE_CURRENT_BINARY_DIR}/qt-headers/Qt${component}"
    SYMBOLIC
  )
  target_include_directories(
    deps
    SYSTEM
    INTERFACE "${QT_HEADERS_PREFIX}/include" "${QT_HEADERS_PREFIX}/lib/Qt${component}.framework/Headers"
  )
  target_link_libraries(deps INTERFACE "${OBS_APP}/Contents/Frameworks/Qt${component}.framework/Qt${component}")
endforeach()
target_link_libraries(
  deps
  INTERFACE
    "${OBS_APP}/Contents/Frameworks/libobs.framework/libobs"
    "${OBS_APP}/Contents/Frameworks/libobs-frontend-api.1.dylib"
)

add_library(canvas STATIC src/canvas.cpp)
set_target_properties(canvas PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(canvas PUBLIC src)
target_link_libraries(canvas PUBLIC deps)
target_compile_options(canvas PRIVATE -Wall -Wextra -Wpedantic)

add_library(obs-program-draw MODULE src/plugin.cpp src/ink-source.cpp)
target_link_libraries(obs-program-draw PRIVATE canvas)
file(
  CREATE_LINK "${QT_SVG_HEADERS_PREFIX}/lib/QtSvg.framework/Headers" "${CMAKE_CURRENT_BINARY_DIR}/qt-headers/QtSvg"
  SYMBOLIC
)
target_include_directories(obs-program-draw SYSTEM PRIVATE "${QT_SVG_HEADERS_PREFIX}/lib/QtSvg.framework/Headers")
target_link_libraries(obs-program-draw PRIVATE "${OBS_APP}/Contents/Frameworks/QtSvg.framework/QtSvg")
target_compile_options(obs-program-draw PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(
  obs-program-draw
  PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION "plugin"
    PREFIX ""
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "@executable_path/../Frameworks"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/release"
)
add_custom_command(
  TARGET obs-program-draw
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:obs-program-draw>/Contents/Resources"
  COMMAND
    ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/third-party/lucide/LICENSE"
    "$<TARGET_BUNDLE_DIR:obs-program-draw>/Contents/Resources/Lucide-LICENSE.txt"
  COMMAND /usr/bin/codesign --force --sign - "$<TARGET_BUNDLE_DIR:obs-program-draw>"
  VERBATIM
)

include(CTest)
if(BUILD_TESTING)
  add_executable(canvas-tests tests/canvas-tests.cpp)
  target_link_libraries(canvas-tests PRIVATE canvas)
  set_target_properties(canvas-tests PROPERTIES BUILD_RPATH "${OBS_APP}/Contents/Frameworks")
  add_test(NAME canvas-behavior COMMAND canvas-tests)
  add_executable(tools-tests tests/tools-tests.cpp)
  target_link_libraries(tools-tests PRIVATE canvas)
  set_target_properties(tools-tests PROPERTIES BUILD_RPATH "${OBS_APP}/Contents/Frameworks")
  add_test(NAME tools-and-layers COMMAND tools-tests)
  add_executable(sports-tests tests/sports-tests.cpp)
  target_link_libraries(sports-tests PRIVATE canvas)
  set_target_properties(sports-tests PROPERTIES BUILD_RPATH "${OBS_APP}/Contents/Frameworks")
  add_test(NAME sports-telestrator COMMAND sports-tests "${CMAKE_BINARY_DIR}/sports-proof.png")
  add_executable(render-tests tests/render-tests.cpp src/ink-source.cpp)
  target_link_libraries(render-tests PRIVATE canvas)
  set_target_properties(render-tests PROPERTIES BUILD_RPATH "${OBS_APP}/Contents/Frameworks")
  add_test(NAME libobs-render-output COMMAND render-tests "${OBS_APP}" "${CMAKE_BINARY_DIR}/rendered-proof.png")
endif()
