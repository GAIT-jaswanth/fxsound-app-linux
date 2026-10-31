# FxSound for Linux - build definition.
#   cmake -S linux -B build -G Ninja && cmake --build build
cmake_minimum_required(VERSION 3.16)
project(FxSoundLinux C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
get_filename_component(FX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

option(FX_SANITIZE "Build with address/undefined sanitizers" OFF)
option(FX_BUILD_GUI "Build the JUCE GUI application" ON)
option(FX_BUILD_TESTS "Build DSP test tools" ON)

if(FX_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake_dsp_sources.cmake)

# ------------------------------------------------- Win32 compatibility layer
# Built WITHOUT the DSP prelude: this is ordinary Linux code.
add_library(fxwincompat STATIC ${CMAKE_CURRENT_SOURCE_DIR}/compat/win_compat.cpp)
target_include_directories(fxwincompat PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/compat)

# ---------------------------------------------------------------- DSP engine
add_library(dfxdsp STATIC
  ${DFXDSP_SOURCES}
  ${CMAKE_CURRENT_SOURCE_DIR}/dsp_support/dsp_support.cpp
  ${FX_ROOT}/audiopassthru/src/MTH/MthBuffer.cpp
  ${FX_ROOT}/audiopassthru/src/MTH/MthUtil.cpp
  ${FX_ROOT}/audiopassthru/src/MTH/Mthfreq.cpp
  ${FX_ROOT}/audiopassthru/src/SLOUT/Slout.cpp
)
# compat/ must come first so its windows.h shim is found.
target_include_directories(dfxdsp BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/compat)
target_include_directories(dfxdsp PRIVATE ${DFXDSP_INCLUDE_DIRS})
target_include_directories(dfxdsp PUBLIC ${FX_ROOT}/dsp/include)
target_compile_definitions(dfxdsp PRIVATE PT_NON_MFC DSPSOFT_TARGET PT_DSP_BUILD=PT_DSP_DFX)
# Give the DSP Windows' 32-bit `long` (see compat/dsp_prelude.h for why).
target_compile_options(dfxdsp PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/compat/dsp_prelude.h)
# Legacy code: silence warnings, accept MSVC-isms g++ would otherwise reject.
target_compile_options(dfxdsp PRIVATE -w $<$<COMPILE_LANGUAGE:CXX>:-fpermissive>)
target_link_libraries(dfxdsp PUBLIC fxwincompat m pthread)

if(FX_BUILD_TESTS)
  add_executable(dsp_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/dsp_test.cpp)
  target_link_libraries(dsp_test dfxdsp)
endif()
if(FX_BUILD_TESTS)
  add_executable(eq_response_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/eq_response_test.cpp)
  target_link_libraries(eq_response_test dfxdsp)
endif()

# ----------------------------------------------------- PipeWire audio backend
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
add_library(fxaudiopassthru STATIC ${CMAKE_CURRENT_SOURCE_DIR}/audiopassthru/AudioPassthruPipeWire.cpp)
target_include_directories(fxaudiopassthru PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/audiopassthru ${FX_ROOT}/dsp/include)
target_compile_options(fxaudiopassthru PRIVATE -Wall -Wextra)
target_link_libraries(fxaudiopassthru PUBLIC PkgConfig::PIPEWIRE pthread)

if(FX_BUILD_TESTS)
  add_executable(passthru_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/passthru_test.cpp)
  target_link_libraries(passthru_test fxaudiopassthru dfxdsp)
endif()
if(FX_BUILD_TESTS)
  add_executable(dsp_process_raw ${CMAKE_CURRENT_SOURCE_DIR}/tests/dsp_process_raw.cpp)
  target_link_libraries(dsp_process_raw dfxdsp)
endif()

# ------------------------------------------------------------------ GUI app
if(FX_BUILD_GUI)
  # JUCE 6.1.6, the version the Windows x64 build uses.
  set(FX_JUCE_DIR "" CACHE PATH "Path to a JUCE 6.1.6 checkout (downloaded if empty)")
  # JUCE 6.1.6 predates GCC 13, which no longer includes <utility> implicitly
  # (std::exchange errors). Force-include it for JUCE only.
  set(_fx_saved_cxx_flags "${CMAKE_CXX_FLAGS}")
  set(FX_JUCE_COMPAT_FLAGS "")
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 13)
    set(FX_JUCE_COMPAT_FLAGS -include utility)
    string(APPEND CMAKE_CXX_FLAGS " -include utility")   # reaches JUCE's juceaide helper build
  endif()
  if(FX_JUCE_DIR)
    add_subdirectory(${FX_JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE EXCLUDE_FROM_ALL)
  else()
    include(FetchContent)
    # Download only (SOURCE_SUBDIR points nowhere), then add JUCE ourselves with
    # EXCLUDE_FROM_ALL so its install rules (headers, CMake files) don't end up
    # in the FxSound package.
    FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 6.1.6
      GIT_SHALLOW TRUE SOURCE_SUBDIR do-not-add)
    FetchContent_MakeAvailable(JUCE)
    add_subdirectory(${juce_SOURCE_DIR} ${CMAKE_BINARY_DIR}/JUCE EXCLUDE_FROM_ALL)
  endif()
  set(CMAKE_CXX_FLAGS "${_fx_saved_cxx_flags}")

  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake_gui_sources.cmake)
  juce_add_gui_app(FxSound
    PRODUCT_NAME "FxSound"
    COMPANY_NAME "FxSound LLC"
    VERSION 1.2.15)
  # Windows-only implementations replaced by Linux versions in linux/gui/.
  list(REMOVE_ITEM FXSOUND_GUI_SOURCES ${FX_ROOT}/fxsound/Source/Utils/SysInfo/SysInfo.cpp)
  target_sources(FxSound PRIVATE ${FXSOUND_GUI_SOURCES} ${FX_ROOT}/fxsound/JuceLibraryCode/BinaryData.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/gui/SysInfoLinux.cpp)

  # Use the Projucer-generated JuceHeader.h/AppConfig.h/BinaryData.h that the
  # Windows build uses, so both platforms get the same JUCE configuration.
  file(GLOB_RECURSE _gui_dirs LIST_DIRECTORIES true ${FX_ROOT}/fxsound/Source/*)
  set(_gui_inc ${FX_ROOT}/fxsound/Source)
  foreach(d ${_gui_dirs})
    if(IS_DIRECTORY ${d})
      list(APPEND _gui_inc ${d})
    endif()
  endforeach()
  target_include_directories(FxSound PRIVATE ${FX_ROOT}/fxsound/JuceLibraryCode ${_gui_inc}
    ${CMAKE_CURRENT_SOURCE_DIR}/dsp_support    # fx_registry.h
    ${CMAKE_CURRENT_SOURCE_DIR}/gui)           # FxInstanceChannel.h
  target_compile_definitions(FxSound PRIVATE
    JUCE_WEB_BROWSER=0            # no embedded browser is used; avoids a webkit2gtk dependency
    JUCE_MODAL_LOOPS_PERMITTED=1  # on by default on Windows; the GUI uses runModalLoop()
    JUCE_USE_CURL=0               # the app makes no HTTP requests (links open in the browser)
    JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1)
  target_compile_options(FxSound PRIVATE ${FX_JUCE_COMPAT_FLAGS})
  target_link_libraries(FxSound PRIVATE
    fxaudiopassthru dfxdsp
    juce::juce_core juce::juce_cryptography juce::juce_data_structures juce::juce_events
    juce::juce_graphics juce::juce_gui_basics juce::juce_gui_extra juce::juce_opengl
    juce::juce_recommended_config_flags)   # JUCE's extra warning flags only add noise on upstream code
endif()

if(FX_BUILD_GUI)
  # Factory presets live next to the executable, as on Windows.
  add_custom_command(TARGET FxSound POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${FX_ROOT}/bin/x64/Factsoft $<TARGET_FILE_DIR:FxSound>/Factsoft)
endif()

# ------------------------------------------------------------ install + .deb
# Layout: the app finds its factory presets next to the executable (as on
# Windows), so both live in /usr/lib/fxsound; /usr/bin/fxsound is a symlink.
if(FX_BUILD_GUI)
  include(GNUInstallDirs)
  install(TARGETS FxSound RUNTIME DESTINATION lib/fxsound)   # app-private dir, not multiarch
  install(DIRECTORY ${FX_ROOT}/bin/x64/Factsoft DESTINATION lib/fxsound)
  install(CODE "file(MAKE_DIRECTORY \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}\")
    execute_process(COMMAND \${CMAKE_COMMAND} -E create_symlink ../lib/fxsound/FxSound
      \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/fxsound\")")
  install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/packaging/fxsound.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/packaging/fxsound.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps)
  install(FILES ${FX_ROOT}/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/fxsound OPTIONAL)

  # Package version = the version the app itself reports (ProjectInfo::versionString).
  file(STRINGS ${FX_ROOT}/fxsound/JuceLibraryCode/JuceHeader.h _ver REGEX "versionString *= *\"[0-9.]+\"")
  string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" FX_APP_VERSION "${_ver}")

  set(CPACK_GENERATOR DEB)
  set(CPACK_PACKAGE_NAME fxsound)
  set(CPACK_PACKAGE_VERSION ${FX_APP_VERSION})
  set(CPACK_PACKAGE_VENDOR "FxSound LLC")
  set(CPACK_PACKAGE_CONTACT "FxSound <support@fxsound.com>")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Audio enhancer: equalizer and effects for all system audio")
  set(CPACK_DEBIAN_PACKAGE_DESCRIPTION "FxSound processes everything your computer plays.\n It adds an \"FxSound Audio Enhancer\" output device to PipeWire, applies\n the FxSound DSP (equalizer, clarity, ambience, surround, dynamic boost,\n bass boost) and plays the result on your speakers or headphones.")
  set(CPACK_DEBIAN_PACKAGE_SECTION sound)
  set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://www.fxsound.com")
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)          # library dependencies from the binary
  # pipewire/wireplumber: the audio server FxSound plugs into. The X11 libraries
  # are dlopen()ed by JUCE at runtime, so the automatic scan cannot see them.
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "pipewire, wireplumber, libx11-6, libxext6, libxrandr2, libxinerama1, libxcursor1")
  set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
  include(CPack)
endif()
