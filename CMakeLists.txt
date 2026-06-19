#
# Rosalie's Mupen GUI CMakeLists.txt
#
cmake_minimum_required(VERSION 3.15)
cmake_policy(SET CMP0177 NEW)

option(PORTABLE_INSTALL "Portable Installation" ON)
option(UPDATER          "Enables updater" ${WIN32})
option(APPIMAGE_UPDATER "Enables AppImage updater" OFF)
option(KCA_DRAG_DROP    "Enables drag and drop using KCoreAddons" OFF) # https://bugreports.qt.io/browse/QTBUG-91357
option(NETPLAY          "Enables netplay" ON)
option(VRU              "Enables VRU support in RMG-Input" ON)
option(USE_CCACHE       "Enables usage of ccache when ccache has been found" ON)
option(NO_ASM           "Disables the usage of assembly in the mupen64plus-core" OFF)
option(USE_ANGRYLION    "Enables building angrylion-rdp-plus which uses a non-GPL compliant license" OFF)
option(POWERPC64_DYNAREC "Enables PowerPC64 new dynamic recompiler (ppc64)" OFF)
option(DYNAREC          "Enables dynamic recompiler (auto-detected for supported architectures)" OFF)

project(RMG)

# Auto-detect PPC64 big-endian
# Tries several methods: CMAKE_SYSTEM_PROCESSOR, compiler target, and preprocessor
include(CheckCSourceCompiles)
set(_POWERPC64_DYNAREC_DEFAULT OFF)
message(STATUS "Host processor: ${CMAKE_SYSTEM_PROCESSOR}")

# Method 1: CMAKE_SYSTEM_PROCESSOR from uname (reliable for native builds)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(powerpc|ppc)64$")
    message(STATUS "Detected PPC64 via CMAKE_SYSTEM_PROCESSOR")
    set(_POWERPC64_DYNAREC_DEFAULT ON)
endif()

# Method 2: CMAKE_C_COMPILER_TARGET from toolchain (reliable for cross-compilation)
# Exclude "le" suffix to avoid enabling for PPC64LE
if(NOT _POWERPC64_DYNAREC_DEFAULT AND CMAKE_C_COMPILER_TARGET MATCHES "powerpc64|ppc64")
    if(NOT CMAKE_C_COMPILER_TARGET MATCHES "le$")
        message(STATUS "Detected PPC64 via CMAKE_C_COMPILER_TARGET (${CMAKE_C_COMPILER_TARGET})")
        set(_POWERPC64_DYNAREC_DEFAULT ON)
    endif()
endif()

# Method 3: Check preprocessor defines via try_compile
# (reliable for both native and cross-compilation with most toolchains)
if(NOT _POWERPC64_DYNAREC_DEFAULT)
    check_c_source_compiles("
        #if !defined(__powerpc64__) && !defined(__ppc64__) && !defined(_ARCH_PPC64)
        #error not ppc64
        #endif
        #if defined(__LITTLE_ENDIAN__) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
        #error not big-endian
        #endif
        int main(void) { return 0; }
    " HAVE_PPC64_BE)
    if(HAVE_PPC64_BE)
        message(STATUS "Detected PPC64 big-endian via compiler preprocessor")
        set(_POWERPC64_DYNAREC_DEFAULT ON)
    endif()
endif()

if(_POWERPC64_DYNAREC_DEFAULT)
    set(POWERPC64_DYNAREC ON CACHE BOOL "Enables PowerPC64 new dynamic recompiler (ppc64)" FORCE)
    message(STATUS "POWERPC64_DYNAREC: ON (auto-detected)")
else()
    message(STATUS "POWERPC64_DYNAREC: OFF (not detected)")
endif()

find_package(Git)
if (GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE GIT_RESULT
        OUTPUT_VARIABLE GIT_VERSION
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif(GIT_FOUND)
if (NOT GIT_FOUND OR NOT ${GIT_RESULT} EQUAL 0)
    if (NOT ${GIT_RESULT} EQUAL 0)
        message(WARNING "git failed to retrieve the current revision, falling back to VERSION file!")
    endif()
    file(STRINGS "VERSION" GIT_VERSION)
endif()

if (USE_CCACHE)
    find_program(CCACHE_FOUND ccache)
    if (CCACHE_FOUND)
        set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE ccache)
    else(CCACHE_FOUND)
        set(USE_CCACHE OFF)
    endif(CCACHE_FOUND)
endif(USE_CCACHE)

if (NOT PORTABLE_INSTALL AND WIN32)
    message(WARNING "Not-Portable build not supported on windows, re-enabling PORTABLE_INSTALL!")
    set(PORTABLE_INSTALL ON)
endif()

if (NOT PORTABLE_INSTALL AND UPDATER AND NOT APPIMAGE_UPDATER)
    message(WARNING "Not-Portable builds don't support the updater, disabling UPDATER")
    set(UPDATER OFF)
endif()

if (APPIMAGE_UPDATER AND WIN32)
    message(WARNING "AppImage updater not supported on windows, disabling APPIMAGE_UPDATER!")
    set(APPIMAGE_UPDATER OFF)
endif()

if (UPDATER AND NOT WIN32 AND NOT APPIMAGE_UPDATER)
    message(WARNING "Only AppImage updater is supported on linux, disabling UPDATER!")
    set(UPDATER OFF)
endif()

if (FORCE_XCB AND WIN32)
    message(WARNING "XCB Qt platform not supported on windows, disabling FORCE_XCB!")
    set(FORCE_XCB OFF)
endif()

if (PORTABLE_INSTALL)
    set(CMAKE_INSTALL_PREFIX "")
    set(INSTALL_PATH "Bin/${CMAKE_BUILD_TYPE}")
    set(RMG_INSTALL_PATH "${INSTALL_PATH}")
    set(SYSTEM_LIB_INSTALL_PATH "${INSTALL_PATH}")
    set(LIB_INSTALL_PATH "${INSTALL_PATH}")
    set(CORE_INSTALL_PATH "${INSTALL_PATH}/Core")
    set(PLUGIN_INSTALL_PATH "${INSTALL_PATH}/Plugin")
    set(DATA_INSTALL_PATH "${INSTALL_PATH}/Data")
    set(INSTALL_DESKTOP_FILE OFF)
else()
    include(GNUInstallDirs)
    set(RMG_INSTALL_PATH "${CMAKE_INSTALL_BINDIR}")
    set(SYSTEM_LIB_INSTALL_PATH "${CMAKE_INSTALL_LIBDIR}")
    set(LIB_INSTALL_PATH "${SYSTEM_LIB_INSTALL_PATH}/RMG")
    set(CORE_INSTALL_PATH "${SYSTEM_LIB_INSTALL_PATH}/RMG/Core")
    set(PLUGIN_INSTALL_PATH "${SYSTEM_LIB_INSTALL_PATH}/RMG/Plugin")
    set(DATA_INSTALL_PATH "${CMAKE_INSTALL_DATADIR}/RMG")
    set(INSTALL_DESKTOP_FILE ON)
    set(METAINFO_INSTALL_PATH "${CMAKE_INSTALL_DATADIR}/metainfo/")
    set(DESKTOP_INSTALL_PATH "${CMAKE_INSTALL_DATADIR}/applications")
    set(ICON_INSTALL_PATH "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps/")
endif()

add_subdirectory(Source/3rdParty)
add_subdirectory(Source/3rdParty/lzma)
if (VRU)
    add_subdirectory(Source/3rdParty/vosk-api)
endif(VRU)

add_subdirectory(Source/RMG-Core)
add_subdirectory(Source/RMG)
add_subdirectory(Source/RMG-Audio)
add_subdirectory(Source/RMG-Input)
add_subdirectory(Source/RMG-Input-GCA)
install(TARGETS RMG-Core
    DESTINATION ${SYSTEM_LIB_INSTALL_PATH}
)
install(TARGETS RMG
    DESTINATION ${RMG_INSTALL_PATH}
)
install(TARGETS RMG-Audio
    DESTINATION ${PLUGIN_INSTALL_PATH}/Audio
)
install(TARGETS RMG-Input RMG-Input-GCA
    DESTINATION ${PLUGIN_INSTALL_PATH}/Input
)

if (WIN32)
    add_subdirectory(Source/Installer)

    add_custom_target(bundle_dependencies
        COMMAND bash "${CMAKE_SOURCE_DIR}/Source/Script/BundleDependencies.sh" "${CMAKE_SOURCE_DIR}/Bin/${CMAKE_BUILD_TYPE}/RMG.exe" "${CMAKE_SOURCE_DIR}/Bin/${CMAKE_BUILD_TYPE}"
	)
endif(WIN32)

set(LIB_PERMISSIONS OWNER_READ OWNER_EXECUTE OWNER_WRITE
                    GROUP_READ GROUP_EXECUTE
                    WORLD_READ WORLD_EXECUTE)

install(FILES ${MUPEN64PLUSCORE_LIB}
    DESTINATION ${CORE_INSTALL_PATH}
    PERMISSIONS ${LIB_PERMISSIONS}
)
install(FILES ${MUPEN64PLUSCORE_INI} Data/font.ttf
    DESTINATION ${DATA_INSTALL_PATH}
)
if (VRU)
    install(FILES ${VOSK_API_LIBRARY}
        DESTINATION ${LIB_INSTALL_PATH}
        PERMISSIONS ${LIB_PERMISSIONS}
    )
    install(FILES ${VOSK_API_MODEL}
        DESTINATION ${DATA_INSTALL_PATH}
    )
endif(VRU)
install(FILES ${SDL_GAMECONTROLLERDB} ${CMAKE_SOURCE_DIR}/Data/InputProfileDB.json
    DESTINATION ${DATA_INSTALL_PATH}
)
file(GLOB GENERATED_CHEAT_FILES ${CMAKE_SOURCE_DIR}/Data/Cheats/Generated/*.cht)
file(GLOB CUSTOM_CHEAT_FILES ${CMAKE_SOURCE_DIR}/Data/Cheats/Custom/*.cht)
install(FILES ${GENERATED_CHEAT_FILES} ${CUSTOM_CHEAT_FILES}
    DESTINATION ${DATA_INSTALL_PATH}/Cheats
)
install(FILES ${MUPEN64PLUS_PLUGIN_RSP_CXD4}
    DESTINATION ${PLUGIN_INSTALL_PATH}/RSP
    PERMISSIONS ${LIB_PERMISSIONS}
)
install(FILES ${MUPEN64PLUS_PLUGIN_RSP_HLE}
    DESTINATION ${PLUGIN_INSTALL_PATH}/RSP
    PERMISSIONS ${LIB_PERMISSIONS}
)
if (NOT POWERPC64_DYNAREC)
    install(FILES ${MUPEN64PLUS_PLUGIN_RSP_PARALLEL}
        DESTINATION ${PLUGIN_INSTALL_PATH}/RSP
        PERMISSIONS ${LIB_PERMISSIONS}
    )
endif()
install(FILES ${MUPEN64PLUS_PLUGIN_INPUT_RAPHNET}
    DESTINATION ${PLUGIN_INSTALL_PATH}/Input
    PERMISSIONS ${LIB_PERMISSIONS}
)
if (USE_ANGRYLION)
    install(FILES ${MUPEN64PLUS_PLUGIN_GFX_ANGRYLION}
        DESTINATION ${PLUGIN_INSTALL_PATH}/GFX
        PERMISSIONS ${LIB_PERMISSIONS}
    )
endif(USE_ANGRYLION)
install(FILES ${MUPEN64PLUS_PLUGIN_GFX_GLIDEN64}
    DESTINATION ${PLUGIN_INSTALL_PATH}/GFX
    PERMISSIONS ${LIB_PERMISSIONS}
)
install(FILES ${MUPEN64PLUS_PLUGIN_GFX_GLIDEN64_DATA}
    DESTINATION ${DATA_INSTALL_PATH}
)
install(FILES ${MUPEN64PLUS_PLUGIN_GFX_RICE}
    DESTINATION ${PLUGIN_INSTALL_PATH}/GFX
    PERMISSIONS ${LIB_PERMISSIONS}
)
install(FILES ${MUPEN64PLUS_PLUGIN_GFX_PARALLEL}
    DESTINATION ${PLUGIN_INSTALL_PATH}/GFX
    PERMISSIONS ${LIB_PERMISSIONS}
)

if (INSTALL_DESKTOP_FILE)
    install(FILES Package/com.github.Rosalie241.RMG.desktop
        DESTINATION ${DESKTOP_INSTALL_PATH}
    )
    install(FILES Package/com.github.Rosalie241.RMG.svg
        DESTINATION ${ICON_INSTALL_PATH}
    )
    install(FILES Package/com.github.Rosalie241.RMG.metainfo.xml
        DESTINATION ${METAINFO_INSTALL_PATH}
    )
endif(INSTALL_DESKTOP_FILE)
