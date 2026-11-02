project('termuxwc', 'c', version: '0.5', default_options: [
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

# Core dependencies (wlroots 0.19.x)
wlroots_dep = dependency('wlroots-0.19', version: '>=0.19.0', required: true)
wayland_server_dep = dependency('wayland-server', required: true)
xkbcommon_dep = dependency('xkbcommon', required: true)
pixman_dep = dependency('pixman-1', required: true)

# Optional backends for the native Linux port:
#   - DRM backend works on bare-metal Linux consoles (auto-detected by wlroots)
#   - Wayland backend enables nested sessions (termuxwc inside a Wayland desktop)
#   - No X11 backend: this project is pure Wayland by design
# On Termux/Android none of these are available and the headless fallback is
# used automatically, so they are all marked optional here.
if cc.has_header('wlr/backend/drm.h')
  add_project_arguments('-DTWC_HAS_DRM', language: 'c')
endif

local_include = include_directories('include')
protocols_inc = include_directories('include/protocols')

termuxwc_sources = [
  'src/main.c',
  'src/common.c',
  'src/config.c',
  'src/layout.c',
  'src/xdg.c',
  'src/cursor.c',
  'src/inputs.c',
  'src/layers.c',
  'src/outputs.c',
  'include/protocols/xdg-shell-protocol.c',
  'include/protocols/wlr-layer-shell-unstable-v1-protocol.c',
]

executable('termuxwc',
  termuxwc_sources,
  include_directories: [local_include, protocols_inc],
  dependencies: [wlroots_dep, wayland_server_dep, xkbcommon_dep, pixman_dep],
  install: true,
)

# Demo/test client (GPU cube) — only built when its GL deps are present so
# that minimal Termux environments can still build the compositor alone.
wayland_client_dep = dependency('wayland-client', required: false)
wayland_egl_dep = dependency('wayland-egl', required: false)
egl_dep = dependency('egl', required: false)
glesv2_dep = dependency('glesv2', required: false)
m_dep = cc.find_library('m', required: false)

if wayland_client_dep.found() and wayland_egl_dep.found() and egl_dep.found() and glesv2_dep.found()
  executable('termuxwc-cube',
    [
      'tools/termuxwc-cube.c',
      'include/protocols/xdg-shell-protocol.c',
    ],
    include_directories: [local_include, protocols_inc],
    dependencies: [wayland_client_dep, wayland_egl_dep, egl_dep, glesv2_dep, m_dep],
    install: true,
  )
endif
