# Plugin sources
plugin_sources = files(
  'plugin.cpp',
  'jxl_codec.cpp',
  'dicom_handler.cpp',
  'config.cpp'
)

# Build shared library
orthanc_jxl = shared_library('OrthancJxl',
  sources: plugin_sources,
  include_directories: [inc_dirs],
  dependencies: [jxl_dep, jxl_threads_dep, dcmtk_dep, json_dep],
  cpp_args: ['-I' + meson.project_build_root()],
  name_prefix: 'lib',
  install: true,
  install_dir: get_option('libdir') / 'orthanc' / 'plugins'
)
