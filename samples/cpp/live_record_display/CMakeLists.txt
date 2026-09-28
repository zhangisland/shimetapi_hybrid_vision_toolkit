# live_record_display: MIPI-HVS 实时预览 + 按键录制（需 OpenCV）
if(CMAKE_CROSSCOMPILING)
    # 交叉编译时直接使用 toolkit 自带的 aarch64 OpenCV
    set(_AARCH64_OPENCV_DIR "${CMAKE_SOURCE_DIR}/third_party/aarch64_opencv")
    if(EXISTS "${_AARCH64_OPENCV_DIR}/include/opencv4/opencv2" AND
       EXISTS "${_AARCH64_OPENCV_DIR}/lib/aarch64-linux-gnu/libopencv_core.so")
        add_executable(hv_sample_live_record_display main.cpp live_widgets.cpp)
        target_include_directories(hv_sample_live_record_display PRIVATE
            "${_AARCH64_OPENCV_DIR}/include/opencv4")
        target_link_directories(hv_sample_live_record_display PRIVATE
            "${_AARCH64_OPENCV_DIR}/lib/aarch64-linux-gnu")
        target_link_libraries(hv_sample_live_record_display PRIVATE
            HVToolkit::shimetapi_hv
            HVToolkit::shimetapi_codec
            HVToolkit::shimetapi_io
            opencv_core opencv_imgproc opencv_highgui opencv_imgcodecs opencv_videoio)
        message(STATUS "live_record_display: using aarch64 OpenCV from third_party")
    else()
        message(STATUS "aarch64 OpenCV not found in third_party — skipping hv_sample_live_record_display")
    endif()
else()
    # 本地编译：通过 find_package 查找系统 OpenCV
    find_package(OpenCV QUIET)
    if(OpenCV_FOUND)
        add_executable(hv_sample_live_record_display main.cpp live_widgets.cpp)
        target_link_libraries(hv_sample_live_record_display PRIVATE
            HVToolkit::shimetapi_hv
            HVToolkit::shimetapi_codec
            HVToolkit::shimetapi_io
            ${OpenCV_LIBS})
        target_include_directories(hv_sample_live_record_display PRIVATE ${OpenCV_INCLUDE_DIRS})
    else()
        message(STATUS "OpenCV not found — skipping hv_sample_live_record_display")
    endif()
endif()

option(HV_X5_NATIVE "Build official-example-based dual-VC VIN + APS ISP live path" OFF)
set(HV_PLATFORM_SAMPLES "/app/multimedia_samples" CACHE PATH "Matching official multimedia_samples source tree")
set(HV_X5_SDK_ROOT "/usr/hobot" CACHE PATH "Matching X5 SDK prefix (include and lib)")
set(HV_X5_SDK_INCLUDE_DIRS "" CACHE STRING "Additional matching X5 SDK include directories")
if(HV_X5_NATIVE)
    if(NOT HV_TOOLKIT_ARCH_DIR STREQUAL "x5" OR NOT TARGET hv_sample_live_record_display)
        message(FATAL_ERROR "HV_X5_NATIVE requires x5 and OpenCV development libraries")
    endif()
    enable_language(C)
    set(_dual "${HV_PLATFORM_SAMPLES}/sample_apx003cc/sample_hvs/dual_vc_vin_src")
    include("${CMAKE_CURRENT_LIST_DIR}/FindX5Sdk.cmake")
    hv_find_x5_sdk(_x5_sdk_includes)
    foreach(_file "${HV_PLATFORM_SAMPLES}/vp_sensors/vp_sensors.c"
                  "${_dual}/src/hvs_config.c")
        if(NOT EXISTS "${_file}")
            message(FATAL_ERROR "Native capture needs matching SDK/sample source: ${_file}")
        endif()
    endforeach()
    # Same vp_sensors compilation set as the official Makefile; do not replace
    # the board's sensor driver or link stale sensor object files from a bundle.
    file(GLOB_RECURSE _sensor_sources CONFIGURE_DEPENDS "${HV_PLATFORM_SAMPLES}/vp_sensors/*.c")
    target_sources(hv_sample_live_record_display PRIVATE native_live.cpp x5_capture.c
        "${_dual}/src/hvs_config.c" ${_sensor_sources})
    target_compile_definitions(hv_sample_live_record_display PRIVATE HV_X5_NATIVE=1)
    target_include_directories(hv_sample_live_record_display PRIVATE
        ${_x5_sdk_includes}
        "${HV_PLATFORM_SAMPLES}/vp_sensors" "${_dual}/include")
    target_link_directories(hv_sample_live_record_display PRIVATE "${HV_X5_SDK_ROOT}/lib")
    target_link_libraries(hv_sample_live_record_display PRIVATE cam vpf hbmem pthread m)
endif()
