#pragma once

#include <QColor>
#include <QImage>
#include <QPoint>
#include <QWidget>

#include <array>

#include <vtkSmartPointer.h>

class vtkActor;
class vtkImageData;
class vtkMarchingCubes;
class vtkPolyDataMapper;
class vtkRenderWindow;
class vtkRenderer;
class vtkSphereSource;
class vtkWindowToImageFilter;

/*
 * 右下角 3D 标签预览窗口。
 * 这里不依赖 QVTKOpenGLNativeWidget，而是使用 VTK 离屏渲染后再贴到 QWidget，
 * 先落一版可运行的 3D 标签表面预览，支持基础旋转和缩放。
 */
class MprVolumeView : public QWidget
{
    Q_OBJECT

public:
    explicit MprVolumeView(QWidget* parent = nullptr);

    void setLabelData(vtkSmartPointer<vtkImageData> labelData);
    void setCrosshair(const std::array<int, 3>& crosshair);
    void setBackgroundColor(const QColor& color);
    void setLabelVisible(bool visible);
    void setLabelOpacity(double opacity);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    QSize minimumSizeHint() const override;

private:
    void initializeScene();
    void rebuildSurface();
    void updateMarker();
    void renderToCache();

private:
    vtkSmartPointer<vtkImageData> m_labelData;
    vtkSmartPointer<vtkRenderer> m_renderer;
    vtkSmartPointer<vtkRenderWindow> m_renderWindow;
    vtkSmartPointer<vtkWindowToImageFilter> m_windowToImage;
    vtkSmartPointer<vtkMarchingCubes> m_surfaceExtractor;
    vtkSmartPointer<vtkPolyDataMapper> m_surfaceMapper;
    vtkSmartPointer<vtkActor> m_surfaceActor;
    vtkSmartPointer<vtkSphereSource> m_markerSource;
    vtkSmartPointer<vtkPolyDataMapper> m_markerMapper;
    vtkSmartPointer<vtkActor> m_markerActor;

    std::array<int, 3> m_crosshair{{0, 0, 0}};
    QColor m_backgroundColor{QColor("#0f172a")};
    QPoint m_lastMousePos;
    bool m_leftButtonPressed = false;
    bool m_labelVisible = true;
    double m_labelOpacity = 0.9;
    bool m_hasVisibleSurface = false;
    bool m_initialized = false;
    bool m_resetCameraOnNextSurface = true;
    QImage m_cachedImage;
};
