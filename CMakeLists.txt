cmake_minimum_required(VERSION 3.18)
project(minkcompat C CXX ASM)

set(QCBOR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../QCBOR CACHE PATH "QCBOR source tree")
set(QUIC_TEEC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../quic-teec CACHE PATH "quic-teec source tree")
set(MINKIPC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../minkipc CACHE PATH "minkipc source tree")

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(qcbor_static STATIC
	${QCBOR_DIR}/src/UsefulBuf.c
	${QCBOR_DIR}/src/qcbor_encode.c
	${QCBOR_DIR}/src/qcbor_decode.c
	${QCBOR_DIR}/src/ieee754.c
	${QCBOR_DIR}/src/qcbor_err_to_str.c)
target_include_directories(qcbor_static PUBLIC ${QCBOR_DIR}/inc)

add_library(qcomtee_static STATIC
	${QUIC_TEEC_DIR}/libqcomtee/src/qcomtee_object.c
	${QUIC_TEEC_DIR}/libqcomtee/src/objects/credentials_obj.c
	${QUIC_TEEC_DIR}/libqcomtee/src/objects/mem_obj.c)
target_compile_definitions(qcomtee_static PRIVATE USE_QCBOR)
target_include_directories(qcomtee_static
	PUBLIC ${QUIC_TEEC_DIR}/libqcomtee/include
	PRIVATE ${QUIC_TEEC_DIR}/libqcomtee/src)
target_link_libraries(qcomtee_static PRIVATE qcbor_static)

add_library(minkadaptor_static STATIC
	${MINKIPC_DIR}/libminkadaptor/src/supplicant.c
	${MINKIPC_DIR}/libminkadaptor/src/mink_adaptor.c
	${MINKIPC_DIR}/libminkadaptor/src/syscall.S)
target_include_directories(minkadaptor_static
	PUBLIC ${MINKIPC_DIR}/libminkadaptor/include
	PRIVATE ${MINKIPC_DIR}/libminkadaptor/src)
target_link_libraries(minkadaptor_static PUBLIC qcomtee_static)

add_library(minkdescriptor SHARED
	src/tzcom.cpp
	src/timed_retry_forwarder.cpp
	src/fdwrapper.cpp)
target_link_libraries(minkdescriptor PRIVATE minkadaptor_static qcbor_static log)
target_link_options(minkdescriptor PRIVATE
	-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/libminkdescriptor.map
	-Wl,-soname,libminkdescriptor.so)

add_executable(minkcompat_test test/minkcompat_test.cpp)
target_include_directories(minkcompat_test PRIVATE ${MINKIPC_DIR}/libminkadaptor/include)
target_link_libraries(minkcompat_test PRIVATE minkdescriptor)
