benchmark_exe = executable('jxl-benchmark',
  'benchmark.cpp',
  '../src/jxl_codec.cpp',
  '../src/dicom_handler.cpp',
  include_directories: inc_dirs,
  dependencies: [jxl_dep, jxl_threads_dep, dcmtk_dep],
)
