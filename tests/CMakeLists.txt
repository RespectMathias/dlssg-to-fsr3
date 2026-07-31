find_package(GTest CONFIG REQUIRED)
find_package(Vulkan REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_program(
	SPIRV_VAL_EXECUTABLE
	spirv-val
	HINTS
		"${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/spirv-tools"
		"$ENV{VULKAN_SDK}/Bin"
		"$ENV{VULKAN_TOOLS}/Bin"
)

if(NOT SPIRV_VAL_EXECUTABLE)
    message(FATAL_ERROR "spirv-val not found. Install spirv-tools with the tools feature.")
endif()

include(GoogleTest)

add_library(
    dlssg_test_support
    STATIC
        support/frame_statistics.cpp
        support/image_metrics.cpp
        support/json_report.cpp
        support/ngx_abi.cpp
        support/ngx_parameters.cpp
        support/procedural_scene.cpp
        support/wic_image_writer.cpp
)
target_compile_features(dlssg_test_support PUBLIC cxx_std_23)
target_compile_definitions(dlssg_test_support PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(dlssg_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(dlssg_test_support PUBLIC Vulkan::Headers windowscodecs ole32)
if(MSVC)
    target_compile_options(dlssg_test_support PRIVATE /EHsc /W4)
endif()

function(configure_test_target TARGET)
    target_compile_features(${TARGET} PRIVATE cxx_std_23)
    target_compile_definitions(${TARGET} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(${TARGET} PRIVATE /EHsc /W4)
    endif()
    add_dependencies(${TARGET} dlssg_to_fsr)
endfunction()

function(discover_dlssg_tests TARGET LABEL)
    gtest_discover_tests(
        ${TARGET}
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:dlssg_to_fsr>"
        PROPERTIES
            REQUIRED_FILES "$<TARGET_FILE:dlssg_to_fsr>"
            ENVIRONMENT "DLSSG_TO_FSR_DLL=$<TARGET_FILE:dlssg_to_fsr>"
            LABELS "${LABEL}"
            TIMEOUT 120
    )
endfunction()

add_executable(AbiTests NGX/AbiTests.cpp)
target_compile_definitions(
    AbiTests
    PRIVATE
        "DLSSG_TEST_DLL_PATH=L\"$<TARGET_FILE:dlssg_to_fsr>\""
        "DLSSG_TEST_OPTISCALER_DLL_PATH=L\"$<TARGET_FILE_DIR:dlssg_to_fsr>/dlssg_to_fsr3_amd_is_better.dll\""
)
target_link_libraries(AbiTests PRIVATE GTest::gtest_main)
configure_test_target(AbiTests)
add_custom_command(
    TARGET AbiTests
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:dlssg_to_fsr>"
        "$<TARGET_FILE_DIR:dlssg_to_fsr>/dlssg_to_fsr3_amd_is_better.dll"
)
discover_dlssg_tests(AbiTests CPU)

find_program(BASH_EXECUTABLE bash)
if(BASH_EXECUTABLE)
    add_test(NAME Setup.LinuxSyntax COMMAND "${BASH_EXECUTABLE}" -n resources/linux_install.sh)
    set_tests_properties(Setup.LinuxSyntax PROPERTIES LABELS CPU WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()

add_executable(SupportTests support/SupportTests.cpp)
target_link_libraries(SupportTests PRIVATE dlssg_test_support GTest::gtest_main)
configure_test_target(SupportTests)
discover_dlssg_tests(SupportTests CPU)

add_executable(
    Dx12NgxE2ETests
        e2e/dx12/dx12_ngx_backend.cpp
        e2e/dx12/dx12_ngx_e2e_test.cpp
)
target_link_libraries(Dx12NgxE2ETests PRIVATE dlssg_test_support GTest::gtest_main d3d12 dxgi)
configure_test_target(Dx12NgxE2ETests)
discover_dlssg_tests(Dx12NgxE2ETests GPU)

add_executable(
    VulkanNgxE2ETests
        e2e/vulkan/vulkan_ngx_e2e_backend.cpp
        e2e/vulkan/vulkan_ngx_e2e_test.cpp
)
target_link_libraries(VulkanNgxE2ETests PRIVATE dlssg_test_support GTest::gtest_main Vulkan::Vulkan)
configure_test_target(VulkanNgxE2ETests)
discover_dlssg_tests(VulkanNgxE2ETests GPU)

add_executable(
    BackendParityTests
        e2e/BackendParityTests.cpp
        e2e/dx12/dx12_ngx_backend.cpp
        e2e/vulkan/vulkan_ngx_e2e_backend.cpp
)
target_link_libraries(BackendParityTests PRIVATE dlssg_test_support GTest::gtest_main Vulkan::Vulkan d3d12 dxgi)
configure_test_target(BackendParityTests)
discover_dlssg_tests(BackendParityTests GPU)

add_executable(
    FrameGenerationTestbed
        game/testbed.cpp
        e2e/dx12/dx12_ngx_backend.cpp
        e2e/vulkan/vulkan_ngx_e2e_backend.cpp
)
target_link_libraries(
    FrameGenerationTestbed
    PRIVATE
        dlssg_test_support
        Vulkan::Vulkan
        glfw
        imgui::imgui
        d3d12
        dxgi
        opengl32
)
configure_test_target(FrameGenerationTestbed)

add_test(
    NAME Shader.VulkanCopySpirvValid
    COMMAND "${SPIRV_VAL_EXECUTABLE}" "${CMAKE_BINARY_DIR}/src/generated/VulkanCopy.spv"
)
set_tests_properties(
    Shader.VulkanCopySpirvValid
    PROPERTIES
        LABELS CPU
        REQUIRED_FILES "${CMAKE_BINARY_DIR}/src/generated/VulkanCopy.spv"
)

add_test(
    NAME Testbed.Dx12ToggleHeadless
    COMMAND FrameGenerationTestbed
        --backend=dx12 --fg=compare --headless --frames=4 --width=128 --height=72
        "--report=$<TARGET_FILE_DIR:FrameGenerationTestbed>/dx12-toggle-report.json"
)
set_tests_properties(
    Testbed.Dx12ToggleHeadless
    PROPERTIES
        LABELS GPU
        ENVIRONMENT "DLSSG_TO_FSR_DLL=$<TARGET_FILE:dlssg_to_fsr>"
        TIMEOUT 120
)

add_test(
    NAME Testbed.VulkanToggleHeadless
    COMMAND FrameGenerationTestbed
        --backend=vulkan --fg=compare --headless --frames=4 --width=128 --height=72
        --submit=submit2 --copy=transfer
        "--report=$<TARGET_FILE_DIR:FrameGenerationTestbed>/vulkan-toggle-report.json"
)
set_tests_properties(
    Testbed.VulkanToggleHeadless
    PROPERTIES
        LABELS GPU
        ENVIRONMENT "DLSSG_TO_FSR_DLL=$<TARGET_FILE:dlssg_to_fsr>;VK_LOADER_LAYERS_DISABLE=~implicit~"
        TIMEOUT 120
)
