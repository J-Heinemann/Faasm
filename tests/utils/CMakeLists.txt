include_directories(${CMAKE_CURRENT_LIST_DIR})

set(LIB_FILES
        matrix_utils.cpp
        system_utils.cpp
        worker_utils.cpp
        utils.h
        )

faasm_private_lib(test_utils "${LIB_FILES}")

add_dependencies(test_utils catch2_ext)
target_link_libraries(test_utils
        faabric_test_utils
        faasm
        upload_lib
        faaslet_lib
        emulator
        )
