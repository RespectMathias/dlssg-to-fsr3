cmake_minimum_required(VERSION 3.26)

project(
    dlssg-to-fsr
    VERSION 1.0
    LANGUAGES CXX
)

option(BUILD_TESTING "Build verification targets" OFF)

set(PROJECT_DEPENDENCIES_PATH "${CMAKE_CURRENT_LIST_DIR}/external")
set(PROJECT_RESOURCES_PATH "${CMAKE_CURRENT_LIST_DIR}/resources")
set(PROJECT_SRC_PATH "${CMAKE_CURRENT_LIST_DIR}/src")
set(FIDELITYFX_API_DLL "${PROJECT_DEPENDENCIES_PATH}/FidelityFX-SDK/PrebuiltSignedDLL/amd_fidelityfx_dx12.dll")

#
# Store the current git commit hash for later use
#
execute_process(
    COMMAND git log -1 --format=%h
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
    OUTPUT_VARIABLE BUILD_GIT_COMMIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

#
# Always build the library
#
add_subdirectory("${PROJECT_SRC_PATH}")

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

#
# Then install everything
#
set(CPACK_PACKAGE_FILE_NAME "${CMAKE_PROJECT_NAME}-${CMAKE_PROJECT_VERSION_MAJOR}.${CMAKE_PROJECT_VERSION_MINOR}")

install(TARGETS dlssg_to_fsr RUNTIME DESTINATION "/")
install(FILES "${FIDELITYFX_API_DLL}" DESTINATION "/")
install(
    FILES
        "${PROJECT_RESOURCES_PATH}/windows_install.bat"
        "${PROJECT_RESOURCES_PATH}/DisableNvidiaSignatureChecks.reg"
        "${PROJECT_RESOURCES_PATH}/RestoreNvidiaSignatureChecks.reg"
        "${PROJECT_RESOURCES_PATH}/dlssg_to_fsr.ini"
        "${PROJECT_RESOURCES_PATH}/README_EXTRACT_FILES_TO_GAME_FOLDER.txt"
    DESTINATION "/"
)
install(
    FILES "${PROJECT_RESOURCES_PATH}/linux_install.sh"
    DESTINATION "/"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
)

install(DIRECTORY "${PROJECT_RESOURCES_PATH}/licenses" DESTINATION "/")

set(CPACK_GENERATOR "ZIP")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
include(CPack)
