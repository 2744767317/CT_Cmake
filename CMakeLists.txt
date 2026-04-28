
cmake_minimum_required(VERSION 3.16)
project(HNUCT VERSION 0.1 LANGUAGES CXX)

# -------------------------
# Qt 自动代码生成开关
# AUTOUIC  : 让 .ui 自动生成界面代码
# AUTOMOC  : 让包含 Q_OBJECT 的类自动生成 moc 文件
# AUTORCC  : 如果后续加入 .qrc，可自动处理资源
# -------------------------
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# 统一使用 C++17。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -------------------------
# 电机控制库
# 这是厂商提供的现成 DLL/导入库，不参与本项目源码编译，
# 这里只把它声明成“导入目标”，后续直接链接即可。
# -------------------------
set(MOTOR_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/motor)
add_library(MCC6DLL SHARED IMPORTED)
set_target_properties(MCC6DLL PROPERTIES
    IMPORTED_IMPLIB "${MOTOR_ROOT}/MCC6DLL_x64_Release.lib"
    IMPORTED_LOCATION "${MOTOR_ROOT}/MCC6DLL_x64_Release.dll"
)

# -------------------------
# 探测器 SDK 运行目录
# post-build 阶段会把这里的整套 DLL 拷贝到可执行文件旁边。
# -------------------------
set(IRAY_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/detector)
set(IRAY_BIN_DIR ${IRAY_ROOT}/x64)

# -------------------------
# 第三方依赖路径
# 这些默认值对应当前开发机环境，但都做成 CACHE PATH，
# 这样在 Qt Creator / CMake GUI / 命令行里都可以直接覆盖，
# 不需要每次手改 CMakeLists.txt。
# -------------------------
set(HNUCT_QT_ROOT
    "F:/QT/6.10.2/msvc2022_64"
    CACHE PATH "Qt 安装根目录（应包含 lib/cmake/Qt6）")
set(ITK_DIR
    "C:/Users/Administrator/Desktop/GuangSuo/ITK_INSTALL/install_debug/lib/cmake/ITK-5.4"
    CACHE PATH "ITKConfig.cmake 所在目录")
set(HNUCT_ITK_MODULES_DIR
    "C:/Users/Administrator/Desktop/GuangSuo/ITK_INSTALL/install_debug/lib/cmake/ITK-5.4/Modules"
    CACHE PATH "ITK Modules 目录，用于补充 CMAKE_PREFIX_PATH")
set(VTK_DIR
    "C:/Users/Administrator/Desktop/GuangSuo/VTK_INSTALL/install_debug/lib/cmake/vtk-9.3"
    CACHE PATH "vtk-config.cmake 所在目录")
set(HNUCT_CUDA_INCLUDE_DIR
    "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.8/include"
    CACHE PATH "当 CudaCommon 目标存在时，强制使用的 CUDA include 目录")

list(PREPEND CMAKE_PREFIX_PATH
    "${HNUCT_QT_ROOT}"
    "${HNUCT_ITK_MODULES_DIR}"
    "${VTK_DIR}")

# -------------------------
# 查找 Qt / ITK / VTK
# Qt 既兼容 Qt6 也兼容 Qt5，但当前工程实际主要按 Qt6 路径开发。
# ITK 这里显式要求 RTK 与 ImageIO，是本项目重建链路的核心依赖。
# -------------------------
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets SerialPort Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets SerialPort Network)

find_package(ITK REQUIRED COMPONENTS RTK ITKImageIO)
include(${ITK_USE_FILE})

find_package(VTK REQUIRED COMPONENTS
    vtkCommonCore
    vtkCommonDataModel
    vtkIOImage
    vtkImagingCore
    vtkFiltersCore
    vtkFiltersSources
    vtkRenderingCore
    vtkRenderingOpenGL2
    vtkInteractionStyle
    vtkRenderingFreeType
)
include(${VTK_USE_FILE})

# 某些 RTK / ITK 组合下会导出 CudaCommon 目标。
# 如果该目标存在，则把头文件路径显式改到当前机器使用的 CUDA 版本，
# 避免被旧版本 CUDA 路径污染。
if(TARGET CudaCommon)
    set_target_properties(CudaCommon PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${HNUCT_CUDA_INCLUDE_DIR}"
    )
endif()

# 主界面相关源码单独列出来，方便后续继续拆分模块。
set(PROJECT_SOURCES
        main.cpp
        mainwindow.cpp
        mainwindow.h
        mainwindow.ui
)

# -------------------------
# 主可执行程序
# 当前项目是单 EXE 架构，设备控制、采集、重建和可视化都编译进同一程序。
# -------------------------
if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(HNUCT
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
        logger.h logger.cpp
        xraycontroller.h xraycontroller.cpp
        xrayworker.h xrayworker.cpp
        detector/IRayAttrDef.h detector/IRayAuthDef.h detector/IRayCmdDef.h detector/IRayCommon.h detector/IRayDetFinder.h detector/IRayEnumDef.h detector/IRayErrDef.h detector/IRayEventDef.h detector/IRayFpdSys.h detector/IRayFpdSysEx.h detector/IRayImage.h detector/IRayInclude.h detector/IRayVariant.h
        detector.cpp detector.h
        winevent.cpp winevent.h
        detector/Util.h
        detectorcontroller.cpp detectorcontroller.h detectorworker.cpp detectorworker.h
        acquisitionsaveworker.h acquisitionsaveworker.cpp
        fdkpipeline.h fdkpipeline.cpp
        motorcontroller.h motorcontroller.cpp
        motorworker.h motorworker.cpp
        motor/MCC6DLL.h
        sartpipeline.h sartpipeline.cpp
        workdirmanager.h workdirmanager.cpp
        reconworker.h reconworker.cpp
        imageviewer.h imageviewer.cpp
        histogramwidget.h histogramwidget.cpp
        visualizationtypes.h
        mprsliceview.h mprsliceview.cpp
        mprvolumeview.h mprvolumeview.cpp
    )
# Define target properties for Android with Qt 6 as:
#    set_property(TARGET HNUCT APPEND PROPERTY QT_ANDROID_PACKAGE_SOURCE_DIR
#                 ${CMAKE_CURRENT_SOURCE_DIR}/android)
# For more information, see https://doc.qt.io/qt-6/qt-add-executable.html#target-creation
else()
    if(ANDROID)
        add_library(HNUCT SHARED
            ${PROJECT_SOURCES}
        )
# Define properties for Android with Qt 5 after find_package() calls as:
#    set(ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android")
    else()
        add_executable(HNUCT
            ${PROJECT_SOURCES}
        )
    endif()
endif()

target_link_libraries(HNUCT PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::SerialPort
    Qt${QT_VERSION_MAJOR}::Network
    MCC6DLL
    ${ITK_LIBRARIES}
    ${VTK_LIBRARIES}
)

vtk_module_autoinit(
    TARGETS HNUCT
    MODULES ${VTK_LIBRARIES}
)

# -------------------------
# 运行时部署
# 目标是让编译产出的 HNUCT.exe 在输出目录中尽量具备“可直接运行”的条件：
# 1. 拷贝电机控制 DLL
# 2. 拷贝探测器 SDK 整个运行目录
# 3. 拷贝编译器/三方运行时 DLL
# 4. 执行 windeployqt，把 Qt 插件和模块补齐
# -------------------------
add_custom_command(TARGET HNUCT POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_PROPERTY:MCC6DLL,IMPORTED_LOCATION>"
            "$<TARGET_FILE_DIR:HNUCT>"
)

add_custom_command(TARGET HNUCT POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${IRAY_BIN_DIR}"
            $<TARGET_FILE_DIR:HNUCT>
)

add_custom_command(TARGET HNUCT POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:HNUCT>
        $<TARGET_FILE_DIR:HNUCT>
    COMMAND_EXPAND_LISTS
)

get_target_property(_qmake_executable Qt6::qmake IMPORTED_LOCATION)
get_filename_component(_qt_bin_dir "${_qmake_executable}" DIRECTORY)
find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${_qt_bin_dir}")
add_custom_command(TARGET HNUCT POST_BUILD
    COMMAND "${WINDEPLOYQT_EXECUTABLE}"
            --no-compiler-runtime
            --no-translations
            "$<TARGET_FILE:HNUCT>"
    COMMENT "Running windeployqt..."
)

# 下面这段主要是 Qt 模板自带的 bundle 属性设置。
# 对当前 Windows 桌面程序影响不大，但保留后续跨平台扩展的空间。
if(${QT_VERSION} VERSION_LESS 6.1.0)
  set(BUNDLE_ID_OPTION MACOSX_BUNDLE_GUI_IDENTIFIER com.example.HNUCT)
endif()
set_target_properties(HNUCT PROPERTIES
    ${BUNDLE_ID_OPTION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

# 安装规则：目前只安装主程序目标本身。
include(GNUInstallDirs)
install(TARGETS HNUCT
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Qt6 目标最后做一次 finalize，补齐自动生成的细节。
if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(HNUCT)
endif()
