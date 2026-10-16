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
    // Read BYTES, then pack 4 bytes little-endian per word.
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
    std::vector<Word> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), words.size() * 4);
    return words;
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

    // Depth-config axis (argv[2], only meaningful when withDepth):
    //   0 = D32 + DONT_CARE store (current)   1 = D32 + STORE
    //   2 = D24_UNORM_S8_UINT + DONT_CARE     3 = D32_S8_UINT + DONT_CARE
    //   4 = D32 + full depth-aware deps       5 = D32 + initialLayout DEPTH_OPT
    const int dCfg = argc > 2 ? std::atoi(argv[2]) : 0;
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
    switch (dCfg) {
        case 2: att[1].format = VK_FORMAT_D24_UNORM_S8_UINT; break;
        case 3: att[1].format = VK_FORMAT_D32_SFLOAT_S8_UINT; break;
        default: break;
    }
    att[1].storeOp = (dCfg == 1) ? VK_ATTACHMENT_STORE_OP_STORE
                                 : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].initialLayout = (dCfg == 5)
        ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
        : VK_IMAGE_LAYOUT_UNDEFINED;
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
    if (withDepth && dCfg == 4) {
        deps[0].srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                             | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
                             | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].dstStageMask = deps[0].srcStageMask;
        deps[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                              | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[0].dstAccessMask = deps[0].srcAccessMask;
    } else {
        deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    }
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

    // Depth-enable override for the DS state (engine self-test draws with
    // depth disabled while the renderpass has a depth attachment).
    if (argc > 3 && argv[3][0] == 'd') {
        ds.depthTestEnable = static_cast<VkBool32>(std::atoi(&argv[3][1]));
        ds.depthWriteEnable = ds.depthTestEnable;
    }

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

    // Depth-enable axis: argv[2]=="9" + argv[3]=="d<0|1>" overrides the DS
    // state's depthTestEnable (engine's self-test uses depth disabled).
    // (Parsed after ds is built — see below.)

    // Probe mode: argv[3]=="p" varies pipeline-shape hypotheses that all use
    // the depth renderpass (rp 0) and the passthrough shader.
    const char* probe = argc > 3 ? argv[3] : nullptr;
    if (probe && probe[0] == 'p') {
        const int pv = argc > 4 ? std::atoi(argv[4]) : 0;
        switch (pv) {
            case 0: pi.pDepthStencilState = nullptr; break;             // no DS state at all
            case 1: rpi.dependencyCount = 0; break;                     // zero dependencies (renderpass rebuilt below)
            case 2: pi.pColorBlendState = nullptr; break;               // no blend state
            case 3: pi.pVertexInputState = nullptr; break;              // no vertex input
            case 4: pi.pMultisampleState = nullptr; break;              // no multisample
            case 5: dyn.dynamicStateCount = 0; dyn.pDynamicStates = nullptr; break; // no dynamic state
            default: break;
        }
        if (pv == 1) {
            // rebuild renderpass with zero deps
            VkRenderPassCreateInfo rpi2 = rpi;
            rpi2.dependencyCount = 0;
            rpi2.pDependencies = nullptr;
            VkRenderPass rp2;
            if (CRP(dev, &rpi2, nullptr, &rp2) != VK_SUCCESS) { std::printf("probe rp fail\n"); return 1; }
            pi.renderPass = rp2;
        }
        VkPipeline pipe = VK_NULL_HANDLE;
        const VkResult r = CGP(dev, nullptr, 1, &pi, nullptr, &pipe);
        std::printf("probe %d -> VkResult %d %s\n", pv, (int)r,
                    r == VK_SUCCESS ? "OK" : "FAIL");
        return r == VK_SUCCESS ? 0 : 1;
    }

    VkPipeline pipe = VK_NULL_HANDLE;
    // Full-render mode: argv[3]=="r" — after pipeline creation, perform the
    // complete offscreen render (images, framebuffer, draw, submit, readback)
    // exactly like the engine does. Isolates submit-time shader crashes.
    if (probe && probe[0] == 'r') {
        auto CIMG = reinterpret_cast<PFN_vkCreateImage>(gipa(inst, "vkCreateImage"));
        auto CBUFF = reinterpret_cast<PFN_vkCreateBuffer>(gipa(inst, "vkCreateBuffer"));
        auto ALLOC = reinterpret_cast<PFN_vkAllocateMemory>(gipa(inst, "vkAllocateMemory"));
        auto BINDI = reinterpret_cast<PFN_vkBindImageMemory>(gipa(inst, "vkBindImageMemory"));
        auto BINDB = reinterpret_cast<PFN_vkBindBufferMemory>(gipa(inst, "vkBindBufferMemory"));
        auto GIMR = reinterpret_cast<PFN_vkGetImageMemoryRequirements>(gipa(inst, "vkGetImageMemoryRequirements"));
        auto GBMR = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(gipa(inst, "vkGetBufferMemoryRequirements"));
        auto GMP = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa(inst, "vkGetPhysicalDeviceMemoryProperties"));
        auto MAP = reinterpret_cast<PFN_vkMapMemory>(gipa(inst, "vkMapMemory"));
        auto CP = reinterpret_cast<PFN_vkCreateCommandPool>(gipa(inst, "vkCreateCommandPool"));
        auto ACB = reinterpret_cast<PFN_vkAllocateCommandBuffers>(gipa(inst, "vkAllocateCommandBuffers"));
        auto CF = reinterpret_cast<PFN_vkCreateFence>(gipa(inst, "vkCreateFence"));
        auto WF = reinterpret_cast<PFN_vkWaitForFences>(gipa(inst, "vkWaitForFences"));
        auto RF = reinterpret_cast<PFN_vkResetFences>(gipa(inst, "vkResetFences"));
        auto QSUB = reinterpret_cast<PFN_vkQueueSubmit>(gipa(inst, "vkQueueSubmit"));
        auto GDQ = reinterpret_cast<PFN_vkGetDeviceQueue>(gipa(inst, "vkGetDeviceQueue"));
        auto CV = reinterpret_cast<PFN_vkCreateImageView>(gipa(inst, "vkCreateImageView"));
        auto CFB = reinterpret_cast<PFN_vkCreateFramebuffer>(gipa(inst, "vkCreateFramebuffer"));
        VkPhysicalDeviceMemoryProperties mp{};
        reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa(inst, "vkGetPhysicalDeviceMemoryProperties"))(phys, &mp);

        VkQueue queue; GDQ(dev, gfx, 0, &queue);
        const VkDeviceSize imgSize = 64ull * 64ull * 4ull;

        auto makeImg = [&](VkFormat fmt, VkImageUsageFlags usage) {
            VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ii.imageType = VK_IMAGE_TYPE_2D;
            ii.format = fmt;
            ii.extent = {64, 64, 1};
            ii.mipLevels = 1; ii.arrayLayers = 1;
            ii.samples = VK_SAMPLE_COUNT_1_BIT;
            ii.tiling = VK_IMAGE_TILING_OPTIMAL;
            ii.usage = usage;
            ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkImage img; VkDeviceMemory mem;
            if (CIMG(dev, &ii, nullptr, &img) != VK_SUCCESS) return std::pair<VkImage, VkDeviceMemory>{};
            VkMemoryRequirements mr; GIMR(dev, img, &mr);
            uint32_t mt = 0;
            for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
                if (mr.memoryTypeBits & (1u << i)) { mt = i; break; }
            VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            mai.allocationSize = mr.size; mai.memoryTypeIndex = mt;
            ALLOC(dev, &mai, nullptr, &mem);
            BINDI(dev, img, mem, 0);
            return std::pair<VkImage, VkDeviceMemory>{img, mem};
        };

        auto color = makeImg(VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        auto depth = makeImg(VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
        auto readback = makeImg(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        std::printf("  images created\n");

        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vi.image = color.first;
        VkImageView colView; CV(dev, &vi, nullptr, &colView);
        vi.format = VK_FORMAT_D32_SFLOAT;
        vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vi.image = depth.first;
        VkImageView depView; CV(dev, &vi, nullptr, &depView);
        VkImageView atts[2] = {colView, depView};
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = rp; fi.attachmentCount = 2; fi.pAttachments = atts;
        fi.width = 64; fi.height = 64; fi.layers = 1;
        VkFramebuffer fb;
        if (CFB(dev, &fi, nullptr, &fb) != VK_SUCCESS) { std::printf("fb fail\n"); return 1; }
        std::printf("  framebuffer created\n");

        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = 4096; bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        VkBuffer vbuf;
        if (CBUFF(dev, &bci, nullptr, &vbuf) != VK_SUCCESS) { std::printf("buf fail\n"); return 1; }
        VkMemoryRequirements mr; GBMR(dev, vbuf, &mr);
        uint32_t mt = 0;
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((mr.memoryTypeBits & (1u << i)) &&
                (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
                { mt = i; break; }
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = mr.size; mai.memoryTypeIndex = mt;
        VkDeviceMemory vmem;
        ALLOC(dev, &mai, nullptr, &vmem);
        BINDB(dev, vbuf, vmem, 0);
        void* vptr = nullptr;
        MAP(dev, vmem, 0, VK_WHOLE_SIZE, 0, &vptr);
        const float verts[6][8] = {
            {-0.5f,-0.5f,0.5f,1, 1,0,0,1}, {0.5f,-0.5f,0.5f,1, 0,1,0,1}, {0,0.5f,0.5f,1, 0,0,1,1},
        };
        std::memcpy(vptr, verts, sizeof(verts));
        std::printf("  vertex buffer created+mapped\n");

        VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpi.queueFamilyIndex = gfx;
        VkCommandPool pool; CP(dev, &cpi, nullptr, &pool);
        VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbi.commandPool = pool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
        VkCommandBuffer cb; ACB(dev, &cbi, &cb);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence; CF(dev, &fci, nullptr, &fence);
        std::printf("  command infrastructure ready\n");
        if (CGP(dev, nullptr, 1, &pi, nullptr, &pipe) != VK_SUCCESS) {
            std::printf("  pipeline create fail\n");
            return 1;
        }
        std::printf("  pipeline created\n");

        auto GQ = reinterpret_cast<PFN_vkGetDeviceQueue>(gipa(inst, "vkGetDeviceQueue"));
        (void)GQ;
        auto Begin = reinterpret_cast<PFN_vkBeginCommandBuffer>(gipa(inst, "vkBeginCommandBuffer"));
        auto End = reinterpret_cast<PFN_vkEndCommandBuffer>(gipa(inst, "vkEndCommandBuffer"));
        auto BRP = reinterpret_cast<PFN_vkCmdBeginRenderPass>(gipa(inst, "vkCmdBeginRenderPass"));
        auto ERP = reinterpret_cast<PFN_vkCmdEndRenderPass>(gipa(inst, "vkCmdEndRenderPass"));
        auto SVP = reinterpret_cast<PFN_vkCmdSetViewport>(gipa(inst, "vkCmdSetViewport"));
        auto SSC = reinterpret_cast<PFN_vkCmdSetScissor>(gipa(inst, "vkCmdSetScissor"));
        auto BP = reinterpret_cast<PFN_vkCmdBindPipeline>(gipa(inst, "vkCmdBindPipeline"));
        auto BV = reinterpret_cast<PFN_vkCmdBindVertexBuffers>(gipa(inst, "vkCmdBindVertexBuffers"));
        auto DR = reinterpret_cast<PFN_vkCmdDraw>(gipa(inst, "vkCmdDraw"));
        auto CPY = reinterpret_cast<PFN_vkCmdCopyImage>(gipa(inst, "vkCmdCopyImage"));
        auto BAR = reinterpret_cast<PFN_vkCmdPipelineBarrier>(gipa(inst, "vkCmdPipelineBarrier"));

        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (Begin(cb, &bi) != VK_SUCCESS) { std::printf("begin fail\n"); return 1; }
        VkClearValue cv[2]{};
        cv[0].color = {{0.f, 0.f, 0.f, 1.f}};
        cv[1].depthStencil = {1.f, 0};
        VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rbi.renderPass = rp; rbi.framebuffer = fb;
        rbi.renderArea = {{0, 0}, {64, 64}};
        rbi.clearValueCount = 2; rbi.pClearValues = cv;
        BRP(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        const VkViewport vp{0.f, 0.f, 64.f, 64.f, 0.f, 1.f};
        const VkRect2D sc{{0, 0}, {64, 64}};
        SVP(cb, 0, 1, &vp);
        SSC(cb, 0, 1, &sc);
        BP(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        VkDeviceSize off = 0;
        BV(cb, 0, 1, &vbuf, &off);
        DR(cb, 3, 1, 0, 0);
        ERP(cb);
        VkImageMemoryBarrier rb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        rb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        rb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        rb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        rb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        rb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        rb.image = readback.first;
        rb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        BAR(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &rb);
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent = {64, 64, 1};
        CPY(cb, color.first, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            readback.first, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        if (End(cb) != VK_SUCCESS) { std::printf("end fail\n"); return 1; }
        std::printf("  command buffer recorded, submitting...\n");

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        const VkResult sub = QSUB(queue, 1, &si, fence);
        std::printf("  submit -> %d\n", (int)sub);
        if (sub == VK_SUCCESS)
            std::printf("  wait -> %d\n",
                        (int)WF(dev, 1, &fence, VK_TRUE, 10ull * 1000ull * 1000ull * 1000ull));
        std::printf("FULL RENDER DONE\n");
        return 0;
    }

    const VkResult r = CGP(dev, nullptr, 1, &pi, nullptr, &pipe);
    std::printf("combo %d dcfg=%d (vs=%s rp=%s dyn=%d) -> VkResult %d %s\n", combo, dCfg,
                vsPaths[vIdx] ? vsPaths[vIdx] : "garbage",
                rpIdx == 0 ? "DEPTH+TS" : rpIdx == 1 ? "DEPTH+CA" : "COLORONLY",
                dynIdx, (int)r, r == VK_SUCCESS ? "OK" : "FAIL");
    return r == VK_SUCCESS ? 0 : 1;
}
