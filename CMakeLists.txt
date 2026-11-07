cmake_minimum_required(VERSION 3.19)
project(mpdplaycount VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)

find_package(Fooyin REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Network)

create_fooyin_plugin(
    mpdplaycount
    JSON_IN ./mpdplaycount.json.in
    DEPENDS
        Fooyin::Core
        Qt6::Network
    SOURCES
        ./mpdplaycountplugin.h
        ./mpdplaycountplugin.cpp
)
