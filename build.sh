#!/usr/bin/env bash
# ******************************************************
# DESC    :
# AUTHOR  : Alex Stocks
# VERSION : 1.0
# LICENCE : Apache License 2.0
# EMAIL   : alexstocks@foxmail.com
# MOD     : 2023-10-18 20:38
# FILE    : build.sh
# ******************************************************

STAGED_INSTALL_PREFIX=./deps/
INSTALL_INCLUDEDIR=$STAGED_INSTALL_PREFIX/include
INSTALL_LIBDIR=$STAGED_INSTALL_PREFIX/lib
CMAKE_INSTALL_LIBDIR
LIB_BUILD_TYPE=DEBUG
rm -rf ./build/*
cmake  -DCMAKE_INSTALL_PREFIX=${STAGED_INSTALL_PREFIX} -DCMAKE_INSTALL_INCLUDEDIR=${INSTALL_INCLUDEDIR} -DCMAKE_INSTALL_LIBDIR=${INSTALL_LIBDIR} -DCMAKE_BUILD_TYPE=${LIB_BUILD_TYPE} ../

