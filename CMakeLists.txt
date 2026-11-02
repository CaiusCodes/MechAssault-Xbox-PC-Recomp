# MechAssault (original Xbox), statically recompiled

cmake_minimum_required(VERSION 3.25)
# mechassault-recomp: the C runtime linked in, so a release's root holds only MechAssault.exe (no VC runtime DLLs)
option(MECHASSAULT_STATIC_CRT "Link the C runtime statically" ON)
if(MECHASSAULT_STATIC_CRT)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
project(game LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# the SDK: sdk/ here, two folders up inside the SDK's templates, a checkout beside this repository, else downloaded
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sdk/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../..)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp)
else()
    include(FetchContent)
    FetchContent_Declare(xbe_sdk GIT_REPOSITORY https://github.com/TGP482/XboxXBE-Recomp.git GIT_TAG main)
    FetchContent_MakeAvailable(xbe_sdk)
    set(SDK ${xbe_sdk_SOURCE_DIR})
endif()
include(${SDK}/cmake/XbeRecomp.cmake)

file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/game/")

# partitions as xbe_symbols wrote them into config/game.toml
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS config/game.toml)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/config/game.toml)
    file(STRINGS config/game.toml GAME_PARTITIONS REGEX "^partitions *=")
    string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${GAME_PARTITIONS}")
endif()
# no config yet: build.bat writes it with xbe_symbols, the change reconfigures
if(NOT GAME_PARTITIONS)
    set(GAME_PARTITIONS 8)
endif()

# target name = GAME_EXE in build.bat, xbe = GAME_XBE
# mechassault-recomp: target must match library = "MechAssault.dll" in game.toml, and main.toml pulls in mechassault.toml
add_xbe_image(MechAssault default.xbe main.toml ${GAME_PARTITIONS} game)

# mechassault-recomp: no icon resource yet (res/ is empty); the window uses the default icon

# mechassault-recomp: development builds. Relinking only the executable leaves MechAssault.dll older than it, and the
# executable's first-start check (runtime/app/game_code.cpp) would then rebuild the DLL itself on every launch. Ninja
# relinks the DLL after this whenever the executable's exports change, so marking it current here is safe.
add_custom_command(TARGET MechAssault POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E touch_nocreate $<TARGET_FILE_DIR:MechAssault>/MechAssault.dll
    VERBATIM)
