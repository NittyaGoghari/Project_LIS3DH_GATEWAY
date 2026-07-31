# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nRF_AT_gateway_final)

target_sources(app PRIVATE src/main.c)
target_sources(app PRIVATE src/atcommand.c )
target_sources(app PRIVATE src/observer.c)
target_sources(app PRIVATE src/certs.c)
target_sources(app PRIVATE src/sdcard.c)