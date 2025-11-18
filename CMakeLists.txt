cmake_minimum_required(VERSION 3.16)
project(buttons_sdk C)

set(CMAKE_C_STANDARD 11)
set(BUTTONS_VERSION 0.1.0)

# RPATH (yerel kurulumlarda .so bulunabilirliği)
set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib")
set(CMAKE_BUILD_WITH_INSTALL_RPATH OFF)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)

option(BUILD_SHARED_LIBS "Build shared library" ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------- libgpiod tespiti (pkg-config) ----------
find_package(PkgConfig REQUIRED)

# Önce libgpiod adına bak (Debian/Ubuntu/Arch çoğunluk)
pkg_check_modules(LIBGPIOD QUIET IMPORTED_TARGET libgpiod)

# Bulunamazsa bazı dağıtımlarda 'gpiod' adı kullanılıyor olabilir
if(NOT LIBGPIOD_FOUND)
  pkg_check_modules(GPIOD QUIET IMPORTED_TARGET gpiod)
  if(NOT GPIOD_FOUND)
    message(FATAL_ERROR "pkg-config: libgpiod.pc veya gpiod.pc bulunamadı (libgpiod-dev kurulu mu?)")
  endif()
  set(GPIOD_TGT PkgConfig::GPIOD)
  set(LIBGPIOD_VERSION "${GPIOD_VERSION}")
else()
  set(GPIOD_TGT PkgConfig::LIBGPIOD)
  set(LIBGPIOD_VERSION "${LIBGPIOD_VERSION}")
endif()

message(STATUS "Detected libgpiod version: ${LIBGPIOD_VERSION}")

# ---------- KÜTÜPHANE ----------
add_library(buttons
  src/buttons.c
  src/gpio_gpiod.c
)

# v1/v2 derleme bayrağı
if(LIBGPIOD_VERSION VERSION_GREATER_EQUAL "2.0")
  target_compile_definitions(buttons PRIVATE USE_GPIOD_V2=1)
else()
  target_compile_definitions(buttons PRIVATE USE_GPIOD_V1=1)
endif()

target_compile_definitions(buttons PRIVATE BUTTONS_VERSION_STR="${BUTTONS_VERSION}")

target_include_directories(buttons PUBLIC
  ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(buttons PUBLIC
  ${GPIOD_TGT}
  pthread
)

# ---------- Uygulama ----------
add_executable(keypad-hid examples/keypad-hid.c)
target_link_libraries(keypad-hid PRIVATE buttons)
target_include_directories(keypad-hid PRIVATE ${CMAKE_SOURCE_DIR}/include)

# ---------- Kurulum ----------
include(GNUInstallDirs)

install(TARGETS buttons
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(TARGETS keypad-hid
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# pkg-config dosyası
configure_file(${CMAKE_SOURCE_DIR}/pkgconfig/buttons.pc.in
               ${CMAKE_BINARY_DIR}/buttons.pc @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/buttons.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# opsiyonel yardımcı betik
if(EXISTS ${CMAKE_SOURCE_DIR}/scripts/keypadctl)
  install(PROGRAMS scripts/keypadctl DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
