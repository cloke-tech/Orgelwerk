# Verovio (music engraving + notation formats) as a static library.
set(VRV_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/verovio)

file(GLOB vrv_SRC
    ${VRV_DIR}/src/*.cpp
    ${VRV_DIR}/libmei/dist/*.cpp
    ${VRV_DIR}/libmei/addons/*.cpp
    ${VRV_DIR}/src/hum/*.cpp
    ${VRV_DIR}/src/midi/*.cpp
    ${VRV_DIR}/src/crc/*.cpp)

add_library(verovio_static STATIC ${vrv_SRC}
    ${VRV_DIR}/src/json/jsonxx.cc
    ${VRV_DIR}/src/pugi/pugixml.cpp)

set_target_properties(verovio_static PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)

set(VRV_INCLUDES
    ${VRV_DIR}/include ${VRV_DIR}/include/crc ${VRV_DIR}/include/midi
    ${VRV_DIR}/include/hum ${VRV_DIR}/include/json ${VRV_DIR}/include/pugi
    ${VRV_DIR}/include/tuning-library ${VRV_DIR}/include/zip ${VRV_DIR}/include/vrv
    ${VRV_DIR}/libmei/dist ${VRV_DIR}/libmei/addons)
if(WIN32)
    list(APPEND VRV_INCLUDES ${VRV_DIR}/include/win32)
endif()
target_include_directories(verovio_static PUBLIC ${VRV_INCLUDES})

target_compile_definitions(verovio_static PRIVATE
    NO_DARMS_SUPPORT NO_RUNTIME RESOURCE_DIR="./data")
if(MSVC)
    target_compile_options(verovio_static PRIVATE /bigobj /W0 /utf-8)
    target_compile_definitions(verovio_static PRIVATE NO_PAE_SUPPORT USE_PAE_OLD_PARSER _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    target_compile_options(verovio_static PRIVATE -w)
endif()
