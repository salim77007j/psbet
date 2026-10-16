// ============================================================================
//  PSBET — vulkan_device.cpp
//  Hardware GPU backend. See vulkan_device.h for design notes.
// ============================================================================
#include "gpu/vulkan_device.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <setjmp.h>
#include <vector>

#include "gpu/spirv_emit.h"
#include "gpu/shader_cache.h"
#include "core/log.h"

#if !defined(_WIN32)
#include <csignal>
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define VK_NO_PROTOTYPES
#include "gpu/vulkan/vulkan_core.h"

namespace psb::gpu {

namespace {

// ---------------------------------------------------------------------------
// Loader
// ---------------------------------------------------------------------------
#define VK_LIST_GLOBAL(F) \
    F(vkCreateInstance) \
    F(vkEnumerateInstanceVersion) \
    F(vkEnumerateInstanceExtensionProperties) \
    F(vkEnumerateInstanceLayerProperties)

#define VK_LIST_INSTANCE(F) \
    F(vkDestroyInstance) \
    F(vkEnumeratePhysicalDevices) \
    F(vkGetPhysicalDeviceProperties) \
    F(vkGetPhysicalDeviceMemoryProperties) \
    F(vkGetPhysicalDeviceQueueFamilyProperties) \
    F(vkCreateDevice) \
    F(vkGetDeviceProcAddr) \
    F(vkGetPhysicalDeviceFormatProperties)

#define VK_LIST_DEVICE(F) \
    F(vkDestroyDevice) \
    F(vkGetDeviceQueue) \
    F(vkCreateCommandPool) \
    F(vkDestroyCommandPool) \
    F(vkResetCommandPool) \
    F(vkAllocateCommandBuffers) \
    F(vkFreeCommandBuffers) \
    F(vkBeginCommandBuffer) \
    F(vkEndCommandBuffer) \
    F(vkResetCommandBuffer) \
    F(vkCmdBeginRenderPass) \
    F(vkCmdEndRenderPass) \
    F(vkCmdSetViewport) \
    F(vkCmdSetScissor) \
    F(vkCmdBindPipeline) \
    F(vkCmdBindVertexBuffers) \
    F(vkCmdDraw) \
    F(vkCmdCopyImage) \
    F(vkCmdPipelineBarrier) \
    F(vkCreateBuffer) \
    F(vkDestroyBuffer) \
    F(vkAllocateMemory) \
    F(vkFreeMemory) \
    F(vkMapMemory) \
    F(vkUnmapMemory) \
    F(vkBindBufferMemory) \
    F(vkBindImageMemory) \
    F(vkCreateImageView) \
    F(vkDestroyImageView) \
    F(vkCreateFramebuffer) \
    F(vkDestroyFramebuffer) \
    F(vkCreateImage) \
    F(vkDestroyImage) \
    F(vkGetBufferMemoryRequirements) \
    F(vkGetImageMemoryRequirements) \
    F(vkCreateRenderPass) \
    F(vkDestroyRenderPass) \
    F(vkCreatePipelineLayout) \
    F(vkDestroyPipelineLayout) \
    F(vkCreateGraphicsPipelines) \
    F(vkDestroyPipeline) \
    F(vkCreateShaderModule) \
    F(vkDestroyShaderModule) \
    F(vkCreateFence) \
    F(vkDestroyFence) \
    F(vkWaitForFences) \
    F(vkResetFences) \
    F(vkQueueSubmit) \
    F(vkQueueWaitIdle) \
    F(vkDeviceWaitIdle) \
    F(vkCreatePipelineCache) \
    F(vkDestroyPipelineCache) \
    F(vkGetPipelineCacheData)

struct Api {
#define VK_DECL(fn) PFN_##fn fn = nullptr;
    VK_LIST_GLOBAL(VK_DECL)
    VK_LIST_INSTANCE(VK_DECL)
    VK_LIST_DEVICE(VK_DECL)
#undef VK_DECL

    bool loadGlobal(void* lib) {
#if defined(_WIN32)
        auto sym = reinterpret_cast<PFN_vkVoidFunction>(GetProcAddress(
            static_cast<HMODULE>(lib), "vkGetInstanceProcAddr"));
#else
        auto sym = reinterpret_cast<PFN_vkVoidFunction>(dlsym(lib, "vkGetInstanceProcAddr"));
#endif
        if (!sym) return false;
        auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(sym);
#define VK_LOAD(fn) fn = reinterpret_cast<PFN_##fn>(gipa(nullptr, #fn));
        VK_LIST_GLOBAL(VK_LOAD)
#undef VK_LOAD
        return vkCreateInstance != nullptr;
    }

    bool loadInstance(VkInstance inst, PFN_vkGetInstanceProcAddr gipa) {
#define VK_LOAD(fn) fn = reinterpret_cast<PFN_##fn>(gipa(inst, #fn));
        VK_LIST_INSTANCE(VK_LOAD)
#undef VK_LOAD
        return vkCreateDevice != nullptr && vkEnumeratePhysicalDevices != nullptr;
    }

    bool loadDevice(VkDevice dev, PFN_vkGetDeviceProcAddr gdpa) {
#define VK_LOAD(fn) fn = reinterpret_cast<PFN_##fn>(gdpa(dev, #fn));
        VK_LIST_DEVICE(VK_LOAD)
#undef VK_LOAD
        return vkQueueSubmit != nullptr && vkCreateGraphicsPipelines != nullptr;
    }
};

Api g_vk;

void* openLoader() {
#if defined(_WIN32)
    return static_cast<void*>(LoadLibraryW(L"vulkan-1.dll"));
#else
    if (void* p = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL)) return p;
    return dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
#endif
}

const char* vkResultStr(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "DEVICE_LOST";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "FEATURE_NOT_PRESENT";
        case VK_ERROR_OUT_OF_DATE_KHR: return "OUT_OF_DATE";
        default: return "VK_ERROR";
    }
}

#define VK_CHECK(call)                                                        \
    do {                                                                      \
        const VkResult _r = (call);                                           \
        if (_r != VK_SUCCESS) {                                               \
            LOG_ERROR("Vulkan: %s failed (%s) at %s:%d", #call,               \
                      vkResultStr(_r), __FILE__, __LINE__);                   \
            return false;                                                     \
        }                                                                     \
    } while (0)

// Step tracing (PSBET_VK_TRACE=1): pinpoints the exact driver call around a
// fault. Cheap: one cached getenv, fprintf only when enabled.
bool vkTrace() {
    static const bool on = [] {
        const char* e = std::getenv("PSBET_VK_TRACE");
        return e && e[0] == '1';
    }();
    return on;
}
#define VK_TRACE(msg)                                                         \
    do {                                                                      \
        if (vkTrace()) std::fprintf(stderr, "[vk-trace] %s\n", msg);           \
    } while (0)

constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;

VkCompareOp depthFuncToVk(DepthFunc f) {
    switch (f) {
        case DepthFunc::Never: return VK_COMPARE_OP_NEVER;
        case DepthFunc::Less:  return VK_COMPARE_OP_LESS;
        case DepthFunc::Equal: return VK_COMPARE_OP_EQUAL;
        case DepthFunc::LEqual:return VK_COMPARE_OP_LESS_OR_EQUAL;
    }
    return VK_COMPARE_OP_LESS;
}

VkCullModeFlags cullModeToVk(CullMode m) {
    return m == CullMode::Back ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
}

u32 pipelineKey(bool depthEnable, DepthFunc df, CullMode cm) {
    return (u32(depthEnable) << 16) | (u32(df) << 8) | u32(cm);
}

struct MemTypeInfo {
    i32 hostVisible = -1;    // HOST_VISIBLE | HOST_COHERENT
    i32 deviceLocal = -1;    // DEVICE_LOCAL (or any) for images
};

} // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------
struct VulkanDevice::Impl {
    // Loader
    void* lib = nullptr;
    PFN_vkGetInstanceProcAddr gipa = nullptr;

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties physProps{};
    VkPhysicalDeviceMemoryProperties memProps{};
    u32 gfxFamily = 0;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    MemTypeInfo mem;

    // Resources
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkPipelineCache pipeCache = VK_NULL_HANDLE;

    VkImage colorImage = VK_NULL_HANDLE, readbackImage = VK_NULL_HANDLE;
    VkDeviceMemory colorMem = VK_NULL_HANDLE, readbackMem = VK_NULL_HANDLE;
    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMem = VK_NULL_HANDLE;
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;

    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;

    VkImageView colorView = VK_NULL_HANDLE, depthView = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;

    // Vertex buffer (orbit allocator, host-visible)
    VkBuffer vbuf = VK_NULL_HANDLE;
    VkDeviceMemory vbufMem = VK_NULL_HANDLE;
    u8* vbufMapped = nullptr;
    VkDeviceSize vbufSize = 0;
    VkDeviceSize vbufUsed = 0;

    u8* readbackMapped = nullptr;
    u8* colorOut = nullptr;
    u32 width = 0, height = 0;

    // Recorded state
    float clear_[4] = {0.f, 0.f, 0.f, 1.f};
    float vp_[4] = {0, 0, 0, 0};   // x y w h
    bool  depthEnable_ = false;
    DepthFunc depthFunc_ = DepthFunc::Less;
    CullMode cull_ = CullMode::None;

    bool  passBegun = false;
    bool  cmdbufOpen = false;
    bool  failed = false;
    std::map<u32, VkPipeline> pipelines;

    // ---- helpers ----
    bool allocMemory(VkDeviceMemory* out, u32 typeIndex, VkDeviceSize size);
    bool createImages();
    bool createVertexBuffer(VkDeviceSize size);
    bool createRenderPass();
    bool createShaderModules();
    VkPipeline getPipeline(bool depthEnable, DepthFunc df, CullMode cm);
    bool beginFrame();
    void beginPass(VkCommandBuffer cb);
    void endFrame();
    bool submitAndWait(VkCommandBuffer cb);
    void destroy();
};

bool VulkanDevice::Impl::allocMemory(VkDeviceMemory* out, u32 typeIndex, VkDeviceSize size) {
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = size;
    ai.memoryTypeIndex = typeIndex;
    return g_vk.vkAllocateMemory(device, &ai, nullptr, out) == VK_SUCCESS;
}

bool VulkanDevice::Impl::createImages() {
    // ---- color attachment ----
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = kColorFormat;
    ii.extent = {width, height, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(g_vk.vkCreateImage(device, &ii, nullptr, &colorImage));

    VkMemoryRequirements mr{};
    g_vk.vkGetImageMemoryRequirements(device, colorImage, &mr);
    u32 type = 0;
    bool found = false;
    for (u32 i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((mr.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            type = i; found = true; break;
        }
    }
    if (!found) {
        for (u32 i = 0; i < memProps.memoryTypeCount; ++i) {
            if (mr.memoryTypeBits & (1u << i)) { type = i; found = true; break; }
        }
    }
    if (!found) return false;
    if (!allocMemory(&colorMem, type, mr.size)) return false;
    VK_CHECK(g_vk.vkBindImageMemory(device, colorImage, colorMem, 0));

    // ---- depth attachment ----
    static const VkFormat depthCandidates[] = {
        VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D16_UNORM };
    depthFormat = VK_FORMAT_UNDEFINED;
    for (VkFormat f : depthCandidates) {
        VkFormatProperties fp{};
        g_vk.vkGetPhysicalDeviceFormatProperties(phys, f, &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            depthFormat = f;
            break;
        }
    }
    if (depthFormat == VK_FORMAT_UNDEFINED) return false;

    ii.format = depthFormat;
    ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VK_CHECK(g_vk.vkCreateImage(device, &ii, nullptr, &depthImage));
    g_vk.vkGetImageMemoryRequirements(device, depthImage, &mr);
    found = false;
    for (u32 i = 0; i < memProps.memoryTypeCount; ++i) {
        if (mr.memoryTypeBits & (1u << i)) { type = i; found = true; break; }
    }
    if (!found) return false;
    if (!allocMemory(&depthMem, type, mr.size)) return false;
    VK_CHECK(g_vk.vkBindImageMemory(device, depthImage, depthMem, 0));

    // ---- readback image (linear, host visible) ----
    ii.format = kColorFormat;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.tiling = VK_IMAGE_TILING_LINEAR;
    VK_CHECK(g_vk.vkCreateImage(device, &ii, nullptr, &readbackImage));
    g_vk.vkGetImageMemoryRequirements(device, readbackImage, &mr);
    found = false;
    for (u32 i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((mr.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            type = i; found = true; break;
        }
    }
    if (!found) return false;
    if (!allocMemory(&readbackMem, type, mr.size)) return false;
    VK_CHECK(g_vk.vkBindImageMemory(device, readbackImage, readbackMem, 0));
    if (g_vk.vkMapMemory(device, readbackMem, 0, VK_WHOLE_SIZE, 0,
                         reinterpret_cast<void**>(&readbackMapped)) != VK_SUCCESS)
        return false;

    // ---- views + framebuffer ----
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = kColorFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vi.image = colorImage;
    VK_CHECK(g_vk.vkCreateImageView(device, &vi, nullptr, &colorView));

    vi.format = depthFormat;
    vi.subresourceRange = {
        static_cast<VkImageAspectFlags>(
            (depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT)
                ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                : VK_IMAGE_ASPECT_DEPTH_BIT),
        0, 1, 0, 1};
    vi.image = depthImage;
    VK_CHECK(g_vk.vkCreateImageView(device, &vi, nullptr, &depthView));

    VkImageView attachments[2] = {colorView, depthView};
    VkFramebufferCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fi.renderPass = renderPass;
    fi.attachmentCount = 2;
    fi.pAttachments = attachments;
    fi.width = width;
    fi.height = height;
    fi.layers = 1;
    if (renderPass == VK_NULL_HANDLE) return false; // created after renderPass
    VK_CHECK(g_vk.vkCreateFramebuffer(device, &fi, nullptr, &framebuffer));
    return true;
}

bool VulkanDevice::Impl::createVertexBuffer(VkDeviceSize size) {
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(g_vk.vkCreateBuffer(device, &bi, nullptr, &vbuf));

    VkMemoryRequirements mr{};
    g_vk.vkGetBufferMemoryRequirements(device, vbuf, &mr);
    u32 type = 0;
    bool found = false;
    for (u32 i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((mr.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            type = i; found = true; break;
        }
    }
    if (!found) return false;
    VkDeviceSize allocSize = std::max<VkDeviceSize>(mr.size, size);
    if (!allocMemory(&vbufMem, type, allocSize)) return false;
    VK_CHECK(g_vk.vkBindBufferMemory(device, vbuf, vbufMem, 0));
    if (g_vk.vkMapMemory(device, vbufMem, 0, VK_WHOLE_SIZE, 0,
                         reinterpret_cast<void**>(&vbufMapped)) != VK_SUCCESS)
        return false;
    vbufSize = size;
    vbufUsed = 0;
    return true;
}

bool VulkanDevice::Impl::createRenderPass() {
    VkAttachmentDescription att[2]{};
    att[0].format = kColorFormat;
    att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    att[1].format = depthFormat;
    att[1].samples = VK_SAMPLE_COUNT_1_BIT;
    att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &cref;
    sp.pDepthStencilAttachment = &dref;

    // External dependency: make render results visible to the transfer that
    // copies the color image to the readback image.
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                     | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
                     | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                     | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
                     | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                      | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                      | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    // Second dependency (renderpass -> transfer): render results visible to
    // the readback copy that runs after vkCmdEndRenderPass.
    VkSubpassDependency deps[2]{};
    deps[0] = dep;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
                         | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                          | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    VkRenderPassCreateInfo rpi{};
    rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpi.attachmentCount = 2;
    rpi.pAttachments = att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sp;
    rpi.dependencyCount = 2;
    rpi.pDependencies = deps;
    VK_CHECK(g_vk.vkCreateRenderPass(device, &rpi, nullptr, &renderPass));

    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VK_CHECK(g_vk.vkCreatePipelineLayout(device, &pli, nullptr, &pipeLayout));
    return true;
}

bool VulkanDevice::Impl::createShaderModules() {
    // Shader cache: env PSBET_SHADER_CACHE=<dir> enables persistence.
    spv::SpirvShaders sh = spv::buildPipelineShaders();

    static ShaderCache* cache = [] {
        const char* dir = std::getenv("PSBET_SHADER_CACHE");
        return new ShaderCache(dir ? dir : "");
    }();

    auto get = [&](const std::vector<spv::Word>& built, const char* domain) {
        const u64 key = ShaderCache::hashKey(domain, built.data(), built.size());
        if (const std::vector<spv::Word>* hit = cache->find(key)) return *hit;
        cache->insert(key, built);
        cache->flush();
        return built;
    };
    const std::vector<spv::Word> vsWords = get(sh.vertex, "psb-fixed-vs");
    const std::vector<spv::Word> fsWords = get(sh.fragment, "psb-fixed-fs");

    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = vsWords.size() * sizeof(spv::Word);
    ci.pCode = vsWords.data();
    VK_CHECK(g_vk.vkCreateShaderModule(device, &ci, nullptr, &vs));
    ci.codeSize = fsWords.size() * sizeof(spv::Word);
    ci.pCode = fsWords.data();
    VK_CHECK(g_vk.vkCreateShaderModule(device, &ci, nullptr, &fs));
    return true;
}

VkPipeline VulkanDevice::Impl::getPipeline(bool depthEnable, DepthFunc df, CullMode cm) {
    const u32 key = pipelineKey(depthEnable, df, cm);
    auto it = pipelines.find(key);
    if (it != pipelines.end()) return it->second;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "psb_vs";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "psb_fs";

    VkVertexInputBindingDescription bind{};
    bind.binding = 0;
    bind.stride = sizeof(GpuVertex);
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0].binding = 0;
    attrs[0].location = 0;
    attrs[0].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs[0].offset = offsetof(GpuVertex, pos);
    attrs[1].binding = 0;
    attrs[1].location = 1;
    attrs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs[1].offset = offsetof(GpuVertex, col);

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = cullModeToVk(cm);
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; // y-down framebuffer CCW = front
    rs.lineWidth = 1.0f;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = depthEnable ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = depthEnable ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = depthFuncToVk(df);
    ds.minDepthBounds = 0.0f;
    ds.maxDepthBounds = 1.0f;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                       | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkDynamicState dynStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dyn;
    pi.layout = pipeLayout;
    pi.renderPass = renderPass;
    pi.subpass = 0;

    VkPipeline pipe = VK_NULL_HANDLE;
    const VkResult r = g_vk.vkCreateGraphicsPipelines(device, pipeCache, 1, &pi, nullptr, &pipe);
    if (r != VK_SUCCESS) {
        LOG_ERROR("Vulkan: pipeline creation failed (VkResult %d, %s) key=%u",
                  static_cast<int>(r), vkResultStr(r), key);
        failed = true;
        return VK_NULL_HANDLE;
    }
    pipelines[key] = pipe;
    return pipe;
}

bool VulkanDevice::Impl::beginFrame() {
    if (cmdbufOpen) return true;
    VK_TRACE("beginFrame: reset pool");
    VK_CHECK(g_vk.vkResetCommandPool(device, cmdPool, 0));
    vbufUsed = 0;

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_TRACE("beginFrame: begin cmd");
    VK_CHECK(g_vk.vkBeginCommandBuffer(cmd, &bi));
    cmdbufOpen = true;
    return true;
}

void VulkanDevice::Impl::beginPass(VkCommandBuffer cb) {
    if (passBegun) return;
    VK_TRACE("beginPass: begin renderpass");

    VkClearValue cv[2]{};
    cv[0].color = {{clear_[0], clear_[1], clear_[2], clear_[3]}};
    cv[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    bi.renderPass = renderPass;
    bi.framebuffer = VK_NULL_HANDLE; // imageless not used; use framebufferless? No —
    // Vulkan requires framebuffers for render passes on 1.0. Create one.
    bi.renderArea.offset = {0, 0};
    bi.renderArea.extent = {width, height};
    bi.clearValueCount = 2;
    bi.pClearValues = cv;

    // One framebuffer (color+depth views) is created at init and reused.
    bi.framebuffer = framebuffer;

    g_vk.vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);

    const VkViewport vp{vp_[0], vp_[1], vp_[2], vp_[3], 0.0f, 1.0f};
    const VkRect2D sc{{0, 0}, {width, height}};
    g_vk.vkCmdSetViewport(cb, 0, 1, &vp);
    g_vk.vkCmdSetScissor(cb, 0, 1, &sc);

    passBegun = true;
    VK_TRACE("beginPass: done");
}

void VulkanDevice::Impl::endFrame() {
    if (!cmdbufOpen) return;
    if (passBegun) {
        VK_TRACE("endFrame: end renderpass");
        g_vk.vkCmdEndRenderPass(cmd);
    }
    VK_TRACE("endFrame: barrier");

    // Barrier: color image TRANSFER_SRC_OPTIMAL (renderpass finalLayout) —
    // readback image UNDEFINED -> GENERAL for the copy destination.
    VkImageMemoryBarrier rb{};
    rb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    rb.srcAccessMask = 0;
    rb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    rb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    rb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    rb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    rb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    rb.image = readbackImage;
    rb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    g_vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                              0, nullptr, 0, nullptr, 1, &rb);

    VK_TRACE("endFrame: copy");
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {width, height, 1};
    g_vk.vkCmdCopyImage(cmd, colorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        readbackImage, VK_IMAGE_LAYOUT_GENERAL, 1, &region);

    VK_TRACE("endFrame: end cmd");
    if (g_vk.vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        failed = true;
        cmdbufOpen = false;
        passBegun = false;
        return;
    }
    cmdbufOpen = false;
    passBegun = false;

    if (!submitAndWait(cmd)) failed = true;
    if (!failed && readbackMapped && colorOut)
        std::memcpy(colorOut, readbackMapped, static_cast<size_t>(width) * height * 4);
}

bool VulkanDevice::Impl::submitAndWait(VkCommandBuffer cb) {
    VK_TRACE("submitAndWait: queue submit");
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    VK_CHECK(g_vk.vkQueueSubmit(queue, 1, &si, fence));
    VK_CHECK(g_vk.vkWaitForFences(device, 1, &fence, VK_TRUE, 5ull * 1000ull * 1000ull * 1000ull));
    VK_CHECK(g_vk.vkResetFences(device, 1, &fence));
    return true;
}

void VulkanDevice::Impl::destroy() {
    if (!device) return;
    g_vk.vkDeviceWaitIdle(device);
    for (auto& kv : pipelines) g_vk.vkDestroyPipeline(device, kv.second, nullptr);
    pipelines.clear();
    if (pipeCache) g_vk.vkDestroyPipelineCache(device, pipeCache, nullptr);
    if (vs) g_vk.vkDestroyShaderModule(device, vs, nullptr);
    if (fs) g_vk.vkDestroyShaderModule(device, fs, nullptr);
    if (renderPass) g_vk.vkDestroyRenderPass(device, renderPass, nullptr);
    if (pipeLayout) g_vk.vkDestroyPipelineLayout(device, pipeLayout, nullptr);
    if (vbuf) g_vk.vkDestroyBuffer(device, vbuf, nullptr);
    if (vbufMem) g_vk.vkFreeMemory(device, vbufMem, nullptr);
    if (framebuffer) g_vk.vkDestroyFramebuffer(device, framebuffer, nullptr);
    if (colorView) g_vk.vkDestroyImageView(device, colorView, nullptr);
    if (depthView) g_vk.vkDestroyImageView(device, depthView, nullptr);
    if (colorImage) g_vk.vkDestroyImage(device, colorImage, nullptr);
    if (colorMem) g_vk.vkFreeMemory(device, colorMem, nullptr);
    if (depthImage) g_vk.vkDestroyImage(device, depthImage, nullptr);
    if (depthMem) g_vk.vkFreeMemory(device, depthMem, nullptr);
    if (readbackImage) g_vk.vkDestroyImage(device, readbackImage, nullptr);
    if (readbackMem) g_vk.vkFreeMemory(device, readbackMem, nullptr);
    if (cmdPool) g_vk.vkDestroyCommandPool(device, cmdPool, nullptr);
    if (fence) g_vk.vkDestroyFence(device, fence, nullptr);
    g_vk.vkDestroyDevice(device, nullptr);
    device = VK_NULL_HANDLE;
    if (instance && g_vk.vkDestroyInstance) g_vk.vkDestroyInstance(instance, nullptr);
    instance = VK_NULL_HANDLE;
#if defined(_WIN32)
    if (lib) FreeLibrary(static_cast<HMODULE>(lib));
#else
    if (lib) dlclose(lib);
#endif
    lib = nullptr;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

VulkanDevice::~VulkanDevice() {
    if (impl_) {
        impl_->destroy();
        delete impl_;
        impl_ = nullptr;
    }
}

void VulkanDevice::setViewport(float x, float y, float w, float h) {
    Impl* p = impl_;
    p->vp_[0] = x; p->vp_[1] = y; p->vp_[2] = w; p->vp_[3] = h;
}

void VulkanDevice::setDepthTest(bool enable, DepthFunc func) {
    impl_->depthEnable_ = enable;
    impl_->depthFunc_ = func;
}

void VulkanDevice::setCullMode(CullMode mode) { impl_->cull_ = mode; }

void VulkanDevice::clear(float r, float g, float b, float a) {
    Impl* p = impl_;
    p->clear_[0] = r; p->clear_[1] = g; p->clear_[2] = b; p->clear_[3] = a;
    // Clear is honored via loadOp at render-pass begin; if a pass is already
    // open (draws before clear), the clear would reset color+depth in the
    // software path — we flag the contract violation instead of silently
    // diverging.
    if (p->passBegun)
        LOG_WARN("Vulkan: CmdClear after draws in one submit — PBFF v1 expects "
                 "Clear before Draw (software would have reset the buffer)");
}

void VulkanDevice::drawTriangles(const GpuVertex* v, u32 count) {
    Impl* p = impl_;
    if (p->failed || count == 0) return;
    if (!p->beginFrame()) { p->failed = true; return; }

    const VkDeviceSize bytes = static_cast<VkDeviceSize>(count) * sizeof(GpuVertex);
    if (p->vbufUsed + bytes > p->vbufSize) {
        LOG_ERROR("Vulkan: vertex buffer exhausted (%llu/%llu bytes this frame)",
                  static_cast<unsigned long long>(p->vbufUsed),
                  static_cast<unsigned long long>(p->vbufSize));
        p->failed = true;
        return;
    }

    const VkDeviceSize offset = p->vbufUsed;
    std::memcpy(p->vbufMapped + offset, v, static_cast<size_t>(bytes));
    p->vbufUsed += bytes;

    p->beginPass(p->cmd);

    VK_TRACE("draw: getPipeline");
    const VkPipeline pipe = p->getPipeline(p->depthEnable_, p->depthFunc_, p->cull_);
    if (pipe == VK_NULL_HANDLE) return;
    VK_TRACE("draw: bindPipeline");
    g_vk.vkCmdBindPipeline(p->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);

    VK_TRACE("draw: bindVertexBuffers");
    VkDeviceSize off = offset;
    g_vk.vkCmdBindVertexBuffers(p->cmd, 0, 1, &p->vbuf, &off);
    VK_TRACE("draw: cmdDraw");
    g_vk.vkCmdDraw(p->cmd, count, 1, 0, 0);
    VK_TRACE("draw: done");
}

void VulkanDevice::frameEnd() {
    Impl* p = impl_;
    if (p->failed) return;
    p->endFrame(); // submits, waits, reads color back into colorOut
    if (p->failed) LOG_ERROR("Vulkan: frame submission failed — backend disabled");
}

bool VulkanDevice::available() {
    void* lib = openLoader();
    if (!lib) return false;
    bool ok = false;
#if defined(_WIN32)
    ok = GetProcAddress(static_cast<HMODULE>(lib), "vkGetInstanceProcAddr") != nullptr;
    FreeLibrary(static_cast<HMODULE>(lib));
#else
    ok = dlsym(lib, "vkGetInstanceProcAddr") != nullptr;
    dlclose(lib);
#endif
    return ok;
}

bool VulkanDevice::selfTest() {
#if !defined(_WIN32)
    // The self-test must survive a driver fault (e.g. a broken JIT segfaults
    // inside vkCreateGraphicsPipelines). Guard with a restore-on-exit sigjmp.
    static sigjmp_buf stEnv;
    struct sigaction sa{}, oldSegv{}, oldIll{}, oldFpe{}, oldBus{};
    sa.sa_sigaction = [](int, siginfo_t*, void*) { siglongjmp(stEnv, 1); };
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &oldSegv);
    sigaction(SIGILL,  &sa, &oldIll);
    sigaction(SIGFPE,  &sa, &oldFpe);
    sigaction(SIGBUS,  &sa, &oldBus);
    const auto restore = [&] {
        sigaction(SIGSEGV, &oldSegv, nullptr);
        sigaction(SIGILL,  &oldIll, nullptr);
        sigaction(SIGFPE,  &oldFpe, nullptr);
        sigaction(SIGBUS,  &oldBus, nullptr);
    };
    if (sigsetjmp(stEnv, 1) != 0) {
        restore();
        LOG_WARN("Vulkan self-test: driver fault (signal) during render probe");
        return false;
    }
    const bool ok = selfTestImpl();
    restore();
    return ok;
#else
    return selfTestImpl();
#endif
}

bool VulkanDevice::selfTestImpl() {
    constexpr u32 W = 64, H = 64;
    std::vector<u8> color(static_cast<size_t>(W) * H * 4, 0);

    auto backend = create(color.data(), W, H);
    if (!backend) {
        LOG_WARN("Vulkan self-test: device creation failed");
        return false;
    }

    backend->clear(0.f, 0.f, 0.f, 1.f);
    backend->setViewport(0, 0, static_cast<float>(W), static_cast<float>(H));
    const GpuVertex tri[3] = {
        {{-0.5f, -0.5f, 0.5f, 1.f}, {1.f, 0.f, 0.f, 1.f}},
        {{ 0.5f, -0.5f, 0.5f, 1.f}, {0.f, 1.f, 0.f, 1.f}},
        {{ 0.0f,  0.5f, 0.5f, 1.f}, {0.f, 0.f, 1.f, 1.f}},
    };
    backend->drawTriangles(tri, 3);
    backend->frameEnd();
    backend.reset();

    // Expect: black background + a few hundred non-black (triangle) pixels.
    size_t triPixels = 0, bgPixels = 0;
    for (size_t i = 0; i < static_cast<size_t>(W) * H; ++i) {
        const u8 r = color[i * 4 + 0], g = color[i * 4 + 1], b = color[i * 4 + 2];
        if (r <= 2 && g <= 2 && b <= 2) bgPixels++;
        else if (r > 20 || g > 20 || b > 20) triPixels++;
    }
    const bool ok = bgPixels > (static_cast<size_t>(W) * H) / 2 && triPixels > 50;
    if (ok) LOG_INFO("Vulkan self-test: render + readback verified (%zu triangle pixels)", triPixels);
    else    LOG_WARN("Vulkan self-test: unexpected output (bg=%zu tri=%zu) — driver broken?",
                     bgPixels, triPixels);
    return ok;
}

std::string VulkanDevice::probeReport() {
    void* lib = openLoader();
    if (!lib) return "Vulkan: loader not found (libvulkan.so.1 / vulkan-1.dll)";
    Api api;
    if (!api.loadGlobal(lib)) {
#if defined(_WIN32)
        FreeLibrary(static_cast<HMODULE>(lib));
#else
        dlclose(lib);
#endif
        return "Vulkan: vkGetInstanceProcAddr missing";
    }

    VkApplicationInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "PSBET";
    ai.applicationVersion = 1;
    ai.pEngineName = "PSBET";
    ai.engineVersion = 1;
    ai.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &ai;

    VkInstance inst = VK_NULL_HANDLE;
    if (api.vkCreateInstance(&ci, nullptr, &inst) != VK_SUCCESS) {
#if defined(_WIN32)
        FreeLibrary(static_cast<HMODULE>(lib));
#else
        dlclose(lib);
#endif
        return "Vulkan: vkCreateInstance failed";
    }
    api.loadInstance(inst, reinterpret_cast<PFN_vkGetInstanceProcAddr>(
#if defined(_WIN32)
        GetProcAddress(static_cast<HMODULE>(lib), "vkGetInstanceProcAddr")
#else
        dlsym(lib, "vkGetInstanceProcAddr")
#endif
    ));

    std::string out;
    char buf[512];
    u32 n = 0;
    if (api.vkEnumeratePhysicalDevices(inst, &n, nullptr) == VK_SUCCESS && n > 0) {
        std::vector<VkPhysicalDevice> devs(n);
        api.vkEnumeratePhysicalDevices(inst, &n, devs.data());
        std::snprintf(buf, sizeof(buf), "Vulkan devices: %u", n);
        out = buf;
        for (u32 i = 0; i < n; ++i) {
            VkPhysicalDeviceProperties pr{};
            api.vkGetPhysicalDeviceProperties(devs[i], &pr);
            std::snprintf(buf, sizeof(buf), "\n  [%u] %s (type %u, api %u.%u.%u)",
                          i, pr.deviceName, pr.deviceType,
                          pr.apiVersion >> 22, (pr.apiVersion >> 12) & 0xFFF,
                          pr.apiVersion & 0xFFF);
            out += buf;
        }
    } else {
        out = "Vulkan: no physical devices";
    }

    api.vkDestroyInstance(inst, nullptr);
#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(lib));
#else
    dlclose(lib);
#endif
    return out;
}

std::unique_ptr<IRasterBackend> VulkanDevice::create(u8* colorBuf, u32 width, u32 height) {
    // Deterministic, single-threaded shader compilation for the software
    // rasterizer driver (llvmpipe). Irrelevant for real GPUs; protects CI and
    // sandboxed environments from nondeterministic JIT worker crashes. Set
    // only if the user has not chosen a value.
#if !defined(_WIN32)
    ::setenv("LP_NUM_THREADS", "1", 0);
#else
    (void)0;
#endif

    std::unique_ptr<VulkanDevice> dev(new VulkanDevice());
    Impl* p = new Impl();
    dev->impl_ = p;
    dev->impl_->colorOut = colorBuf;
    dev->impl_->width = width;
    dev->impl_->height = height;

    // ---- loader ----
    p->lib = openLoader();
    if (!p->lib) { LOG_ERROR("Vulkan: loader library not found"); return nullptr; }
    if (!g_vk.loadGlobal(p->lib)) {
        LOG_ERROR("Vulkan: vkGetInstanceProcAddr not found in loader");
        p->destroy();
        return nullptr;
    }

#if defined(_WIN32)
    p->gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(static_cast<HMODULE>(p->lib), "vkGetInstanceProcAddr"));
#else
    p->gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(p->lib, "vkGetInstanceProcAddr"));
#endif
    if (!p->gipa) { p->destroy(); return nullptr; }

    // ---- instance ----
    VkApplicationInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "PSBET";
    ai.applicationVersion = 1;
    ai.pEngineName = "PSBET";
    ai.engineVersion = 1;
    ai.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;
    if (p->gipa(nullptr, "vkCreateInstance") == nullptr) {
        LOG_ERROR("Vulkan: loader has no vkCreateInstance");
        p->destroy();
        return nullptr;
    }
    {
        auto fn = reinterpret_cast<PFN_vkCreateInstance>(p->gipa(nullptr, "vkCreateInstance"));
        const VkResult r = fn(&ici, nullptr, &p->instance);
        if (r != VK_SUCCESS) {
            LOG_ERROR("Vulkan: instance creation failed (%s)", vkResultStr(r));
            p->destroy();
            return nullptr;
        }
    }
    if (!g_vk.loadInstance(p->instance, p->gipa)) { p->destroy(); return nullptr; }

    // ---- physical device ----
    u32 nd = 0;
    if (g_vk.vkEnumeratePhysicalDevices(p->instance, &nd, nullptr) != VK_SUCCESS || nd == 0) {
        LOG_ERROR("Vulkan: no physical devices");
        return nullptr;
    }
    std::vector<VkPhysicalDevice> devs(nd);
    if (g_vk.vkEnumeratePhysicalDevices(p->instance, &nd, devs.data()) != VK_SUCCESS) {
        LOG_ERROR("Vulkan: physical device enumeration failed");
        return nullptr;
    }

    // Prefer discrete > integrated > others (cpu/lavapipe last).
    auto rank = [&](VkPhysicalDevice d) {
        VkPhysicalDeviceProperties pr{};
        g_vk.vkGetPhysicalDeviceProperties(d, &pr);
        switch (pr.deviceType) {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 0;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 1;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 2;
            default:                                     return 3; // CPU
        }
    };
    VkPhysicalDevice best = devs[0];
    int bestRank = rank(best);
    for (VkPhysicalDevice d : devs) {
        const int r = rank(d);
        if (r < bestRank) { best = d; bestRank = r; }
    }
    p->phys = best;
    g_vk.vkGetPhysicalDeviceProperties(p->phys, &p->physProps);
    g_vk.vkGetPhysicalDeviceMemoryProperties(p->phys, &p->memProps);
    LOG_INFO("Vulkan: device '%s' (api %u.%u.%u, type %u)",
             p->physProps.deviceName, p->physProps.apiVersion >> 22,
             (p->physProps.apiVersion >> 12) & 0xFFF, p->physProps.apiVersion & 0xFFF,
             p->physProps.deviceType);

    // ---- queue family ----
    u32 nq = 0;
    g_vk.vkGetPhysicalDeviceQueueFamilyProperties(p->phys, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qs(nq);
    g_vk.vkGetPhysicalDeviceQueueFamilyProperties(p->phys, &nq, qs.data());
    bool haveGfx = false;
    for (u32 i = 0; i < nq; ++i) {
        if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { p->gfxFamily = i; haveGfx = true; break; }
    }
    if (!haveGfx) { LOG_ERROR("Vulkan: no graphics queue family"); return nullptr; }

    // ---- logical device ----
    VK_TRACE("create: logical device");
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = p->gfxFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    if (g_vk.vkCreateDevice(p->phys, &dci, nullptr, &p->device) != VK_SUCCESS) {
        LOG_ERROR("Vulkan: logical device creation failed");
        return nullptr;
    }
    g_vk.loadDevice(p->device, g_vk.vkGetDeviceProcAddr);
    VK_TRACE("create: get queue");
    g_vk.vkGetDeviceQueue(p->device, p->gfxFamily, 0, &p->queue);

    // ---- command pool + buffer + fence ----
    VK_TRACE("create: command pool");
    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = p->gfxFamily;
    if (g_vk.vkCreateCommandPool(p->device, &cpi, nullptr, &p->cmdPool) != VK_SUCCESS) {
        LOG_ERROR("Vulkan: command pool creation failed");
        return nullptr;
    }

    VkCommandBufferAllocateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = p->cmdPool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    if (g_vk.vkAllocateCommandBuffers(p->device, &cbi, &p->cmd) != VK_SUCCESS) {
        LOG_ERROR("Vulkan: command buffer allocation failed");
        return nullptr;
    }

    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (g_vk.vkCreateFence(p->device, &fi, nullptr, &p->fence) != VK_SUCCESS) {
        LOG_ERROR("Vulkan: fence creation failed");
        return nullptr;
    }

    // ---- resources (renderpass first, then images/framebuffer) ----
    VK_TRACE("create: renderpass");
    if (!p->createRenderPass()) return nullptr;
    VK_TRACE("create: images");
    if (!p->createImages()) { LOG_ERROR("Vulkan: image creation failed"); return nullptr; }
    VK_TRACE("create: shader modules");
    if (!p->createShaderModules()) return nullptr;
    VK_TRACE("create: vertex buffer");
    if (!p->createVertexBuffer(16u << 20)) { LOG_ERROR("Vulkan: vertex buffer failed"); return nullptr; }

    VK_TRACE("create: pipeline cache");
    VkPipelineCacheCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    if (g_vk.vkCreatePipelineCache(p->device, &pci, nullptr, &p->pipeCache) != VK_SUCCESS) {
        LOG_ERROR("Vulkan: pipeline cache creation failed");
        return nullptr;
    }

    return std::unique_ptr<IRasterBackend>(dev.release());
}

} // namespace psb::gpu
