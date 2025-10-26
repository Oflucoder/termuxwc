project('termuxwc', 'c',
  version: '0.3',
  default_options: ['warning_level=2', 'c_std=c11']
)

add_project_arguments('-DWLR_USE_UNSTABLE', language: 'c')

wlroots_dep = dependency('wlroots-0.19', required: true)
wayland_server_dep = dependency('wayland-server', required: true)
vnc_dep = dependency('libvncserver', required: true)

executable('termuxwc',
  ['src/main.c', 'include/protocols/xdg-shell-protocol.c'],
  dependencies: [wlroots_dep, wayland_server_dep, vnc_dep],
  include_directories: include_directories('include/protocols'),
  install: true,
)

