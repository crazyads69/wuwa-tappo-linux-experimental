cmake_minimum_required(VERSION 3.21)
project(wuwatappo-qt VERSION 1.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
include(GNUInstallDirs)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Network Qml Quick DBus Widgets Test)
find_package(PkgConfig QUIET)

# The web view (QtWebEngine) and the layer-shell helper are optional at *build* time: when their development
# packages are missing we declare the two entry points we use ourselves and link the runtime libraries by path.
# At run time both are required (the app prints a clear error if they are absent).
find_package(Qt6 QUIET OPTIONAL_COMPONENTS WebEngineQuick)
if(TARGET Qt6::WebEngineQuick)
    set(WW_WEBENGINE_LIB Qt6::WebEngineQuick)
    set(WW_HAVE_WEBENGINE_HEADERS 1)
else()
    find_library(WW_WEBENGINE_LIB NAMES libQt6WebEngineQuick.so.6 REQUIRED)
endif()
find_package(LayerShellQt QUIET)
if(TARGET LayerShellQt::Interface)
    set(WW_LAYERSHELL_LIB LayerShellQt::Interface)
    set(WW_HAVE_LAYERSHELL_HEADERS 1)
else()
    find_library(WW_LAYERSHELL_LIB NAMES libLayerShellQtInterface.so.6)
endif()
if(PkgConfig_FOUND)
    pkg_check_modules(WAYLAND_CLIENT QUIET IMPORTED_TARGET wayland-client)
    pkg_check_modules(XCB QUIET IMPORTED_TARGET xcb xcb-shape)
endif()

# Everything testable without a display.
add_library(wwcore STATIC
    src/regions.cpp src/store.cpp src/wsframe.cpp src/inputlogic.cpp)
target_include_directories(wwcore PUBLIC src)
target_link_libraries(wwcore PUBLIC Qt6::Core Qt6::Gui)

add_executable(wuwatappo-qt
    src/main.cpp src/backend.cpp src/windows.cpp src/httpserver.cpp src/wsclient.cpp
    src/inputmonitor.cpp src/desktop.cpp src/tray.cpp src/platform.cpp
    resources.qrc)
target_link_libraries(wuwatappo-qt PRIVATE wwcore Qt6::Network Qt6::Qml Qt6::Quick Qt6::DBus Qt6::Widgets
    ${WW_WEBENGINE_LIB})
target_compile_definitions(wuwatappo-qt PRIVATE WW_VERSION="${PROJECT_VERSION}"
    $<$<BOOL:${WW_HAVE_WEBENGINE_HEADERS}>:WW_HAVE_WEBENGINE_HEADERS>
    $<$<BOOL:${WW_HAVE_LAYERSHELL_HEADERS}>:WW_HAVE_LAYERSHELL_HEADERS>
    $<$<BOOL:${WW_LAYERSHELL_LIB}>:WW_HAVE_LAYERSHELL>
    $<$<TARGET_EXISTS:PkgConfig::WAYLAND_CLIENT>:WW_HAVE_WAYLAND_CLIENT>
    $<$<TARGET_EXISTS:PkgConfig::XCB>:WW_HAVE_XCB>)
if(WW_LAYERSHELL_LIB)
    target_link_libraries(wuwatappo-qt PRIVATE ${WW_LAYERSHELL_LIB})
endif()
if(TARGET PkgConfig::WAYLAND_CLIENT)
    target_link_libraries(wuwatappo-qt PRIVATE PkgConfig::WAYLAND_CLIENT)
endif()
if(TARGET PkgConfig::XCB)
    target_link_libraries(wuwatappo-qt PRIVATE PkgConfig::XCB)
endif()

install(TARGETS wuwatappo-qt RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/com.kuro.wuwatappo.global.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/com.kuro.wuwatappo.global.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
# The icon comes from the official installer (see scripts/setup.sh), so it is not shipped in git.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../dist/app-icon.png)
    install(FILES ../dist/app-icon.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps
            RENAME com.kuro.wuwatappo.global.png)
endif()
if(IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../dist)
    install(DIRECTORY ../dist/ DESTINATION ${CMAKE_INSTALL_DATADIR}/wuwatappo/dist)
endif()

enable_testing()
add_subdirectory(tests)
