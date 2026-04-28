#include "reconworker.h"

#include <stdexcept>
#include <string>
#include <vector>

#include <QFileInfo>

#include <itkImageFileWriter.h>
#include <itkMacro.h>
#include <itkPermuteAxesImageFilter.h>

ReconWorker::ReconWorker(const Task& task, QObject* parent)
    : QObject(parent), m_task(task)
{
}

void ReconWorker::requestStop()
{
    m_stopRequested.storeRelease(1);
}

bool ReconWorker::stopRequested() const
{
    return m_stopRequested.loadAcquire() != 0;
}

void ReconWorker::validateTask() const
{
    /* 在真正进入 ITK / RTK 计算前先快速失败，
     * 这样可以尽早拦截空路径、空几何、非法体尺寸等明显问题。
     */
    if (m_task.projectionDirectory.trimmed().isEmpty())
        throw std::invalid_argument("Projection directory is empty.");

    if (m_task.outputFilePath.trimmed().isEmpty())
        throw std::invalid_argument("Output file path is empty.");

    QFileInfo dirInfo(m_task.projectionDirectory);
    if (!dirInfo.exists() || !dirInfo.isDir())
        throw std::runtime_error("Projection directory does not exist: " + m_task.projectionDirectory.toStdString());

    if (m_task.geometryParams.views.empty())
        throw std::invalid_argument("GeometryParams.views is empty.");

    if (m_task.volumeParams.size[0] == 0 ||
        m_task.volumeParams.size[1] == 0 ||
        m_task.volumeParams.size[2] == 0)
    {
        throw std::invalid_argument("Volume size must be > 0 in all dimensions.");
    }

    if (m_task.algorithm == Algorithm::SART)
    {
        if (m_task.sartParams.numberOfIterations == 0)
            throw std::invalid_argument("SART numberOfIterations must be > 0.");

        if (m_task.sartParams.numberOfProjectionsPerSubset == 0)
            throw std::invalid_argument("SART numberOfProjectionsPerSubset must be > 0.");
    }
}

FdkPipeline::ProjectionFileParams ReconWorker::buildProjectionFileParams() const
{
    /* Worker 与 pipeline 的边界尽量保持简单：
     * Task 保留 Qt 类型，进入底层重建时再转换成 STL 类型。
     */
    FdkPipeline::ProjectionFileParams params;
    params.directory = m_task.projectionDirectory.toStdString();

    for (const auto& ext : m_task.extensions)
        params.extensions.push_back(ext.toStdString());

    return params;
}

FdkPipeline::GeometryType::Pointer ReconWorker::buildGeometry() const
{
    return FdkPipeline::BuildGeometry(m_task.geometryParams);
}

void ReconWorker::writeImage(ImageType::Pointer image, const QString& outputFilePath)
{
    /* 当前工程在 UI 侧按 X / Z / Y 的顺序组织重建参数，
     * 为了避免到处分散坐标修正逻辑，这里在落盘前统一做一次轴置换。
     */
    using PermuteType = itk::PermuteAxesImageFilter<ImageType>;
    auto permute = PermuteType::New();
    auto axes = PermuteType::PermuteOrderArrayType();
    axes.SetElement(0, 0);
    axes.SetElement(1, 2);
    axes.SetElement(2, 1);
    permute->SetOrder(axes);
    permute->SetInput(image);

    using WriterType = itk::ImageFileWriter<ImageType>;
    auto writer = WriterType::New();
    writer->SetFileName(outputFilePath.toStdString());
    writer->SetInput(permute->GetOutput());
    writer->Update();
}

void ReconWorker::process()
{
    /* 线程入口。
     * 一个 ReconWorker 只处理一份由 MainWindow 组装好的不可变 Task。
     */
    emit started();

    try
    {
        validateTask();

        if (stopRequested())
        {
            emit canceled("Reconstruction canceled before start.");
            return;
        }

        ImageType::Pointer recon;

        switch (m_task.algorithm)
        {
        case Algorithm::FDK:
            emit progress("Starting FDK reconstruction...");
#ifdef RTK_USE_CUDA
            recon = m_task.useCuda ? runFdkCuda() : runFdkCpu();
#else
            recon = runFdkCpu();
#endif
            break;

        case Algorithm::SART:
            emit progress("Starting SART reconstruction...");
#ifdef RTK_USE_CUDA
            recon = m_task.useCuda ? runSartCuda() : runSartCpu();
#else
            recon = runSartCpu();
#endif
            break;

        default:
            throw std::runtime_error("Unknown reconstruction algorithm.");
        }

        if (stopRequested())
        {
            emit canceled("Reconstruction canceled.");
            return;
        }

        if (!recon)
            throw std::runtime_error("Reconstruction result is null.");

        emit progress("Writing reconstruction image...");
        writeImage(recon, m_task.outputFilePath);

        if (stopRequested())
        {
            emit canceled("Reconstruction canceled after reconstruction.");
            return;
        }

        emit finished(m_task.outputFilePath);
    }
    catch (const itk::ExceptionObject& e)
    {
        emit failed(QString("ITK/RTK error: %1").arg(e.what()));
    }
    catch (const std::exception& e)
    {
        emit failed(QString("Reconstruction error: %1").arg(e.what()));
    }
    catch (...)
    {
        emit failed("Unknown reconstruction error.");
    }
}

ReconWorker::ImageType::Pointer ReconWorker::runFdkCpu()
{
    auto geometry = buildGeometry();

    if (stopRequested())
        return nullptr;

    auto projections = FdkPipeline::ReadProjections(
        buildProjectionFileParams(),
        m_task.projectionImageParams);

    if (stopRequested())
        return nullptr;

    /* 一旦进入 RTK 内部的 Update()，外部就很难再细粒度打断它。
     * 因此这里只能在进入重建主滤波器之前做一次停止检查。
     */
    return FdkPipeline::ReconstructCpu(
        projections,
        geometry,
        m_task.volumeParams,
        m_task.fdkParams);
}

#ifdef RTK_USE_CUDA
ReconWorker::ImageType::Pointer ReconWorker::runFdkCuda()
{
    auto geometry = buildGeometry();

    if (stopRequested())
        return nullptr;

    auto projections = FdkPipeline::ReadProjections(
        buildProjectionFileParams(),
        m_task.projectionImageParams);

    if (stopRequested())
        return nullptr;

    /* CUDA FDK 与 CPU FDK 一样，
     * 一旦进入 RTK 的内部 Update()，就没有安全的细粒度中断点。
     */
    return FdkPipeline::ReconstructCuda(
        projections,
        geometry,
        m_task.volumeParams,
        m_task.fdkParams);
}
#endif

ReconWorker::ImageType::Pointer ReconWorker::runSartCpu()
{
    auto geometry = buildGeometry();

    if (stopRequested())
        return nullptr;

    emit progress("Reading projections...");
    auto projections = FdkPipeline::ReadProjections(
        buildProjectionFileParams(),
        m_task.projectionImageParams);

    if (stopRequested())
        return nullptr;

    emit progress("Building initial volume...");
    auto current = SartPipeline::BuildInitialVolumeCpu(
        projections, geometry, m_task.volumeParams, m_task.initParams);

    if (stopRequested())
        return nullptr;

    const bool useStepwiseTv =
        m_task.tvParams.enabled && m_task.tvParams.applyAfterEachOuterIteration;

    /* 改成逐轮 outer iteration 执行，
     * 这样界面可以持续刷新进度，也能在轮次之间响应停止请求。
     */
    for (unsigned int iter = 0; iter < m_task.sartParams.numberOfIterations; ++iter)
    {
        if (stopRequested())
            return nullptr;

        emit progress(QString("SART iteration %1 / %2")
                          .arg(iter + 1)
                          .arg(m_task.sartParams.numberOfIterations));

        current = SartPipeline::RunSingleOuterSartIterationCpu(
            current, projections, geometry, m_task.sartParams);

        if (stopRequested())
            return nullptr;

        if (useStepwiseTv)
        {
            emit progress(QString("Applying TV after iteration %1...").arg(iter + 1));
            current = SartPipeline::ApplyTvRegularizationCpu(current, m_task.tvParams);
        }
    }

    if (stopRequested())
        return nullptr;

    if (m_task.tvParams.enabled && !m_task.tvParams.applyAfterEachOuterIteration)
    {
        /* 如果用户只希望最后做一次 TV 去噪，
         * 就把 TV 放到主循环之后，避免每轮迭代都额外增加开销。
         */
        emit progress("Applying final TV regularization...");
        current = SartPipeline::ApplyTvRegularizationCpu(current, m_task.tvParams);
    }

    if (stopRequested())
        return nullptr;

    if (!m_task.sartParams.applyFieldOfViewMask)
        return current;

    /* FoV 掩膜单独放在这里做，
     * 这样 worker 可以把“进入 FoV 阶段”明确反馈给 UI。
     */
    emit progress("Applying field-of-view mask...");
    using FovType = rtk::FieldOfViewImageFilter<ImageType, ImageType>;
    auto fov = FovType::New();
    fov->SetInput(0, current);
    fov->SetProjectionsStack(projections);
    fov->SetGeometry(geometry);
    fov->SetOutsideValue(m_task.sartParams.fovOutsideValue);
    fov->SetMask(m_task.sartParams.fovMaskMode);
    fov->Update();

    return fov->GetOutput();
}

#ifdef RTK_USE_CUDA
ReconWorker::ImageType::Pointer ReconWorker::runSartCuda()
{
    auto geometry = buildGeometry();

    if (stopRequested())
        return nullptr;

    emit progress("Reading projections...");
    auto projections = FdkPipeline::ReadProjections(
        buildProjectionFileParams(),
        m_task.projectionImageParams);

    if (stopRequested())
        return nullptr;

    emit progress("Preparing CUDA projections...");
    auto cudaProjections = FdkPipeline::ToCudaImage(projections);

    if (stopRequested())
        return nullptr;

    emit progress("Building initial volume...");
    auto current = SartPipeline::BuildInitialVolumeCuda(
        projections, geometry, m_task.volumeParams, m_task.initParams);

    if (stopRequested())
        return nullptr;

    const bool useStepwiseTv =
        m_task.tvParams.enabled && m_task.tvParams.applyAfterEachOuterIteration;

    /* CUDA 路径与 CPU 路径保持相同的逐轮结构，
     * 区别只是当前估计体始终保留在 GPU 上。
     */
    for (unsigned int iter = 0; iter < m_task.sartParams.numberOfIterations; ++iter)
    {
        if (stopRequested())
            return nullptr;

        emit progress(QString("CUDA SART iteration %1 / %2")
                          .arg(iter + 1)
                          .arg(m_task.sartParams.numberOfIterations));

        current = SartPipeline::RunSingleOuterSartIterationCuda(
            current, cudaProjections, geometry, m_task.sartParams);

        if (stopRequested())
            return nullptr;

        if (useStepwiseTv)
        {
            current = SartPipeline::ApplyTvRegularizationCuda(current, m_task.tvParams);
        }
    }

    if (stopRequested())
        return nullptr;

    if (m_task.tvParams.enabled && !m_task.tvParams.applyAfterEachOuterIteration)
    {
        /* 如果没有启用“每轮 TV”，
         * 就把最终一次 TV 去噪放到循环外面统一执行。
         */
        current = SartPipeline::ApplyTvRegularizationCuda(current, m_task.tvParams);
    }

    if (stopRequested())
        return nullptr;

    auto cpuResult = FdkPipeline::ToCpuImage(current);

    if (!m_task.sartParams.applyFieldOfViewMask)
        return cpuResult;

    /* 这里的 FoV 仍然走 CPU 版滤波器，
     * 所以先把结果从 GPU 拿回 CPU，再完成最后一步。
     */
    emit progress("Applying field-of-view mask...");
    using FovType = rtk::FieldOfViewImageFilter<ImageType, ImageType>;
    auto fov = FovType::New();
    fov->SetInput(0, cpuResult);
    fov->SetProjectionsStack(projections);
    fov->SetGeometry(geometry);
    fov->SetOutsideValue(m_task.sartParams.fovOutsideValue);
    fov->SetMask(m_task.sartParams.fovMaskMode);
    fov->Update();

    return fov->GetOutput();
}
#endif
