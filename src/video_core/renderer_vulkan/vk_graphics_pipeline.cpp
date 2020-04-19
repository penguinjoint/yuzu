// Copyright 2019 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <array>
#include <cstring>
#include <vector>

#include "common/assert.h"
#include "common/common_types.h"
#include "common/microprofile.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/maxwell_to_vk.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_device.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_renderpass_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/renderer_vulkan/wrapper.h"

namespace Vulkan {

MICROPROFILE_DECLARE(Vulkan_PipelineCache);

namespace {

VkStencilOpState GetStencilFaceState(const FixedPipelineState::StencilFace& face) {
    VkStencilOpState state;
    state.failOp = MaxwellToVK::StencilOp(face.action_stencil_fail);
    state.passOp = MaxwellToVK::StencilOp(face.action_depth_pass);
    state.depthFailOp = MaxwellToVK::StencilOp(face.action_depth_fail);
    state.compareOp = MaxwellToVK::ComparisonOp(face.test_func);
    state.compareMask = 0;
    state.writeMask = 0;
    state.reference = 0;
    return state;
}

bool SupportsPrimitiveRestart(VkPrimitiveTopology topology) {
    static constexpr std::array unsupported_topologies = {
        VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
        VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY,
        VK_PRIMITIVE_TOPOLOGY_PATCH_LIST};
    return std::find(std::begin(unsupported_topologies), std::end(unsupported_topologies),
                     topology) == std::end(unsupported_topologies);
}

} // Anonymous namespace

VKGraphicsPipeline::VKGraphicsPipeline(const VKDevice& device, VKScheduler& scheduler,
                                       VKDescriptorPool& descriptor_pool,
                                       VKUpdateDescriptorQueue& update_descriptor_queue,
                                       VKRenderPassCache& renderpass_cache,
                                       const GraphicsPipelineCacheKey& key,
                                       vk::Span<VkDescriptorSetLayoutBinding> bindings,
                                       const SPIRVProgram& program)
    : device{device}, scheduler{scheduler}, fixed_state{key.fixed_state}, hash{key.Hash()},
      descriptor_set_layout{CreateDescriptorSetLayout(bindings)},
      descriptor_allocator{descriptor_pool, *descriptor_set_layout},
      update_descriptor_queue{update_descriptor_queue}, layout{CreatePipelineLayout()},
      descriptor_template{CreateDescriptorUpdateTemplate(program)}, modules{CreateShaderModules(
                                                                        program)},
      renderpass{renderpass_cache.GetRenderPass(key.renderpass_params)}, pipeline{CreatePipeline(
                                                                             key.renderpass_params,
                                                                             program)} {}

VKGraphicsPipeline::~VKGraphicsPipeline() = default;

VkDescriptorSet VKGraphicsPipeline::CommitDescriptorSet() {
    if (!descriptor_template) {
        return {};
    }
    const auto set = descriptor_allocator.Commit(scheduler.GetFence());
    update_descriptor_queue.Send(*descriptor_template, set);
    return set;
}

vk::DescriptorSetLayout VKGraphicsPipeline::CreateDescriptorSetLayout(
    vk::Span<VkDescriptorSetLayoutBinding> bindings) const {
    VkDescriptorSetLayoutCreateInfo ci;
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.pNext = nullptr;
    ci.flags = 0;
    ci.bindingCount = bindings.size();
    ci.pBindings = bindings.data();
    return device.GetLogical().CreateDescriptorSetLayout(ci);
}

vk::PipelineLayout VKGraphicsPipeline::CreatePipelineLayout() const {
    VkPipelineLayoutCreateInfo ci;
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ci.pNext = nullptr;
    ci.flags = 0;
    ci.setLayoutCount = 1;
    ci.pSetLayouts = descriptor_set_layout.address();
    ci.pushConstantRangeCount = 0;
    ci.pPushConstantRanges = nullptr;
    return device.GetLogical().CreatePipelineLayout(ci);
}

vk::DescriptorUpdateTemplateKHR VKGraphicsPipeline::CreateDescriptorUpdateTemplate(
    const SPIRVProgram& program) const {
    std::vector<VkDescriptorUpdateTemplateEntry> template_entries;
    u32 binding = 0;
    u32 offset = 0;
    for (const auto& stage : program) {
        if (stage) {
            FillDescriptorUpdateTemplateEntries(stage->entries, binding, offset, template_entries);
        }
    }
    if (template_entries.empty()) {
        // If the shader doesn't use descriptor sets, skip template creation.
        return {};
    }

    VkDescriptorUpdateTemplateCreateInfoKHR ci;
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO_KHR;
    ci.pNext = nullptr;
    ci.flags = 0;
    ci.descriptorUpdateEntryCount = static_cast<u32>(template_entries.size());
    ci.pDescriptorUpdateEntries = template_entries.data();
    ci.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET_KHR;
    ci.descriptorSetLayout = *descriptor_set_layout;
    ci.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    ci.pipelineLayout = *layout;
    ci.set = DESCRIPTOR_SET;
    return device.GetLogical().CreateDescriptorUpdateTemplateKHR(ci);
}

std::vector<vk::ShaderModule> VKGraphicsPipeline::CreateShaderModules(
    const SPIRVProgram& program) const {
    VkShaderModuleCreateInfo ci;
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.pNext = nullptr;
    ci.flags = 0;

    std::vector<vk::ShaderModule> modules;
    modules.reserve(Maxwell::MaxShaderStage);
    for (std::size_t i = 0; i < Maxwell::MaxShaderStage; ++i) {
        const auto& stage = program[i];
        if (!stage) {
            continue;
        }

        ci.codeSize = stage->code.size() * sizeof(u32);
        ci.pCode = stage->code.data();
        modules.push_back(device.GetLogical().CreateShaderModule(ci));
    }
    return modules;
}

vk::Pipeline VKGraphicsPipeline::CreatePipeline(const RenderPassParams& renderpass_params,
                                                const SPIRVProgram& program) const {
    const auto& vi = fixed_state.vertex_input;
    const auto& ia = fixed_state.input_assembly;
    const auto& ds = fixed_state.depth_stencil;
    const auto& cd = fixed_state.color_blending;
    const auto& ts = fixed_state.tessellation;
    const auto& rs = fixed_state.rasterizer;

    std::vector<VkVertexInputBindingDescription> vertex_bindings;
    std::vector<VkVertexInputBindingDivisorDescriptionEXT> vertex_binding_divisors;
    for (std::size_t i = 0; i < vi.num_bindings; ++i) {
        const auto& binding = vi.bindings[i];
        const bool instanced = binding.divisor != 0;
        const auto rate = instanced ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;

        auto& vertex_binding = vertex_bindings.emplace_back();
        vertex_binding.binding = binding.index;
        vertex_binding.stride = binding.stride;
        vertex_binding.inputRate = rate;

        if (instanced) {
            auto& binding_divisor = vertex_binding_divisors.emplace_back();
            binding_divisor.binding = binding.index;
            binding_divisor.divisor = binding.divisor;
        }
    }

    std::vector<VkVertexInputAttributeDescription> vertex_attributes;
    const auto& input_attributes = program[0]->entries.attributes;
    for (std::size_t i = 0; i < vi.num_attributes; ++i) {
        const auto& attribute = vi.attributes[i];
        if (input_attributes.find(attribute.index) == input_attributes.end()) {
            // Skip attributes not used by the vertex shaders.
            continue;
        }
        auto& vertex_attribute = vertex_attributes.emplace_back();
        vertex_attribute.location = attribute.index;
        vertex_attribute.binding = attribute.buffer;
        vertex_attribute.format = MaxwellToVK::VertexFormat(attribute.type, attribute.size);
        vertex_attribute.offset = attribute.offset;
    }

    VkPipelineVertexInputStateCreateInfo vertex_input_ci;
    vertex_input_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input_ci.pNext = nullptr;
    vertex_input_ci.flags = 0;
    vertex_input_ci.vertexBindingDescriptionCount = static_cast<u32>(vertex_bindings.size());
    vertex_input_ci.pVertexBindingDescriptions = vertex_bindings.data();
    vertex_input_ci.vertexAttributeDescriptionCount = static_cast<u32>(vertex_attributes.size());
    vertex_input_ci.pVertexAttributeDescriptions = vertex_attributes.data();

    VkPipelineVertexInputDivisorStateCreateInfoEXT input_divisor_ci;
    input_divisor_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT;
    input_divisor_ci.pNext = nullptr;
    input_divisor_ci.vertexBindingDivisorCount = static_cast<u32>(vertex_binding_divisors.size());
    input_divisor_ci.pVertexBindingDivisors = vertex_binding_divisors.data();
    if (!vertex_binding_divisors.empty()) {
        vertex_input_ci.pNext = &input_divisor_ci;
    }

    VkPipelineInputAssemblyStateCreateInfo input_assembly_ci;
    input_assembly_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly_ci.pNext = nullptr;
    input_assembly_ci.flags = 0;
    input_assembly_ci.topology = MaxwellToVK::PrimitiveTopology(device, ia.topology);
    input_assembly_ci.primitiveRestartEnable =
        ia.primitive_restart_enable && SupportsPrimitiveRestart(input_assembly_ci.topology);

    VkPipelineTessellationStateCreateInfo tessellation_ci;
    tessellation_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
    tessellation_ci.pNext = nullptr;
    tessellation_ci.flags = 0;
    tessellation_ci.patchControlPoints = ts.patch_control_points;

    VkPipelineViewportStateCreateInfo viewport_ci;
    viewport_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_ci.pNext = nullptr;
    viewport_ci.flags = 0;
    viewport_ci.viewportCount = Maxwell::NumViewports;
    viewport_ci.pViewports = nullptr;
    viewport_ci.scissorCount = Maxwell::NumViewports;
    viewport_ci.pScissors = nullptr;

    VkPipelineRasterizationStateCreateInfo rasterization_ci;
    rasterization_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization_ci.pNext = nullptr;
    rasterization_ci.flags = 0;
    rasterization_ci.depthClampEnable = rs.depth_clamp_enable;
    rasterization_ci.rasterizerDiscardEnable = VK_FALSE;
    rasterization_ci.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization_ci.cullMode =
        rs.cull_enable ? MaxwellToVK::CullFace(rs.cull_face) : VK_CULL_MODE_NONE;
    rasterization_ci.frontFace = MaxwellToVK::FrontFace(rs.front_face);
    rasterization_ci.depthBiasEnable = rs.depth_bias_enable;
    rasterization_ci.depthBiasConstantFactor = 0.0f;
    rasterization_ci.depthBiasClamp = 0.0f;
    rasterization_ci.depthBiasSlopeFactor = 0.0f;
    rasterization_ci.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample_ci;
    multisample_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample_ci.pNext = nullptr;
    multisample_ci.flags = 0;
    multisample_ci.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    multisample_ci.sampleShadingEnable = VK_FALSE;
    multisample_ci.minSampleShading = 0.0f;
    multisample_ci.pSampleMask = nullptr;
    multisample_ci.alphaToCoverageEnable = VK_FALSE;
    multisample_ci.alphaToOneEnable = VK_FALSE;

    VkPipelineDepthStencilStateCreateInfo depth_stencil_ci;
    depth_stencil_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil_ci.pNext = nullptr;
    depth_stencil_ci.flags = 0;
    depth_stencil_ci.depthTestEnable = ds.depth_test_enable;
    depth_stencil_ci.depthWriteEnable = ds.depth_write_enable;
    depth_stencil_ci.depthCompareOp = ds.depth_test_enable
                                          ? MaxwellToVK::ComparisonOp(ds.depth_test_function)
                                          : VK_COMPARE_OP_ALWAYS;
    depth_stencil_ci.depthBoundsTestEnable = ds.depth_bounds_enable;
    depth_stencil_ci.stencilTestEnable = ds.stencil_enable;
    depth_stencil_ci.front = GetStencilFaceState(ds.front_stencil);
    depth_stencil_ci.back = GetStencilFaceState(ds.back_stencil);
    depth_stencil_ci.minDepthBounds = 0.0f;
    depth_stencil_ci.maxDepthBounds = 0.0f;

    std::array<VkPipelineColorBlendAttachmentState, Maxwell::NumRenderTargets> cb_attachments;
    const std::size_t num_attachments =
        std::min(cd.attachments_count, renderpass_params.color_attachments.size());
    for (std::size_t i = 0; i < num_attachments; ++i) {
        static constexpr std::array component_table = {
            VK_COLOR_COMPONENT_R_BIT, VK_COLOR_COMPONENT_G_BIT, VK_COLOR_COMPONENT_B_BIT,
            VK_COLOR_COMPONENT_A_BIT};
        const auto& blend = cd.attachments[i];

        VkColorComponentFlags color_components = 0;
        for (std::size_t j = 0; j < component_table.size(); ++j) {
            if (blend.components[j]) {
                color_components |= component_table[j];
            }
        }

        VkPipelineColorBlendAttachmentState& attachment = cb_attachments[i];
        attachment.blendEnable = blend.enable;
        attachment.srcColorBlendFactor = MaxwellToVK::BlendFactor(blend.src_rgb_func);
        attachment.dstColorBlendFactor = MaxwellToVK::BlendFactor(blend.dst_rgb_func);
        attachment.colorBlendOp = MaxwellToVK::BlendEquation(blend.rgb_equation);
        attachment.srcAlphaBlendFactor = MaxwellToVK::BlendFactor(blend.src_a_func);
        attachment.dstAlphaBlendFactor = MaxwellToVK::BlendFactor(blend.dst_a_func);
        attachment.alphaBlendOp = MaxwellToVK::BlendEquation(blend.a_equation);
        attachment.colorWriteMask = color_components;
    }

    VkPipelineColorBlendStateCreateInfo color_blend_ci;
    color_blend_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend_ci.pNext = nullptr;
    color_blend_ci.flags = 0;
    color_blend_ci.logicOpEnable = VK_FALSE;
    color_blend_ci.logicOp = VK_LOGIC_OP_COPY;
    color_blend_ci.attachmentCount = static_cast<u32>(num_attachments);
    color_blend_ci.pAttachments = cb_attachments.data();
    std::memset(color_blend_ci.blendConstants, 0, sizeof(color_blend_ci.blendConstants));

    static constexpr std::array dynamic_states = {
        VK_DYNAMIC_STATE_VIEWPORT,           VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_DEPTH_BIAS,         VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_DEPTH_BOUNDS,       VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE};

    VkPipelineDynamicStateCreateInfo dynamic_state_ci;
    dynamic_state_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state_ci.pNext = nullptr;
    dynamic_state_ci.flags = 0;
    dynamic_state_ci.dynamicStateCount = static_cast<u32>(dynamic_states.size());
    dynamic_state_ci.pDynamicStates = dynamic_states.data();

    VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT subgroup_size_ci;
    subgroup_size_ci.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT;
    subgroup_size_ci.pNext = nullptr;
    subgroup_size_ci.requiredSubgroupSize = GuestWarpSize;

    std::vector<VkPipelineShaderStageCreateInfo> shader_stages;
    std::size_t module_index = 0;
    for (std::size_t stage = 0; stage < Maxwell::MaxShaderStage; ++stage) {
        if (!program[stage]) {
            continue;
        }
        VkPipelineShaderStageCreateInfo& stage_ci = shader_stages.emplace_back();
        stage_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage_ci.pNext = nullptr;
        stage_ci.flags = 0;
        stage_ci.stage = MaxwellToVK::ShaderStage(static_cast<Tegra::Engines::ShaderType>(stage));
        stage_ci.module = *modules[module_index++];
        stage_ci.pName = "main";
        stage_ci.pSpecializationInfo = nullptr;

        if (program[stage]->entries.uses_warps && device.IsGuestWarpSizeSupported(stage_ci.stage)) {
            stage_ci.pNext = &subgroup_size_ci;
        }
    }

    VkGraphicsPipelineCreateInfo ci;
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.pNext = nullptr;
    ci.flags = 0;
    ci.stageCount = static_cast<u32>(shader_stages.size());
    ci.pStages = shader_stages.data();
    ci.pVertexInputState = &vertex_input_ci;
    ci.pInputAssemblyState = &input_assembly_ci;
    ci.pTessellationState = &tessellation_ci;
    ci.pViewportState = &viewport_ci;
    ci.pRasterizationState = &rasterization_ci;
    ci.pMultisampleState = &multisample_ci;
    ci.pDepthStencilState = &depth_stencil_ci;
    ci.pColorBlendState = &color_blend_ci;
    ci.pDynamicState = &dynamic_state_ci;
    ci.layout = *layout;
    ci.renderPass = renderpass;
    ci.subpass = 0;
    ci.basePipelineHandle = nullptr;
    ci.basePipelineIndex = 0;
    return device.GetLogical().CreateGraphicsPipeline(ci);
}

} // namespace Vulkan
