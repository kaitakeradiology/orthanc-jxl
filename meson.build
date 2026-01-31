project('orthanc-jxl', 'cpp',
  version : '0.2.0',
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

# Orthanc SDK (header-only, system-installed)
# Use compiler argument for system include path
add_project_arguments('-I/usr/include/orthanc', language: 'cpp')

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
  'libjxl': jxl_dep.version(),
  'DCMTK': dcmtk_found_via_pkg ? dcmtk_dep.version() : 'manual',
  'Build type': get_option('buildtype'),
}, section: 'Configuration')
