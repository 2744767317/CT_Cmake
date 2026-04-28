#pragma once

#include <QColor>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QWidget>

#include <array>

#include <vtkSmartPointer.h>

class vtkImageData;

#include "visualizationtypes.h"

/*
 * 单个位面的 MPR 视图。
 * 该控件不直接依赖 VTK 交互窗口，而是基于 vtkImageData 做切面抽样，
 * 再在 QWidget 上完成灰度渲染、十字线叠加和简单画笔编辑。
 */
class MprSliceView : public QWidget
{
    Q_OBJECT

public:
    explicit MprSliceView(SliceOrientation orientation, QWidget* parent = nullptr);

    void setVolumes(vtkSmartPointer<vtkImageData> imageData,
                    vtkSmartPointer<vtkImageData> labelData);
    void setCrosshair(const std::array<int, 3>& crosshair);
    void setWindowLevelWidth(int level, int width);
    void setBackgroundColor(const QColor& color);
    void setZoomPercent(int zoomPercent);
    void setLabelVisible(bool visible);
    void setLabelOpacity(double opacity);
    void setBrushRadius(int radius);
    void setBrushEditingEnabled(bool enabled);
    void setEraseMode(bool eraseMode);
    void setActive(bool active);

    SliceOrientation orientation() const;
    int sliceIndex() const;
    int sliceCount() const;
    int zoomPercent() const;
    QImage snapshotImage() const;

signals:
    void activated(SliceOrientation orientation);
    void crosshairChanged(const std::array<int, 3>& crosshair);
    void labelModified();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    QSize minimumSizeHint() const override;

private:
    struct VolumeIndex
    {
        int x = 0;
        int y = 0;
        int z = 0;
        bool valid = false;
    };

    void invalidateView();
    void rebuildCache() const;
    QSize sourceSliceSize() const;
    QRect targetImageRect(const QSize& canvasSize) const;
    QPointF crosshairDisplayPoint(const QRect& drawRect) const;
    VolumeIndex mapWidgetPointToVolume(const QPoint& pos) const;
    bool applyBrushStroke(const VolumeIndex& center);
    double scalarAt(vtkImageData* imageData, int x, int y, int z) const;
    unsigned char labelAt(int x, int y, int z) const;
    std::array<int, 3> clampedCrosshair(const std::array<int, 3>& crosshair) const;

private:
    SliceOrientation m_orientation;
    vtkSmartPointer<vtkImageData> m_imageData;
    vtkSmartPointer<vtkImageData> m_labelData;
    std::array<int, 3> m_crosshair{{0, 0, 0}};
    QColor m_backgroundColor{Qt::black};
    int m_windowLevel = 40;
    int m_windowWidth = 400;
    int m_zoomPercent = 100;
    int m_brushRadius = 6;
    double m_labelOpacity = 0.45;
    bool m_labelVisible = true;
    bool m_brushEditingEnabled = false;
    bool m_eraseMode = false;
    bool m_active = false;
    bool m_leftButtonPressed = false;

    mutable bool m_cacheDirty = true;
    mutable QImage m_cachedCanvas;
    mutable QRect m_cachedImageRect;
};
