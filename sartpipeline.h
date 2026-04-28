#pragma once

#include "fdkpipeline.h"

#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <rtkSARTConeBeamReconstructionFilter.h>
#include <rtkTotalVariationDenoisingBPDQImageFilter.h>

#ifdef RTK_USE_CUDA
#  include <itkCudaImage.h>
#  include <rtkCudaTotalVariationDenoisingBPDQImageFilter.h>
#endif

// Iterative reconstruction helpers built on top of FdkPipeline primitives.
// CPU and CUDA entry points share the same parameter model so the UI can switch cleanly.
class SartPipeline
{
public:
    using BasePipeline = FdkPipeline;
    using PixelType = BasePipeline::PixelType;
    static constexpr unsigned int Dimension = BasePipeline::Dimension;

    using ImageType = BasePipeline::ImageType;
    using GeometryType = BasePipeline::GeometryType;
    using PointType = BasePipeline::PointType;
    using SpacingType = BasePipeline::SpacingType;
    using SizeType = BasePipeline::SizeType;
    using DirectionType = BasePipeline::DirectionType;

    using ProjectionFileParams = BasePipeline::ProjectionFileParams;
    using ProjectionImageParams = BasePipeline::ProjectionImageParams;
    using ProjectionViewParam = BasePipeline::ProjectionViewParam;
    using GeometryParams = BasePipeline::GeometryParams;
    using VolumeParams = BasePipeline::VolumeParams;
    using FdkParams = BasePipeline::FdkParams;

#ifdef RTK_USE_CUDA
    using CudaImageType = BasePipeline::CudaImageType;
#endif

    using SartFilterType = rtk::SARTConeBeamReconstructionFilter<ImageType, ImageType>;
    using TvFilterType = rtk::TotalVariationDenoisingBPDQImageFilter<ImageType>;

#ifdef RTK_USE_CUDA
    using CudaSartFilterType = rtk::SARTConeBeamReconstructionFilter<CudaImageType, CudaImageType>;
    using CudaTvFilterType = rtk::CudaTotalVariationDenoisingBPDQImageFilter;
#endif

    enum class InitializationMode
    {
        ZERO,
        FDK_CPU,
        FDK_CUDA
    };

    // Parameters for RTK's SART / OS-SART update step.
    struct SartParams
    {
        unsigned int numberOfIterations = 5;
        unsigned int numberOfProjectionsPerSubset = 1; // 1=SART, >1=OS-SART
        double lambda = 0.3;
        bool enforcePositivity = false;
        bool disableDisplacedDetectorFilter = false;
        PixelType divisionThreshold = static_cast<PixelType>(0.0);

        // RTK 2.5.0 的 IterativeConeBeamReconstructionFilter 支持
        // Optional RTK tuning hooks.
        std::optional<double> sigmaZero;
        std::optional<double> alphaPSF;

        // CUDA 路径下选择 back projector
        // CUDA path can switch between ray-cast and voxel-based back projectors.
        bool useCudaRayCastBackProjection = true;

        // Fov
        // Output masking after the iterative updates finish.
        bool applyFieldOfViewMask = true;
        double fovOutsideValue = 0.0;
        bool fovMaskMode = false;
    };

    // Optional TV denoising inserted between or after outer SART iterations.
    struct TvParams
    {
        bool enabled = false;
        int numberOfIterations = 1;
        double gamma = 0.02;
        std::array<bool, 3> dimensionsProcessed{ true, true, true };
        bool boundaryConditionPeriodic = false;

        // true: 每一轮 outer SART 后做 TV
        // false: 全部 SART 后只做一次 TV
        // true: TV after every outer iteration.
        // false: TV once after all SART iterations.
        bool applyAfterEachOuterIteration = true;
    };

    // Initialization for the first volume estimate.
    struct InitializationParams
    {
        InitializationMode mode = InitializationMode::ZERO;
        FdkParams fdkParams{};
    };

public:
    static GeometryType::Pointer BuildGeometry(const GeometryParams& params)
    {
        return BasePipeline::BuildGeometry(params);
    }

    static GeometryType::Pointer LoadGeometryFromXml(const std::string& xmlPath)
    {
        return BasePipeline::LoadGeometryFromXml(xmlPath);
    }

    static ImageType::Pointer ReadProjections(
        const ProjectionFileParams& fileParams,
        const ProjectionImageParams& imageParams)
    {
        return BasePipeline::ReadProjections(fileParams, imageParams);
    }

    static ImageType::Pointer CreateInitialVolume(const VolumeParams& params)
    {
        return BasePipeline::CreateEmptyVolume(params);
    }

    // Build the initial guess used by the iterative solver.
    static ImageType::Pointer BuildInitialVolumeCpu(
        ImageType::Pointer projections,
        GeometryType::Pointer geometry,
        const VolumeParams& volumeParams,
        const InitializationParams& initParams);

    // Apply total variation denoising to the current estimate.
    static ImageType::Pointer ApplyTvRegularizationCpu(
        ImageType::Pointer input,
        const TvParams& tvParams);

    // Run exactly one outer iteration so callers can inject progress and cancellation checks.
    static ImageType::Pointer RunSingleOuterSartIterationCpu(
        ImageType::Pointer currentVolume,
        ImageType::Pointer projections,
        GeometryType::Pointer geometry,
        const SartParams& sartParams);

    static ImageType::Pointer ReconstructCpu(
        ImageType::Pointer projections,
        GeometryType::Pointer geometry,
        const VolumeParams& volumeParams,
        const InitializationParams& initParams,
        const SartParams& sartParams,
        const TvParams& tvParams);

#ifdef RTK_USE_CUDA
    // GPU equivalents of the CPU helpers above.
    static CudaImageType::Pointer BuildInitialVolumeCuda(
        ImageType::Pointer projections,
        GeometryType::Pointer geometry,
        const VolumeParams& volumeParams,
        const InitializationParams& initParams);

    static CudaImageType::Pointer ApplyTvRegularizationCuda(
        CudaImageType::Pointer input,
        const TvParams& tvParams);

    static CudaImageType::Pointer RunSingleOuterSartIterationCuda(
        CudaImageType::Pointer currentVolume,
        CudaImageType::Pointer projections,
        GeometryType::Pointer geometry,
        const SartParams& sartParams);

    static ImageType::Pointer ReconstructCuda(
        ImageType::Pointer projections,
        GeometryType::Pointer geometry,
        const VolumeParams& volumeParams,
        const InitializationParams& initParams,
        const SartParams& sartParams,
        const TvParams& tvParams);
#endif

    static ImageType::Pointer RunCpu(
        const ProjectionFileParams& fileParams,
        const ProjectionImageParams& projectionImageParams,
        GeometryType::Pointer geometry,
        const VolumeParams& volumeParams,
        const InitializationParams& initParams,
        const SartParams& sartParams,
        const TvParams& tvParams);

#ifdef RTK_USE_CUDA
    static ImageType::Pointer RunCuda(
        const ProjectionFileParams& fileParams,
        const ProjectionImageParams& projectionImageParams,
        GeometryType::Pointer geometry,
        const VolumeParams& volumeParams,
        const InitializationParams& initParams,
        const SartParams& sartParams,
        const TvParams& tvParams);
#endif
};

