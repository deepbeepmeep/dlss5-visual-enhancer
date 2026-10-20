// WanGP Linux Frame Generation worker. Uses the public NVIDIA NGX Vulkan API.
// The stdin/stdout protocol is shared with FrameGenerationSession in runtime.py.
// Copyright (c) 2026 DeepBeepMeep contributors. SPDX-License-Identifier: MIT
// Ported from WanGP's experimental native/dlss5/dlssg_worker_vk.cpp.
#include <vulkan/vulkan.h>
#include <nvsdk_ngx_helpers_dlssg_vk.h>
#include <algorithm>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr const char* project_id = "794ac426-49d0-4ea0-b97b-619b1c96d337";
constexpr uint32_t setup_magic = 0x31534746, setup_reply = 0x31524746;
constexpr uint32_t frame_magic = 0x31464746, frame_reply = 0x314f4746;

void vk_check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + ": Vulkan error " + std::to_string(result));
}
void ngx_check(NVSDK_NGX_Result result, const char* operation) {
    if (NVSDK_NGX_FAILED(result)) {
        char code[32];
        std::snprintf(code, sizeof(code), "0x%08x", static_cast<unsigned>(result));
        throw std::runtime_error(std::string(operation) + ": NGX error " + code);
    }
}
void read_exact(void* data, size_t bytes) {
    size_t received = std::fread(data, 1, bytes, stdin);
    if (received != bytes) throw std::runtime_error("Truncated worker input");
}
void write_exact(const void* data, size_t bytes) {
    if (std::fwrite(data, 1, bytes, stdout) != bytes) throw std::runtime_error("Worker output pipe closed");
}
void reply(uint32_t magic, uint32_t status, uint32_t count, uint32_t disabled) {
    const uint32_t words[] = {magic, status, count, disabled};
    write_exact(words, sizeof(words));
}
std::string json_string(const std::string& value) {
    std::string result = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') result += '\\';
        if (c < 32) {
            char escaped[7];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
            result += escaped;
        } else result += c;
    }
    return result + '"';
}

struct Buffer {
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
};
struct Image {
    VkImage handle = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFormat format{};
    NVSDK_NGX_Resource_VK resource{};
};

class Session {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    NVSDK_NGX_Parameter* parameters = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    bool ngx_initialized = false;
    Image color, motion, depth, output;
    Buffer upload, readback, disable;
    uint32_t width = 0, height = 0;
    size_t plane_bytes = 0;

    uint32_t memory_type(uint32_t mask, VkMemoryPropertyFlags flags) {
        VkPhysicalDeviceMemoryProperties properties;
        vkGetPhysicalDeviceMemoryProperties(physical, &properties);
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
            if ((mask & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags) return i;
        throw std::runtime_error("Required Vulkan memory type unavailable");
    }
    void create_buffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage, bool host) {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = usage;
        vk_check(vkCreateBuffer(device, &info, nullptr, &b.handle), "Create buffer");
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device, b.handle, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type(requirements.memoryTypeBits, host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vk_check(vkAllocateMemory(device, &allocation, nullptr, &b.memory), "Allocate buffer memory");
        vk_check(vkBindBufferMemory(device, b.handle, b.memory, 0), "Bind buffer memory");
        if (host) vk_check(vkMapMemory(device, b.memory, 0, size, 0, &b.mapped), "Map buffer");
    }
    void create_image(Image& image, VkFormat format, bool writable) {
        image.format = format;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {width, height, 1};
        info.mipLevels = info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (writable) info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        vk_check(vkCreateImage(device, &info, nullptr, &image.handle), "Create image");
        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(device, image.handle, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vk_check(vkAllocateMemory(device, &allocation, nullptr, &image.memory), "Allocate image memory");
        vk_check(vkBindImageMemory(device, image.handle, image.memory, 0), "Bind image memory");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image.handle;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk_check(vkCreateImageView(device, &view, nullptr, &image.view), "Create image view");
        image.resource.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
        image.resource.ReadWrite = writable;
        image.resource.Resource.ImageViewInfo = {image.view, image.handle, view.subresourceRange, format, width, height};
    }
    void begin() {
        vk_check(vkResetCommandPool(device, pool, 0), "Reset command pool");
        VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk_check(vkBeginCommandBuffer(command, &info), "Begin commands");
    }
    void submit() {
        vk_check(vkEndCommandBuffer(command), "End commands");
        vk_check(vkResetFences(device, 1, &fence), "Reset fence");
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        info.commandBufferCount = 1;
        info.pCommandBuffers = &command;
        vk_check(vkQueueSubmit(queue, 1, &info, fence), "Submit commands");
        vk_check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "Wait for commands");
    }
    void transition(Image& image, VkImageLayout layout) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = image.layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.oldLayout = image.layout;
        barrier.newLayout = layout;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image.handle;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        image.layout = layout;
    }
    void memory_barrier(VkAccessFlags source, VkAccessFlags destination, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = source;
        barrier.dstAccessMask = destination;
        vkCmdPipelineBarrier(command, src_stage, dst_stage, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
    VkBufferImageCopy region(size_t offset = 0) const {
        VkBufferImageCopy copy{};
        copy.bufferOffset = offset;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        return copy;
    }
    void upload_image(Image& image, size_t offset) {
        transition(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        auto copy = region(offset);
        vkCmdCopyBufferToImage(command, upload.handle, image.handle, image.layout, 1, &copy);
        transition(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    static void ngx_log(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
        std::fprintf(stderr, "%s\n", message);
    }

public:
    std::string gpu;
    Session() = default;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session() {
        if (device) vkDeviceWaitIdle(device);
        if (feature) NVSDK_NGX_VULKAN_ReleaseFeature(feature);
        if (parameters) NVSDK_NGX_VULKAN_DestroyParameters(parameters);
        if (ngx_initialized) NVSDK_NGX_VULKAN_Shutdown1(device);
        for (auto* image : {&color, &motion, &depth, &output}) {
            if (image->view) vkDestroyImageView(device, image->view, nullptr);
            if (image->handle) vkDestroyImage(device, image->handle, nullptr);
            if (image->memory) vkFreeMemory(device, image->memory, nullptr);
        }
        for (auto* buffer : {&upload, &readback, &disable}) {
            if (buffer->mapped) vkUnmapMemory(device, buffer->memory);
            if (buffer->handle) vkDestroyBuffer(device, buffer->handle, nullptr);
            if (buffer->memory) vkFreeMemory(device, buffer->memory, nullptr);
        }
        if (fence) vkDestroyFence(device, fence, nullptr);
        if (pool) vkDestroyCommandPool(device, pool, nullptr);
        if (device) vkDestroyDevice(device, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
    }
    void initialize(bool self_test = false) {
        auto directory = std::filesystem::canonical("/proc/self/exe").parent_path().wstring();
        const wchar_t* paths[] = {directory.c_str()};
        NVSDK_NGX_FeatureCommonInfo common{};
        common.PathListInfo = {paths, 1};
        common.LoggingInfo = {ngx_log, NVSDK_NGX_LOGGING_LEVEL_ON, true};
        NVSDK_NGX_FeatureDiscoveryInfo discovery{};
        discovery.SDKVersion = NVSDK_NGX_Version_API;
        discovery.FeatureID = NVSDK_NGX_Feature_FrameGeneration;
        discovery.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
        discovery.Identifier.v.ProjectDesc = {project_id, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0"};
        discovery.ApplicationDataPath = directory.c_str();
        discovery.FeatureInfo = &common;
        uint32_t count = 0;
        VkExtensionProperties* required = nullptr;
        std::vector<const char*> extensions;
        if (!self_test) {
            ngx_check(NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&discovery, &count, &required), "NGX instance extensions");
            for (uint32_t i = 0; i < count; ++i) extensions.push_back(required[i].extensionName);
        }
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "WanGP Frame Generation";
        app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo = &app;
        info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        info.ppEnabledExtensionNames = extensions.data();
        vk_check(vkCreateInstance(&info, nullptr, &instance), "Create Vulkan instance");
        vk_check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "Enumerate GPUs");
        std::vector<VkPhysicalDevice> devices(count);
        vk_check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "Enumerate GPUs");
        for (auto candidate : devices) {
            VkPhysicalDeviceProperties properties;
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (!self_test && properties.vendorID != 0x10de) continue;
            if (properties.apiVersion < VK_API_VERSION_1_2) continue;
            if (!self_test) {
                NVSDK_NGX_FeatureRequirement requirement{};
                auto result = NVSDK_NGX_VULKAN_GetFeatureRequirements(instance, candidate, &discovery, &requirement);
                if (NVSDK_NGX_FAILED(result) || requirement.FeatureSupported != 0) {
                    std::fprintf(stderr, "%s: NGX requirements result 0x%x, support flags %u\n", properties.deviceName, result, requirement.FeatureSupported);
                    continue;
                }
            }
            physical = candidate;
            gpu = properties.deviceName;
            break;
        }
        if (!physical) throw std::runtime_error("No compatible NVIDIA Vulkan GPU found; check the NVIDIA driver and Vulkan ICD (CUDA availability alone is insufficient)");
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        uint32_t family = 0;
        for (; family < count; ++family)
            if ((families[family].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) break;
        if (family == count) throw std::runtime_error("No Vulkan graphics/compute queue available");
        extensions.clear();
        if (!self_test) {
            ngx_check(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(instance, physical, &discovery, &count, &required), "NGX device extensions");
            for (uint32_t i = 0; i < count; ++i) extensions.push_back(required[i].extensionName);
        }
        VkPhysicalDeviceOpticalFlowFeaturesNV optical{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV};
        VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan11Features features11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &features11;
        features11.pNext = &features12;
        for (const char* name : extensions)
            if (std::strcmp(name, VK_NV_OPTICAL_FLOW_EXTENSION_NAME) == 0) features12.pNext = &optical;
        vkGetPhysicalDeviceFeatures2(physical, &features);
        // Enable supported core features: NGX creates its own compute pipelines.
        float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device_info.pNext = &features;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        device_info.ppEnabledExtensionNames = extensions.data();
        vk_check(vkCreateDevice(physical, &device_info, nullptr, &device), "Create Vulkan device");
        vkGetDeviceQueue(device, family, 0, &queue);
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.queueFamilyIndex = family;
        vk_check(vkCreateCommandPool(device, &pool_info, nullptr, &pool), "Create command pool");
        VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        command_info.commandPool = pool;
        command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_info.commandBufferCount = 1;
        vk_check(vkAllocateCommandBuffers(device, &command_info, &command), "Allocate commands");
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vk_check(vkCreateFence(device, &fence_info, nullptr, &fence), "Create fence");
        if (self_test) return;
        ngx_check(NVSDK_NGX_VULKAN_Init_with_ProjectID(project_id, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", directory.c_str(), instance, physical, device, vkGetInstanceProcAddr, vkGetDeviceProcAddr, &common), "Initialize NGX");
        ngx_initialized = true;
        ngx_check(NVSDK_NGX_VULKAN_GetCapabilityParameters(&parameters), "Query NGX capabilities");
        int available = 0;
        ngx_check(parameters->Get(NVSDK_NGX_Parameter_FrameGeneration_Available, &available), "Query Frame Generation support");
        if (!available) throw std::runtime_error("NGX reports Frame Generation unavailable; check GPU and NVIDIA driver support");
    }
    void configure(uint32_t w, uint32_t h, bool self_test = false) {
        width = w; height = h;
        plane_bytes = size_t(w) * h * 4;
        create_buffer(upload, plane_bytes * 3, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        create_buffer(readback, plane_bytes + 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        create_buffer(disable, 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
        create_image(color, VK_FORMAT_R8G8B8A8_UNORM, false);
        create_image(motion, VK_FORMAT_R16G16_SFLOAT, false);
        create_image(depth, VK_FORMAT_R32_SFLOAT, false);
        create_image(output, VK_FORMAT_R8G8B8A8_UNORM, true);
        auto* depth_values = reinterpret_cast<float*>(static_cast<uint8_t*>(upload.mapped) + plane_bytes * 2);
        std::fill(depth_values, depth_values + size_t(w) * h, 0.5f);
        begin();
        upload_image(depth, plane_bytes * 2);
        transition(output, VK_IMAGE_LAYOUT_GENERAL);
        if (!self_test) {
            NVSDK_NGX_DLSSG_Create_Params create{};
            create.Width = create.RenderWidth = w;
            create.Height = create.RenderHeight = h;
            create.NativeBackbufferFormat = VK_FORMAT_R8G8B8A8_UNORM;
            parameters->Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, 1u);
            ngx_check(NGX_VK_CREATE_DLSSG(command, 1, 1, &feature, parameters, &create), "Create Frame Generation feature");
        }
        submit();
    }
    bool evaluate(uint32_t index, bool reset) {
        read_exact(upload.mapped, plane_bytes * 2); // RGBA8 followed by pixel-space RG16F backward flow.
        begin();
        upload_image(color, 0);
        upload_image(motion, plane_bytes);
        transition(output, VK_IMAGE_LAYOUT_GENERAL);
        memory_barrier(VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdFillBuffer(command, disable.handle, 0, 4, 0);
        memory_barrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        NVSDK_NGX_Resource_VK disable_resource{};
        disable_resource.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_BUFFER;
        disable_resource.ReadWrite = true;
        disable_resource.Resource.BufferInfo = {disable.handle, 4};
        NVSDK_NGX_VK_DLSSG_Eval_Params eval{};
        eval.pBackbuffer = eval.pHudless = &color.resource;
        eval.pMVecs = &motion.resource;
        eval.pDepth = &depth.resource;
        eval.pOutputInterpFrame = &output.resource;
        eval.pOutputDisableInterpolation = &disable_resource;
        NVSDK_NGX_DLSSG_Opt_Eval_Params options{};
        options.multiFrameCount = options.multiFrameIndex = 1; // x2 only.
        for (int i = 0; i < 4; ++i) {
            options.cameraViewToClip[i][i] = options.clipToCameraView[i][i] = 1;
            options.clipToLensClip[i][i] = options.clipToPrevClip[i][i] = options.prevClipToClip[i][i] = 1;
        }
        options.mvecScale[0] = 1.0f / width;
        options.mvecScale[1] = 1.0f / height;
        options.cameraUp[1] = options.cameraRight[0] = options.cameraFwd[2] = 1;
        options.cameraNear = 0.1f; options.cameraFar = 1000.0f;
        options.cameraFOV = 1.04719755f;
        options.cameraAspectRatio = float(width) / height;
        options.cameraMotionIncluded = true;
        options.orthoProjection = true; // Recorded-video screen space; no game camera projection.
        options.motionVectorsDilated = true;
        options.motionVectorsInvalidValue = 65504.0f;
        options.reset = reset || index == 0;
        options.mvecsSubrectSize = options.depthSubrectSize = options.hudLessSubrectSize = {width, height};
        options.backbufferSubrectSize = options.outputInterpSubrectSize = {width, height};
        parameters->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, static_cast<unsigned long long>(index));
        ngx_check(NGX_VK_EVALUATE_DLSSG(command, feature, parameters, &eval, &options), "Evaluate Frame Generation");
        transition(output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        auto copy = region();
        vkCmdCopyImageToBuffer(command, output.handle, output.layout, readback.handle, 1, &copy);
        memory_barrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferCopy disabled_copy{0, plane_bytes, 4};
        vkCmdCopyBuffer(command, disable.handle, readback.handle, 1, &disabled_copy);
        memory_barrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT);
        submit();
        return !options.reset && static_cast<uint8_t*>(readback.mapped)[plane_bytes] == 0;
    }
    void write_frame() const { write_exact(readback.mapped, plane_bytes); }
    void transfer_test() {
        // Explicit diagnostic only: never used as an interpolation fallback.
        configure(64, 48, true);
        auto* input = static_cast<uint8_t*>(upload.mapped);
        for (unsigned pass = 0; pass < 3; ++pass) {
            size_t offset = 0;
            for (auto* image : {&color, &motion, &depth}) {
                for (size_t i = 0; i < plane_bytes; ++i) input[offset + i] = static_cast<uint8_t>(i * 37 + pass * 13);
                begin();
                upload_image(*image, offset);
                transition(*image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                auto copy = region();
                vkCmdCopyImageToBuffer(command, image->handle, image->layout, readback.handle, 1, &copy);
                memory_barrier(VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                vkCmdFillBuffer(command, disable.handle, 0, 4, pass);
                memory_barrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkBufferCopy disabled_copy{0, plane_bytes, 4};
                vkCmdCopyBuffer(command, disable.handle, readback.handle, 1, &disabled_copy);
                memory_barrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT);
                submit();
                uint32_t disabled;
                std::memcpy(&disabled, static_cast<uint8_t*>(readback.mapped) + plane_bytes, sizeof(disabled));
                if (std::memcmp(input + offset, readback.mapped, plane_bytes) || disabled != pass)
                    throw std::runtime_error("Vulkan upload/readback mismatch");
                offset += plane_bytes;
            }
        }
    }
};

void serve() {
    uint32_t setup[5];
    bool setup_sent = false;
    try {
        read_exact(setup, sizeof(setup));
        if (setup[0] != setup_magic || setup[1] == 0 || setup[2] == 0 || setup[1] > 8192 || setup[2] > 8192 || uint64_t(setup[1]) * setup[2] > 7680ull * 4320 || setup[3] == 0 || setup[4] != 1)
            throw std::runtime_error("Invalid setup: Linux worker requires x2 and dimensions within a 7680x4320 pixel budget");
        Session session;
        session.initialize();
        session.configure(setup[1], setup[2]);
        reply(setup_reply, 0, 1, 0);
        if (std::fflush(stdout) != 0) throw std::runtime_error("Worker output pipe closed");
        setup_sent = true;
        for (uint32_t index = 0; index < setup[3]; ++index) {
            uint32_t frame[4];
            int64_t timestamp[2];
            read_exact(frame, sizeof(frame));
            read_exact(timestamp, sizeof(timestamp));
            if (frame[0] != frame_magic || frame[1] != index || frame[2] > 1 || frame[3] != 0 || timestamp[0] < 0 || timestamp[1] <= 0)
                throw std::runtime_error("Invalid frame header");
            bool generated = session.evaluate(index, frame[2] != 0);
            reply(frame_reply, 0, generated ? 1 : 0, generated ? 0 : 1);
            if (generated) session.write_frame();
            if (std::fflush(stdout) != 0) throw std::runtime_error("Worker output pipe closed");
        }
    } catch (...) {
        reply(setup_sent ? frame_reply : setup_reply, 1, 0, 0);
        std::fflush(stdout);
        throw;
    }
}
} // namespace

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    bool probe = argc == 2 && std::strcmp(argv[1], "--probe") == 0;
    try {
        if (probe) {
            Session session;
            session.initialize();
            std::cout << "{\"available\":true,\"worker_version\":2,\"multi_frame_count_max\":1,\"gpu\":" << json_string(session.gpu) << "}\n";
        } else if (argc == 2 && std::strcmp(argv[1], "--serve") == 0) {
            serve();
        } else if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0) {
            Session session;
            session.initialize(true);
            session.transfer_test();
            std::cout << "Vulkan transfer self-test passed on " << session.gpu << "; DLSS was not evaluated.\n";
        } else throw std::runtime_error("Usage: dlssg-worker --probe | --serve | --self-test");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        if (probe) std::cout << "{\"available\":false,\"worker_version\":2,\"multi_frame_count_max\":1,\"reason\":" << json_string(error.what()) << "}\n";
        return 1;
    }
}
