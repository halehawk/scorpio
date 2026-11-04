cmake_minimum_required(VERSION 3.12)
project(adios2_zarr_plugin CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# OFF: serial plugin, no MPI (chunks are built from one process's blocks).
# ON:  chunks are assembled across MPI ranks (uses MPI_COMM_WORLD).
option(ZARR_PLUGIN_USE_MPI "Assemble Zarr chunks across MPI ranks" OFF)

if (ZARR_PLUGIN_USE_MPI)
  find_package(MPI REQUIRED COMPONENTS CXX)
  find_package(ADIOS2 REQUIRED COMPONENTS CXX MPI)
  set(zarr_plugin_libs adios2::cxx_mpi MPI::MPI_CXX)
else ()
  find_package(ADIOS2 REQUIRED COMPONENTS CXX)
  set(zarr_plugin_libs adios2::cxx)
endif ()

# The plugin: load it with engine "Plugin", PluginName=ZarrWrite,
# PluginLibrary=adios2_zarr_plugin, and ADIOS2_PLUGIN_PATH pointing here.
add_library(adios2_zarr_plugin SHARED ZarrWritePlugin.cpp)
target_link_libraries(adios2_zarr_plugin ${zarr_plugin_libs})

add_executable(zarr_plugin_writer zarr_plugin_writer.cpp)
target_link_libraries(zarr_plugin_writer ${zarr_plugin_libs})

if (ZARR_PLUGIN_USE_MPI)
  target_compile_definitions(adios2_zarr_plugin PRIVATE ZARR_PLUGIN_MPI)
  target_compile_definitions(zarr_plugin_writer PRIVATE ZARR_PLUGIN_MPI)
endif ()

install(TARGETS adios2_zarr_plugin zarr_plugin_writer)

#===== Test: a plain ADIOS2 application with Zarr (plugin) vs BP5 backends =====
# Needs a Python with zarr>=3 and the adios2 module of the same ADIOS2.
enable_testing()
find_package(Python3 COMPONENTS Interpreter)

add_executable(zarr_backend_test test/zarr_backend_test.cpp)
target_link_libraries(zarr_backend_test ${zarr_plugin_libs})
if (ZARR_PLUGIN_USE_MPI)
  target_compile_definitions(zarr_backend_test PRIVATE ZARR_PLUGIN_MPI)
  find_program(MPIEXEC_EXE NAMES mpiexec mpirun HINTS ${MPIEXEC_EXECUTABLE})
  set(zarr_backend_tests "mpi_1:1" "mpi_3:3" "mpi_4:4")
else ()
  set(zarr_backend_tests "serial:0")
endif ()

foreach (t ${zarr_backend_tests})
  string(REPLACE ":" ";" t ${t})
  list(GET t 0 tname)
  list(GET t 1 np)
  set(launcher)
  if (np GREATER 0)
    set(launcher ${MPIEXEC_EXE} -n ${np})
  endif ()
  add_test(NAME zarr_backend_${tname}
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run_backend_test.sh
            ${CMAKE_CURRENT_BINARY_DIR}/test_${tname} $<TARGET_FILE:zarr_backend_test>
            ${CMAKE_CURRENT_SOURCE_DIR}/test ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/test/compare_zarr_bp.py ${launcher})
  set_tests_properties(zarr_backend_${tname} PROPERTIES
    ENVIRONMENT "ADIOS2_PLUGIN_PATH=$<TARGET_FILE_DIR:adios2_zarr_plugin>"
    TIMEOUT 120)
endforeach ()
