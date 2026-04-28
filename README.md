# HNUCT - 医学CT图像采集与重建软件

一个基于 Qt + ITK + VTK + RTK 构建的医学CT图像软件，支持探测器控制、电机控制、X射线源控制、图像采集和FDK/SART重建算法。

## 项目架构

### 核心模块

| 模块 | 说明 | 关键文件 |
|------|------|----------|
| **探测器控制** | 对接IRay探测器SDK，负责图像采集 | `detectorcontroller.h/cpp`, `detectorworker.h/cpp`, `detector/` |
| **电机控制** | 对接MCC6运动控制卡，支持6轴控制 | `motorcontroller.h/cpp`, `motorworker.h/cpp`, `motor/` |
| **X射线控制** | 通过TCP协议控制X射线源 | `xraycontroller.h/cpp`, `xrayworker.h/cpp` |
| **图像重建** | FDK和SART两种重建算法 | `fdkpipeline.h/cpp`, `sartpipeline.h/cpp`, `reconworker.h/cpp` |
| **可视化** | 3D图像显示与窗宽窗位调节 | `imageviewer.h/cpp`, `histogramwidget.h/cpp`, `mprvolumeview.h/cpp` |

### 线程模型

项目采用多线程架构，每个硬件子系统独立运行在专用线程中：

- `DetectorWorker` - 探测器采集线程
- `MotorWorker` - 电机控制线程
- `XRayWorker` - X射线控制线程
- `AcquisitionSaveWorker` - 图像保存线程
- `ReconWorker` - 重建计算线程

`MainWindow` 作为总控层，负责界面状态管理和跨模块信号连接。

## 技术栈

- **Qt 6.x** - GUI框架，支持Widgets、SerialPort、Network模块
- **ITK 5.4** - 医学图像处理库
- **VTK 9.x** - 3D可视化库
- **RTK** - 锥形束CT重建库（支持CUDA加速）
- **Eigen3** - 线性代数库
- **CMake** - 构建工具

## 目录结构

```
HNUCT/
├── detector/          # 探测器SDK头文件和运行时DLL
│   ├── x64/           # 64位探测器驱动DLL
│   ├── IRay*.h        # 探测器SDK头文件
│   └── Util.h
├── motor/             # 电机控制库
│   ├── MCC6DLL.h      # 电机SDK头文件
│   ├── MCC6DLL_x64_Release.dll
│   └── MCC6DLL_x64_Release.lib
├── ITKFactoryRegistration/  # ITK工厂注册
├── *.h/cpp            # 主程序源码
├── mainwindow.ui      # Qt设计师界面文件
├── CMakeLists.txt     # CMake构建脚本
└── Eigen3Config.cmake # Eigen3配置
```

## 构建说明

### 前置依赖

需要预先安装以下依赖库：

| 依赖 | 版本要求 | 说明 |
|------|----------|------|
| Qt | 6.x 或 5.x | 需包含 Widgets、SerialPort、Network 模块 |
| ITK | 5.4+ | 需启用 RTK 和 ImageIO 模块 |
| VTK | 9.3+ | 核心可视化模块 |
| CUDA | 可选 | 用于GPU加速重建 |

### CMake 配置选项

```cmake
# 以下路径需根据实际安装位置调整
set(HNUCT_QT_ROOT "path/to/Qt" CACHE PATH "Qt安装根目录")
set(ITK_DIR "path/to/ITK/lib/cmake/ITK-5.4" CACHE PATH "ITK配置目录")
set(VTK_DIR "path/to/VTK/lib/cmake/vtk-9.3" CACHE PATH "VTK配置目录")
set(HNUCT_CUDA_INCLUDE_DIR "path/to/CUDA/include" CACHE PATH "CUDA头文件目录")
```

### 构建步骤

```bash
# 创建构建目录
mkdir build && cd build

# 配置CMake（Windows示例）
cmake .. -G "Visual Studio 17 2022" -A x64 ^
    -DHNUCT_QT_ROOT="F:/QT/6.10.2/msvc2022_64" ^
    -DITK_DIR="C:/ITK_INSTALL/lib/cmake/ITK-5.4" ^
    -DVTK_DIR="C:/VTK_INSTALL/lib/cmake/vtk-9.3"

# 编译
cmake --build . --config Release
```

## 功能特性

### 1. 探测器控制
- 探测器连接与断开
- 实时图像采集（单帧/连续模式）
- 采集参数设置（偏移校正、增益校正、缺陷校正）
- 图像预览与保存

### 2. 电机控制
- 6轴电机独立控制
- 绝对移动/相对移动
- 归零操作
- 急停功能
- 速度与加速度参数配置

### 3. X射线控制
- 高压开关控制
- kV/mA参数设置
- 状态实时监测

### 4. 图像重建
- **FDK算法**：滤波反投影重建，支持CUDA加速
- **SART算法**：代数重建技术
- 重建参数配置（视野、体素尺寸等）

### 5. 可视化
- 多平面重建（MPR）
- 窗宽窗位调节
- 图像缩放与切片选择
- 直方图显示

## 设备要求

### 硬件
- IRay平板探测器
- MCC6运动控制卡
- X射线源（支持TCP控制协议）

### 软件
- Windows 10/11 (64位)
- Visual Studio 2022 或更高版本

## 许可证

本项目仅供研究和教学使用。探测器SDK、电机控制库等第三方组件需遵守各自的许可协议。

## 注意事项

1. **探测器DLL**：`detector/x64/` 目录包含厂商提供的运行时库，需确保与探测器硬件版本匹配
2. **电机驱动**：`motor/` 目录包含MCC6卡的驱动文件，需正确安装硬件驱动
3. **CUDA加速**：启用CUDA需在编译ITK/RTK时开启GPU支持
4. **工作目录**：运行前需设置正确的工作目录，用于保存采集图像和重建结果
