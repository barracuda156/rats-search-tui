add_executable(ratsn_wire_codec_test wire_codec_test.cpp)
target_link_libraries(ratsn_wire_codec_test PRIVATE ratsn_core)
target_compile_definitions(ratsn_wire_codec_test PRIVATE
    RATSN_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

add_test(NAME wire_codec_test COMMAND ratsn_wire_codec_test)
