/*
** Copyright (c) 2020 LunarG, Inc.
** Copyright (c) 2022-2023 Advanced Micro Devices, Inc. All rights reserved.
**
** Permission is hereby granted, free of charge, to any person obtaining a
** copy of this software and associated documentation files (the "Software"),
** to deal in the Software without restriction, including without limitation
** the rights to use, copy, modify, merge, publish, distribute, sublicense,
** and/or sell copies of the Software, and to permit persons to whom the
** Software is furnished to do so, subject to the following conditions:
**
** The above copyright notice and this permission notice shall be included in
** all copies or substantial portions of the Software.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
** AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
** LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
** FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
** DEALINGS IN THE SOFTWARE.
*/

#include "project_version.h"

#include "decode/decode_api_detection.h"
#include "decode/stat_consumer.h"
#include "decode/stat_consumer_base.h"
#include "decode/stat_decoder_base.h"
#include "decode/file_processor.h"
#include "format/format.h"
#include "format/format_util.h"
#include "generated/generated_vulkan_consumer.h"
#include "generated/generated_vulkan_decoder.h"
#include "decode/info_decoder.h"
#include "decode/info_consumer.h"
#include "decode/vulkan_detection_consumer.h"
#include "decode/vulkan_stats_consumer.h"
#if defined(D3D12_SUPPORT)
#include "decode/dx12_stats_consumer.h"
#include "generated/generated_dx12_decoder.h"
#include "decode/dx12_detection_consumer.h"
#include "graphics/dx12_util.h"
#endif
#include "util/argument_parser.h"
#include "util/strings.h"
#include "util/logging.h"
#include "util/to_string.h"

#include "vulkan/vulkan.h"

#include <cassert>
#include <cstdlib>
#include <limits>
#include <set>
#include <string>
#include <unordered_map>

const char kHelpShortOption[]   = "-h";
const char kHelpLongOption[]    = "--help";
const char kVersionOption[]     = "--version";
const char kNoDebugPopup[]      = "--no-debug-popup";
const char kExeInfoOnlyOption[] = "--exe-info-only";
const char kEnumGpuIndices[]    = "--enum-gpu-indices";

const char kOptions[] = "-h|--help,--version,--no-debug-popup,--exe-info-only,--enum-gpu-indices";

const char kUnrecognizedFormatString[] = "<unrecognized-format>";

struct ApiAgnosticStats
{
    gfxrecon::format::CompressionType      compression_type;
    uint32_t                               trim_start_frame;
    uint32_t                               frame_count;
    gfxrecon::decode::FileProcessor::Error error_state;
};

std::string AdapterTypeToString(gfxrecon::format::AdapterType type)
{
    switch (type)
    {
        case gfxrecon::format::AdapterType::kUnknownAdapter:
            return "Unknown type (DXGI 1.0)";
        case gfxrecon::format::AdapterType::kSoftwareAdapter:
            return "Software";
        case gfxrecon::format::AdapterType::kHardwareAdapter:
            return "Hardware";
        default:
            return "Unknown";
    }
}

static void PrintUsage(const char* exe_name)
{
    std::string app_name     = exe_name;
    size_t      dir_location = app_name.find_last_of("/\\");
    if (dir_location >= 0)
    {
        app_name.replace(0, dir_location + 1, "");
    }
    GFXRECON_WRITE_CONSOLE("\n%s - Print statistics for a GFXReconstruct capture file.\n", app_name.c_str());
    GFXRECON_WRITE_CONSOLE("Usage:");
    GFXRECON_WRITE_CONSOLE("  %s [-h | --help] [--version] [--exe-info-only] <file>\n", app_name.c_str());
    GFXRECON_WRITE_CONSOLE("Required arguments:");
    GFXRECON_WRITE_CONSOLE("  <file>\t\tThe GFXReconstruct capture file to be processed.");
    GFXRECON_WRITE_CONSOLE("\nOptional arguments:");
    GFXRECON_WRITE_CONSOLE("  -h\t\t\tPrint usage information and exit (same as --help).");
    GFXRECON_WRITE_CONSOLE("  --version\t\tPrint version information and exit.");
    GFXRECON_WRITE_CONSOLE("  --exe-info-only\tQuickly exit after extracting captured application's executable name");
#if defined(WIN32) && defined(_DEBUG)
    GFXRECON_WRITE_CONSOLE("  --no-debug-popup\tDisable the 'Abort, Retry, Ignore' message box");
    GFXRECON_WRITE_CONSOLE("        \t\tdisplayed when abort() is called (Windows debug only).");
#endif
#if defined(WIN32)
    GFXRECON_WRITE_CONSOLE("  --enum-gpu-indices\tPrint GPU indices and exit");
#endif
}

static bool CheckOptionPrintUsage(const char* exe_name, const gfxrecon::util::ArgumentParser& arg_parser)
{
    if (arg_parser.IsOptionSet(kHelpShortOption) || arg_parser.IsOptionSet(kHelpLongOption))
    {
        PrintUsage(exe_name);
        return true;
    }

    return false;
}

static bool CheckOptionPrintVersion(const char* exe_name, const gfxrecon::util::ArgumentParser& arg_parser)
{
    if (arg_parser.IsOptionSet(kVersionOption))
    {
        std::string app_name     = exe_name;
        size_t      dir_location = app_name.find_last_of("/\\");

        if (dir_location >= 0)
        {
            app_name.replace(0, dir_location + 1, "");
        }

        GFXRECON_WRITE_CONSOLE("%s version info:", app_name.c_str());
        GFXRECON_WRITE_CONSOLE("  GFXReconstruct Version %s", GFXRECON_PROJECT_VERSION_STRING);
        GFXRECON_WRITE_CONSOLE("  Vulkan Header Version %u.%u.%u",
                               VK_VERSION_MAJOR(VK_HEADER_VERSION_COMPLETE),
                               VK_VERSION_MINOR(VK_HEADER_VERSION_COMPLETE),
                               VK_VERSION_PATCH(VK_HEADER_VERSION_COMPLETE));

        return true;
    }

    return false;
}

static std::string GetVersionString(uint32_t api_version)
{
    uint32_t major = api_version >> 22;
    uint32_t minor = (api_version >> 12) & 0x3ff;
    uint32_t patch = api_version & 0xfff;

    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

void GatherApiAgnosticStats(ApiAgnosticStats&                api_agnostic_stats,
                            gfxrecon::decode::FileProcessor& file_processor,
                            gfxrecon::decode::StatConsumer&  stat_consumer)
{
    api_agnostic_stats.error_state = file_processor.GetErrorState();

    // File options.
    gfxrecon::format::CompressionType compression_type = gfxrecon::format::CompressionType::kNone;

    auto file_options = file_processor.GetFileOptions();
    for (const auto& option : file_options)
    {
        if (option.key == gfxrecon::format::FileOption::kCompressionType)
        {
            compression_type = static_cast<gfxrecon::format::CompressionType>(option.value);
        }
    }
    api_agnostic_stats.compression_type = compression_type;
    api_agnostic_stats.trim_start_frame = stat_consumer.GetTrimmedStartFrame();
    api_agnostic_stats.frame_count      = file_processor.GetCurrentFrameNumber();
}

void PrintDriverInfo(const gfxrecon::decode::InfoConsumer& driver_info_consumer)
{
    GFXRECON_WRITE_CONSOLE("");
    GFXRECON_WRITE_CONSOLE("Driver info:");
    GFXRECON_WRITE_CONSOLE("\t%s", driver_info_consumer.GetDriverDesc());
}

void PrintExeInfo(const gfxrecon::decode::InfoConsumer& info_consumer)
{
    GFXRECON_WRITE_CONSOLE("Exe info:");
    GFXRECON_WRITE_CONSOLE("\tApplication exe name: %s", info_consumer.GetAppExeName().c_str());

    auto exe_version = info_consumer.GetAppVersion();
    GFXRECON_WRITE_CONSOLE(
        "\tApplication version: %d.%d.%d.%d", exe_version[0], exe_version[1], exe_version[2], exe_version[3]);
    GFXRECON_WRITE_CONSOLE("\tApplication Company name: %s", info_consumer.GetCompanyName());

    // we are combining file description and product name and presenting both only if they are not same
    std::string app_data = info_consumer.GetFileDescription();
    if (strcmp(info_consumer.GetProductName(), "N/A") != 0)
    {
        if (strcmp(info_consumer.GetProductName(), info_consumer.GetFileDescription()) != 0)
        {
            app_data += " // ";
            app_data += info_consumer.GetProductName();
        }
    }
    GFXRECON_WRITE_CONSOLE("\tProduct name: %s", app_data.c_str());
}

void PrintVulkanStats(const gfxrecon::decode::VulkanStatsConsumer& vulkan_stats_consumer,
                      const gfxrecon::decode::FileProcessor&       file_processor,
                      const ApiAgnosticStats&                      api_agnostic_stats)
{
    if (api_agnostic_stats.error_state == gfxrecon::decode::FileProcessor::kErrorNone)
    {
        GFXRECON_WRITE_CONSOLE("File info:");
        gfxrecon::format::CompressionType compression_type = gfxrecon::format::CompressionType::kNone;

        auto file_options = file_processor.GetFileOptions();
        for (const auto& option : file_options)
        {
            if (option.key == gfxrecon::format::FileOption::kCompressionType)
            {
                compression_type = static_cast<gfxrecon::format::CompressionType>(option.value);
            }
        }

        // Compression type.
        std::string compression_type_name = gfxrecon::format::GetCompressionTypeName(compression_type);
        if (!compression_type_name.empty())
        {
            GFXRECON_WRITE_CONSOLE("\tCompression format: %s", compression_type_name.c_str());
        }
        else
        {
            GFXRECON_WRITE_CONSOLE("\tCompression format: %s", kUnrecognizedFormatString);
        }

        // Frame counts.
        uint32_t trim_start_frame = vulkan_stats_consumer.GetTrimmedStartFrame();
        uint32_t frame_count      = file_processor.GetCurrentFrameNumber();

        if (trim_start_frame == 0)
        {
            // Not a trimmed file.
            GFXRECON_WRITE_CONSOLE("\tTotal frames: %u", frame_count);
        }
        else
        {
            // Include the frame range for trimmed files.
            GFXRECON_WRITE_CONSOLE("\tTotal frames: %u (trimmed frame range %u-%u)",
                                   frame_count,
                                   trim_start_frame,
                                   trim_start_frame + frame_count - 1);
        }

        // Application info.
        uint32_t api_version = vulkan_stats_consumer.GetApiVersion();
        GFXRECON_WRITE_CONSOLE("\nApplication info:");
        GFXRECON_WRITE_CONSOLE("\tApplication name: %s", vulkan_stats_consumer.GetAppName().c_str());
        GFXRECON_WRITE_CONSOLE("\tApplication version: %u", vulkan_stats_consumer.GetAppVersion());
        GFXRECON_WRITE_CONSOLE("\tEngine name: %s", vulkan_stats_consumer.GetEngineName().c_str());
        GFXRECON_WRITE_CONSOLE("\tEngine version: %u", vulkan_stats_consumer.GetEngineVersion());
        GFXRECON_WRITE_CONSOLE("\tTarget API version: %u (%s)", api_version, GetVersionString(api_version).c_str());

        // Properties for physical devices used to create logical devices.
        std::vector<const VkPhysicalDeviceProperties*> used_device_properties;
        auto                                           used_devices = vulkan_stats_consumer.GetInstantiatedDevices();
        for (auto entry : used_devices)
        {
            auto properties = vulkan_stats_consumer.GetDeviceProperties(entry);
            if (properties != nullptr)
            {
                used_device_properties.push_back(properties);
            }
        }

        // Don't print anything if no queries were made for VkPhysicalDeviceProperties.
        if (!used_device_properties.empty())
        {
            for (auto entry : used_device_properties)
            {
                GFXRECON_WRITE_CONSOLE("\nPhysical device info:");
                GFXRECON_WRITE_CONSOLE("\tDevice name: %s", entry->deviceName);
                GFXRECON_WRITE_CONSOLE("\tDevice ID: 0x%x", entry->deviceID);
                GFXRECON_WRITE_CONSOLE("\tVendor ID: 0x%x", entry->vendorID);
                GFXRECON_WRITE_CONSOLE("\tDriver version: %u (0x%x)", entry->driverVersion, entry->driverVersion);
                GFXRECON_WRITE_CONSOLE(
                    "\tAPI version: %u (%s)", entry->apiVersion, GetVersionString(entry->apiVersion).c_str());
            }
        }

        auto alocation_count = vulkan_stats_consumer.GetAllocationCount();
        GFXRECON_WRITE_CONSOLE("\nDevice memory allocation info:");
        GFXRECON_WRITE_CONSOLE("\tTotal allocations: %" PRIu64, alocation_count);

        if (alocation_count > 0)
        {
            GFXRECON_WRITE_CONSOLE("\tMin allocation size: %" PRIu64, vulkan_stats_consumer.GetMinAllocationSize());
            GFXRECON_WRITE_CONSOLE("\tMax allocation size: %" PRIu64, vulkan_stats_consumer.GetMaxAllocationSize());
        }

        GFXRECON_WRITE_CONSOLE("\nPipeline info:");
        GFXRECON_WRITE_CONSOLE("\tTotal graphics pipelines: %" PRIu64,
                               vulkan_stats_consumer.GetGraphicsPipelineCount());
        GFXRECON_WRITE_CONSOLE("\tTotal compute pipelines: %" PRIu64, vulkan_stats_consumer.GetComputePipelineCount());

        const auto annotation_count = vulkan_stats_consumer.GetAnnotationCount();
        if (annotation_count > 0)
        {
            GFXRECON_WRITE_CONSOLE("\nAnnotation info:");
            GFXRECON_WRITE_CONSOLE("\tTotal annotations: %" PRIu64, annotation_count);
            auto& operation_annotation_datas = vulkan_stats_consumer.GetOperationAnnotationDatas();
            if (operation_annotation_datas.size() > 0)
            {
                GFXRECON_WRITE_CONSOLE("\tOperation annotations: %" PRIu64 "\n", operation_annotation_datas.size());
                for (const auto& operation : operation_annotation_datas)
                {
                    auto tabbed = gfxrecon::util::strings::TabRight(operation);
                    GFXRECON_WRITE_CONSOLE(tabbed.c_str());
                }
            }
        }

        // TODO: This is the number of recorded draw calls, which will not reflect the number of draw calls
        // executed when recorded once to a command buffer that is submitted/replayed more than once.
        // GFXRECON_WRITE_CONSOLE("\nDraw/dispatch call info:");
        // GFXRECON_WRITE_CONSOLE("\tTotal draw calls: %" PRIu64, stats_consumer.GetDrawCount());
        // GFXRECON_WRITE_CONSOLE("\tTotal dispatch calls: %" PRIu64, stats_consumer.GetDispatchCount());

        if (file_processor.GetCurrentFrameNumber() == 0)
        {
            GFXRECON_WRITE_CONSOLE("\nFile did not contain any frames");
        }
    }
    else if (api_agnostic_stats.error_state != gfxrecon::decode::FileProcessor::kErrorNone)
    {
        GFXRECON_WRITE_CONSOLE("A failure has occurred during file processing");
        gfxrecon::util::Log::Release();
        exit(-1);
    }
    else
    {
        GFXRECON_WRITE_CONSOLE("File did not contain any frames");
    }
}

void GatherExeInfo(const std::string& input_filename, gfxrecon::decode::InfoConsumer& info_consumer)
{
    gfxrecon::decode::FileProcessor file_processor;
    if (file_processor.Initialize(input_filename))
    {
        gfxrecon::decode::InfoDecoder info_decoder;
        info_decoder.AddConsumer(&info_consumer);
        file_processor.AddDecoder(&info_decoder);
        file_processor.ProcessAllFrames();
    }
}

void GatherVulkanStats(const std::string& input_filename)
{
    gfxrecon::decode::FileProcessor file_processor;
    if (file_processor.Initialize(input_filename))
    {
        gfxrecon::decode::VulkanStatsConsumer vulkan_stats_consumer;
        gfxrecon::decode::StatDecoderBase     stat_decoder;
        gfxrecon::decode::StatConsumer        stat_consumer;
        gfxrecon::decode::VulkanDecoder       vulkan_decoder;

        stat_decoder.AddConsumer(&stat_consumer);
        file_processor.AddDecoder(&stat_decoder);
        vulkan_decoder.AddConsumer(&vulkan_stats_consumer);
        file_processor.SetAnnotationProcessor(&vulkan_stats_consumer);
        file_processor.AddDecoder(&vulkan_decoder);

        file_processor.ProcessAllFrames();
        if (file_processor.GetErrorState() == gfxrecon::decode::FileProcessor::kErrorNone)
        {
            ApiAgnosticStats api_agnostic_stats = {};
            GatherApiAgnosticStats(api_agnostic_stats, file_processor, stat_consumer);
            gfxrecon::decode::InfoConsumer info_consumer(true);
            GatherExeInfo(input_filename, info_consumer);
            PrintExeInfo(info_consumer);
            PrintVulkanStats(vulkan_stats_consumer, file_processor, api_agnostic_stats);
        }
        else
        {
            GFXRECON_WRITE_CONSOLE("Encountered error while reading capture. Stats unavailable.");
        }
    }
}

#if defined(D3D12_SUPPORT)
void PrintDx12RuntimeInfo(gfxrecon::decode::Dx12StatsConsumer& dx12_consumer)
{
    GFXRECON_WRITE_CONSOLE("D3D12 runtime info:");

    gfxrecon::format::Dx12RuntimeInfo runtime_info = dx12_consumer.GetDx12RuntimeInfo();

    std::string runtime_src = runtime_info.src;
    std::string runtime_ver = "";

    if (runtime_src.empty() == false)
    {
        runtime_ver = std::to_string(runtime_info.version[0]) + "." + std::to_string(runtime_info.version[1]) + "." +
                      std::to_string(runtime_info.version[2]) + "." + std::to_string(runtime_info.version[3]);
        GFXRECON_WRITE_CONSOLE("\tVersion: %s", runtime_ver.c_str());
        GFXRECON_WRITE_CONSOLE("\tSource: %s", runtime_src.c_str());
    }
    else
    {
        GFXRECON_WRITE_CONSOLE("\tVersion: N/A");
        GFXRECON_WRITE_CONSOLE("\tSource: N/A");
    }

    GFXRECON_WRITE_CONSOLE("");
}

void PrintDx12AdapterInfo(gfxrecon::decode::Dx12StatsConsumer& dx12_consumer)
{
    GFXRECON_WRITE_CONSOLE("D3D12 adapter info:");
    const std::vector<gfxrecon::format::DxgiAdapterDesc> adapters = dx12_consumer.GetAdapters();

    if (adapters.empty() == false)
    {
        std::unordered_map<int64_t, std::string> adapter_workload;
        dx12_consumer.CalcAdapterWorkload(adapter_workload, adapters);

        for (const auto& adapter : adapters)
        {
            const int64_t luid = (adapter.LuidHighPart << 31) | adapter.LuidLowPart;

            std::string adapter_workload_pct = "";

            if (adapter_workload.count(luid) > 0)
            {
                if (adapter_workload[luid] != "")
                {
                    adapter_workload_pct = "(" + adapter_workload[luid] + "% of GPU submissions)";
                }
            }
            else if (adapter_workload.size() > 0)
            {
                adapter_workload_pct = "(0% of GPU submissions)";
            }

            std::string adapter_type =
                AdapterTypeToString(gfxrecon::graphics::dx12::ExtractAdapterType(adapter.extra_info));

            GFXRECON_WRITE_CONSOLE("\tDescription: %s %s",
                                   gfxrecon::util::WCharArrayToString(adapter.Description).c_str(),
                                   adapter_workload_pct.c_str());
            GFXRECON_WRITE_CONSOLE("\tVendor ID: 0x%x", adapter.VendorId);
            GFXRECON_WRITE_CONSOLE("\tDevice ID: 0x%x", adapter.DeviceId);
            GFXRECON_WRITE_CONSOLE("\tSubsys ID: 0x%x", adapter.SubSysId);
            GFXRECON_WRITE_CONSOLE("\tRevision: %u", adapter.Revision);
            GFXRECON_WRITE_CONSOLE("\tDedicated Video Memory: %" PRIu64, adapter.DedicatedVideoMemory);
            GFXRECON_WRITE_CONSOLE("\tDedicated System Memory: %" PRIu64, adapter.DedicatedSystemMemory);
            GFXRECON_WRITE_CONSOLE("\tShared System Memory: %" PRIu64, adapter.SharedSystemMemory);
            GFXRECON_WRITE_CONSOLE("\tLUID LowPart: 0x%x", adapter.LuidLowPart);
            GFXRECON_WRITE_CONSOLE("\tLUID HighPart: 0x%x", adapter.LuidHighPart);
            GFXRECON_WRITE_CONSOLE("\tAdapter type: %s", adapter_type.c_str());
            GFXRECON_WRITE_CONSOLE("");
        }
    }
    else
    {
        GFXRECON_WRITE_CONSOLE("\tAdapter info not available.");
        GFXRECON_WRITE_CONSOLE("");
    }
}

void PrintDx12SwapchainInfo(gfxrecon::decode::Dx12StatsConsumer& dx12_consumer)
{
    GFXRECON_WRITE_CONSOLE("D3D12 swapchain info:");

    if (dx12_consumer.FoundSwapchainInfo())
    {
        GFXRECON_WRITE_CONSOLE("\tDimensions: %s", dx12_consumer.GetSwapchainDimensions().c_str());
    }
    else
    {
        GFXRECON_WRITE_CONSOLE("\tDimensions not available.");
    }

    GFXRECON_WRITE_CONSOLE("");
}

void PrintDxrEiInfo(gfxrecon::decode::Dx12StatsConsumer& dx12_consumer)
{
    if (dx12_consumer.ContainsEiWorkload())
    {
        GFXRECON_WRITE_CONSOLE("D3D12 EI workload: yes");
    }
    else
    {
        GFXRECON_WRITE_CONSOLE("D3D12 EI workload: no");
    }

    GFXRECON_WRITE_CONSOLE("");

    if (dx12_consumer.ContainsDxrWorkload())
    {
        GFXRECON_WRITE_CONSOLE("D3D12 DXR workload: yes");
    }
    else
    {
        GFXRECON_WRITE_CONSOLE("D3D12 DXR workload: no");
    }

    GFXRECON_WRITE_CONSOLE("");

    if (dx12_consumer.ContainsEiWorkload() || dx12_consumer.ContainsDxrWorkload())
    {
        if (dx12_consumer.ContainsOptFillMem())
        {
            GFXRECON_WRITE_CONSOLE("D3D12 DXR/EI optimized: yes");
        }
        else
        {
            GFXRECON_WRITE_CONSOLE("D3D12 DXR/EI optimized: no");
        }
    }
}

void PrintD3D12Stats(gfxrecon::decode::Dx12StatsConsumer& dx12_consumer,
                     const ApiAgnosticStats&              api_agnostic_stats,
                     gfxrecon::decode::InfoConsumer&      info_consumer)
{
    if (api_agnostic_stats.error_state == gfxrecon::decode::FileProcessor::kErrorNone)
    {
        GFXRECON_WRITE_CONSOLE("");
        GFXRECON_WRITE_CONSOLE("File info:");

        // Compression type.
        std::string compression_type_name =
            gfxrecon::format::GetCompressionTypeName(api_agnostic_stats.compression_type);
        if (!compression_type_name.empty())
        {
            GFXRECON_WRITE_CONSOLE("\tCompression format: %s", compression_type_name.c_str());
        }
        else
        {
            GFXRECON_WRITE_CONSOLE("\tCompression format: %s", kUnrecognizedFormatString);
        }

        if (api_agnostic_stats.trim_start_frame == 0)
        {
            // Not a trimmed file.
            GFXRECON_WRITE_CONSOLE("\tTotal frames: %u", api_agnostic_stats.frame_count);
        }
        else
        {
            GFXRECON_WRITE_CONSOLE("\tBlank frames: %u", dx12_consumer.GetDummyFrameCount());
            GFXRECON_WRITE_CONSOLE("\tCaptured frames: %u", api_agnostic_stats.frame_count);
            GFXRECON_WRITE_CONSOLE("\tTotal frames: %u",
                                   dx12_consumer.GetDummyFrameCount() + api_agnostic_stats.frame_count);

            GFXRECON_WRITE_CONSOLE("\tApplication frame range: %u-%u",
                                   api_agnostic_stats.trim_start_frame,
                                   api_agnostic_stats.trim_start_frame + api_agnostic_stats.frame_count - 1);
        }

        PrintDriverInfo(info_consumer);

        PrintDx12RuntimeInfo(dx12_consumer);

        PrintDx12AdapterInfo(dx12_consumer);

        PrintDx12SwapchainInfo(dx12_consumer);

        PrintDxrEiInfo(dx12_consumer);
    }
    else if (api_agnostic_stats.error_state != gfxrecon::decode::FileProcessor::kErrorNone)
    {
        GFXRECON_WRITE_CONSOLE("A failure has occurred during file processing");
        gfxrecon::util::Log::Release();
        exit(-1);
    }
    else
    {
        GFXRECON_WRITE_CONSOLE("File did not contain any frames");
    }
}

static bool CheckOptionEnumGpuIndices(const char* exe_name, const gfxrecon::util::ArgumentParser& arg_parser)
{
    if (arg_parser.IsOptionSet(kEnumGpuIndices))
    {
        IDXGIFactory1* factory1 = nullptr;

        HRESULT result = CreateDXGIFactory1(IID_IDXGIFactory1, reinterpret_cast<void**>(&factory1));

        if (SUCCEEDED(result))
        {
            gfxrecon::graphics::dx12::ActiveAdapterMap adapters{};
            gfxrecon::graphics::dx12::TrackAdapters(result, reinterpret_cast<void**>(&factory1), adapters);

            GFXRECON_WRITE_CONSOLE("GPU index\tGPU name");
            for (size_t index = 0; index < adapters.size(); ++index)
            {
                for (auto adapter : adapters)
                {
                    if (index == adapter.second.adapter_idx)
                    {
                        std::string replay_adapter_str =
                            gfxrecon::util::WCharArrayToString(adapter.second.internal_desc.Description);

                        GFXRECON_WRITE_CONSOLE("%-9x\t%s", adapter.second.adapter_idx, replay_adapter_str.c_str());
                        adapter.second.adapter->Release();
                        break;
                    }
                }
            }
            factory1->Release();
        }
        else
        {
            GFXRECON_LOG_ERROR("Failed to enumerate GPU indices");
        }

        return true;
    }

    return false;
}
#endif

void GatherD3D12Stats(const std::string& input_filename)
{
#if defined(D3D12_SUPPORT)
    gfxrecon::decode::FileProcessor file_processor;

    if (file_processor.Initialize(input_filename))
    {
        gfxrecon::decode::InfoConsumer info_consumer;
        gfxrecon::decode::InfoDecoder  driver_info_decoder;
        driver_info_decoder.AddConsumer(&info_consumer);
        file_processor.AddDecoder(&driver_info_decoder);

        gfxrecon::decode::Dx12StatsConsumer dx12_consumer;
        gfxrecon::decode::StatDecoderBase   stat_decoder;
        gfxrecon::decode::StatConsumer      stat_consumer;
        gfxrecon::decode::Dx12Decoder       dx12_decoder;

        stat_decoder.AddConsumer(&stat_consumer);
        file_processor.AddDecoder(&stat_decoder);
        dx12_decoder.AddConsumer(&dx12_consumer);
        file_processor.AddDecoder(&dx12_decoder);

        file_processor.ProcessAllFrames();
        if (file_processor.GetErrorState() == gfxrecon::decode::FileProcessor::kErrorNone)
        {
            ApiAgnosticStats api_agnostic_stats = {};
            GatherApiAgnosticStats(api_agnostic_stats, file_processor, stat_consumer);

            PrintExeInfo(info_consumer);
            PrintD3D12Stats(dx12_consumer, api_agnostic_stats, info_consumer);
        }
        else
        {
            GFXRECON_WRITE_CONSOLE("Encountered error while reading capture. Stats unavailable.");
        }
    }
#endif
}

int main(int argc, const char** argv)
{
    gfxrecon::util::Log::Init();

    gfxrecon::util::ArgumentParser arg_parser(argc, argv, kOptions, "");

    if (CheckOptionPrintUsage(argv[0], arg_parser) || CheckOptionPrintVersion(argv[0], arg_parser))
    {
        gfxrecon::util::Log::Release();
        exit(0);
    }
#if defined(D3D12_SUPPORT)
    else if (CheckOptionEnumGpuIndices(argv[0], arg_parser))
    {
        gfxrecon::util::Log::Release();
        exit(0);
    }
#endif
    else if (arg_parser.IsInvalid() || (arg_parser.GetPositionalArgumentsCount() != 1))
    {
        PrintUsage(argv[0]);
        gfxrecon::util::Log::Release();
        exit(-1);
    }
    else
    {
#if defined(WIN32) && defined(_DEBUG)
        if (arg_parser.IsOptionSet(kNoDebugPopup))
        {
            _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        }
#endif
    }

    const std::vector<std::string>& positional_arguments = arg_parser.GetPositionalArguments();
    std::string                     input_filename       = positional_arguments[0];

    bool exe_info_only = arg_parser.IsOptionSet(kExeInfoOnlyOption);

    if (exe_info_only == true)
    {
        gfxrecon::decode::InfoConsumer info_consumer(true);
        GatherExeInfo(input_filename, info_consumer);
        PrintExeInfo(info_consumer);
    }
    else
    {
        bool detected_d3d12  = false;
        bool detected_vulkan = false;
        if (gfxrecon::decode::DetectAPIs(input_filename, detected_d3d12, detected_vulkan))
        {
            if (detected_d3d12)
            {
                GatherD3D12Stats(input_filename);
            }
            if (detected_vulkan)
            {
                GatherVulkanStats(input_filename);
            }
        }
    }

    gfxrecon::util::Log::Release();
    return 0;
}
