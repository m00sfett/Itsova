cmake_minimum_required(VERSION 3.25)

project(glimply VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.12 REQUIRED COMPONENTS Widgets)
find_package(KF6Config REQUIRED)
find_package(KF6GlobalAccel REQUIRED)
find_package(KF6StatusNotifierItem REQUIRED)

add_executable(glimply
    src/main.cpp
)

target_link_libraries(glimply
    PRIVATE
        Qt6::Widgets
        KF6::ConfigCore
        KF6::GlobalAccel
        KF6::StatusNotifierItem
)
