project(
  'ami-host-inventory',
  'cpp',
  meson_version: '>=1.1.1',
  default_options: [
    'cpp_std=c++23',
    'warning_level=3',
    'werror=true',
  ],
  version: '1.0',
)

boost = dependency('boost', modules: ['system'])
nlohmann_json = dependency('nlohmann_json')
phosphor_logging = dependency('phosphor-logging')
sdbusplus = dependency('sdbusplus')
threads = dependency('threads')

inventory = static_library(
  'ami-inventory-parser',
  'ami_inventory.cpp',
  dependencies: [nlohmann_json],
)

bios = static_library(
  'ami-bios-parser',
  'ami_bios.cpp',
  dependencies: [nlohmann_json, sdbusplus],
)

executable(
  'ami-host-inventory',
  'ami_inventory_daemon.cpp',
  dependencies: [
    boost,
    nlohmann_json,
    phosphor_logging,
    sdbusplus,
    threads,
  ],
  install: true,
  link_with: [inventory, bios],
)

if get_option('tests').enabled()
  gtest = dependency('gtest', main: true)
  test_executable = executable(
    'test-ami-host-inventory',
    'test_ami_inventory.cpp',
    dependencies: [gtest, nlohmann_json],
    link_with: inventory,
  )
  test('ami-host-inventory', test_executable)

  test_bios_executable = executable(
    'test-ami-bios',
    'test_ami_bios.cpp',
    dependencies: [gtest, nlohmann_json, sdbusplus],
    link_with: bios,
  )
  test('ami-bios', test_bios_executable)
endif
