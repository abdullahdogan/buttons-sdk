cmake_minimum_required(VERSION 3.16)
project(buttons_sdk C)

set(CMAKE_C_STANDARD 11)
set(BUTTONS_VERSION 0.1.0)
option(BUILD_SHARED_LIBS "Build shared library" ON)

# --- KÜTÜPHANE (yalnızca gpiod backend) ---
add_library(buttons
    src/buttons.c
    src/gpio_gpiod.c
)
target_include_directories(buttons PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(buttons PUBLIC gpiod pthread)
target_compile_definitions(buttons PRIVATE BUTTONS_VERSION_STR="${BUTTONS_VERSION}")


# zorunlu: sanal klavye daemon'u
add_executable(keypad-hid examples/keypad-hid.c)
target_link_libraries(keypad-hid PRIVATE buttons)
target_include_directories(keypad-hid PRIVATE ${CMAKE_SOURCE_DIR}/include)

# --- KURULUM ---
include(GNUInstallDirs)
install(TARGETS buttons
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS keypad-hid RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# (demo'yu kurmak istemiyorsan bu satırı çıkar)

# pkg-config dosyası
configure_file(${CMAKE_SOURCE_DIR}/pkgconfig/buttons.pc.in
               ${CMAKE_BINARY_DIR}/buttons.pc @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/buttons.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
