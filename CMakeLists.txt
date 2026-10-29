cmake_minimum_required(VERSION 3.25)
project(Aurum64 VERSION 0.1.0 LANGUAGES C CXX OBJCXX)
if(NOT APPLE OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    message(FATAL_ERROR "Aurum64 currently targets Apple Silicon macOS only.")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_BUILD_RPATH "@loader_path")
set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
set(CMAKE_INSTALL_RPATH "@loader_path")
set(API "${CMAKE_SOURCE_DIR}/third_party/mupen64plus-core/src/api")
set(RUNTIME "${CMAKE_SOURCE_DIR}/Sources/AurumRuntime/include")
add_library(aurum-runtime SHARED Sources/AurumRuntime/Runtime.cpp)
target_include_directories(aurum-runtime PUBLIC "${RUNTIME}" PRIVATE "${API}")
add_library(aurum-capture STATIC Sources/AurumRDP/Capture.cpp)
target_include_directories(aurum-capture PUBLIC Sources/AurumRDP)
add_library(aurum-rdp-decode STATIC Sources/AurumRDP/RDP.cpp)
target_include_directories(aurum-rdp-decode PUBLIC Sources/AurumRDP)
add_library(aurum-rdp STATIC Sources/AurumRDP/RDP.cpp Sources/AurumRDP/Renderer.mm)
target_include_directories(aurum-rdp PUBLIC Sources/AurumRDP)
target_compile_options(aurum-rdp PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
target_link_libraries(aurum-rdp PUBLIC aurum-capture "-framework Metal" "-framework Foundation")
foreach(plugin IN ITEMS video audio input)
    string(SUBSTRING "${plugin}" 0 1 first)
    string(TOUPPER "${first}" first)
    string(SUBSTRING "${plugin}" 1 -1 rest)
    add_library(aurum-${plugin} SHARED Sources/Plugins/${first}${rest}.cpp)
    target_include_directories(aurum-${plugin} PRIVATE "${API}")
    target_link_libraries(aurum-${plugin} PRIVATE aurum-runtime)
endforeach()
add_library(aurum-dpc STATIC Sources/Plugins/DpcDispatch.cpp)
target_include_directories(aurum-dpc PUBLIC Sources/Plugins)
target_link_libraries(aurum-video PRIVATE aurum-dpc)
target_link_libraries(aurum-video PRIVATE aurum-rdp)
target_link_libraries(aurum-audio PRIVATE "-framework AudioToolbox")
add_custom_command(OUTPUT AurumRDP.metallib
    COMMAND xcrun -sdk macosx metal "-fmodules-cache-path=${CMAKE_BINARY_DIR}/metal-module-cache" -c "${CMAKE_SOURCE_DIR}/Sources/AurumRDP/RDP.metal" -o AurumRDP.air
    COMMAND xcrun -sdk macosx metal "-fmodules-cache-path=${CMAKE_BINARY_DIR}/metal-module-cache" -c "${CMAKE_SOURCE_DIR}/Sources/AurumRDP/Presentation.metal" -o Presentation.air
    COMMAND xcrun -sdk macosx metallib AurumRDP.air Presentation.air -o AurumRDP.metallib
    DEPENDS Sources/AurumRDP/RDP.metal Sources/AurumRDP/ShadeMath.h Sources/AurumRDP/Presentation.metal
        Sources/AurumRDP/TriangleTextureMath.h Sources/AurumRDP/PerspectiveMath.h
        Sources/AurumRDP/TextureDepthMath.h Sources/AurumRDP/CombinerMath.h
        Sources/AurumRDP/TriangleColorMath.h Sources/AurumRDP/DepthMath.h
        Sources/AurumRDP/TriangleDepthMath.h VERBATIM)
add_custom_target(shaders ALL DEPENDS AurumRDP.metallib)
add_executable(aurum-rdp-tests Tests/RDPTests.cpp)
target_link_libraries(aurum-rdp-tests PRIVATE aurum-rdp)
add_dependencies(aurum-rdp-tests shaders)
add_executable(aurum-capture-tests Tests/CaptureTests.cpp)
target_link_libraries(aurum-capture-tests PRIVATE aurum-capture)
add_executable(aurum-dpc-tests Tests/DpcDispatchTests.cpp)
target_link_libraries(aurum-dpc-tests PRIVATE aurum-dpc)
add_executable(aurum-memory-tests Tests/MemoryBusTests.cpp)
target_include_directories(aurum-memory-tests PRIVATE Sources/AurumRDP)
add_executable(aurum-tmem-tests Tests/TmemTests.cpp)
target_include_directories(aurum-tmem-tests PRIVATE Sources/AurumRDP)
add_executable(aurum-texture-upload-tests Tests/TextureUploadTests.cpp)
target_include_directories(aurum-texture-upload-tests PRIVATE Sources/AurumRDP)
add_executable(aurum-texture-fetch-tests Tests/TextureFetchTests.cpp)
target_include_directories(aurum-texture-fetch-tests PRIVATE Sources/AurumRDP)
add_executable(aurum-texture-coordinate-tests Tests/TextureCoordinateTests.cpp)
target_include_directories(aurum-texture-coordinate-tests PRIVATE Sources/AurumRDP)
target_link_libraries(aurum-texture-coordinate-tests PRIVATE aurum-rdp-decode)
add_executable(aurum-aurcap-replay Sources/AurumRDP/AurcapReplay.cpp)
target_link_libraries(aurum-aurcap-replay PRIVATE aurum-capture aurum-rdp-decode)
add_executable(aurum-metal-replay Sources/AurumRDP/AurcapReplay.cpp)
target_compile_definitions(aurum-metal-replay PRIVATE AURUM_METAL_REPLAY)
target_link_libraries(aurum-metal-replay PRIVATE aurum-rdp)
add_dependencies(aurum-metal-replay shaders)
add_library(angrylion-core STATIC
    Sources/AurumRDP/AngrylionCore.c
    third_party/angrylion-rdp-plus/src/core/parallel.cpp)
target_include_directories(angrylion-core PUBLIC third_party/angrylion-rdp-plus/src)
target_compile_options(angrylion-core PRIVATE -Wno-switch)
add_executable(aurum-angrylion-reference Sources/AurumRDP/AngrylionReference.cpp)
target_link_libraries(aurum-angrylion-reference PRIVATE angrylion-core aurum-capture)
add_executable(aurum-reference-diff Sources/AurumRDP/ReferenceDiff.cpp)
add_executable(aurum-smoke Tests/Smoke.cpp)
target_include_directories(aurum-smoke PRIVATE "${RUNTIME}")
enable_testing()
add_test(NAME capture COMMAND aurum-capture-tests)
add_test(NAME dpc COMMAND aurum-dpc-tests)
add_test(NAME memory COMMAND aurum-memory-tests)
add_test(NAME tmem COMMAND aurum-tmem-tests)
add_test(NAME texture-upload COMMAND aurum-texture-upload-tests)
add_test(NAME texture-fetch COMMAND aurum-texture-fetch-tests)
set(COORDINATE_CASES "${CMAKE_BINARY_DIR}/coordinate-tests/cases.bin")
set(COORDINATE_REFERENCE "${CMAKE_BINARY_DIR}/coordinate-tests/reference.bin")
add_test(NAME texture-coordinates-generate COMMAND aurum-texture-coordinate-tests --generate "${COORDINATE_CASES}")
set_tests_properties(texture-coordinates-generate PROPERTIES FIXTURES_SETUP coordinate-input)
add_test(NAME texture-coordinates-reference COMMAND aurum-angrylion-reference --texture-coordinates "${COORDINATE_CASES}" "${COORDINATE_REFERENCE}")
set_tests_properties(texture-coordinates-reference PROPERTIES FIXTURES_REQUIRED coordinate-input FIXTURES_SETUP coordinate-reference)
add_test(NAME texture-coordinates COMMAND aurum-texture-coordinate-tests --compare "${COORDINATE_CASES}" "${COORDINATE_REFERENCE}")
set_tests_properties(texture-coordinates PROPERTIES FIXTURES_REQUIRED coordinate-reference)
add_test(NAME rdp COMMAND aurum-rdp-tests "${CMAKE_BINARY_DIR}/AurumRDP.metallib")
add_executable(aurum-triangle-tests Tests/TriangleTests.cpp)
target_link_libraries(aurum-triangle-tests PRIVATE aurum-capture aurum-rdp-decode)
add_test(NAME triangle-unit COMMAND aurum-triangle-tests)
add_executable(aurum-attribute-tests Tests/AttributeTests.cpp)
target_link_libraries(aurum-attribute-tests PRIVATE aurum-rdp-decode)
set(ATTRIBUTE_FIXTURES "${CMAKE_BINARY_DIR}/attribute-tests")
add_test(NAME attributes-generate COMMAND aurum-attribute-tests --generate "${ATTRIBUTE_FIXTURES}/cases.bin")
set_tests_properties(attributes-generate PROPERTIES FIXTURES_SETUP attribute-input)
add_test(NAME attributes-reference COMMAND aurum-angrylion-reference --triangle-attributes
    "${ATTRIBUTE_FIXTURES}/cases.bin" "${ATTRIBUTE_FIXTURES}/reference.bin")
set_tests_properties(attributes-reference PROPERTIES FIXTURES_REQUIRED attribute-input FIXTURES_SETUP attribute-reference)
add_test(NAME attributes-differential COMMAND aurum-attribute-tests --compare
    "${ATTRIBUTE_FIXTURES}/cases.bin" "${ATTRIBUTE_FIXTURES}/reference.bin")
set_tests_properties(attributes-differential PROPERTIES FIXTURES_REQUIRED attribute-reference)
add_executable(aurum-perspective-tests Tests/PerspectiveTests.cpp)
target_include_directories(aurum-perspective-tests PRIVATE Sources/AurumRDP)
set(PERSPECTIVE_FIXTURES "${CMAKE_BINARY_DIR}/perspective-tests")
add_test(NAME perspective-generate COMMAND aurum-perspective-tests --generate "${PERSPECTIVE_FIXTURES}/cases.bin")
set_tests_properties(perspective-generate PROPERTIES FIXTURES_SETUP perspective-input)
add_test(NAME perspective-reference COMMAND aurum-angrylion-reference --perspective
    "${PERSPECTIVE_FIXTURES}/cases.bin" "${PERSPECTIVE_FIXTURES}/reference.bin")
set_tests_properties(perspective-reference PROPERTIES FIXTURES_REQUIRED perspective-input FIXTURES_SETUP perspective-reference)
add_test(NAME perspective-differential COMMAND aurum-perspective-tests --compare
    "${PERSPECTIVE_FIXTURES}/cases.bin" "${PERSPECTIVE_FIXTURES}/reference.bin")
set_tests_properties(perspective-differential PROPERTIES FIXTURES_REQUIRED perspective-reference)
add_custom_command(OUTPUT StageMath.metallib
    COMMAND xcrun -sdk macosx metal "-fmodules-cache-path=${CMAKE_BINARY_DIR}/metal-module-cache"
        "-I${CMAKE_SOURCE_DIR}/Sources/AurumRDP" -c "${CMAKE_SOURCE_DIR}/Tests/StageMath.metal" -o StageMath.air
    COMMAND xcrun -sdk macosx metallib StageMath.air -o StageMath.metallib
    DEPENDS Tests/StageMath.metal Sources/AurumRDP/ShadeMath.h
        Sources/AurumRDP/TextureDepthMath.h Sources/AurumRDP/PerspectiveMath.h VERBATIM)
add_custom_target(stage-shaders ALL DEPENDS StageMath.metallib)
add_executable(aurum-stage-metal-tests Tests/StageMathTests.mm)
target_include_directories(aurum-stage-metal-tests PRIVATE Sources/AurumRDP)
target_compile_options(aurum-stage-metal-tests PRIVATE -fobjc-arc)
target_link_libraries(aurum-stage-metal-tests PRIVATE "-framework Metal" "-framework Foundation")
add_dependencies(aurum-stage-metal-tests stage-shaders)
add_test(NAME perspective-metal COMMAND aurum-stage-metal-tests "${CMAKE_BINARY_DIR}/StageMath.metallib"
    "${PERSPECTIVE_FIXTURES}/cases.bin" "${PERSPECTIVE_FIXTURES}/reference.bin")
set_tests_properties(perspective-metal PROPERTIES FIXTURES_REQUIRED perspective-reference)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME stage-probe-negative COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/Tests/probe_negative.py" "${CMAKE_BINARY_DIR}")
set_tests_properties(stage-probe-negative PROPERTIES FIXTURES_REQUIRED "attribute-reference;perspective-reference")
set(TRIANGLE_FIXTURES "${CMAKE_BINARY_DIR}/triangle-tests")
add_test(NAME triangle-generate COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/scripts/make_triangle_fixtures.py" "${TRIANGLE_FIXTURES}/matrix.aurcap")
set_tests_properties(triangle-generate PROPERTIES FIXTURES_SETUP triangle-input)
add_test(NAME triangle-reference COMMAND aurum-angrylion-reference
    "${TRIANGLE_FIXTURES}/matrix.aurcap" "${TRIANGLE_FIXTURES}/reference")
set_tests_properties(triangle-reference PROPERTIES FIXTURES_REQUIRED triangle-input FIXTURES_SETUP triangle-reference)
add_test(NAME triangle-differential COMMAND aurum-triangle-tests
    "${TRIANGLE_FIXTURES}/matrix.aurcap" "${TRIANGLE_FIXTURES}/reference.rdram")
set_tests_properties(triangle-differential PROPERTIES FIXTURES_REQUIRED triangle-reference)
add_test(NAME triangle-metal COMMAND aurum-metal-replay
    "${TRIANGLE_FIXTURES}/matrix.aurcap" "${TRIANGLE_FIXTURES}/metal" "${CMAKE_BINARY_DIR}/AurumRDP.metallib")
set_tests_properties(triangle-metal PROPERTIES FIXTURES_REQUIRED triangle-input FIXTURES_SETUP triangle-metal)
add_test(NAME triangle-metal-differential COMMAND aurum-reference-diff
    "${TRIANGLE_FIXTURES}/reference" "${TRIANGLE_FIXTURES}/metal" "${TRIANGLE_FIXTURES}/matrix.aurcap")
set_tests_properties(triangle-metal-differential PROPERTIES FIXTURES_REQUIRED "triangle-reference;triangle-metal")
add_test(NAME alpha-compare-triangle-generate COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/scripts/make_triangle_fixtures.py"
    "${TRIANGLE_FIXTURES}/alpha-compare.aurcap" --alpha-compare)
set_tests_properties(alpha-compare-triangle-generate PROPERTIES FIXTURES_SETUP alpha-compare-triangle-input)
add_test(NAME alpha-compare-triangle-reference COMMAND aurum-angrylion-reference
    "${TRIANGLE_FIXTURES}/alpha-compare.aurcap" "${TRIANGLE_FIXTURES}/alpha-compare-reference")
set_tests_properties(alpha-compare-triangle-reference PROPERTIES
    FIXTURES_REQUIRED alpha-compare-triangle-input FIXTURES_SETUP alpha-compare-triangle-reference)
add_test(NAME alpha-compare-triangle-differential COMMAND aurum-triangle-tests
    "${TRIANGLE_FIXTURES}/alpha-compare.aurcap" "${TRIANGLE_FIXTURES}/alpha-compare-reference.rdram")
set_tests_properties(alpha-compare-triangle-differential PROPERTIES
    FIXTURES_REQUIRED alpha-compare-triangle-reference)
add_test(NAME alpha-compare-triangle-metal COMMAND aurum-metal-replay
    "${TRIANGLE_FIXTURES}/alpha-compare.aurcap" "${TRIANGLE_FIXTURES}/alpha-compare-metal"
    "${CMAKE_BINARY_DIR}/AurumRDP.metallib")
set_tests_properties(alpha-compare-triangle-metal PROPERTIES
    FIXTURES_REQUIRED alpha-compare-triangle-input FIXTURES_SETUP alpha-compare-triangle-metal)
add_test(NAME alpha-compare-triangle-metal-differential COMMAND aurum-reference-diff
    "${TRIANGLE_FIXTURES}/alpha-compare-reference" "${TRIANGLE_FIXTURES}/alpha-compare-metal"
    "${TRIANGLE_FIXTURES}/alpha-compare.aurcap")
set_tests_properties(alpha-compare-triangle-metal-differential PROPERTIES
    FIXTURES_REQUIRED "alpha-compare-triangle-reference;alpha-compare-triangle-metal")
add_test(NAME depth-triangle-generate COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/scripts/make_triangle_fixtures.py"
    "${TRIANGLE_FIXTURES}/depth.aurcap" --depth)
set_tests_properties(depth-triangle-generate PROPERTIES FIXTURES_SETUP depth-triangle-input)
add_test(NAME depth-triangle-reference COMMAND aurum-angrylion-reference
    "${TRIANGLE_FIXTURES}/depth.aurcap" "${TRIANGLE_FIXTURES}/depth-reference")
set_tests_properties(depth-triangle-reference PROPERTIES
    FIXTURES_REQUIRED depth-triangle-input FIXTURES_SETUP depth-triangle-reference)
add_test(NAME depth-triangle-differential COMMAND aurum-triangle-tests
    "${TRIANGLE_FIXTURES}/depth.aurcap" "${TRIANGLE_FIXTURES}/depth-reference.rdram")
set_tests_properties(depth-triangle-differential PROPERTIES
    FIXTURES_REQUIRED depth-triangle-reference)
add_test(NAME depth-triangle-metal COMMAND aurum-metal-replay
    "${TRIANGLE_FIXTURES}/depth.aurcap" "${TRIANGLE_FIXTURES}/depth-metal"
    "${CMAKE_BINARY_DIR}/AurumRDP.metallib")
set_tests_properties(depth-triangle-metal PROPERTIES
    FIXTURES_REQUIRED depth-triangle-input FIXTURES_SETUP depth-triangle-metal)
add_test(NAME depth-triangle-metal-differential COMMAND aurum-reference-diff
    "${TRIANGLE_FIXTURES}/depth-reference" "${TRIANGLE_FIXTURES}/depth-metal"
    "${TRIANGLE_FIXTURES}/depth.aurcap")
set_tests_properties(depth-triangle-metal-differential PROPERTIES
    FIXTURES_REQUIRED "depth-triangle-reference;depth-triangle-metal")
add_test(NAME shade-generate COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/scripts/make_triangle_fixtures.py" "${TRIANGLE_FIXTURES}/shade.aurcap" --shaded)
set_tests_properties(shade-generate PROPERTIES FIXTURES_SETUP shade-input)
add_test(NAME shade-reference COMMAND aurum-angrylion-reference
    "${TRIANGLE_FIXTURES}/shade.aurcap" "${TRIANGLE_FIXTURES}/shade-reference")
set_tests_properties(shade-reference PROPERTIES FIXTURES_REQUIRED shade-input FIXTURES_SETUP shade-reference)
add_test(NAME shade-differential COMMAND aurum-triangle-tests
    "${TRIANGLE_FIXTURES}/shade.aurcap" "${TRIANGLE_FIXTURES}/shade-reference.rdram")
set_tests_properties(shade-differential PROPERTIES FIXTURES_REQUIRED shade-reference)
add_test(NAME shade-metal COMMAND aurum-metal-replay
    "${TRIANGLE_FIXTURES}/shade.aurcap" "${TRIANGLE_FIXTURES}/shade-metal" "${CMAKE_BINARY_DIR}/AurumRDP.metallib")
set_tests_properties(shade-metal PROPERTIES FIXTURES_REQUIRED shade-input FIXTURES_SETUP shade-metal)
add_test(NAME shade-metal-differential COMMAND aurum-reference-diff
    "${TRIANGLE_FIXTURES}/shade-reference" "${TRIANGLE_FIXTURES}/shade-metal" "${TRIANGLE_FIXTURES}/shade.aurcap")
set_tests_properties(shade-metal-differential PROPERTIES FIXTURES_REQUIRED "shade-reference;shade-metal")
add_test(NAME shade-textured-triangle-generate COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/scripts/make_triangle_fixtures.py"
    "${TRIANGLE_FIXTURES}/shade-textured.aurcap" --shaded --textured)
set_tests_properties(shade-textured-triangle-generate PROPERTIES FIXTURES_SETUP shade-textured-triangle-input)
add_test(NAME shade-textured-triangle-reference COMMAND aurum-angrylion-reference
    "${TRIANGLE_FIXTURES}/shade-textured.aurcap" "${TRIANGLE_FIXTURES}/shade-textured-reference")
set_tests_properties(shade-textured-triangle-reference PROPERTIES
    FIXTURES_REQUIRED shade-textured-triangle-input FIXTURES_SETUP shade-textured-triangle-reference)
add_test(NAME shade-textured-triangle-differential COMMAND aurum-triangle-tests
    "${TRIANGLE_FIXTURES}/shade-textured.aurcap" "${TRIANGLE_FIXTURES}/shade-textured-reference.rdram")
set_tests_properties(shade-textured-triangle-differential PROPERTIES
    FIXTURES_REQUIRED shade-textured-triangle-reference)
add_test(NAME shade-textured-triangle-metal COMMAND aurum-metal-replay
    "${TRIANGLE_FIXTURES}/shade-textured.aurcap" "${TRIANGLE_FIXTURES}/shade-textured-metal"
    "${CMAKE_BINARY_DIR}/AurumRDP.metallib")
set_tests_properties(shade-textured-triangle-metal PROPERTIES
    FIXTURES_REQUIRED shade-textured-triangle-input FIXTURES_SETUP shade-textured-triangle-metal)
add_test(NAME shade-textured-triangle-metal-differential COMMAND aurum-reference-diff
    "${TRIANGLE_FIXTURES}/shade-textured-reference" "${TRIANGLE_FIXTURES}/shade-textured-metal"
    "${TRIANGLE_FIXTURES}/shade-textured.aurcap")
set_tests_properties(shade-textured-triangle-metal-differential PROPERTIES
    FIXTURES_REQUIRED "shade-textured-triangle-reference;shade-textured-triangle-metal")
add_test(NAME textured-triangle-generate COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/scripts/make_triangle_fixtures.py" "${TRIANGLE_FIXTURES}/textured.aurcap" --textured)
set_tests_properties(textured-triangle-generate PROPERTIES FIXTURES_SETUP textured-triangle-input)
add_test(NAME textured-triangle-reference COMMAND aurum-angrylion-reference
    "${TRIANGLE_FIXTURES}/textured.aurcap" "${TRIANGLE_FIXTURES}/textured-reference")
set_tests_properties(textured-triangle-reference PROPERTIES FIXTURES_REQUIRED textured-triangle-input FIXTURES_SETUP textured-triangle-reference)
add_test(NAME textured-triangle-differential COMMAND aurum-triangle-tests
    "${TRIANGLE_FIXTURES}/textured.aurcap" "${TRIANGLE_FIXTURES}/textured-reference.rdram")
set_tests_properties(textured-triangle-differential PROPERTIES FIXTURES_REQUIRED textured-triangle-reference)
add_test(NAME textured-triangle-metal COMMAND aurum-metal-replay
    "${TRIANGLE_FIXTURES}/textured.aurcap" "${TRIANGLE_FIXTURES}/textured-metal" "${CMAKE_BINARY_DIR}/AurumRDP.metallib")
set_tests_properties(textured-triangle-metal PROPERTIES FIXTURES_REQUIRED textured-triangle-input FIXTURES_SETUP textured-triangle-metal)
add_test(NAME textured-triangle-metal-differential COMMAND aurum-reference-diff
    "${TRIANGLE_FIXTURES}/textured-reference" "${TRIANGLE_FIXTURES}/textured-metal" "${TRIANGLE_FIXTURES}/textured.aurcap")
set_tests_properties(textured-triangle-metal-differential PROPERTIES FIXTURES_REQUIRED "textured-triangle-reference;textured-triangle-metal")
add_executable(aurum-texture-copy-tests Tests/TextureCopyTests.cpp)
target_link_libraries(aurum-texture-copy-tests PRIVATE aurum-rdp)
add_dependencies(aurum-texture-copy-tests shaders)
add_test(NAME texture-copy COMMAND aurum-texture-copy-tests "${CMAKE_BINARY_DIR}/AurumRDP.metallib")
add_executable(aurum-presentation-tests Tests/PresentationTests.mm)
target_compile_options(aurum-presentation-tests PRIVATE -fobjc-arc)
target_link_libraries(aurum-presentation-tests PRIVATE "-framework Metal" "-framework Foundation")
add_dependencies(aurum-presentation-tests shaders)
add_test(NAME presentation COMMAND aurum-presentation-tests "${CMAKE_BINARY_DIR}/AurumRDP.metallib")
