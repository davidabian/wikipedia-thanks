#!/bin/bash
set -euo pipefail

g++ -O3 -march=native -flto -std=c++17 \
  -DEXTRACT_THANKS_VERBOSE_CAPABLE=0 \
  -D_GLIBCXX_ASSERTIONS \
  -Wall -Wextra -Wpedantic -Werror \
  -Wformat=2 -Wformat-security \
  -Wnull-dereference \
  -Wimplicit-fallthrough=5 \
  -Wvla \
  extract_thanks.cpp \
  $(pkg-config --cflags --libs libxml-2.0) -lz \
  -o extract_thanks
