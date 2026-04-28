#ifndef HISTOGRAMWIDGET_H
#define HISTOGRAMWIDGET_H

#include <QMouseEvent>
#include <QVector>
#include <QWidget>

#include <vtkImageData.h>
#include <vtkSmartPointer.h>

#include "visualizationtypes.h"

class QPainter;

class HistogramWidget : public QWidget
{
    Q_OBJECT

public:
    enum class ScaleMode
    {
        Linear,
        Logarithmic
    };

    explicit HistogramWidget(QWidget* parent = nullptr);
    ~HistogramWidget() override = default;

    void setImageData(vtkSmartPointer<vtkImageData> imageData);
    void setCurrentSlice(int slice);
    void setSliceOrientation(SliceOrientation orientation);
    void setScaleMode(ScaleMode mode);
    void setWindowLevel(int level, int width);
    void setDarkTheme(bool darkTheme);

signals:
    void windowLevelChanged(int level, int width);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    enum class DraggingMode
    {
        None,
        WindowLevel,
        WindowMin,
        WindowMax
    };

    void calculateHistogram();
    void drawHistogram(QPainter* painter);
    void drawWindowLevelRange(QPainter* painter);

private:
    vtkSmartPointer<vtkImageData> m_imageData;
    QVector<int> m_histogram;
    ScaleMode m_scaleMode = ScaleMode::Linear;
    SliceOrientation m_sliceOrientation = SliceOrientation::Axial;
    int m_windowLevel = 128;
    int m_windowWidth = 255;
    double m_minValue = 0.0;
    double m_maxValue = 255.0;
    int m_currentSlice = 0;
    bool m_mousePressed = false;
    bool m_darkTheme = false;
    QPoint m_lastMousePos;
    DraggingMode m_draggingMode = DraggingMode::None;
};

#endif // HISTOGRAMWIDGET_H
