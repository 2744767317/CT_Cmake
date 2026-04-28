#include "fdkpipeline.h"

#include <cmath>
#include <sstream>

#include <itkMath.h>

std::string FdkPipeline::ToLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool FdkPipeline::HasAnyExtension(
    const std::filesystem::path& path,
    const std::vector<std::string>& extensions)
{
    if (extensions.empty())
        return true;

    const std::string ext = ToLower(path.extension().string());
    for (const auto& e : extensions)
    {
        if (ext == ToLower(e))
            return true;
    }
    return false;
}

std::vector<std::string> FdkPipeline::CollectProjectionFiles(const ProjectionFileParams& params)
{
    // Reconstruction quality depends on view order, so keep file ordering explicit and stable.
    if (!params.fileNames.empty())
    {
        std::vector<std::string> files = params.fileNames;
        if (params.sortLexicographically)
            std::sort(files.begin(), files.end());
        return files;
    }

    if (params.directory.empty())
        throw std::invalid_argument("ProjectionFileParams: both fileNames and directory are empty.");

    std::filesystem::path dir(params.directory);
    if (!std::filesystem::exists(dir))
        throw std::runtime_error("Projection directory does not exist: " + params.directory);
    if (!std::filesystem::is_directory(dir))
        throw std::runtime_error("Projection path is not a directory: " + params.directory);

    std::vector<std::string> files;

    if (params.recursive)
    {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir))
        {
            if (!entry.is_regular_file())
                continue;
            if (!HasAnyExtension(entry.path(), params.extensions))
                continue;
            files.push_back(entry.path().string());
        }
    }
    else
    {
        for (const auto& entry : std::filesystem::directory_iterator(dir))
        {
            if (!entry.is_regular_file())
                continue;
            if (!HasAnyExtension(entry.path(), params.extensions))
                continue;
            files.push_back(entry.path().string());
        }
    }

    if (params.sortLexicographically)
        std::sort(files.begin(), files.end());

    if (files.empty())
    {
        std::ostringstream oss;
        oss << "No projection files found in: " << params.directory;
        if (!params.extensions.empty())
        {
            oss << " with extensions: ";
            for (size_t i = 0; i < params.extensions.size(); ++i)
            {
                oss << params.extensions[i];
                if (i + 1 < params.extensions.size())
                    oss << ", ";
            }
        }
        throw std::runtime_error(oss.str());
    }

    return files;
}

FdkPipeline::ImageType::Pointer
FdkPipeline::ReadProjections(
    const ProjectionFileParams& fileParams,
    const ProjectionImageParams& imageParams)
{
    // rtk::ProjectionsReader stacks 2D images into a 3D projection volume with the supplied
    // detector metadata. The UI-derived origin/spacing land here.
    using ReaderType = rtk::ProjectionsReader<ImageType>;
    auto reader = ReaderType::New();

    const auto files = CollectProjectionFiles(fileParams);
    reader->SetFileNames(files);
    reader->SetOrigin(imageParams.origin);
    reader->SetSpacing(imageParams.spacing);
    reader->SetDirection(imageParams.direction);
    reader->SetComputeLineIntegral(imageParams.computeLineIntegral);
    reader->SetIDark(imageParams.iDark);

    if (imageParams.i0.has_value())
        reader->SetI0(*imageParams.i0);

    reader->Update();
    return reader->GetOutput();
}

FdkPipeline::GeometryType::Pointer
FdkPipeline::BuildGeometry(const GeometryParams& params)
{
    if (params.views.empty())
        throw std::invalid_argument("GeometryParams.views is empty.");

    // Geometry is defined view-by-view so acquisition code can stay independent from RTK classes.
    auto geometry = GeometryType::New();

    for (const auto& view : params.views)
    {
        geometry->AddProjection(
            view.sid,
            view.sdd,
            view.gantryAngleDeg,
            view.projOffsetX,
            view.projOffsetY,
            view.outOfPlaneAngleDeg,
            view.inPlaneAngleDeg,
            view.sourceOffsetX,
            view.sourceOffsetY);
    }

    return geometry;
}

FdkPipeline::GeometryType::Pointer
FdkPipeline::LoadGeometryFromXml(const std::string& xmlPath)
{
    if (xmlPath.empty())
        throw std::invalid_argument("Geometry XML path is empty.");

    auto reader = rtk::ThreeDCircularProjectionGeometryXMLFileReader::New();
    reader->SetFilename(xmlPath.c_str());
    reader->GenerateOutputInformation();
    return reader->GetModifiableGeometry();
}

FdkPipeline::ImageType::Pointer
FdkPipeline::CreateEmptyVolume(const VolumeParams& params)
{
    // RTK expects an input volume seed that defines the reconstruction grid.
    using ConstantImageSourceType = rtk::ConstantImageSource<ImageType>;

    auto source = ConstantImageSourceType::New();
    source->SetOrigin(params.origin);
    source->SetSpacing(params.spacing);
    source->SetSize(params.size);
    source->SetConstant(params.constantValue);
    source->Update();

    return source->GetOutput();
}

FdkPipeline::ImageType::Pointer
FdkPipeline::ReconstructCpu(
    ImageType::Pointer projections,
    GeometryType::Pointer geometry,
    const VolumeParams& volumeParams,
    const FdkParams& fdkParams)
{
    if (!projections)
        throw std::invalid_argument("ReconstructCpu: projections is null.");
    if (!geometry)
        throw std::invalid_argument("ReconstructCpu: geometry is null.");

    using ParkerType = rtk::ParkerShortScanImageFilter<ImageType>;
    using FdkType = rtk::FDKConeBeamReconstructionFilter<ImageType, ImageType>;
    using FovType = rtk::FieldOfViewImageFilter<ImageType, ImageType>;

    auto reconstructionSeed = CreateEmptyVolume(volumeParams);

    ImageType::Pointer fdkInputProjections = projections;

    if (fdkParams.useParkerShortScan)
    {
        // Parker weighting compensates short-scan redundancy before the analytic backprojection.
        auto parker = ParkerType::New();
        parker->SetInput(projections);
        parker->SetGeometry(geometry);
        parker->SetAngularGapThreshold(
            fdkParams.parkerAngularGapThresholdDeg * itk::Math::pi / 180.0);
        parker->Update();
        fdkInputProjections = parker->GetOutput();
    }

    auto fdk = FdkType::New();
    fdk->SetInput(0, reconstructionSeed);
    fdk->SetInput(1, fdkInputProjections);
    fdk->SetGeometry(geometry);
    fdk->SetProjectionSubsetSize(fdkParams.projectionSubsetSize);
    fdk->GetRampFilter()->SetTruncationCorrection(fdkParams.truncationCorrection);
    fdk->GetRampFilter()->SetHannCutFrequency(fdkParams.hannCutFrequency);

    if (!fdkParams.applyFieldOfViewMask)
    {
        fdk->Update();
        return fdk->GetOutput();
    }

    // FoV masking is applied after reconstruction so outside-of-support voxels are easy to inspect.
    auto fov = FovType::New();
    fov->SetInput(0, fdk->GetOutput());
    fov->SetProjectionsStack(projections);
    fov->SetGeometry(geometry);
    fov->SetOutsideValue(fdkParams.fovOutsideValue);
    fov->SetMask(fdkParams.fovMaskMode);
    fov->Update();

    return fov->GetOutput();
}

#ifdef RTK_USE_CUDA

FdkPipeline::CudaImageType::Pointer
FdkPipeline::CreateEmptyCudaVolume(const VolumeParams& params)
{
    using CudaConstantVolumeSourceType = rtk::CudaConstantVolumeSource;
    auto source = CudaConstantVolumeSourceType::New();

    source->SetOrigin(params.origin);
    source->SetSpacing(params.spacing);
    source->SetSize(params.size);
    source->SetConstant(params.constantValue);
    source->Update();

    return source->GetOutput();
}

FdkPipeline::CudaImageType::Pointer
FdkPipeline::ToCudaImage(ImageType::Pointer image)
{
    using CastFilterType = itk::CastImageFilter<ImageType, CudaImageType>;
    auto caster = CastFilterType::New();
    caster->SetInput(image);
    caster->Update();
    return caster->GetOutput();
}

FdkPipeline::ImageType::Pointer
FdkPipeline::ToCpuImage(CudaImageType::Pointer image)
{
    using CastFilterType = itk::CastImageFilter<CudaImageType, ImageType>;
    auto caster = CastFilterType::New();
    caster->SetInput(image);
    caster->Update();
    return caster->GetOutput();
}

FdkPipeline::ImageType::Pointer
FdkPipeline::ReconstructCuda(
    ImageType::Pointer projections,
    GeometryType::Pointer geometry,
    const VolumeParams& volumeParams,
    const FdkParams& fdkParams)
{
    if (!projections)
        throw std::invalid_argument("ReconstructCuda: projections is null.");
    if (!geometry)
        throw std::invalid_argument("ReconstructCuda: geometry is null.");

    using CudaParkerType = rtk::CudaParkerShortScanImageFilter;
    using CudaFdkType = rtk::CudaFDKConeBeamReconstructionFilter;
    using CpuFovType = rtk::FieldOfViewImageFilter<ImageType, ImageType>;

    // Only the core FDK stage runs on GPU here; geometry and final masking stay CPU-side.
    auto cudaProjections = ToCudaImage(projections);
    auto cudaVolumeSeed = CreateEmptyCudaVolume(volumeParams);

    CudaImageType::Pointer cudaFdkInputProjections = cudaProjections;

    if (fdkParams.useParkerShortScan)
    {
        // Match the CPU path's short-scan weighting before CUDA FDK.
        auto parker = CudaParkerType::New();
        parker->SetInput(cudaProjections);
        parker->SetGeometry(geometry);
        parker->SetAngularGapThreshold(
            fdkParams.parkerAngularGapThresholdDeg * itk::Math::pi / 180.0);
        parker->Update();
        cudaFdkInputProjections = parker->GetOutput();
    }

    auto fdk = CudaFdkType::New();
    fdk->SetInput(0, cudaVolumeSeed);
    fdk->SetInput(1, cudaFdkInputProjections);
    fdk->SetGeometry(geometry);
    fdk->SetProjectionSubsetSize(fdkParams.projectionSubsetSize);
    fdk->GetRampFilter()->SetTruncationCorrection(fdkParams.truncationCorrection);
    fdk->GetRampFilter()->SetHannCutFrequency(fdkParams.hannCutFrequency);
    fdk->Update();

    auto cpuRecon = ToCpuImage(fdk->GetOutput());

    if (!fdkParams.applyFieldOfViewMask)
        return cpuRecon;

    // The FoV filter used in this project is CPU-based, so finish the pipeline on CPU.
    auto fov = CpuFovType::New();
    fov->SetInput(0, cpuRecon);
    fov->SetProjectionsStack(projections);
    fov->SetGeometry(geometry);
    fov->SetOutsideValue(fdkParams.fovOutsideValue);
    fov->SetMask(fdkParams.fovMaskMode);
    fov->Update();

    return fov->GetOutput();
}

#endif

FdkPipeline::ImageType::Pointer
FdkPipeline::RunCpu(
    const ProjectionFileParams& fileParams,
    const ProjectionImageParams& projectionImageParams,
    GeometryType::Pointer geometry,
    const VolumeParams& volumeParams,
    const FdkParams& fdkParams)
{
    auto projections = ReadProjections(fileParams, projectionImageParams);
    return ReconstructCpu(projections, geometry, volumeParams, fdkParams);
}

#ifdef RTK_USE_CUDA
FdkPipeline::ImageType::Pointer
FdkPipeline::RunCuda(
    const ProjectionFileParams& fileParams,
    const ProjectionImageParams& projectionImageParams,
    GeometryType::Pointer geometry,
    const VolumeParams& volumeParams,
    const FdkParams& fdkParams)
{
    auto projections = ReadProjections(fileParams, projectionImageParams);
    return ReconstructCuda(projections, geometry, volumeParams, fdkParams);
}
#endif

