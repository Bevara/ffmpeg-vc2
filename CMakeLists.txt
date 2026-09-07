cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(FF_SRC_VC2
        ${CMAKE_CURRENT_SOURCE_DIR}/dec_vc2.c
)

SET(FF_LIB_VC2
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libavcodec.a
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libavutil.a
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libswscale.a
)

SET(FF_INCLUDE_VC2
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_filter(ffmpeg-vc2
        "${FF_SRC_VC2}"
        "${FF_LIB_VC2}"
        []
        GPAC_HAS_FFMPEG
        "${FF_INCLUDE_VC2}"
        ""
        "1")
