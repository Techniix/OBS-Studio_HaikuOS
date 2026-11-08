cmake_minimum_required(VERSION 3.28...3.30)

# OpenGL driver self-test. gl-haiku.c runs it before it uses EGL, so a driver that hangs or renders garbage
# under OBS's usage pattern costs a fallback to OSMesa instead of a frozen OBS. Installed next to the obs
# executable, where gl-haiku.c looks for it. Uses the same glad EGL/GL loaders as libobs-opengl.
add_executable(obs-haiku-gl-probe)
target_sources(obs-haiku-gl-probe PRIVATE gl-probe.c)
target_link_libraries(obs-haiku-gl-probe PRIVATE OBS::glad OpenGL::EGL)
target_compile_options(obs-haiku-gl-probe PRIVATE $<$<COMPILE_LANG_AND_ID:C,AppleClang,Clang>:-Wno-strict-prototypes>)

set_target_properties_obs(obs-haiku-gl-probe PROPERTIES FOLDER core)
