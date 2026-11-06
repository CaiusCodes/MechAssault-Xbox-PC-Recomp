# mechassault-recomp: writes build_info.h at every build, touching it only when it changes: the version from VERSION,
# the commit (+ for changes not committed) and git describe as people read it ("v0.5.1+3 (abc1234)", the tag alone on
# a tagged commit, * for changes not committed, empty outside git).
# cmake -DROOT=<repo> -DOUT=<header> -P cmake/build_info.cmake
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
execute_process(COMMAND git describe --tags --long --always WORKING_DIRECTORY "${ROOT}" OUTPUT_VARIABLE describe
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
set(pretty "")
if(describe MATCHES "^(.+)-([0-9]+)-g([0-9a-f]+)$")
    if(CMAKE_MATCH_2 STREQUAL "0")
        set(pretty "${CMAKE_MATCH_1}")
    else()
        set(pretty "${CMAKE_MATCH_1}+${CMAKE_MATCH_2} (${CMAKE_MATCH_3})")
    endif()
elseif(describe)
    set(pretty "${describe}")
endif()
if(pretty AND dirty)
    set(pretty "${pretty}*")
endif()
set(text "#pragma once\n#define MECHASSAULT_VERSION \"${version}\"\n#define MECHASSAULT_COMMIT \"${commit}\"\n#define MECHASSAULT_DESCRIBE \"${pretty}\"\n")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
    file(WRITE "${OUT}" "${text}")
endif()
