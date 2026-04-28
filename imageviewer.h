#ifndef IMAGEVIEWER_H
#define IMAGEVIEWER_H

#include <QColor>
#include <QImage>
#include <QWidget>

#include <array>

#include <vtkSmartPointer.h>

#include "visualizationtypes.h"

class QCheckBox;
class QPushButton;
class QSlider;
class QSpinBox;
class QTimer;
class QWidget;

class MprSliceView;
class MprVolumeView;
class vtkImageData;
class vtkMetaImageReader;
class vtkNIFTIImageReader;

/*
 * 可视化页主查看器。
 * 对外仍保持原有的窗宽窗位、缩放、切片等接口，
 * 但内部已经从单轴向视图重构成三正交 MPR + 3D 标签预览的复合控件。
 */
class ImageViewer : public QWidget
{
    Q_OBJECT

public:
    explicit ImageViewer(QWidget* parent = nullptr);
    ~ImageViewer() override = default;

    bool loadImage(const QString& fileName);
    bool saveImage(const QString& fileName);
    void setWindowLevel(int level);
    void setWindowWidth(int width);
    void setWindowLevelWidth(int level, int width);
    void setBackgroundColor(const QString& color);
    void setInterfaceTheme(bool darkTheme);
    void loadTestImage();
    void setZoomLevel(int level);
    int getZoomLevel() const;

    void setCurrentSlice(int slice);
    int getCurrentSlice() const;

    int getWindowLevel() const;
    int getWindowWidth() const;
    int getTotalSlices() const;
    double getScalarMinimum() const;
    double getScalarMaximum() const;
    vtkSmartPointer<vtkImageData> getImageData() const;
    SliceOrientation getCurrentOrientation() const;

signals:
    void imageLoaded();
    void windowLevelChanged(int level, int width);
    void viewStateChanged();

private slots:
    void onSliceActivated(SliceOrientation orientation);
    void onSliceCrosshairChanged(const std::array<int, 3>& crosshair);
    void onSliceLabelModified();
    void onBrushModeToggled(bool checked);
    void onEraseModeToggled(bool checked);
    void onLabelVisibleToggled(bool checked);
    void onLabelOpacityChanged(int value);
    void onBrushRadiusChanged(int value);
    void onClearLabelClicked();
    void flushPendingLabelUpdate();

private:
    void setupViewer();
    void setupToolbar();
    void setupViewGrid();
    void applyToolbarTheme();
    bool adoptVolume(vtkSmartPointer<vtkImageData> imageData, bool seedDemoLabel);
    void createEmptyLabelVolume();
    void createDemoLabelVolume();
    void applyWindowLevelInternal(int level, int width, bool notify);
    void applyBackgroundColor();
    void applySlicePresentation();
    void applyVolumePresentation();
    void rebuildVolumeSurface();
    void scheduleVolumeRebuild();
    void setActiveOrientation(SliceOrientation orientation, bool notify);
    std::array<int, 3> clampedCrosshair(const std::array<int, 3>& crosshair) const;
    void updateCrosshair(const std::array<int, 3>& crosshair, bool notify);
    QImage currentSliceSnapshot() const;

private:
    vtkSmartPointer<vtkMetaImageReader> m_mhdReader;
    vtkSmartPointer<vtkNIFTIImageReader> m_niiReader;
    vtkSmartPointer<vtkImageData> m_imageData;
    vtkSmartPointer<vtkImageData> m_labelData;

    MprSliceView* m_axialView = nullptr;
    MprSliceView* m_coronalView = nullptr;
    MprSliceView* m_sagittalView = nullptr;
    MprVolumeView* m_volumeView = nullptr;
    QWidget* m_toolbarContainer = nullptr;
    QWidget* m_viewGridContainer = nullptr;

    QCheckBox* m_showLabelCheck = nullptr;
    QCheckBox* m_brushCheck = nullptr;
    QCheckBox* m_eraseCheck = nullptr;
    QSlider* m_labelOpacitySlider = nullptr;
    QSpinBox* m_brushRadiusSpin = nullptr;
    QPushButton* m_clearLabelButton = nullptr;
    QTimer* m_labelRefreshTimer = nullptr;

    QColor m_backgroundColor{Qt::black};
    std::array<int, 3> m_crosshair{{0, 0, 0}};
    SliceOrientation m_currentOrientation = SliceOrientation::Axial;
    double m_scalarMinimum = 0.0;
    double m_scalarMaximum = 255.0;
    int m_windowLevel = 128;
    int m_windowWidth = 255;
    int m_zoomLevel = 100;
    int m_brushRadius = 6;
    bool m_labelVisible = true;
    bool m_brushEditingEnabled = false;
    bool m_eraseMode = false;
    double m_labelOpacity = 0.45;
    bool m_darkTheme = true;
};

#endif // IMAGEVIEWER_H
