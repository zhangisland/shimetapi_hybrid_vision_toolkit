find_package(Threads REQUIRED)
add_executable(hv_hvs_record main.cpp)
target_link_libraries(hv_hvs_record PRIVATE
    HVToolkit::shimetapi_hv HVToolkit::shimetapi_io Threads::Threads)
