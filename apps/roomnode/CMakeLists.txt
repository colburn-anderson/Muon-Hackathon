# Room node: a display app and its main companion, one deliverable. The main
# UF2 carries the display image inside it, so `fw flash roomnode` is the whole
# update -- there is no separate display step, by design (see AGENTS.md).
#
# The display half includes ../../apps/roomnode/room_state.h, which is the wire
# format between the two: keeping it beside this file rather than inside either
# half is what keeps each side honest about the other.
# qrcodegen (Project Nayuki, MIT -- see LICENSES/qrcodegen-MIT.txt) is vendored
# rather than depended on: a display app that stops building because a submodule
# moved is a display that shows nothing.
add_executable(roomnode_display display/main.c vendor/qrcodegen/qrcodegen.c)
target_include_directories(roomnode_display PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/qrcodegen)
target_link_libraries(roomnode_display fwog_display_bsp)
fwog_display_app(roomnode_display
    VERSION 001
    DESCRIPTION "Room status display: state, elapsed time, alerts, reservation QR")

add_executable(roomnode_main main/main.c
    main/presence.c main/servo.c main/esp_link.c)
target_include_directories(roomnode_main PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# hardware_pwm is not in fwog_main_bsp: the servo is the first main-CPU user.
target_link_libraries(roomnode_main fwog_main_bsp hardware_pwm)
# Must come after target_link_libraries: the generated source needs
# fwog_main_bsp's include path.
fwog_embed_display_image(roomnode_main)
fwog_main_app(roomnode_main
    VERSION 001
    DESCRIPTION "Room node main CPU: presence sensor, door-lock servo, ESP32-C6 link")
