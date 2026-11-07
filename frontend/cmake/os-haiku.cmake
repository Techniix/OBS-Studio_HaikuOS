target_sources(
  obs-studio
  PRIVATE
    utility/CrashHandler_Haiku.cpp
    utility/NativeEventFilter.cpp
    utility/platform-haiku.cpp
    utility/system-info-haiku.cpp
)
target_compile_definitions(
  obs-studio
  PRIVATE OBS_INSTALL_PREFIX="${OBS_INSTALL_PREFIX}" $<$<BOOL:${ENABLE_PORTABLE_CONFIG}>:ENABLE_PORTABLE_CONFIG>
)

# libbe: BMessage/BString and BLocaleRoster (the locale kit is part of libbe), libnetwork: sockets
target_link_libraries(obs-studio PRIVATE be network)

if(TARGET OBS::python)
  find_package(Python REQUIRED COMPONENTS Interpreter Development)
  target_link_libraries(obs-studio PRIVATE Python::Python)
endif()

# Haiku packages describe the application through resources rather than .desktop files; the HaikuPorts
# recipe adds the Deskbar entry. Install the icon so the recipe can pick it up.
install(
  FILES cmake/linux/icons/obs-logo-256.png
  DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-studio/images"
  RENAME obs-logo-256.png
)
