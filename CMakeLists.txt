project(auto-apps)

cmake_minimum_required (VERSION 3.5)

include(GNUInstallDirs)

find_package (vsomeip3 2.6.0 REQUIRED)
set(CURSES_NEED_NCURSES TRUE)
set(CURSES_NEED_WIDE TRUE)
find_package(Curses REQUIRED)
find_package( Boost 1.55 COMPONENTS system thread log REQUIRED )

# libcaca decodes native multi-frame canvases and ASCII/ANSI artwork.  Keep
# ncursesw as the only terminal renderer (never invoke libcaca's display API).
find_path(LIBCACA_INCLUDE_DIR NAMES caca.h)
find_library(LIBCACA_LIBRARY NAMES caca)
if(NOT LIBCACA_INCLUDE_DIR OR NOT LIBCACA_LIBRARY)
    message(FATAL_ERROR "libcaca development headers/library not found (install libcaca-devel)")
endif()

set (INSTALL_LIB_DIR ${CMAKE_INSTALL_LIBDIR} CACHE PATH "Installation directory for libraries")
set (INSTALL_BIN_DIR ${CMAKE_INSTALL_BINDIR} CACHE PATH "Installation directory for executables")
set (INSTALL_INCLUDE_DIR ${CMAKE_INSTALL_INCLUDEDIR} CACHE PATH "Installation directory for header files")

include_directories (
    ${Boost_INCLUDE_DIR}
    ${VSOMEIP_INCLUDE_DIRS}
)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(radio-service radio-service.cpp services.hpp demo-health.hpp radio-demo-health.hpp radio-stations.cpp)
target_link_libraries(radio-service  vsomeip3 ${Boost_LIBRARIES})

add_executable(radio-client radio-client.cpp radio-video.cpp radio-video.hpp
               radio-client-state.hpp demo-health.hpp services.hpp)
target_include_directories(radio-client PRIVATE ${CURSES_INCLUDE_DIRS} ${LIBCACA_INCLUDE_DIR})
target_compile_definitions(radio-client PRIVATE
    RADIO_DEFAULT_VIDEO_DIR=\"${CMAKE_INSTALL_FULL_DATADIR}/auto-apps/animations\")
target_link_libraries(radio-client  vsomeip3 ${Boost_LIBRARIES} ${CURSES_LIBRARIES} ${LIBCACA_LIBRARY})

add_executable(engine-service engine-service.cpp services.hpp demo-health.hpp engine-demo-memory.hpp)
target_link_libraries(engine-service  vsomeip3 ${Boost_LIBRARIES})

install (
    TARGETS radio-service radio-client engine-service
    RUNTIME DESTINATION "${INSTALL_BIN_DIR}" COMPONENT bin
)

# Pack an immediately playable 100x30/10fps sample into RPM and container.
install(DIRECTORY animations/
        DESTINATION "${CMAKE_INSTALL_DATADIR}/auto-apps/animations"
        FILES_MATCHING PATTERN "*.caca" PATTERN "*.ansi" PATTERN "*.ans"
        PATTERN "*.txt" PATTERN "*.asc" PATTERN "*.fps")

include(CTest)
if(BUILD_TESTING)
    add_executable(radio-video-test tests/radio-video-test.cpp radio-video.cpp radio-video.hpp)
    target_include_directories(radio-video-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${LIBCACA_INCLUDE_DIR})
    target_link_libraries(radio-video-test ${LIBCACA_LIBRARY})
    target_compile_definitions(radio-video-test PRIVATE
        VIDEO_DEMO_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/animations/city-traffic.ansi\")
    add_test(NAME radio-video-test COMMAND radio-video-test)
    add_executable(demo-health-test tests/demo-health-test.cpp
                   demo-health.hpp engine-demo-memory.hpp radio-client-state.hpp)
    target_include_directories(demo-health-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME demo-health-test COMMAND demo-health-test)
    add_executable(radio-demo-health-test tests/radio-demo-health-test.cpp
                   demo-health.hpp radio-demo-health.hpp radio-client-state.hpp)
    target_include_directories(radio-demo-health-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME radio-demo-health-test COMMAND radio-demo-health-test)
endif()
