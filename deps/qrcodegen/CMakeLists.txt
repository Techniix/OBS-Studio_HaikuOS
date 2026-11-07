# Bundled copy of Project Nayuki's QR Code generator (C++), v1.8.0, MIT license.
# https://github.com/nayuki/QR-Code-generator
#
# Used by obs-websocket on platforms whose package repositories don't ship qrcodegencpp (Haiku).

add_library(qrcodegencpp-bundled STATIC qrcodegen.cpp qrcodegen.hpp)
target_include_directories(qrcodegencpp-bundled PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(qrcodegencpp-bundled PUBLIC cxx_std_11)
set_target_properties(qrcodegencpp-bundled PROPERTIES POSITION_INDEPENDENT_CODE TRUE FOLDER deps)
