project('termuxwc', 'c', version: '0.4', default_options: [
  'warning_level=2',
  'c_std=c11',
  'buildtype=release',
  'optimization=3',
  'b_lto=true',
  'b_ndebug=true'
])

add_project_arguments([
  '-DWLR_USE_UNSTABLE',
  '-fomit-frame-pointer'
], language: 'c')

cc = meson.get_compiler('c')
wlroots_dep = dependency('wlroots-0.19', version: '>=0.19.0', required: true)
wayland_server_dep = dependency('wayland-server', required: true)
xkbcommon_dep = dependency('xkbcommon', required: true)
pixman_dep = dependency('pixman-1', required: true)

local_include = include_directories('include')
protocols_inc = include_directories('include/protocols')

executable('termuxwc',
  [
    'src/main.c',
    'include/protocols/xdg-shell-protocol.c',
    'include/protocols/wlr-layer-shell-unstable-v1-protocol.c'
  ],
  include_directories: [local_include, protocols_inc],
  dependencies: [wlroots_dep, wayland_server_dep, xkbcommon_dep, pixman_dep],
  install: true,
)

wayland_client_dep = dependency('wayland-client', required: true)
wayland_egl_dep = dependency('wayland-egl', required: true)
egl_dep = dependency('egl', required: true)
glesv2_dep = dependency('glesv2', required: true)
m_dep = cc.find_library('m', required: false)

executable('termuxwc-cube',
  [
    'tools/termuxwc-cube.c',
    'include/protocols/xdg-shell-protocol.c'
  ],
  include_directories: [local_include, protocols_inc],
  dependencies: [wayland_client_dep, wayland_egl_dep, egl_dep, glesv2_dep, m_dep],
  install: true,
)
