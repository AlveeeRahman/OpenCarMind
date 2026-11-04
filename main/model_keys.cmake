# Writes carmind_model_keys.h into the build folder from the environment of the build. Run as a script at every build
# (main/CMakeLists.txt), so a key added or removed takes effect without a reconfigure.
#
#   cmake -DOUT=<header> -DMODELS="EV|<file>[|REV|<file>]" [-DPYTHON=<python> -DCHECK=<tools/check_model_key.py>] -P model_keys.cmake
#
# For each model id, CARMIND_MODEL_KEY_<id> is either unset/empty (that model stays locked) or 32 hex characters: the
# AES-128 key of its encrypted .espdl. The key goes only into the generated header, never into the source tree, the
# CMake cache or the log. The header rewrites itself only when its content changes, so builds without a key change do not
# recompile anything.
cmake_minimum_required(VERSION 3.16)

set(body "/* Generated at build time by main/model_keys.cmake from CARMIND_MODEL_KEY_*. Lives in the build folder only. */\n#pragma once\n#include <stdint.h>\n")
string(REPLACE "|" ";" MODELS "${MODELS}")
list(LENGTH MODELS n)
math(EXPR last "${n} - 1")
foreach(i RANGE 0 ${last} 2)
    math(EXPR j "${i} + 1")
    list(GET MODELS ${i} id)
    list(GET MODELS ${j} model_file)
    set(var "CARMIND_MODEL_KEY_${id}")
    string(STRIP "$ENV{${var}}" key)
    if(key STREQUAL "")
        string(APPEND body "\n#define ML_KEY_${id}_PRESENT 0\n")
        message(STATUS "CarMind model ${id}: no key, this model will be locked")
        continue()
    endif()
    string(LENGTH "${key}" len)
    if(NOT len EQUAL 32 OR NOT key MATCHES "^[0-9a-fA-F]+$")
        message(FATAL_ERROR "${var} is set but is not 32 hex characters (the AES-128 key of the model). Unset it to build without the model.")
    endif()
    if(PYTHON AND CHECK)
        # the key reaches the check through the environment this script inherited, never on a command line
        execute_process(COMMAND "${PYTHON}" "${CHECK}" "${model_file}" "${var}" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
        string(STRIP "${out}" out)
        if(rc EQUAL 1)
            message(FATAL_ERROR "${out}")
        endif()
        message(STATUS "CarMind model ${id}: ${out}")
    endif()
    set(bytes "")
    foreach(k RANGE 0 30 2)
        string(SUBSTRING "${key}" ${k} 2 b)
        string(APPEND bytes "0x${b}, ")
    endforeach()
    string(APPEND body "\n#define ML_KEY_${id}_PRESENT 1\nstatic const uint8_t ML_KEY_${id}[16] = {${bytes}};\n")
endforeach()

set(old "")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL body)
    file(WRITE "${OUT}" "${body}")
endif()
