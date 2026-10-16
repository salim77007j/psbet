// One-combo-per-run bisect for lavapipe vkCreateGraphicsPipelines.
// Usage: vk_bisect <combo>  where combo = vs_idx*9 + rp_idx*3 + dyn_idx
//   vs_idx: 0=glslang reference  1=psbet passthrough  2=psbet y-flip  3=garbage
//   rp_idx: 0=depth+TRANSFER_SRC final  1=depth+COLOR_ATT final  2=color-only
//   dyn_idx: 0=static viewport  1=dynamic viewport
#define VK_NO_PROTOTYPES
#include "gpu/vulkan/vulkan_core.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <vector>
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

using Word = unsigned int;

static void segvHandler(int sig, siginfo_t* info, void*) {
    void* frames[32];
    const int n = backtrace(frames, 32);
    fprintf(stderr, "\nFATAL sig %d addr %p\n", sig, info->si_addr);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    _exit(139);
}

static std::vector<Word> readSpv(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::printf("cannot open %s\n", path); exit(2); }
    return std::vector<Word>((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    struct sigaction sa{};
    sa.sa_sigaction = segvHandler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr);

    const int combo = argc > 1 ? std::atoi(argv[1]) : 0;
    const int vIdx = combo / 9, rpIdx = (combo / 3) % 3, dynIdx = combo % 3;

    void* lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) { std::printf("no loader\n"); return 1; }
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
    if (!gipa) { std::printf("no gipa\n"); return 1; }

    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "bisect";
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &ai;
    VkInstance inst;
    if (reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"))(
            &ici, nullptr, &inst) != VK_SUCCESS) { std::printf("instance fail\n"); return 1; }

    auto E = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(inst, "vkEnumeratePhysicalDevices"));
    auto QF = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties"));
    auto CD = reinterpret_cast<PFN_vkCreateDevice>(gipa(inst, "vkCreateDevice"));
    auto CSM = reinterpret_cast<PFN_vkCreateShaderModule>(gipa(inst, "vkCreateShaderModule"));
    auto CRP = reinterpret_cast<PFN_vkCreateRenderPass>(gipa(inst, "vkCreateRenderPass"));
    auto CPL = reinterpret_cast<PFN_vkCreatePipelineLayout>(gipa(inst, "vkCreatePipelineLayout"));
    auto CGP = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(gipa(inst, "vkCreateGraphicsPipelines"));

    uint32_t nd = 0;
    E(inst, &nd, nullptr);
    std::vector<VkPhysicalDevice> devs(nd);
    E(inst, &nd, devs.data());
    VkPhysicalDevice phys = devs[0];

    uint32_t nq = 0;
    QF(phys, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qs(nq);
    QF(phys, &nq, qs.data());
    uint32_t gfx = 0;
    for (uint32_t i = 0; i < nq; ++i)
        if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { gfx = i; break; }

    const float prio = 1.f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = gfx; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice dev;
    if (CD(phys, &dci, nullptr, &dev) != VK_SUCCESS) { std::printf("device fail\n"); return 1; }

    const char* vsPaths[] = {"ref_vs.spv", "vs_pass.spv", "vs_flip.spv", nullptr};
    std::vector<Word> vsCode = vsPaths[vIdx] ? readSpv(vsPaths[vIdx])
                                             : std::vector<Word>{0x07230203u, 0x00010000u, 0x000b0001u, 64u, 0u};
    std::vector<Word> fsCode = readSpv(vIdx == 0 ? "ref_fs.spv" : "fs.spv");

    VkShaderModuleCreateInfo vci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    vci.codeSize = vsCode.size() * 4; vci.pCode = vsCode.data();
    VkShaderModule vs = VK_NULL_HANDLE;
    const VkResult vsr = CSM(dev, &vci, nullptr, &vs);
    if (vsr != VK_SUCCESS) { std::printf("vs module rejected: %d\n", (int)vsr); return 0; }
    VkShaderModuleCreateInfo fci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    fci.codeSize = fsCode.size() * 4; fci.pCode = fsCode.data();
    VkShaderModule fs = VK_NULL_HANDLE;
    if (CSM(dev, &fci, nullptr, &fs) != VK_SUCCESS) { std::printf("fs module rejected\n"); return 0; }

    const bool withDepth = rpIdx < 2;
    VkAttachmentDescription att[2]{};
    att[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[0].finalLayout = rpIdx == 0 ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                    : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att[1] = att[0];
    att[1].format = VK_FORMAT_D32_SFLOAT;
    att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &cref;
    if (withDepth) sp.pDepthStencilAttachment = &dref;

    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = withDepth ? (VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT)
                                     : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = rpIdx == 0 ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    deps[1].srcAccessMask = withDepth ? (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)
                                      : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = rpIdx == 0 ? VK_ACCESS_TRANSFER_READ_BIT : 0;

    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = withDepth ? 2 : 1;
    rpi.pAttachments = att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sp;
    rpi.dependencyCount = 2;
    rpi.pDependencies = deps;
    VkRenderPass rp;
    if (CRP(dev, &rpi, nullptr, &rp) != VK_SUCCESS) { std::printf("renderpass fail\n"); return 1; }

    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout;
    if (CPL(dev, &pli, nullptr, &layout) != VK_SUCCESS) { std::printf("layout fail\n"); return 1; }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "psb_vs";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "psb_fs";

    VkVertexInputBindingDescription bind{0, 32, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16};

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport staticVp{0.f, 0.f, 64.f, 64.f, 0.f, 1.f};
    VkRect2D staticSc{{0, 0}, {64, 64}};
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    if (!dynIdx) { vp.pViewports = &staticVp; vp.pScissors = &staticSc; }

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_BACK_BIT;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.f;

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = withDepth ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = withDepth ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkDynamicState dynStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    if (dynIdx) { dyn.dynamicStateCount = 2; dyn.pDynamicStates = dynStates; }

    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = withDepth ? &ds : nullptr;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dyn;
    pi.layout = layout;
    pi.renderPass = rp;
    pi.subpass = 0;

    VkPipeline pipe = VK_NULL_HANDLE;
    const VkResult r = CGP(dev, nullptr, 1, &pi, nullptr, &pipe);
    std::printf("combo %d (vs=%s rp=%s dyn=%d) -> VkResult %d %s\n", combo,
                vsPaths[vIdx] ? vsPaths[vIdx] : "garbage",
                rpIdx == 0 ? "DEPTH+TS" : rpIdx == 1 ? "DEPTH+CA" : "COLORONLY",
                dynIdx, (int)r, r == VK_SUCCESS ? "OK" : "FAIL");
    return r == VK_SUCCESS ? 0 : 1;
}
