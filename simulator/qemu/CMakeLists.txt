cmake_minimum_required(VERSION 3.16)

# This project is only a launcher/configuration anchor for Espressif QEMU.
# Pin the target here so a clean clone can build without first running
# `idf.py set-target esp32s3`.
set(IDF_TARGET "esp32s3")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(m4_qemu_smoke)
