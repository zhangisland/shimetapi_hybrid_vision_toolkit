# Resolve the actual include directories instead of assuming every SDK header
# lives immediately under <prefix>/include. Keep normal CMake/sysroot lookup.
function(hv_find_x5_sdk output)
    set(_hints ${HV_X5_SDK_INCLUDE_DIRS}
        "${HV_X5_SDK_ROOT}/include" "${HV_X5_SDK_ROOT}/include/HAL"
        "${HV_PLATFORM_SAMPLES}/include")
    set(_includes "${HV_X5_SDK_ROOT}/include" "${HV_X5_SDK_ROOT}/include/HAL")
    foreach(_header hbn_isp_api.h hbn_api.h hb_camera_interface.h
                    vin_cfg.h isp_cfg.h n2d_cfg.h hb_camera_data_config.h cam_def.h)
        # Do not retain a successful path from a different --sdk-root in cache.
        unset(_hv_header_dir CACHE)
        unset(_hv_header_dir)
        find_path(_hv_header_dir NAMES "${_header}" HINTS ${_hints}
                  PATH_SUFFIXES HAL hobot hobot/HAL)
        if(NOT _hv_header_dir)
            message(FATAL_ERROR
                "X5 SDK header not found: ${_header}\n"
                "Searched SDK prefix ${HV_X5_SDK_ROOT}, include/HAL, sample include, "
                "explicit HV_X5_SDK_INCLUDE_DIRS and CMake system/sysroot paths.\n"
                "On the board run: find /usr/hobot /usr/include /app/multimedia_samples "
                "-type f -name '${_header}' 2>/dev/null\n"
                "Pass its containing directory with hvs.py build --sdk-include-dir DIR "
                "(repeat for split SDK headers). If absent, obtain the matching SDK "
                "development headers; do not substitute graphics headers.")
        endif()
        message(STATUS "X5 SDK ${_header}: ${_hv_header_dir}")
        list(APPEND _includes "${_hv_header_dir}")
    endforeach()
    unset(_hv_header_dir CACHE)
    list(REMOVE_DUPLICATES _includes)
    set(${output} ${_includes} PARENT_SCOPE)
endfunction()
