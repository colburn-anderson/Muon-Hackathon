# ESP32 UART monitor. A paired display+main app: main reads the ESP32 on the
# breakout UART and forwards it over the inter-CPU link; the display renders
# it as text on the LCD. Both halves also mirror the raw bytes to USB CDC.
add_executable(espuart_display display/main.c)
target_link_libraries(espuart_display fwog_display_bsp)
fwog_display_app(espuart_display
    VERSION 001
    DESCRIPTION "ESP32 UART text console on the LCD")

add_executable(espuart_main main/main.c)
target_link_libraries(espuart_main fwog_main_bsp)
# The display image rides inside this UF2 -- never flash the display directly.
fwog_embed_display_image(espuart_main)
fwog_main_app(espuart_main
    VERSION 001
    DESCRIPTION "Reads the ESP32 UART and mirrors it to the display and USB CDC")
