benchmark_exe = executable('jxl-benchmark',
  'benchmark.cpp',
  '../src/jxl_codec.cpp',
  '../src/dicom_handler.cpp',
  include_directories: inc_dirs,
  dependencies: [jxl_dep, jxl_threads_dep, dcmtk_dep],
)

roundtrip_exe = executable('jxl-roundtrip',
  'roundtrip.cpp',
  '../src/jxl_codec.cpp',
  '../src/dicom_handler.cpp',
  '../src/transcode.cpp',
  '../src/config.cpp',
  include_directories: inc_dirs,
  dependencies: [jxl_dep, jxl_threads_dep, dcmtk_dep, json_dep],
)

throughput_exe = executable('jxl-throughput',
  'throughput.cpp',
  '../src/jxl_codec.cpp',
  '../src/dicom_handler.cpp',
  '../src/transcode.cpp',
  '../src/config.cpp',
  include_directories: inc_dirs,
  dependencies: [jxl_dep, jxl_threads_dep, dcmtk_dep, json_dep],
)

test_data = meson.current_source_dir() / 'data'
test('roundtrip', roundtrip_exe, args: [
  test_data / 'test_ct1.dcm',                 # 512x512 16-bit single-frame CT
  test_data / 'synthetic_mf_gray16.dcm',      # multi-frame 16-bit grayscale
  test_data / 'synthetic_mf_rgb_planar.dcm',  # multi-frame planar RGB
])
