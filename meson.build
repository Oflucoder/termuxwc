project('termuxwc', 'c',
  version: '0.3',
  default_options: ['warning_level=2', 'c_std=c11']
)

add_project_arguments('-DWLR_USE_UNSTABLE', language: 'c')

wlroots_dep = dependency('wlroots-0.20', version: '>=0.20.0', required: true)
wayland_server_dep = dependency('wayland-server', required: true)
vnc_dep = dependency('libvncserver', required: true)
terminal_dep = dependency('weston', required: true)

executable('termuxwc',
  ['src/main.c', 'include/protocols/xdg-shell-protocol.c'],
  dependencies: [wlroots_dep, wayland_server_dep, vnc_dep, terminal_dep],
  include_directories: include_directories('./include/protocols', './include'),
  install: true,
)

