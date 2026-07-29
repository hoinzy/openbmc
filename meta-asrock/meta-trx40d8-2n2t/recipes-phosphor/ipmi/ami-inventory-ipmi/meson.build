project(
  'ami-inventory-ipmi',
  'cpp',
  version: '0.1',
  meson_version: '>=1.1.1',
  default_options: [
    'cpp_std=c++23',
    'warning_level=3',
    'werror=true',
  ])

cpp = meson.get_compiler('cpp')
add_project_arguments(
  cpp.get_supported_arguments([
    '-Wno-psabi',
  ]),
  language: 'cpp')

root_inc = include_directories('.')

ipmid_dep = dependency('libipmid')
logging_dep = dependency('phosphor-logging')
sdbusplus_dep = dependency('sdbusplus', required: false, include_type: 'system')

parser_lib = static_library(
  'ami-inventory-parser',
  'ami_inventory.cpp',
  include_directories: root_inc,
  implicit_include_directories: false)

parser_dep = declare_dependency(
  include_directories: root_inc,
  link_with: parser_lib)

library(
  'amiinventory',
  'ami_inventory_handler.cpp',
  include_directories: root_inc,
  implicit_include_directories: false,
  dependencies: [parser_dep, ipmid_dep, logging_dep, sdbusplus_dep],
  override_options: ['b_lundef=false'],
  version: meson.project_version(),
  install: true,
  install_dir: get_option('libdir') / 'ipmid-providers')

parser_test = executable(
  'ami-inventory-parser-test',
  'test_ami_inventory.cpp',
  include_directories: root_inc,
  implicit_include_directories: false,
  dependencies: parser_dep)

test('ami-inventory-parser', parser_test)
