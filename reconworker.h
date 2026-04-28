#pragma once

#include <QObject>
#include <QString>
#include <QAtomicInt>

#include "fdkpipeline.h"
#include "sartpipeline.h"

/* 重建工作对象。
 * MainWindow 在 UI 线程里组装 Task，然后把 ReconWorker 放进独立线程；
 * 真正耗时的 RTK / ITK 计算都在这里执行，避免阻塞界面。
 */
class ReconWorker : public QObject
{
    Q_OBJECT

public:
    enum class Algorithm
    {
        FDK,
        SART
    };
    Q_ENUM(Algorithm)

    struct Task
    {
        Algorithm algorithm = Algorithm::FDK;
        bool useCuda = false;

        /* 输入投影目录和输出体数据路径。 */
        QString projectionDirectory;
        QString outputFilePath;

        /* 扫描投影目录时接受的文件扩展名。 */
        QStringList extensions{".mha"};

        /* 读投影图像时附带的元数据，例如 spacing / origin / direction。 */
        FdkPipeline::ProjectionImageParams projectionImageParams{};

        /* 扫描几何参数，由 UI 根据 SID / SDD / 角度数量等直接拼装。 */
        FdkPipeline::GeometryParams geometryParams{};

        /* 输出体数据网格参数。 */
        FdkPipeline::VolumeParams volumeParams{};

        /* FDK 解析重建参数。 */
        FdkPipeline::FdkParams fdkParams{};

        /* SART 初始化、迭代与 TV 正则化参数。 */
        SartPipeline::InitializationParams initParams{};
        SartPipeline::SartParams sartParams{};
        SartPipeline::TvParams tvParams{};
    };

    explicit ReconWorker(const Task& task, QObject* parent = nullptr);
    ~ReconWorker() override = default;

public slots:
    void process();

    /* 协作式停止：这里只写一个原子标志位，不直接强杀线程。 */
    void requestStop();

signals:
    void started();
    void progress(QString message);
    void finished(QString outputFilePath);
    void failed(QString errorMessage);
    void canceled(QString message);

private:
    using ImageType = FdkPipeline::ImageType;
#ifdef RTK_USE_CUDA
    using CudaImageType = FdkPipeline::CudaImageType;
#endif

    /* 进入耗时滤波器前，先做一轮便宜但关键的参数检查。 */
    void validateTask() const;
    bool stopRequested() const;

    /* 把 Qt 风格的文件参数转换成 pipeline 使用的 STL 风格参数。 */
    FdkPipeline::ProjectionFileParams buildProjectionFileParams() const;

    /* 基于 Task 中的 views 构造 RTK 几何对象。 */
    FdkPipeline::GeometryType::Pointer buildGeometry() const;

    /* 输出前做一次轴置换，保证落盘坐标顺序与项目其余模块约定一致。 */
    static void writeImage(ImageType::Pointer image, const QString& outputFilePath);

    ImageType::Pointer runFdkCpu();
#ifdef RTK_USE_CUDA
    ImageType::Pointer runFdkCuda();
#endif

    ImageType::Pointer runSartCpu();
#ifdef RTK_USE_CUDA
    ImageType::Pointer runSartCuda();
#endif

private:
    Task m_task;
    QAtomicInt m_stopRequested{0};
};
