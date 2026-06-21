project('orthanc-jxl', 'cpp',
  version : '0.3.0',
  license : 'GPL-3.0-or-later',
  default_options : [
    'cpp_std=c++17',
    'warning_level=2',
    'buildtype=release',
  ]
)

# Compiler setup
cpp = meson.get_compiler('cpp')
add_project_arguments('-DORTHANC_PLUGINS_VERSION_IS_ABOVE_1_12_0=1', language: 'cpp')
add_project_arguments('-DHAS_ORTHANC_EXCEPTION=0', language: 'cpp')

# Dependencies

# libjxl via pkg-config
jxl_dep = dependency('libjxl', method: 'pkg-config')
jxl_threads_dep = dependency('libjxl_threads', method: 'pkg-config')

# nlohmann/json for config parsing
json_dep = dependency('nlohmann_json', fallback: ['nlohmann_json', 'nlohmann_json_dep'])

# DCMTK - try pkg-config first, fallback to manual
dcmtk_dep = dependency('dcmtk', method: 'pkg-config', required: false)
dcmtk_found_via_pkg = dcmtk_dep.found()
if not dcmtk_found_via_pkg
  # Manual fallback for systems without pkg-config
  dcmtk_dep = declare_dependency(
    dependencies: [
      cpp.find_library('dcmdata'),
      cpp.find_library('ofstd'),
      cpp.find_library('oflog'),
      cpp.find_library('z')
    ]
  )
endif

# Orthanc plugin SDK (header-only: orthanc/OrthancCPlugin.h)
# Resolution order: explicit override -> system header -> pinned configure-time fetch.
orthanc_sdk_override = get_option('orthanc_sdk_include')
if orthanc_sdk_override != ''
  # Arbitrary (typically absolute, out-of-tree) SDK location: use a raw -I so meson
  # does not reject absolute paths the way include_directories() does.
  orthanc_sdk_dep = declare_dependency(compile_args: ['-I' + orthanc_sdk_override])
  orthanc_sdk_source = 'override (' + orthanc_sdk_override + ')'
elif cpp.has_header('orthanc/OrthancCPlugin.h')
  orthanc_sdk_dep = declare_dependency()
  orthanc_sdk_source = 'system'
else
  orthanc_sdk_dir = meson.current_source_dir() / 'third_party' / 'orthanc-sdk'
  run_command(
    find_program('python3'),
    meson.current_source_dir() / 'scripts' / 'fetch_orthanc_sdk.py',
    '--dest', orthanc_sdk_dir,
    check: true,
  )
  orthanc_sdk_dep = declare_dependency(
    include_directories: include_directories('third_party' / 'orthanc-sdk'))
  orthanc_sdk_source = 'fetched (1.12.10)'
endif

# Version header
version_data = configuration_data()
version_data.set_quoted('PLUGIN_NAME', 'orthanc-jxl')
version_data.set_quoted('PLUGIN_VERSION', meson.project_version())
version_data.set_quoted('PLUGIN_DESCRIPTION',
  'JPEG-XL transfer syntax plugin for Orthanc')

configure_file(
  input: 'src/version.h.in',
  output: 'version.h',
  configuration: version_data
)

# Include directories
inc_dirs = include_directories('src')

# Build the plugin
subdir('src')

# Tests
if get_option('tests')
  subdir('tests')
endif

# Summary
summary({
  'Version': meson.project_version(),
  'Orthanc SDK': orthanc_sdk_source,
  'libjxl': jxl_dep.version(),
  'DCMTK': dcmtk_found_via_pkg ? dcmtk_dep.version() : 'manual',
  'Build type': get_option('buildtype'),
}, section: 'Configuration')
