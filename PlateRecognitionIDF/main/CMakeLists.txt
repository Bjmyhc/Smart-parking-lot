set(srcs main.cpp)
# esp-dl: 端侧推理框架; esp32-camera: 仓库内本地组件 (PlateRecognitionIDF/components/esp32-camera),
# 相机参数与 PlateRecognition/PlateRecognition.ino 的 vCameraInit() 逐行一致
set(requires esp-dl esp32-camera esp_jpeg)

idf_build_get_property(component_targets __COMPONENT_TARGETS)
if ("___idf_espressif__esp-dl" IN_LIST component_targets)
    idf_component_get_property(espdl_dir espressif__esp-dl COMPONENT_DIR)
elseif ("___idf_esp-dl" IN_LIST component_targets)
    idf_component_get_property(espdl_dir esp-dl COMPONENT_DIR)
endif()
set(cmake_dir ${espdl_dir}/fbs_loader/cmake)
include(${cmake_dir}/utilities.cmake)

set(embed_files
    ${CMAKE_CURRENT_LIST_DIR}/models/lprnet_s3.espdl
    ${CMAKE_CURRENT_LIST_DIR}/models/test_input.bin
    ${CMAKE_CURRENT_LIST_DIR}/models/probe_crop.bin
    ${CMAKE_CURRENT_LIST_DIR}/models/prov_s3.espdl)

idf_component_register(SRCS ${srcs}
                       REQUIRES ${requires}
                       INCLUDE_DIRS ".")

# 注意: target_add_aligned_binary_data 一次只处理一个文件, 多文件需逐个调用
foreach(f ${embed_files})
    target_add_aligned_binary_data(${COMPONENT_LIB} ${f} BINARY)
endforeach()