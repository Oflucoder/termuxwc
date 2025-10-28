project('termuxwc', 'c',
  version: '0.3',
  default_options: ['warning_level=2', 'c_std=c11']
)

add_project_arguments('-DWLR_USE_UNSTABLE', language: 'c')

# Sistemdeki eksik wlroots kurulumuna hâlâ ihtiyacımız var
wlroots_dep = dependency('wlroots-0.20', version: '>=0.20.0', required: true)
wayland_server_dep = dependency('wayland-server', required: true)
vnc_dep = dependency('libvncserver', required: true)

# Kendi manuel eklediğimiz header'ları bulması için
local_include = include_directories('./include')
local_protocols_inc = include_directories('./include/protocols')

executable('termuxwc',
  [
    'src/main.c', 
    'include/protocols/xdg-shell-protocol.c',
    'src/matrix.c'  # <-- EKSİK C KODUNU BURADA DERLİYORUZ
  ],
  dependencies: [wlroots_dep, wayland_server_dep, vnc_dep],
  
  # <-- HEADER DOSYASINI BURADA BULUYOR
  include_directories: [local_protocols_inc, local_include], 
  
  install: true,
)
