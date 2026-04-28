#ifndef FDKPIPELINE_H
#define FDKPIPELINE_H

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <algorithm>
#include <stdexcept>

#include <itkImage.h>
#include <itkMatrix.h>

#include <rtkProjectionsReader.h>
#include <rtkConstantImageSource.h>
#include <rtkThreeDCircularProjectionGeometry.h>
#include <rtkThreeDCircularProjectionGeometryXMLFileReader.h>
#include <rtkFDKConeBeamReconstructionFilter.h>
#include <rtkParkerShortScanImageFilter.h>
#include <rtkFieldOfViewImageFilter.h>

#ifdef RTK_USE_CUDA
#  include <itkCudaImage.h>
#  include <itkCastImageFilter.h>
#  include <rtkCudaConstantVolumeSource.h>
#  include <rtkCudaFDKConeBeamReconstructionFilter.h>
#  include <rtkCudaParkerShortScanImageFilter.h>
#endif

// Thin RTK/ITK helper around the common cone-beam FDK building blocks:
// projection file collection, metadata loading, geometry creation, and CPU/CUDA execution.
class FdkPipeline
{
public:
    using PixelType = float;
    static constexpr unsigned int Dimension = 3;

    using ImageType = itk::Image<PixelType, Dimension>;
    using GeometryType = rtk::ThreeDCircularProjectionGeometry;
    using DirectionType = ImageType::DirectionType;
    using PointType = ImageType::PointType;
    using SpacingType = ImageType::SpacingType;
    using SizeType = ImageType::SizeType;

#ifdef RTK_USE_CUDA
    using CudaImageType = itk::CudaImage<PixelType, Dimension>;
#endif

    // Metadata applied when 2D detector files are stacked into one RTK projection volume.
    struct ProjectionImageParams
    {
        PointType origin{};
        SpacingType spacing{};
        DirectionType direction{};

        // Optional Beer-Lambert conversion from raw intensity to line integral.
        bool computeLineIntegral = false;
        std::optional<double> i0;
        double iDark = 0.0;

        ProjectionImageParams()
        {
            origin.Fill(0.0);
            spacing.Fill(1.0);
            direction.SetIdentity();
        }
    };

    // Either fileNames or directory is used. The UI normally fills directory + extensions.
    struct ProjectionFileParams
    {
        std::vector<std::string> fileNames;
        std::string directory;
        std::vector<std::string> extensions;
        bool recursive = false;
        bool sortLexicographically = true;
    };

    // One geometry entry per projection view.
    struct ProjectionViewParam
    {
        double sid = 0.0;
        double sdd = 0.0;
        double gantryAngleDeg = 0.0;

        double projOffsetX = 0.0;
        double projOffsetY = 0.0;
        double outOfPlaneAngleDeg = 0.0;
        double inPlaneAngleDeg = 0.0;
        double sourceOffsetX = 0.0;
        double sourceOffsetY = 0.0;
    };

    // Explicit list of scan views, decoupled from how files are stored on disk.
    struct GeometryParams
    {
        std::vector<ProjectionViewParam> views;
    };

    // Defines the reconstructed voxel grid and seed value.
    struct VolumeParams
    {
        PointType origin{};
        SpacingType spacing{};
        SizeType size{};
        PixelType constantValue = 0.0f;

        VolumeParams()
        {
            origin.Fill(0.0);
            spacing.Fill(1.0);
            size.Fill(0);
        }
    };

    // Parameters forwarded to RTK's FDK filter and optional post-processing stages.
    struct FdkParams
    {
        unsigned int projectionSubsetSize = 16;
        double hannCutFrequency = 0.0;
        double truncationCorrection = 0.0;

        bool useParkerShortScan = false;
        double parkerAngularGapThresholdDeg = 20.0;

        bool applyFieldOfViewMask = true;
        double fovOutsideValue = 0.0;
        bool fovMaskMode = false;
    };

public:
    // Scan a directory with optional extension filtering and stable ordering.
    static std::vector<std::string> CollectProjectionFiles(const ProjectionFileParams& params);

    // Read projection files into a 3D RTK volume.
    static ImageType::Pointer ReadProjections(
        const ProjectionFileParams& fileParams,
        const ProjectionImageParams& imageParams);

    // Convert the app's geometry description into RTK's circular geometry object.
    static GeometryType::Pointer BuildGeometry(const GeometryParams& params);

    static GeometryType::Pointer LoadGeometryFromXml(const std::string& xmlPath);

    static ImageType::Pointer CreateEmptyVolume(const VolumeParams& params);

    // CPU FDK path. Optional Parker weighting and FoV masking are handled here.
    static ImageType::Pointer ReconstructCpu(
        ImageType::Pointer projections,
        GeometryType::Pointer geometry,
        const VolumeParams& volumeParams,
        const FdkParams& fdkParams);

#ifdef RTK_USE_CUDA
    static CudaImageType::Pointer CreateEmptyCudaVolume(const VolumeParams& params);

    static CudaImageType::Pointer ToCudaImage(ImageType::Pointer image);

    static ImageType::Pointer ToCpuImage(CudaImageType::Pointer image);

    // CUDA FDK path. Reconstruction runs on GPU, FoV masking remains on CPU.
    static ImageType::Pointer ReconstructCuda(
        ImageType::Pointer projections,
        GeometryType::Pointer geometry,
        const VolumeParams& volumeParams,
        const FdkParams& fdkParams);
#endif

    static ImageType::Pointer RunCpu(
        const ProjectionFileParams& fileParams,
        const ProjectionImageParams& projectionImageParams,
        GeometryType::Pointer geometry,
        const VolumeParams& volumeParams,
        const FdkParams& fdkParams);

#ifdef RTK_USE_CUDA
    static ImageType::Pointer RunCuda(
        const ProjectionFileParams& fileParams,
        const ProjectionImageParams& projectionImageParams,
        GeometryType::Pointer geometry,
        const VolumeParams& volumeParams,
        const FdkParams& fdkParams);
#endif

private:
    static bool HasAnyExtension(
        const std::filesystem::path& path,
        const std::vector<std::string>& extensions);

    static std::string ToLower(std::string s);
};

#endif // FDKPIPELINE_H
