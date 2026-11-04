# mechassault-recomp: writes build_info.h (the version from VERSION and the commit) at every build, touching it only when
# it changes. cmake -DROOT=<repo> -DOUT=<header> -P cmake/build_info.cmake
file(READ "${ROOT}/VERSION" version)
string(STRIP "${version}" version)
execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY "${ROOT}" OUTPUT_VARIABLE commit
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git status --porcelain --untracked-files=no WORKING_DIRECTORY "${ROOT}" OUTPUT_VARIABLE dirty
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT commit)
    set(commit "local")
elseif(dirty)
    set(commit "${commit}+")
endif()
set(text "#pragma once\n#define MECHASSAULT_VERSION \"${version}\"\n#define MECHASSAULT_COMMIT \"${commit}\"\n")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
    file(WRITE "${OUT}" "${text}")
endif()
