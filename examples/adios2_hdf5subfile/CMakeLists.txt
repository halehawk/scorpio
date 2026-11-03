cmake_minimum_required(VERSION 3.12)
project(adios2_hdf5subfile CXX)

find_package(MPI REQUIRED COMPONENTS CXX)
find_package(ADIOS2 REQUIRED COMPONENTS CXX MPI)

add_executable(adios2_hdf5subfile adios2_hdf5subfile.cpp)
target_link_libraries(adios2_hdf5subfile adios2::cxx11_mpi MPI::MPI_CXX)
configure_file(input.xml ${CMAKE_CURRENT_BINARY_DIR}/input.xml COPYONLY)

add_executable(adios2_hdf5subfile_read adios2_hdf5subfile_read.cpp)
target_link_libraries(adios2_hdf5subfile_read adios2::cxx11_mpi MPI::MPI_CXX)
