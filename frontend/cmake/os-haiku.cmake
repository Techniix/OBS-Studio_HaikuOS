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

# Application identity: signature, launch flags and version as Haiku resources in the executable (see
# frontend/cmake/haiku/obs-studio.rdef.in). Done like Haiku's own build (build/jam/BeOSRules): "rc -o <rsrc> <rdef>"
# compiles the definitions, "xres -o <executable> <rsrc>" replaces the executable's resources with them and
# "mimeset -f <executable>" updates its MIME attributes (BEOS:TYPE, BEOS:APP_SIG, ...) from them. The tools come
# with Haiku; if one is missing (e.g. a cross build), configuring still succeeds and the executable just has no
# resources. These POST_BUILD steps are added before set_target_properties_obs() adds the copy to the rundir, so
# the copied executable already contains the resources (resources are part of the file; the attributes set by
# mimeset are not copied, but the registrar and Qt read the signature from the resources as well).
set(OBS_HAIKU_APP_SIGNATURE "application/x-vnd.obs-studio")
if(OBS_RELEASE_CANDIDATE GREATER 0)
  set(OBS_HAIKU_APP_VARIETY B_APPV_GAMMA)
  set(OBS_HAIKU_APP_INTERNAL ${OBS_RELEASE_CANDIDATE})
elseif(OBS_BETA GREATER 0)
  set(OBS_HAIKU_APP_VARIETY B_APPV_BETA)
  set(OBS_HAIKU_APP_INTERNAL ${OBS_BETA})
else()
  set(OBS_HAIKU_APP_VARIETY B_APPV_FINAL)
  set(OBS_HAIKU_APP_INTERNAL 0)
endif()
configure_file(cmake/haiku/obs-studio.rdef.in "${CMAKE_CURRENT_BINARY_DIR}/obs-studio.rdef" @ONLY)

find_program(HAIKU_RC_EXECUTABLE rc)
find_program(HAIKU_XRES_EXECUTABLE xres)
find_program(HAIKU_MIMESET_EXECUTABLE mimeset)
mark_as_advanced(HAIKU_RC_EXECUTABLE HAIKU_XRES_EXECUTABLE HAIKU_MIMESET_EXECUTABLE)

if(HAIKU_RC_EXECUTABLE AND HAIKU_XRES_EXECUTABLE)
  set(_obs_haiku_rsrc "${CMAKE_CURRENT_BINARY_DIR}/obs-studio.rsrc")
  set(_obs_haiku_mimeset_command)
  if(HAIKU_MIMESET_EXECUTABLE)
    set(_obs_haiku_mimeset_command COMMAND "${HAIKU_MIMESET_EXECUTABLE}" -f "$<TARGET_FILE:obs-studio>")
  endif()
  add_custom_command(
    TARGET obs-studio
    POST_BUILD
    COMMAND "${HAIKU_RC_EXECUTABLE}" -o "${_obs_haiku_rsrc}" "${CMAKE_CURRENT_BINARY_DIR}/obs-studio.rdef"
    COMMAND "${HAIKU_XRES_EXECUTABLE}" -o "$<TARGET_FILE:obs-studio>" "${_obs_haiku_rsrc}"
    ${_obs_haiku_mimeset_command}
    COMMENT "Adding Haiku application resources to obs-studio (${OBS_HAIKU_APP_SIGNATURE})"
    VERBATIM
  )
  unset(_obs_haiku_rsrc)
  unset(_obs_haiku_mimeset_command)
else()
  message(
    WARNING
    "Haiku resource tools (rc, xres) not found: OBS is built without its application signature "
    "${OBS_HAIKU_APP_SIGNATURE}. Settings Renga keeps per application (screen recording permission, audio) "
    "will not stay attached to OBS."
  )
endif()

# There is no vector icon (HVIF) for OBS yet; it would go into the .rdef as "resource vector_icon".
# The HaikuPorts recipe adds the Deskbar entry. Install the PNG icon so the recipe can pick it up.
install(
  FILES cmake/linux/icons/obs-logo-256.png
  DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-studio/images"
  RENAME obs-logo-256.png
)
