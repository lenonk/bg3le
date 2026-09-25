// The overlay on an HDR swapchain.
//
// Upstream draws ImGui straight into the swapchain, which is right for SDR.
// When the compositor offers HDR the game presents HDR10, and ImGui's sRGB
// colours -- and its blending, which the hardware does in the swapchain's
// encoding -- come out wrong. So for an HDR swapchain the overlay is drawn
// into an SDR image of its own, exactly as upstream would draw it, and a
// fullscreen pass lays it over the game's frame: the game's pixel is brought
// to SDR, the overlay blended over it as in an SDR swapchain, and the result
// encoded back (shaders/hdr_composite.frag). An SDR swapchain is untouched.

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>
#include <vulkan/vulkan.h>

#include <cstdlib>
#include <vector>

#include "../log.h"
#include "imgui_hdr_shaders.h"

namespace bg3le {
namespace {

constexpr VkFormat kOverlayFormat = VK_FORMAT_B8G8R8A8_UNORM;

// The game draws its own UI white at 300 nits, measured from its HDR10
// output; the overlay's white is put at the same level.
float ui_white_nits() {
    char const* opt = std::getenv("BG3LE_HDR_UI_NITS");
    const float nits = opt != nullptr ? std::strtof(opt, nullptr) : 0.0f;
    return nits > 10.0f && nits < 10000.0f ? nits : 300.0f;
}

struct Frame {
    VkImage Swap{VK_NULL_HANDLE};
    VkImageView SwapView{VK_NULL_HANDLE};
    VkFramebuffer CompositeFb{VK_NULL_HANDLE};
    VkImage Overlay{VK_NULL_HANDLE};
    VkDeviceMemory OverlayMem{VK_NULL_HANDLE};
    VkImageView OverlayView{VK_NULL_HANDLE};
    VkFramebuffer OverlayFb{VK_NULL_HANDLE};
    VkImage Copy{VK_NULL_HANDLE};
    VkDeviceMemory CopyMem{VK_NULL_HANDLE};
    VkImageView CopyView{VK_NULL_HANDLE};
    VkDescriptorSet Set{VK_NULL_HANDLE};
};

struct State {
    VkDevice Device{VK_NULL_HANDLE};
    VkPhysicalDevice Physical{VK_NULL_HANDLE};
    VkFormat SwapFormat{VK_FORMAT_UNDEFINED};
    VkExtent2D Extent{};
    int Encoding{0};
    float White{300.0f};
    VkRenderPass OverlayPass{VK_NULL_HANDLE};
    VkRenderPass CompositePass{VK_NULL_HANDLE};
    VkSampler Sampler{VK_NULL_HANDLE};
    VkDescriptorSetLayout SetLayout{VK_NULL_HANDLE};
    VkDescriptorPool Pool{VK_NULL_HANDLE};
    VkPipelineLayout Layout{VK_NULL_HANDLE};
    VkPipeline Pipeline{VK_NULL_HANDLE};
    std::vector<Frame> Frames;
};

State g;

bool ok(VkResult r, char const* what) {
    if (r == VK_SUCCESS) return true;
    logf("imgui: HDR compositor: %s failed (%d)", what, (int)r);
    return false;
}

std::uint32_t memory_type(std::uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(g.Physical, &props);
    for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) return i;
    }
    return UINT32_MAX;
}

bool make_image(VkFormat format, VkImageUsageFlags usage, VkImage* image,
                VkDeviceMemory* memory, VkImageView* view) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {g.Extent.width, g.Extent.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!ok(vkCreateImage(g.Device, &info, nullptr, image), "vkCreateImage")) return false;

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g.Device, *image, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX) {
        logf("imgui: HDR compositor: no device-local memory for an image");
        return false;
    }
    if (!ok(vkAllocateMemory(g.Device, &alloc, nullptr, memory), "vkAllocateMemory")) return false;
    if (!ok(vkBindImageMemory(g.Device, *image, *memory, 0), "vkBindImageMemory")) return false;

    VkImageViewCreateInfo vinfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vinfo.image = *image;
    vinfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vinfo.format = format;
    vinfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return ok(vkCreateImageView(g.Device, &vinfo, nullptr, view), "vkCreateImageView");
}

VkRenderPass make_pass(VkFormat format, VkAttachmentLoadOp load, VkImageLayout initial,
                       VkImageLayout final) {
    VkAttachmentDescription att{};
    att.format = format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = load;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = initial;
    att.finalLayout = final;

    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;

    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = 1;
    info.pAttachments = &att;
    info.subpassCount = 1;
    info.pSubpasses = &sub;
    info.dependencyCount = 2;
    info.pDependencies = deps;
    VkRenderPass pass = VK_NULL_HANDLE;
    ok(vkCreateRenderPass(g.Device, &info, nullptr, &pass), "vkCreateRenderPass");
    return pass;
}

VkShaderModule make_module(std::uint32_t const* code, std::size_t bytes) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytes;
    info.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    ok(vkCreateShaderModule(g.Device, &info, nullptr, &module), "vkCreateShaderModule");
    return module;
}

bool make_pipeline() {
    VkSamplerCreateInfo sinfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sinfo.magFilter = sinfo.minFilter = VK_FILTER_NEAREST;
    sinfo.addressModeU = sinfo.addressModeV = sinfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (!ok(vkCreateSampler(g.Device, &sinfo, nullptr, &g.Sampler), "vkCreateSampler")) return false;

    VkDescriptorSetLayoutBinding bindings[2]{};
    for (std::uint32_t i = 0; i < 2; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[i].pImmutableSamplers = &g.Sampler;
    }
    VkDescriptorSetLayoutCreateInfo linfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    linfo.bindingCount = 2;
    linfo.pBindings = bindings;
    if (!ok(vkCreateDescriptorSetLayout(g.Device, &linfo, nullptr, &g.SetLayout), "vkCreateDescriptorSetLayout")) return false;

    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8};
    VkPipelineLayoutCreateInfo plinfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plinfo.setLayoutCount = 1;
    plinfo.pSetLayouts = &g.SetLayout;
    plinfo.pushConstantRangeCount = 1;
    plinfo.pPushConstantRanges = &push;
    if (!ok(vkCreatePipelineLayout(g.Device, &plinfo, nullptr, &g.Layout), "vkCreatePipelineLayout")) return false;

    VkShaderModule vs = make_module(kHdrComposite_vert, sizeof(kHdrComposite_vert));
    VkShaderModule fs = make_module(kHdrComposite_frag, sizeof(kHdrComposite_frag));
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0, 0, (float)g.Extent.width, (float)g.Extent.height, 0, 1};
    VkRect2D scissor{{0, 0}, g.Extent};
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.pViewports = &viewport;
    vp.scissorCount = 1;
    vp.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &vp;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &ms;
    info.pColorBlendState = &cb;
    info.layout = g.Layout;
    info.renderPass = g.CompositePass;
    const bool made = ok(vkCreateGraphicsPipelines(g.Device, VK_NULL_HANDLE, 1, &info, nullptr, &g.Pipeline),
                         "vkCreateGraphicsPipelines");
    vkDestroyShaderModule(g.Device, vs, nullptr);
    vkDestroyShaderModule(g.Device, fs, nullptr);
    return made;
}

bool make_frames(VkImage const* images, std::uint32_t count) {
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * count};
    VkDescriptorPoolCreateInfo pinfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pinfo.maxSets = count;
    pinfo.poolSizeCount = 1;
    pinfo.pPoolSizes = &size;
    if (!ok(vkCreateDescriptorPool(g.Device, &pinfo, nullptr, &g.Pool), "vkCreateDescriptorPool")) return false;

    g.Frames.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        Frame& f = g.Frames[i];
        f.Swap = images[i];

        VkImageViewCreateInfo vinfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vinfo.image = f.Swap;
        vinfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vinfo.format = g.SwapFormat;
        vinfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!ok(vkCreateImageView(g.Device, &vinfo, nullptr, &f.SwapView), "vkCreateImageView")) return false;

        if (!make_image(kOverlayFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        &f.Overlay, &f.OverlayMem, &f.OverlayView)) return false;
        if (!make_image(g.SwapFormat, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        &f.Copy, &f.CopyMem, &f.CopyView)) return false;

        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb.attachmentCount = 1;
        fb.width = g.Extent.width;
        fb.height = g.Extent.height;
        fb.layers = 1;
        fb.renderPass = g.OverlayPass;
        fb.pAttachments = &f.OverlayView;
        if (!ok(vkCreateFramebuffer(g.Device, &fb, nullptr, &f.OverlayFb), "vkCreateFramebuffer")) return false;
        fb.renderPass = g.CompositePass;
        fb.pAttachments = &f.SwapView;
        if (!ok(vkCreateFramebuffer(g.Device, &fb, nullptr, &f.CompositeFb), "vkCreateFramebuffer")) return false;

        VkDescriptorSetAllocateInfo ainfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ainfo.descriptorPool = g.Pool;
        ainfo.descriptorSetCount = 1;
        ainfo.pSetLayouts = &g.SetLayout;
        if (!ok(vkAllocateDescriptorSets(g.Device, &ainfo, &f.Set), "vkAllocateDescriptorSets")) return false;

        VkDescriptorImageInfo views[2]{};
        views[0].imageView = f.OverlayView;
        views[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        views[1].imageView = f.CopyView;
        views[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet writes[2]{};
        for (std::uint32_t b = 0; b < 2; ++b) {
            writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet = f.Set;
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[b].pImageInfo = &views[b];
        }
        vkUpdateDescriptorSets(g.Device, 2, writes, 0, nullptr);
    }
    return true;
}

void destroy_all() {
    if (g.Device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(g.Device);
    for (Frame& f : g.Frames) {
        vkDestroyFramebuffer(g.Device, f.CompositeFb, nullptr);
        vkDestroyFramebuffer(g.Device, f.OverlayFb, nullptr);
        vkDestroyImageView(g.Device, f.SwapView, nullptr);
        vkDestroyImageView(g.Device, f.OverlayView, nullptr);
        vkDestroyImage(g.Device, f.Overlay, nullptr);
        vkFreeMemory(g.Device, f.OverlayMem, nullptr);
        vkDestroyImageView(g.Device, f.CopyView, nullptr);
        vkDestroyImage(g.Device, f.Copy, nullptr);
        vkFreeMemory(g.Device, f.CopyMem, nullptr);
    }
    g.Frames.clear();
    vkDestroyPipeline(g.Device, g.Pipeline, nullptr);
    vkDestroyPipelineLayout(g.Device, g.Layout, nullptr);
    vkDestroyDescriptorPool(g.Device, g.Pool, nullptr);
    vkDestroyDescriptorSetLayout(g.Device, g.SetLayout, nullptr);
    vkDestroySampler(g.Device, g.Sampler, nullptr);
    vkDestroyRenderPass(g.Device, g.OverlayPass, nullptr);
    vkDestroyRenderPass(g.Device, g.CompositePass, nullptr);
    g = State{};
}

void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
             VkAccessFlags srcAccess, VkAccessFlags dstAccess) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
}

}  // namespace

// From the backend as the game creates a swapchain. Sets up the compositor
// for an HDR one; an SDR one leaves the overlay drawing straight in.
void hdr_swapchain_created(VkDevice device, VkPhysicalDevice physical,
                           VkSwapchainCreateInfoKHR const* info,
                           VkImage const* images, std::uint32_t count) {
    destroy_all();

    int encoding = -1;
    if (info->imageColorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) encoding = 0;
    if (info->imageColorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) encoding = 1;
    logf("imgui: swapchain format %d, colour space %d", (int)info->imageFormat,
         (int)info->imageColorSpace);
    if (encoding < 0) return;

    if ((info->imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0) {
        logf("imgui: HDR swapchain cannot be copied from; the overlay draws straight in");
        return;
    }

    g.Device = device;
    g.Physical = physical;
    g.SwapFormat = info->imageFormat;
    g.Extent = info->imageExtent;
    g.Encoding = encoding;
    g.White = ui_white_nits();
    g.OverlayPass = make_pass(kOverlayFormat, VK_ATTACHMENT_LOAD_OP_CLEAR,
                              VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    g.CompositePass = make_pass(g.SwapFormat, VK_ATTACHMENT_LOAD_OP_LOAD,
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    if (g.OverlayPass == VK_NULL_HANDLE || g.CompositePass == VK_NULL_HANDLE
        || !make_pipeline() || !make_frames(images, count)) {
        logf("imgui: HDR compositor could not be set up; the overlay draws straight in");
        destroy_all();
        return;
    }
    logf("imgui: HDR swapchain; the overlay is composited at a %.0f-nit UI white", g.White);
}

void hdr_swapchain_released() { destroy_all(); }

// The render pass ImGui draws in while the compositor is up, else null.
VkRenderPass hdr_overlay_pass() { return g.OverlayPass; }

// Draws the overlay into its SDR image and lays it over swapchain image
// `index`, which arrives and leaves in PRESENT_SRC.
void hdr_record(VkCommandBuffer cmd, std::uint32_t index, ImDrawData* draw) {
    Frame& f = g.Frames[index];

    VkClearValue clear{};
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = g.OverlayPass;
    begin.framebuffer = f.OverlayFb;
    begin.renderArea = {{0, 0}, g.Extent};
    begin.clearValueCount = 1;
    begin.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(draw, cmd);
    vkCmdEndRenderPass(cmd);

    barrier(cmd, f.Swap, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    barrier(cmd, f.Copy, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {g.Extent.width, g.Extent.height, 1};
    vkCmdCopyImage(cmd, f.Swap, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, f.Copy,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(cmd, f.Copy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    barrier(cmd, f.Swap, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

    begin.renderPass = g.CompositePass;
    begin.framebuffer = f.CompositeFb;
    begin.clearValueCount = 0;
    begin.pClearValues = nullptr;
    vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.Pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.Layout, 0, 1, &f.Set, 0, nullptr);
    struct { float White; int Encoding; } params{g.White, g.Encoding};
    vkCmdPushConstants(cmd, g.Layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(params), &params);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}

}  // namespace bg3le
