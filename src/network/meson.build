network_driver_sources = [
  'bridge_driver.c',
  'bridge_driver_platform.c',
]

if conf.has('WITH_NETWORK')
  network_driver_impl = static_library(
    'virt_driver_network_impl',
    [
      network_driver_sources,
    ],
    dependencies: [
      access_dep,
      dbus_dep,
      libnl_dep,
      src_dep,
    ],
    include_directories: [
      conf_inc_dir,
    ],
  )
endif