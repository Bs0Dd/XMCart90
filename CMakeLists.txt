cmake_minimum_required(VERSION 3.13)

# initialize the SDK based on PICO_SDK_PATH
# note: this must happen before project()
include(pico_sdk_import.cmake)
include(pico_extras_import.cmake)

project(xmcart C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 11)

# initialize the Raspberry Pi Pico SDK
pico_sdk_init()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "build/")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "build/")

add_executable(xmcart
    main.c
    msc_disk.c
    usb_descriptors.c
    fatfs/diskio.c
    fatfs/ff.c
)

pico_generate_pio_header(xmcart ${CMAKE_CURRENT_LIST_DIR}/mk90.pio)

target_include_directories(xmcart PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(
    xmcart 
    pico_stdlib 
    tinyusb_device 
    hardware_flash
	hardware_sleep
    hardware_pio
    hardware_adc
    hardware_xosc
)

pico_set_binary_type(xmcart copy_to_ram)

# create map/bin/hex/uf2 file in addition to ELF.
pico_add_extra_outputs(xmcart)

add_custom_command(TARGET xmcart
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/build/xmcart.uf2 xmcart.uf2
)
