# Pinned wake word models. models.lock.json in this directory names every
# microWakeWord model the firmware may embed, with the digest (sha256:<hex>) of its .tflite
# file and of its .json manifest. kbx_mww_verify_models(<dir> <id>...) runs at
# configure time, before any model is embedded, and stops the build when
#   - the lock is missing or has another schema,
#   - a requested <id> has no lock entry,
#   - a requested model or manifest is missing or its digest differs from the lock,
#   - any .tflite file in <dir> is not in the lock (no loose models).
# The lock and the verified files are configure dependencies, so changing one
# re-runs the check. A model reaches this directory only through the training
# pipeline's pin step (see README.md).
if(CMAKE_VERSION VERSION_LESS 3.19)
    message(FATAL_ERROR "kbx_wake: verifying models.lock.json needs CMake 3.19 or newer (string(JSON))")
endif()

function(kbx_mww_verify_file dir entry id name_key digest_key)
    string(JSON name ERROR_VARIABLE err GET "${entry}" "${name_key}")
    if(err)
        message(FATAL_ERROR "kbx_wake: models.lock.json entry '${id}' has no '${name_key}'")
    endif()
    string(JSON want ERROR_VARIABLE err GET "${entry}" "${digest_key}")
    if(err)
        message(FATAL_ERROR "kbx_wake: models.lock.json entry '${id}' has no '${digest_key}'")
    endif()
    if(NOT EXISTS "${dir}/${name}")
        message(FATAL_ERROR "kbx_wake: pinned wake model file ${dir}/${name} is missing")
    endif()
    file(SHA256 "${dir}/${name}" hex)
    set(got "sha256:${hex}")
    if(NOT got STREQUAL want)
        message(FATAL_ERROR "kbx_wake: ${dir}/${name} has SHA-256 ${got}, models.lock.json pins ${want}. "
                            "Pin a retrained model with the training pipeline's pin step; never copy one in.")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${dir}/${name}")
endfunction()

function(kbx_mww_verify_models dir)
    set(lock_file "${dir}/models.lock.json")
    if(NOT EXISTS "${lock_file}")
        message(FATAL_ERROR "kbx_wake: ${lock_file} is missing; wake models are pinned build inputs")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${lock_file}")
    file(READ "${lock_file}" lock)
    string(JSON schema ERROR_VARIABLE err GET "${lock}" schema)
    if(err OR NOT schema STREQUAL "kombify.box.wake-models.v1")
        message(FATAL_ERROR "kbx_wake: ${lock_file} is not a kombify.box.wake-models.v1 lock")
    endif()
    string(JSON models GET "${lock}" models)
    string(JSON count LENGTH "${models}")

    set(locked_files)
    if(count GREATER 0)
        math(EXPR last "${count} - 1")
        foreach(i RANGE ${last})
            string(JSON id MEMBER "${models}" ${i})
            string(JSON file_name GET "${models}" "${id}" file)
            list(APPEND locked_files "${file_name}")
        endforeach()
    endif()
    file(GLOB present CONFIGURE_DEPENDS RELATIVE "${dir}" "${dir}/*.tflite")
    foreach(file_name IN LISTS present)
        if(NOT file_name IN_LIST locked_files)
            message(FATAL_ERROR "kbx_wake: ${dir}/${file_name} is not pinned in models.lock.json")
        endif()
    endforeach()

    foreach(id IN LISTS ARGN)
        string(JSON entry ERROR_VARIABLE err GET "${models}" "${id}")
        if(err)
            message(FATAL_ERROR "kbx_wake: wake model '${id}' is not pinned in ${lock_file}")
        endif()
        kbx_mww_verify_file("${dir}" "${entry}" "${id}" file sha256)
        kbx_mww_verify_file("${dir}" "${entry}" "${id}" manifest manifest_sha256)
    endforeach()
    list(LENGTH ARGN verified)
    message(STATUS "kbx_wake: ${verified} wake models match models.lock.json")
endfunction()
