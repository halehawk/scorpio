! pio_zarr_f.F90
!
! Writes a Zarr v3 store with the native Zarr iotype (PIO_iotype_zarr) and
! Zarr-chunked I/O decompositions (pio_set_zarr_decomp), using every task
! as an I/O task. check_zarr_f.py checks the store with zarr-python and
! xarray (the Zarr iotype is write only).
!
!   mpiexec -n <nprocs> ./pio_zarr_f [store name (default pio_zarr_f.zarr)]
!
! Contents (NetCDF order; Fortran order is reversed), with f = 0..NFRAMES-1,
! j = 0..NLAT-1, i = 0..NLON-1:
!   dims   time (unlimited, NFRAMES records), lat = NLAT, lon = NLON
!   lat(lat), lon(lon)    double  coordinates, pio_put_var
!   time(time)            double  one pio_put_var per record: 6 f
!   scale                 double  scalar, pio_put_var: 2.5
!   T(time, lat, lon)     double  250 + f + 0.01 j + 0.0001 i
!                                 default (zarr-python) chunks (1, 128, 256)
!   Tf(time, lat, lon)    float   T written from double data
!   U(time, lat, lon)     double  10 f + j - 0.5 i, chunks (1, 64, 100)
!                                 set with pio_def_var_chunking
!   mask(lat, lon)        int     j NLON + i, except fill where mod(i+j, 7) == 0
!                                 (holes in the decomposition)
!   attributes: global "title", "version"; T "units", "_FillValue";
!               U "valid_range"
! Rank r holds the lat rows j with mod(j, nprocs) == r, so the data is
! rearranged onto the I/O tasks' chunk-aligned boxes.

program pio_zarr_f

  use pio
  use pio_kinds, only : PIO_OFFSET_KIND
#ifndef NO_MPIMOD
  use mpi
#endif
  implicit none
#ifdef NO_MPIMOD
  include 'mpif.h'
#endif

  integer, parameter :: NLAT = 256, NLON = 512, NFRAMES = 3
  character(len=*), parameter :: TITLE = "SCORPIO native Zarr Fortran test"

  type(iosystem_desc_t) :: iosys
  type(file_desc_t)     :: file
  type(io_desc_t)       :: iodesc_d, iodesc_i
  type(var_desc_t)      :: vd_lat, vd_lon, vd_time, vd_scale, vd_t, vd_tf, vd_u, vd_mask
  integer(PIO_OFFSET_KIND), allocatable :: compdof(:), compdof_i(:)
  double precision, allocatable :: t(:), u(:), coord(:)
  integer, allocatable :: mask(:)
  integer :: my_rank, nprocs, nrows, iotype, ret, ierr, nerr, nerr_all
  integer :: dim_lat, dim_lon, dim_time, f, i, j, k, storage, chunks(3), varid
  integer(PIO_OFFSET_KIND) :: dimlen
  character(len=PIO_MAX_NAME) :: fname

  call MPI_Init(ierr)
  call MPI_Comm_rank(MPI_COMM_WORLD, my_rank, ierr)
  call MPI_Comm_size(MPI_COMM_WORLD, nprocs, ierr)
  nerr = 0

  fname = "pio_zarr_f.zarr"
  if (command_argument_count() >= 1) call get_command_argument(1, fname)

  iotype = PIO_iotype_zarr
  if (.not. pio_iotype_available(iotype)) then
    if (my_rank == 0) write(0,'(a)') "Zarr iotype not available in this SCORPIO build"
    call MPI_Abort(MPI_COMM_WORLD, 1, ierr)
  end if

  ! Every task is an I/O task; decompositions are split into Zarr chunks
  call pio_init(my_rank, MPI_COMM_WORLD, nprocs, 0, 1, PIO_rearr_box, iosys)
  call pio_set_zarr_decomp(.true., ret)
  call check(ret, "pio_set_zarr_decomp")

  ! My lat rows: j = my_rank, my_rank + nprocs, ... (0-based)
  nrows = 0
  do j = my_rank, NLAT - 1, nprocs
    nrows = nrows + 1
  end do
  allocate(compdof(nrows * NLON), compdof_i(nrows * NLON))
  allocate(t(nrows * NLON), u(nrows * NLON), mask(nrows * NLON))
  k = 0
  do j = my_rank, NLAT - 1, nprocs
    do i = 0, NLON - 1
      k = k + 1
      compdof(k) = int(j, PIO_OFFSET_KIND) * NLON + i + 1
      if (mod(i + j, 7) == 0) then
        compdof_i(k) = 0  ! hole: written as the fill value
      else
        compdof_i(k) = compdof(k)
      end if
      mask(k) = j * NLON + i
    end do
  end do
  call pio_initdecomp(iosys, PIO_double, (/NLON, NLAT/), compdof, iodesc_d)
  call pio_initdecomp(iosys, PIO_int, (/NLON, NLAT/), compdof_i, iodesc_i)

  !---------------------------------------------------------------- define
  call check(pio_createfile(iosys, file, iotype, trim(fname), PIO_CLOBBER), "pio_createfile")
  call check(pio_def_dim(file, "time", PIO_unlimited, dim_time), "pio_def_dim time")
  call check(pio_def_dim(file, "lat", NLAT, dim_lat), "pio_def_dim lat")
  call check(pio_def_dim(file, "lon", NLON, dim_lon), "pio_def_dim lon")

  call check(pio_def_var(file, "lat", PIO_double, (/dim_lat/), vd_lat), "pio_def_var lat")
  call check(pio_def_var(file, "lon", PIO_double, (/dim_lon/), vd_lon), "pio_def_var lon")
  call check(pio_def_var(file, "time", PIO_double, (/dim_time/), vd_time), "pio_def_var time")
  call check(pio_def_var(file, "scale", PIO_double, vd_scale), "pio_def_var scale")
  call check(pio_def_var(file, "T", PIO_double, (/dim_lon, dim_lat, dim_time/), vd_t), &
             "pio_def_var T")
  call check(pio_def_var(file, "Tf", PIO_real, (/dim_lon, dim_lat, dim_time/), vd_tf), &
             "pio_def_var Tf")
  call check(pio_def_var(file, "U", PIO_double, (/dim_lon, dim_lat, dim_time/), vd_u), &
             "pio_def_var U")
  call check(pio_def_var(file, "mask", PIO_int, (/dim_lon, dim_lat/), vd_mask), &
             "pio_def_var mask")
  ! Fortran order (lon, lat, time) -> Zarr chunks (1, 64, 100)
  call check(pio_def_var_chunking(file, vd_u, PIO_chunked, (/100, 64, 1/)), &
             "pio_def_var_chunking U")

  call check(pio_put_att(file, PIO_global, "title", TITLE), "pio_put_att title")
  call check(pio_put_att(file, PIO_global, "version", 3), "pio_put_att version")
  call check(pio_put_att(file, vd_t, "units", "K"), "pio_put_att T units")
  call check(pio_put_att(file, vd_t, "_FillValue", -999.0d0), "pio_put_att T _FillValue")
  call check(pio_put_att(file, vd_u, "valid_range", (/-300.0, 300.0/)), &
             "pio_put_att U valid_range")
  call check(pio_enddef(file), "pio_enddef")

  ! Metadata is answered by SCORPIO itself (the store is not read back)
  call check(pio_inq_dimid(file, "lat", k), "pio_inq_dimid lat")
  call check(pio_inq_dimlen(file, k, dimlen), "pio_inq_dimlen lat")
  call expect(dimlen == NLAT, "lat length")
  call check(pio_inq_varid(file, "U", varid), "pio_inq_varid U")
  call expect(varid == vd_u%varid, "U varid")
  chunks = -1
  call check(pio_inq_var_chunking(file, vd_u, storage, chunks), "pio_inq_var_chunking U")
  call expect(storage == PIO_chunked .and. all(chunks == (/100, 64, 1/)), "U chunk sizes")
  chunks = -1
  call check(pio_inq_var_chunking(file, vd_t, storage, chunks), "pio_inq_var_chunking T")
  call expect(all(chunks == (/256, 128, 1/)), "T default (zarr-python) chunk sizes")

  !----------------------------------------------------------------- write
  allocate(coord(max(NLAT, NLON)))
  do j = 0, NLAT - 1
    coord(j + 1) = -90.0d0 + j * (180.0d0 / (NLAT - 1))
  end do
  call check(pio_put_var(file, vd_lat, coord(1:NLAT)), "pio_put_var lat")
  do i = 0, NLON - 1
    coord(i + 1) = i * (360.0d0 / NLON)
  end do
  call check(pio_put_var(file, vd_lon, coord(1:NLON)), "pio_put_var lon")
  call check(pio_put_var(file, vd_scale, 2.5d0), "pio_put_var scale")

  call pio_write_darray(file, vd_mask, iodesc_i, mask, ret)
  call check(ret, "pio_write_darray mask")

  do f = 0, NFRAMES - 1
    call check(pio_put_var(file, vd_time, (/f + 1/), (/1/), (/6.0d0 * f/)), "pio_put_var time")
    k = 0
    do j = my_rank, NLAT - 1, nprocs
      do i = 0, NLON - 1
        k = k + 1
        t(k) = 250.0d0 + f + 0.01d0 * j + 0.0001d0 * i
        u(k) = 10.0d0 * f + j - 0.5d0 * i
      end do
    end do
    ! pio_setframe takes 1-based frame numbers
    call pio_setframe(file, vd_t, int(f + 1, PIO_OFFSET_KIND))
    call pio_write_darray(file, vd_t, iodesc_d, t, ret)
    call check(ret, "pio_write_darray T")
    call pio_setframe(file, vd_tf, int(f + 1, PIO_OFFSET_KIND))
    call pio_write_darray(file, vd_tf, iodesc_d, t, ret)
    call check(ret, "pio_write_darray Tf")
    call pio_setframe(file, vd_u, int(f + 1, PIO_OFFSET_KIND))
    call pio_write_darray(file, vd_u, iodesc_d, u, ret)
    call check(ret, "pio_write_darray U")
  end do
  call pio_closefile(file)

  call pio_freedecomp(iosys, iodesc_d)
  call pio_freedecomp(iosys, iodesc_i)
  call pio_finalize(iosys, ret)

  call MPI_Allreduce(nerr, nerr_all, 1, MPI_INTEGER, MPI_SUM, MPI_COMM_WORLD, ierr)
  if (my_rank == 0) then
    if (nerr_all == 0) then
      write(*,'(a,a,a,i0,a)') "PASS: wrote ", trim(fname), " (", nprocs, " ranks)"
    else
      write(*,'(a,i0,a)') "FAIL: ", nerr_all, " check(s) failed"
    end if
  end if
  if (nerr_all /= 0) call MPI_Abort(MPI_COMM_WORLD, 1, ierr)
  call MPI_Finalize(ierr)

contains

  subroutine check(code, what)
    integer, intent(in) :: code
    character(len=*), intent(in) :: what
    integer :: ierr2
    if (code /= PIO_noerr) then
      write(0,'(a,i0,a,a,a,i0)') "rank ", my_rank, ": ", what, " failed, ret=", code
      call MPI_Abort(MPI_COMM_WORLD, 1, ierr2)
    end if
  end subroutine check

  subroutine expect(ok, what)
    logical, intent(in) :: ok
    character(len=*), intent(in) :: what
    if (.not. ok) then
      write(0,'(a,i0,a,a)') "rank ", my_rank, ": wrong ", what
      nerr = nerr + 1
    end if
  end subroutine expect

end program pio_zarr_f
