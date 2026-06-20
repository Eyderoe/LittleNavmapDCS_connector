cmake_minimum_required(VERSION 3.28)
project(LittleNavmapDCS_connector)

#-DCMAKE_PREFIX_PATH="D:/Qt/Qt6/6.9.3/mingw_64"
#-DCMAKE_PREFIX_PATH="/Users/eyderoe/Qt/6.9.3/macos/lib/cmake"

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)


find_package(Qt6 COMPONENTS
        Core
        Sql
        Xml
        Network
        Core5Compat
        REQUIRED)

# 通过伪 X-Plane 目录生成数据库
set(DATAREAD_EMBED_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/embed_atools")
file(GLOB_RECURSE DATAREAD_ATOOLS_SOURCES CONFIGURE_DEPENDS
        "${DATAREAD_EMBED_ROOT}/src/*.cpp"
        "${DATAREAD_EMBED_ROOT}/src/*.c"
)
# 注意：保留 fs/weather（simconnectdata → Metar → MetarParser 需要）；排除 src/grib/、src/g2clib/（GRIB2 C 库；与 dataRead 无关且链不齐）
foreach (_dataread_exclude IN ITEMS "/gui/" "/logging/" "/grib/" "/fs/grib/" "/fs/track/" "/fs/ns/" "/fs/online/" "/fs/routing/" "/timezone/database/builder/" "/g2clib/")
    list(FILTER DATAREAD_ATOOLS_SOURCES EXCLUDE REGEX ".*${_dataread_exclude}.*")
endforeach ()
list(FILTER DATAREAD_ATOOLS_SOURCES EXCLUDE REGEX "crashhandler\\.cpp$")
# 纯 CLI：不编译依赖 QAction(QtGui) 的 userdata/logbook 数据管理，导航编译不需要它们
list(FILTER DATAREAD_ATOOLS_SOURCES EXCLUDE REGEX ".*/datamanagerbase\\.cpp$")
list(FILTER DATAREAD_ATOOLS_SOURCES EXCLUDE REGEX ".*/userdatamanager\\.cpp$")
list(FILTER DATAREAD_ATOOLS_SOURCES EXCLUDE REGEX ".*/logdatamanager\\.cpp$")
# 时区边界 shapefile → 二进制库的工具链（需 shapelib），运行时只用 timezonemanager + zonedetect
list(FILTER DATAREAD_ATOOLS_SOURCES EXCLUDE REGEX ".*/timezone/main\\.cpp$")
# 纯 CLI：不编译依赖 QtGui/QtWidgets 的 util（QPainter、QTableView、QPolygonF 等），导航编译不需要
list(FILTER DATAREAD_ATOOLS_SOURCES EXCLUDE REGEX ".*/(htmlbuilder|csvexporter|paintercontextsaver|polygontools|roundedpolygon)\\.cpp$")
add_executable(dataRead
        navdataReadMain.cpp
        ${DATAREAD_ATOOLS_SOURCES}
        "${DATAREAD_EMBED_ROOT}/atools.qrc"
)
target_include_directories(dataRead PRIVATE "${DATAREAD_EMBED_ROOT}/src")
target_link_libraries(dataRead PRIVATE
        Qt6::Core
        Qt6::Sql
        Qt6::Xml
        Qt6::Network
        Qt6::Core5Compat
)
if (WIN32)
    target_compile_definitions(dataRead PRIVATE _USE_MATH_DEFINES NOMINMAX)
endif ()
target_compile_definitions(dataRead PRIVATE
        DISABLE_CRASHHANDLER
        QT_NO_CAST_FROM_BYTEARRAY
        QT_NO_CAST_TO_ASCII
        "VERSION_NUMBER_ATOOLS=\"4.1.0.embed\""
        "GIT_REVISION_ATOOLS=\"unknown\""
)
# 3.DCS 插件



if (WIN32)
    set(_lndcs_qt_prefer_debug_suffix "")
    if (MSVC AND CMAKE_BUILD_TYPE MATCHES "^[Dd]ebug$")
        set(_lndcs_qt_prefer_debug_suffix "d")
    endif ()

    macro(_lndcs_resolve_win_qt_dll stem out_var)
        set(${out_var} "")
        if (_lndcs_qt_prefer_debug_suffix STREQUAL "d" AND EXISTS "${stem}d.dll")
            set(${out_var} "${stem}d.dll")
        elseif (EXISTS "${stem}.dll")
            set(${out_var} "${stem}.dll")
        endif ()
    endmacro()

    set(QT_INSTALL_PATH "${CMAKE_PREFIX_PATH}")
    if (NOT EXISTS "${QT_INSTALL_PATH}/bin")
        set(QT_INSTALL_PATH "${QT_INSTALL_PATH}/..")
        if (NOT EXISTS "${QT_INSTALL_PATH}/bin")
            set(QT_INSTALL_PATH "${QT_INSTALL_PATH}/..")
        endif ()
    endif ()
    get_property(_lndcs_win_targets DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach (_lndcs_tgt IN LISTS _lndcs_win_targets)
        get_target_property(_lndcs_tgt_type ${_lndcs_tgt} TYPE)
        if (NOT _lndcs_tgt_type STREQUAL "EXECUTABLE")
            continue ()
        endif ()
        _lndcs_resolve_win_qt_dll("${QT_INSTALL_PATH}/plugins/platforms/qwindows" _lndcs_plat_dll)
        if (NOT _lndcs_plat_dll STREQUAL "")
            add_custom_command(TARGET ${_lndcs_tgt} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                    "$<TARGET_FILE_DIR:${_lndcs_tgt}>/plugins/platforms/")
            add_custom_command(TARGET ${_lndcs_tgt} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy
                    "${_lndcs_plat_dll}"
                    "$<TARGET_FILE_DIR:${_lndcs_tgt}>/plugins/platforms/")
        endif ()
        _lndcs_resolve_win_qt_dll("${QT_INSTALL_PATH}/plugins/sqldrivers/qsqlsqlite" _lndcs_sql_dll)
        if (NOT _lndcs_sql_dll STREQUAL "")
            add_custom_command(TARGET ${_lndcs_tgt} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                    "$<TARGET_FILE_DIR:${_lndcs_tgt}>/plugins/sqldrivers/")
            add_custom_command(TARGET ${_lndcs_tgt} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy
                    "${_lndcs_sql_dll}"
                    "$<TARGET_FILE_DIR:${_lndcs_tgt}>/plugins/sqldrivers/")
        endif ()
        foreach (QT_LIB IN ITEMS Core Sql Xml Network Core5Compat)
            _lndcs_resolve_win_qt_dll("${QT_INSTALL_PATH}/bin/Qt6${QT_LIB}" _lndcs_qt_bin_dll)
            if (NOT _lndcs_qt_bin_dll STREQUAL "")
                add_custom_command(TARGET ${_lndcs_tgt} POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy
                        "${_lndcs_qt_bin_dll}"
                        "$<TARGET_FILE_DIR:${_lndcs_tgt}>")
            endif ()
        endforeach ()
    endforeach ()
endif ()
