#include "sartpipeline.h"

SartPipeline::ImageType::Pointer
SartPipeline::BuildInitialVolumeCpu(
    ImageType::Pointer projections,
    GeometryType::Pointer geometry,
    const VolumeParams& volumeParams,
    const InitializationParams& initParams)
{
    // SART can either start from zeros or from an FDK bootstrap volume.
    switch (initParams.mode)
    {
    case InitializationMode::ZERO:
        return CreateInitialVolume(volumeParams);

    case InitializationMode::FDK_CPU:
        return BasePipeline::ReconstructCpu(
            projections, geometry, volumeParams, initParams.fdkParams);

    case InitializationMode::FDK_CUDA:
#ifdef RTK_USE_CUDA
        return BasePipeline::ReconstructCuda(
            projections, geometry, volumeParams, initParams.fdkParams);
#else
        throw std::runtime_error(
            "InitializationMode::FDK_CUDA requested but RTK_USE_CUDA is OFF.");
#endif

    default:
        throw std::runtime_error("Unknown InitializationMode in BuildInitialVolumeCpu.");
    }
}

SartPipeline::ImageType::Pointer
SartPipeline::ApplyTvRegularizationCpu(
    ImageType::Pointer input,
    const TvParams& tvParams)
{
    if (!input)
        throw std::invalid_argument("ApplyTvRegularizationCpu: input is null.");

    if (!tvParams.enabled)
        return input;

    // TV is optional because it trades sharpness/noise against runtime and smoothing.
    auto tv = TvFilterType::New();
    tv->SetInput(input);
    tv->SetGamma(tvParams.gamma);
    tv->SetNumberOfIterations(tvParams.numberOfIterations);

    bool dims[Dimension];
    for (unsigned int i = 0; i < Dimension; ++i)
        dims[i] = tvParams.dimensionsProcessed[i];

    tv->SetDimensionsProcessed(dims);

    if (tvParams.boundaryConditionPeriodic)
        tv->SetBoundaryConditionToPeriodic();

    tv->Update();
    return tv->GetOutput();
}

SartPipeline::ImageType::Pointer
SartPipeline::RunSingleOuterSartIterationCpu(
    ImageType::Pointer currentVolume,
    ImageType::Pointer projections,
    GeometryType::Pointer geometry,
    const SartParams& sartParams)
{
    if (!currentVolume)
        throw std::invalid_argument("RunSingleOuterSartIterationCpu: currentVolume is null.");
    if (!projections)
        throw std::invalid_argument("RunSingleOuterSartIterationCpu: projections is null.");
    if (!geometry)
        throw std::invalid_argument("RunSingleOuterSartIterationCpu: geometry is null.");

    auto sart = SartFilterType::New();
    sart->SetInput(0, currentVolume);
    sart->SetInput(1, projections);
    sart->SetGeometry(geometry);

    // 每次只跑 1 个 outer iteration
    // Intentionally run exactly one outer iteration so higher layers can report progress
    // and check for cancellation between iterations.
    sart->SetNumberOfIterations(1);
    sart->SetNumberOfProjectionsPerSubset(sartParams.numberOfProjectionsPerSubset);
    sart->SetLambda(sartParams.lambda);
    sart->SetEnforcePositivity(sartParams.enforcePositivity);
    sart->SetDisableDisplacedDetectorFilter(sartParams.disableDisplacedDetectorFilter);
    sart->SetDivisionThreshold(sartParams.divisionThreshold);

    if (sartParams.sigmaZero.has_value())
        sart->SetSigmaZero(*sartParams.sigmaZero);
    if (sartParams.alphaPSF.has_value())
        sart->SetAlphaPSF(*sartParams.alphaPSF);

    sart->Update();
    return sart->GetOutput();
}

SartPipeline::ImageType::Pointer
SartPipeline::ReconstructCpu(
    ImageType::Pointer projections,
    GeometryType::Pointer geometry,
    const VolumeParams& volumeParams,
    const InitializationParams& initParams,
    const SartParams& sartParams,
    const TvParams& tvParams)
{
    if (!projections)
        throw std::invalid_argument("ReconstructCpu: projections is null.");
    if (!geometry)
        throw std::invalid_argument("ReconstructCpu: geometry is null.");
    if (sartParams.numberOfIterations == 0)
        throw std::invalid_argument("ReconstructCpu: numberOfIterations must be > 0.");
    if (sartParams.numberOfProjectionsPerSubset == 0)
        throw std::invalid_argument("ReconstructCpu: numberOfProjectionsPerSubset must be > 0.");

    auto current = BuildInitialVolumeCpu(projections, geometry, volumeParams, initParams);

    if (tvParams.enabled && tvParams.applyAfterEachOuterIteration)
    {
        // Stepwise TV keeps the control flow open for progress/cancel-aware callers.
        for (unsigned int iter = 0; iter < sartParams.numberOfIterations; ++iter)
        {
            current = RunSingleOuterSartIterationCpu(current, projections, geometry, sartParams);
            current = ApplyTvRegularizationCpu(current, tvParams);
        }
        return current;
    }

    // If TV is not interleaved, let RTK run the whole iterative loop in one filter invocation.
    auto sart = SartFilterType::New();
    sart->SetInput(0, current);
    sart->SetInput(1, projections);
    sart->SetGeometry(geometry);

    sart->SetNumberOfIterations(sartParams.numberOfIterations);
    sart->SetNumberOfProjectionsPerSubset(sartParams.numberOfProjectionsPerSubset);
    sart->SetLambda(sartParams.lambda);
    sart->SetEnforcePositivity(sartParams.enforcePositivity);
    sart->SetDisableDisplacedDetectorFilter(sartParams.disableDisplacedDetectorFilter);
    sart->SetDivisionThreshold(sartParams.divisionThreshold);

    if (sartParams.sigmaZero.has_value())
        sart->SetSigmaZero(*sartParams.sigmaZero);
    if (sartParams.alphaPSF.has_value())
        sart->SetAlphaPSF(*sartParams.alphaPSF);

    sart->Update();
    current = sart->GetOutput();

    if (tvParams.enabled)
        current = ApplyTvRegularizationCpu(current, tvParams);

    if (!sartParams.applyFieldOfViewMask)
        return current;

    // Masking is kept outside the iterative filter so it can be toggled independently.
    using FovType = rtk::FieldOfViewImageFilter<ImageType, ImageType>;
    auto fov = FovType::New();
    fov->SetInput(0, current);
    fov->SetProjectionsStack(projections);
    fov->SetGeometry(geometry);
    fov->SetOutsideValue(sartParams.fovOutsideValue);
    fov->SetMask(sartParams.fovMaskMode);
    fov->Update();

    return fov->GetOutput();
}

#ifdef RTK_USE_CUDA

SartPipeline::CudaImageType::Pointer
SartPipeline::BuildInitialVolumeCuda(
    ImageType::Pointer projections,
    GeometryType::Pointer geometry,
    const VolumeParams& volumeParams,
    const InitializationParams& initParams)
{
    // The initial estimate may still come from CPU FDK, then be copied to GPU for SART.
    switch (initParams.mode)
    {
    case InitializationMode::ZERO:
        return BasePipeline::CreateEmptyCudaVolume(volumeParams);

    case InitializationMode::FDK_CPU:
    {
        auto initCpu = BasePipeline::ReconstructCpu(
            projections, geometry, volumeParams, initParams.fdkParams);
        return BasePipeline::ToCudaImage(initCpu);
    }

    case InitializationMode::FDK_CUDA:
    {
        auto initCpu = BasePipeline::ReconstructCuda(
            projections, geometry, volumeParams, initParams.fdkParams);
        return BasePipeline::ToCudaImage(initCpu);
    }

    default:
        throw std::runtime_error("Unknown InitializationMode in BuildInitialVolumeCuda.");
    }
}

SartPipeline::CudaImageType::Pointer
SartPipeline::ApplyTvRegularizationCuda(
    CudaImageType::Pointer input,
    const TvParams& tvParams)
{
    if (!input)
        throw std::invalid_argument("ApplyTvRegularizationCuda: input is null.");

    if (!tvParams.enabled)
        return input;

    // CUDA TV mirrors the CPU path but keeps the volume resident on GPU.
    auto tv = CudaTvFilterType::New();
    tv->SetInput(input);
    tv->SetGamma(tvParams.gamma);
    tv->SetNumberOfIterations(tvParams.numberOfIterations);

    bool dims[Dimension];
    for (unsigned int i = 0; i < Dimension; ++i)
        dims[i] = tvParams.dimensionsProcessed[i];

    tv->SetDimensionsProcessed(dims);

    if (tvParams.boundaryConditionPeriodic)
        tv->SetBoundaryConditionToPeriodic();

    tv->Update();
    return tv->GetOutput();
}

SartPipeline::CudaImageType::Pointer
SartPipeline::RunSingleOuterSartIterationCuda(
    CudaImageType::Pointer currentVolume,
    CudaImageType::Pointer projections,
    GeometryType::Pointer geometry,
    const SartParams& sartParams)
{
    if (!currentVolume)
        throw std::invalid_argument("RunSingleOuterSartIterationCuda: currentVolume is null.");
    if (!projections)
        throw std::invalid_argument("RunSingleOuterSartIterationCuda: projections is null.");
    if (!geometry)
        throw std::invalid_argument("RunSingleOuterSartIterationCuda: geometry is null.");

    auto sart = CudaSartFilterType::New();
    sart->SetInput(0, currentVolume);
    sart->SetInput(1, projections);
    sart->SetGeometry(geometry);

    // Same one-iteration contract as the CPU path, with explicit projector selection for CUDA.
    sart->SetNumberOfIterations(1);
    sart->SetNumberOfProjectionsPerSubset(sartParams.numberOfProjectionsPerSubset);
    sart->SetLambda(sartParams.lambda);
    sart->SetEnforcePositivity(sartParams.enforcePositivity);
    sart->SetDisableDisplacedDetectorFilter(sartParams.disableDisplacedDetectorFilter);
    sart->SetDivisionThreshold(sartParams.divisionThreshold);

    if (sartParams.sigmaZero.has_value())
        sart->SetSigmaZero(*sartParams.sigmaZero);
    if (sartParams.alphaPSF.has_value())
        sart->SetAlphaPSF(*sartParams.alphaPSF);

    sart->SetForwardProjectionFilter(CudaSartFilterType::FP_CUDARAYCAST);
    sart->SetBackProjectionFilter(
        sartParams.useCudaRayCastBackProjection
            ? CudaSartFilterType::BP_CUDARAYCAST
            : CudaSartFilterType::BP_CUDAVOXELBASED);

    sart->Update();
    return sart->GetOutput();
}

SartPipeline::ImageType::Pointer
SartPipeline::ReconstructCuda(
    ImageType::Pointer projections,
    GeometryType::Pointer geometry,
    const VolumeParams& volumeParams,
    const InitializationParams& initParams,
    const SartParams& sartParams,
    const TvParams& tvParams)
{
    if (!projections)
        throw std::invalid_argument("ReconstructCuda: projections is null.");
    if (!geometry)
        throw std::invalid_argument("ReconstructCuda: geometry is null.");
    if (sartParams.numberOfIterations == 0)
        throw std::invalid_argument("ReconstructCuda: numberOfIterations must be > 0.");
    if (sartParams.numberOfProjectionsPerSubset == 0)
        throw std::invalid_argument("ReconstructCuda: numberOfProjectionsPerSubset must be > 0.");

    auto cudaProjections = BasePipeline::ToCudaImage(projections);
    auto current = BuildInitialVolumeCuda(projections, geometry, volumeParams, initParams);

    if (tvParams.enabled && tvParams.applyAfterEachOuterIteration)
    {
        // Stepwise structure keeps the calling worker responsive between outer iterations.
        for (unsigned int iter = 0; iter < sartParams.numberOfIterations; ++iter)
        {
            current = RunSingleOuterSartIterationCuda(current, cudaProjections, geometry, sartParams);
            current = ApplyTvRegularizationCuda(current, tvParams);
        }
        return BasePipeline::ToCpuImage(current);
    }

    // Otherwise let RTK execute the whole iteration stack in one CUDA filter.
    auto sart = CudaSartFilterType::New();
    sart->SetInput(0, current);
    sart->SetInput(1, cudaProjections);
    sart->SetGeometry(geometry);

    sart->SetNumberOfIterations(sartParams.numberOfIterations);
    sart->SetNumberOfProjectionsPerSubset(sartParams.numberOfProjectionsPerSubset);
    sart->SetLambda(sartParams.lambda);
    sart->SetEnforcePositivity(sartParams.enforcePositivity);
    sart->SetDisableDisplacedDetectorFilter(sartParams.disableDisplacedDetectorFilter);
    sart->SetDivisionThreshold(sartParams.divisionThreshold);

    if (sartParams.sigmaZero.has_value())
        sart->SetSigmaZero(*sartParams.sigmaZero);
    if (sartParams.alphaPSF.has_value())
        sart->SetAlphaPSF(*sartParams.alphaPSF);

    sart->SetForwardProjectionFilter(CudaSartFilterType::FP_CUDARAYCAST);
    sart->SetBackProjectionFilter(
        sartParams.useCudaRayCastBackProjection
            ? CudaSartFilterType::BP_CUDARAYCAST
            : CudaSartFilterType::BP_CUDAVOXELBASED);

    sart->Update();
    current = sart->GetOutput();

    if (tvParams.enabled)
        current = ApplyTvRegularizationCuda(current, tvParams);


    if (!sartParams.applyFieldOfViewMask)
        return BasePipeline::ToCpuImage(current);

    // FoV masking still uses the CPU filter in this project.
    using FovType = rtk::FieldOfViewImageFilter<ImageType, ImageType>;
    auto fov = FovType::New();
    fov->SetInput(0, BasePipeline::ToCpuImage(current));
    fov->SetProjectionsStack(projections);
    fov->SetGeometry(geometry);
    fov->SetOutsideValue(sartParams.fovOutsideValue);
    fov->SetMask(sartParams.fovMaskMode);
    fov->Update();

    return fov->GetOutput();
}

#endif

SartPipeline::ImageType::Pointer
SartPipeline::RunCpu(
    const ProjectionFileParams& fileParams,
    const ProjectionImageParams& projectionImageParams,
    GeometryType::Pointer geometry,
    const VolumeParams& volumeParams,
    const InitializationParams& initParams,
    const SartParams& sartParams,
    const TvParams& tvParams)
{
    auto projections = ReadProjections(fileParams, projectionImageParams);
    return ReconstructCpu(projections, geometry, volumeParams, initParams, sartParams, tvParams);
}

#ifdef RTK_USE_CUDA
SartPipeline::ImageType::Pointer
SartPipeline::RunCuda(
    const ProjectionFileParams& fileParams,
    const ProjectionImageParams& projectionImageParams,
    GeometryType::Pointer geometry,
    const VolumeParams& volumeParams,
    const InitializationParams& initParams,
    const SartParams& sartParams,
    const TvParams& tvParams)
{
    auto projections = ReadProjections(fileParams, projectionImageParams);
    return ReconstructCuda(projections, geometry, volumeParams, initParams, sartParams, tvParams);
}
#endif
