function(copyOutputs TARGET_FOLDER)
    # Copy the SKSE plugin .dll files into the SKSE/Plugins/ folder.
    set(DLL_FOLDER "${TARGET_FOLDER}/SKSE/Plugins")

    message(STATUS "SKSE plugin output folder: ${DLL_FOLDER}")

    add_custom_command(
        TARGET "${PROJECT_NAME}"
        POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DLL_FOLDER}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
        VERBATIM
    )

    if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        add_custom_command(
            TARGET "${PROJECT_NAME}"
            POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_PDB_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_PDB_FILE_NAME:${PROJECT_NAME}>"
            VERBATIM
        )
    endif()

    # Install packaged mod files. Never overwrites a file the user already has.
    if(EXISTS "${PROJECT_SOURCE_DIR}/data")
        add_custom_command(
            TARGET "${PROJECT_NAME}"
            POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -DSOURCE_DIR=${PROJECT_SOURCE_DIR}/data -DDEST_DIR=${TARGET_FOLDER} -P "${PROJECT_SOURCE_DIR}/cmake/lib/copyNewOnly.cmake"
            VERBATIM
        )
    endif()
endfunction()
