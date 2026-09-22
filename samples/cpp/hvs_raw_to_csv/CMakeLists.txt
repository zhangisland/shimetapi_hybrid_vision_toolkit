add_executable(hv_hvs_raw_to_csv main.cpp)
target_link_libraries(hv_hvs_raw_to_csv PRIVATE
    HVToolkit::shimetapi_io HVToolkit::shimetapi_codec)
