# The XWA wiki reader core (lib/Xwa) against archives written by xwa-tool's
# own writer (see gen/README.md): every article, search, image and formula
# must read back exactly as xwa-tool reads it.

file(GLOB XWA_ZSTD_SOURCES
  ${REPO_ROOT}/lib/zstd/src/common/*.c
  ${REPO_ROOT}/lib/zstd/src/decompress/*.c
)
add_library(xwa_zstd STATIC ${XWA_ZSTD_SOURCES})
target_include_directories(xwa_zstd PUBLIC ${REPO_ROOT}/lib/zstd/src)
target_compile_definitions(xwa_zstd PUBLIC
  ZSTD_LEGACY_SUPPORT=0
  ZSTD_DISABLE_ASM
  ZSTD_TRACE=0
  ZSTD_STRIP_ERROR_STRINGS
  XXH_NAMESPACE=ZSTD_
  DEBUGLEVEL=0
)

file(GLOB XWA_SOURCES ${REPO_ROOT}/lib/Xwa/*.cpp)
add_executable(XwaReaderTest
  XwaReaderTest.cpp
  ${XWA_SOURCES}
  ${REPO_ROOT}/lib/miniz/src/InflateStream.cpp
  ${REPO_ROOT}/lib/miniz/src/miniz_impl.c
  ${REPO_ROOT}/lib/Memory/BuildScratch.cpp
  ${REPO_ROOT}/lib/hal/HalMemory.cpp
)
target_include_directories(XwaReaderTest PRIVATE
  ${REPO_ROOT}/test/inflate_stream/stubs
  ${REPO_ROOT}/lib/Xwa
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/miniz/src
)
target_compile_definitions(XwaReaderTest PRIVATE XWA_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_link_libraries(XwaReaderTest PRIVATE crosspoint_test_common xwa_zstd GTest::gtest_main)
gtest_discover_tests(XwaReaderTest)
