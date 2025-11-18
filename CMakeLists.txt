cmake_minimum_required(VERSION 3.16)
project(buttons_sdk C)

set(CMAKE_C_STANDARD 11)
set(BUTTONS_VERSION 0.1.0)

# RPATH (yerel kurulumlarda .so bulunabilirliği)
set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib")
set(CMAKE_BUILD_WITH_INSTALL_RPATH OFF)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)

# Paylaşımlı kütüphane
option(BUILD_SHARED_LIBS "Build shared library" ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Varsayılanı Release yap (isteğe bağlı)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# --- libgpiod sürüm tespiti (pkg-config) ---
find_package(PkgConfig REQUIRED)
pkg_check_modules(GPIOD REQUIRED IMPORTED_TARGET gpiod)
message(STATUS "Detected libgpiod version: ${GPIOD_VERSION}")

# --- KÜTÜPHANE (yalnızca gpiod backend) ---
add_library(buttons
    src/buttons.c
    src/gpio_gpiod.c
)

# libgpiod v1/v2 için derleme bayrağı
if(GPIOD_VERSION VERSION_GREATER_EQUAL "2.0")
  target_compile_definitions(buttons PRIVATE USE_GPIOD_V2=1)
else()
  target_compile_definitions(buttons PRIVATE USE_GPIOD_V1=1)
endif()

# Genel sürüm bilgisi
target_compile_definitions(buttons PRIVATE BUTTONS_VERSION_STR="${BUTTONS_VERSION}")

# Dahil dizinleri ve linkler
target_include_directories(buttons PUBLIC
  ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(buttons PUBLIC
  PkgConfig::GPIOD
  pthread
)

# --- Zorunlu: sanal klavye uygulaması ---
add_executable(keypad-hid examples/keypad-hid.c)
target_link_libraries(keypad-hid PRIVATE buttons)
target_include_directories(keypad-hid PRIVATE ${CMAKE_SOURCE_DIR}/include)

# --- KURULUM ---
include(GNUInstallDirs)

# libbuttons.so / libbuttons.a
install(TARGETS buttons
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

# headers
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# ikili
install(TARGETS keypad-hid RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# pkg-config dosyası
configure_file(${CMAKE_SOURCE_DIR}/pkgconfig/buttons.pc.in
               ${CMAKE_BINARY_DIR}/buttons.pc @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/buttons.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# opsiyonel yardımcı betik (varsa kur)
if(EXISTS ${CMAKE_SOURCE_DIR}/scripts/keypadctl)
  install(PROGRAMS scripts/keypadctl DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
