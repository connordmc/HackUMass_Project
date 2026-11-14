cmake_minimum_required(VERSION 3.16)
project(faceshield_kv260 CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(FACESHIELD_WITH_VITIS "Build the Vitis AI DPU face detector" ON)

find_package(OpenCV REQUIRED COMPONENTS core imgproc)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
  gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)

add_library(faceshield_core STATIC src/censor.cpp src/pipeline.cpp)
target_include_directories(faceshield_core PUBLIC src ${OpenCV_INCLUDE_DIRS})
target_link_libraries(faceshield_core PUBLIC ${OpenCV_LIBS})
target_compile_options(faceshield_core PRIVATE -Wall -Wextra)

add_executable(faceshield_kv260 src/main.cpp src/detector.cpp)
target_link_libraries(faceshield_kv260 PRIVATE faceshield_core PkgConfig::GST)
target_compile_options(faceshield_kv260 PRIVATE -Wall -Wextra)

if(FACESHIELD_WITH_VITIS)
  # Vitis AI Library, as installed on the KV260 image / Smart Camera container.
  find_path(VITIS_INCLUDE vitis/ai/facedetect.hpp)
  find_library(VITIS_FACEDETECT vitis_ai_library-facedetect)
  if(NOT VITIS_INCLUDE OR NOT VITIS_FACEDETECT)
    message(FATAL_ERROR
      "Vitis AI Library face detection not found. Build on the KV260 "
      "(see README.md) or configure with -DFACESHIELD_WITH_VITIS=OFF.")
  endif()
  target_include_directories(faceshield_kv260 PRIVATE ${VITIS_INCLUDE})
  target_link_libraries(faceshield_kv260 PRIVATE ${VITIS_FACEDETECT})
  foreach(lib vitis_ai_library-dpu_task vitis_ai_library-model_config
              vitis_ai_library-xnnpp vitis_ai_library-math vart-util xir glog)
    find_library(VITIS_LIB_${lib} ${lib})
    if(VITIS_LIB_${lib})
      target_link_libraries(faceshield_kv260 PRIVATE ${VITIS_LIB_${lib}})
    endif()
  endforeach()
  target_compile_definitions(faceshield_kv260 PRIVATE FACESHIELD_HAVE_VITIS)
endif()

enable_testing()
add_executable(test_censor tests/test_censor.cpp)
target_link_libraries(test_censor PRIVATE faceshield_core)
add_test(NAME censor COMMAND test_censor)
