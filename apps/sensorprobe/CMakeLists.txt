# Main-CPU-only bring-up app: scans breakout I2C0 and identifies the
# STHS34PF80 before a driver gets written against it. No display companion on
# purpose -- flashing this leaves whatever is on the display CPU untouched.
add_executable(sensorprobe_main main/main.c)
target_link_libraries(sensorprobe_main fwog_main_bsp hardware_i2c)
fwog_main_app(sensorprobe_main
    VERSION 001
    DESCRIPTION "Scans breakout I2C0 (SDA 16 / SCL 17) and identifies the STHS34PF80")
