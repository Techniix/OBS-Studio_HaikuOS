if(NOT TARGET LibDataChannel::LibDataChannel)
  add_library(LibDataChannel::LibDataChannel SHARED IMPORTED)
  set_target_properties(LibDataChannel::LibDataChannel PROPERTIES IMPORTED_LOCATION "@SYSROOT@/boot/system/develop/lib/libdatachannel.so" IMPORTED_NO_SONAME TRUE INTERFACE_INCLUDE_DIRECTORIES "@SYSROOT@/boot/system/develop/headers")
endif()
