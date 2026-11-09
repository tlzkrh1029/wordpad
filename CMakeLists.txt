cmake_minimum_required(VERSION 3.16)
project(RichPad VERSION 1.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(RICHPAD_SOURCES
  src/main.cpp
  src/strings.cpp
  src/theme.cpp
  src/icons.cpp
  src/commandbar.cpp
  src/ruler.cpp
  src/statusbar.cpp
  src/editor.cpp
  src/imaging.cpp
  src/fileio.cpp
  src/dialogs.cpp
  src/print.cpp
  src/popups.cpp
  src/settings.cpp
  src/selftest.cpp
  src/convert/zip.cpp
  src/convert/xml.cpp
  src/convert/model.cpp
  src/convert/rtf_reader.cpp
  src/convert/rtf_writer.cpp
  src/convert/docx.cpp
  src/convert/odt.cpp
  res/richpad.rc
)

add_executable(RichPad WIN32 ${RICHPAD_SOURCES})
target_include_directories(RichPad PRIVATE src res)
target_compile_definitions(RichPad PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
  _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000007 _CRT_SECURE_NO_WARNINGS)
target_link_libraries(RichPad PRIVATE
  comctl32 comdlg32 shell32 shlwapi ole32 oleaut32 oledlg uuid gdi32 user32 advapi32
  gdiplus dwmapi uxtheme winspool)

if(MSVC)
  target_compile_options(RichPad PRIVATE /utf-8 /W3 /EHsc /permissive- /MP)
  set_property(TARGET RichPad PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_link_options(RichPad PRIVATE /MANIFEST:NO /ENTRY:wWinMainCRTStartup)
else()
  target_compile_options(RichPad PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
    -finput-charset=UTF-8 -fno-rtti -ffunction-sections -fdata-sections)
  target_link_options(RichPad PRIVATE -municode -static -static-libgcc -static-libstdc++
    -Wl,--gc-sections $<$<CONFIG:Release>:-s>)
  set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -c 65001")
endif()

# Portable unit tests for the document converters (also buildable natively).
option(RICHPAD_BUILD_TESTS "Build converter unit tests" OFF)
