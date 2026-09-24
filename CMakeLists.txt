cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(solar-node)

target_sources(app PRIVATE 
    src/main.cpp
    src/wifi_autoconnect.c
    src/mqtt_client.c
)