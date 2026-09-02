! pio_sst_writer_f.F90
!
! ADIOS2 SST streaming test — Fortran writer.
! Paired with pio_sst_reader_f; launched together via MPMD mpirun.
!
! All ranks in this job act as the SST writer.  Opens an SST stream,
! defines a 1-D distributed integer variable, writes NFRAMES timesteps,
! then closes the stream.
!
! Validates: FR-002, FR-003, FR-007, FR-009, SC-002, SC-005, SC-006.

program pio_sst_writer_f

  use pio
  use pio_kinds, only : PIO_OFFSET_KIND
#ifndef NO_MPIMOD
  use mpi
#endif
  implicit none
#ifdef NO_MPIMOD
  include 'mpif.h'
#endif

  integer, parameter :: NDIMS          = 1
  integer, parameter :: NFRAMES        = 3
  integer, parameter :: ELEMENTS_PER_PE = 4

  type(iosystem_desc_t) :: iosys
  type(file_desc_t)     :: file
  type(io_desc_t)       :: iodesc
  type(var_desc_t)      :: vdesc
  integer(PIO_OFFSET_KIND), allocatable :: compdof(:)
  integer :: data(ELEMENTS_PER_PE)
  integer :: my_rank, nprocs, gdim, dimid
  integer :: iotype, ret, t, i, ierr
  integer(PIO_OFFSET_KIND) :: frame_num

  call MPI_Init(ierr)
  call MPI_Comm_rank(MPI_COMM_WORLD, my_rank, ierr)
  call MPI_Comm_size(MPI_COMM_WORLD, nprocs, ierr)

  ! All ranks participate as I/O tasks (stride=1, base=0)
  call pio_init(my_rank, MPI_COMM_WORLD, nprocs, 0, 1, PIO_rearr_subset, iosys)

  ! 1-D decomposition: each rank owns ELEMENTS_PER_PE consecutive elements (1-based)
  gdim = nprocs * ELEMENTS_PER_PE
  allocate(compdof(ELEMENTS_PER_PE))
  do i = 1, ELEMENTS_PER_PE
    compdof(i) = int(my_rank * ELEMENTS_PER_PE + i, PIO_OFFSET_KIND)
  end do
  call pio_initdecomp(iosys, PIO_int, (/gdim/), compdof, iodesc)
  deallocate(compdof)

  ! Open SST stream (writer side)
  iotype = PIO_iotype_adios_sst
  ret = pio_createfile(iosys, file, iotype, "scorpio_sst_test_stream_f", PIO_CLOBBER)
  if (ret /= PIO_noerr) then
    write(0,'(a,i0)') "SST writer: pio_createfile failed, ret=", ret
    call MPI_Abort(MPI_COMM_WORLD, ret, ierr)
  end if

  ! Define global dimension and variable
  ret = pio_def_dim(file, "x", gdim, dimid)
  if (ret /= PIO_noerr) then
    write(0,'(a,i0)') "SST writer: pio_def_dim failed, ret=", ret
    call MPI_Abort(MPI_COMM_WORLD, ret, ierr)
  end if

  ret = pio_def_var(file, "data", PIO_int, (/dimid/), vdesc)
  if (ret /= PIO_noerr) then
    write(0,'(a,i0)') "SST writer: pio_def_var failed, ret=", ret
    call MPI_Abort(MPI_COMM_WORLD, ret, ierr)
  end if

  ret = pio_enddef(file)
  if (ret /= PIO_noerr) then
    write(0,'(a,i0)') "SST writer: pio_enddef failed, ret=", ret
    call MPI_Abort(MPI_COMM_WORLD, ret, ierr)
  end if

  ! Write NFRAMES timesteps
  do t = 0, NFRAMES - 1
    do i = 1, ELEMENTS_PER_PE
      data(i) = t * 1000 + my_rank * ELEMENTS_PER_PE + (i - 1)
    end do

    frame_num = int(t, PIO_OFFSET_KIND)
    call pio_setframe(file, vdesc, frame_num)
    call pio_write_darray(file, vdesc, iodesc, data, ret)
    if (ret /= PIO_noerr) then
      write(0,'(a,i0,a,i0)') "SST writer: pio_write_darray failed at frame ", t, ", ret=", ret
      call MPI_Abort(MPI_COMM_WORLD, ret, ierr)
    end if
  end do

  call pio_syncfile(file)
  call pio_closefile(file)
  call pio_freedecomp(iosys, iodesc)
  call pio_finalize(iosys, ret)

  call MPI_Finalize(ierr)

  if (my_rank == 0) write(*,'(a)') "SST Fortran writer finished."

end program pio_sst_writer_f
