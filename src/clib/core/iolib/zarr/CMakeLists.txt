#==============================================================================
#  DEFINE THE TARGET LIBRARY
#==============================================================================
message(STATUS "===== Configuring SCORPIO C Core (zarr)... =====")
# Native Zarr iotype (PIO_IOTYPE_ZARR), using the header-only zarr_chunk
# chunker. spio_zarr.cpp is empty when PIO_ENABLE_ZARR is off.
set (spio_core_iolib_zarr_src spio_zarr.cpp)

set (spio_clib_src_dir "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
set (spio_clib_bin_dir "${CMAKE_CURRENT_BINARY_DIR}/../../..")

add_library (spio_core_iolib_zarr OBJECT ${spio_core_iolib_zarr_src})
target_include_directories(spio_core_iolib_zarr
  PUBLIC .
  PRIVATE ${spio_clib_src_dir} ${spio_clib_bin_dir} ${spio_clib_src_dir}/util ${spio_clib_src_dir}/core ${spio_clib_src_dir}/core/util ${spio_clib_src_dir}/core/progress_engine)
target_link_libraries(spio_core_iolib_zarr
  PUBLIC spio_default_public_options
  PRIVATE spio_default_private_options)
# zarr_chunk.hpp needs C++17; this comes after the -std=c++14 in CMAKE_CXX_FLAGS
if (PIO_ENABLE_ZARR)
  target_compile_options(spio_core_iolib_zarr PRIVATE -std=c++17)
endif ()
