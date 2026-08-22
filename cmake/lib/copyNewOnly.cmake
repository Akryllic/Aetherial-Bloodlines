# Copy every file under SOURCE_DIR into DEST_DIR, skipping any that already exist.
#
# The blanket copy_directory this replaces overwrote the shipped INI on every build, which silently
# discarded whatever the player had tuned in game. A deployed INI belongs to the user, not the repo.
# Run with: cmake -DSOURCE_DIR=... -DDEST_DIR=... -P copyNewOnly.cmake

file(GLOB_RECURSE _files RELATIVE "${SOURCE_DIR}" "${SOURCE_DIR}/*")

foreach(_rel IN LISTS _files)
    set(_dest "${DEST_DIR}/${_rel}")
    if(EXISTS "${_dest}")
        message(STATUS "keeping existing ${_rel}")
    else()
        get_filename_component(_destdir "${_dest}" DIRECTORY)
        file(MAKE_DIRECTORY "${_destdir}")
        file(COPY "${SOURCE_DIR}/${_rel}" DESTINATION "${_destdir}")
        message(STATUS "installed ${_rel}")
    endif()
endforeach()
