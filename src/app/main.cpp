/*
 Copyright 2019-2020 Google Inc.
 
 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at
 
 http://www.apache.org/licenses/LICENSE-2.0
 
 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
*/

#include "vkex/Application.h"

#include "AssetUtil.h"

#include "AppCore.h"

// TODO: Simplify shader management
// * Move constant buffer structs to another file?
//  * Possibly share structs with shaders?
// * Shared header for stuff like TG dims
// * Provide list of shaders, and structs returned with layouts/sets
//   along with shared descriptor pool

void VkexInfoApp::Configure(const vkex::ArgParser& args, vkex::Configuration& configuration)
{
    configuration.window.resizeable = false;

    // TODO: We need to use UNORM because it supports storage on AMD,
    // but we need to make sure the rest of the render chain correctly
    // handle sRGB-ness through the chain, which they do not right now
    // Alternatively, the final copy could be a graphics blit?
    configuration.swapchain.color_format = VK_FORMAT_B8G8R8A8_UNORM;
    configuration.swapchain.paced_frame_rate = 60;

    // INFO: Right now, Application::InitializeVkexSwapchain() creates a renderpass
    // for the swapchain images based on some of the config bits. Right now, it's
    // hard-coded to load the existing contents, plus all layouts are VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL.
    // This really should be configurable, allowing for different uses without
    // touching the vkex lib code.

    configuration.graphics_debug.enable = false;
    configuration.graphics_debug.message_severity.info = false;
    configuration.graphics_debug.message_severity.warning = false;
    configuration.graphics_debug.message_severity.error = false;
    configuration.graphics_debug.message_type.validation = false;
}

void VkexInfoApp::Setup()
{
    // Geometry data
    vkex::PlatonicSolid::Options cube_options = {};
    cube_options.vertex_colors = true;
    vkex::PlatonicSolid cube = vkex::PlatonicSolid::Cube(cube_options);
    const vkex::VertexBufferData* p_vertex_buffer_data = cube.GetVertexBufferByIndex(0);

    // Geometry draw renderpasses at different resolutions
    {
        vkex::Result vkex_result = vkex::Result::Undefined;
        VKEX_CALL(CreateSimpleRenderPass(GetDevice(),
            GetConfiguration().window.width, GetConfiguration().window.height,
            GetConfiguration().swapchain.color_format,
            VK_FORMAT_D32_SFLOAT,
            &m_target_res_draw.simple_render_pass));
    }
    {
        vkex::Result vkex_result = vkex::Result::Undefined;
        VKEX_CALL(CreateSimpleRenderPass(GetDevice(),
            GetConfiguration().window.width / 2, GetConfiguration().window.height / 2,
            GetConfiguration().swapchain.color_format,
            VK_FORMAT_D32_SFLOAT,
            &m_half_res_draw.simple_render_pass));
    }

    // TODO: Future shader infrastructure
    // 1. create shader programs + descriptor layouts + pipeline layout + pipeline
    // 2. Specify descriptor pool sizes + create pool + allocate descriptors according to requests

    // Shaders
    {
        VKEX_CALL(asset_util::CreateShaderProgram(
            GetDevice(),
            GetAssetPath("shaders/draw_vertex.vs.spv"),
            GetAssetPath("shaders/draw_vertex.ps.spv"),
            &m_simple_draw_shader_program));
    }
    {
        VKEX_CALL(asset_util::CreateShaderProgramCompute(
            GetDevice(),
            GetAssetPath("shaders/copy_texture.cs.spv"),
            &m_scaled_tex_copy_shader_program));
    }

    // Descriptor set layouts
    {
        const vkex::ShaderInterface& shader_interface = m_simple_draw_shader_program->GetInterface();
        vkex::DescriptorSetLayoutCreateInfo create_info = ToVkexCreateInfo(shader_interface.GetSet(0));
        VKEX_CALL(GetDevice()->CreateDescriptorSetLayout(create_info, &m_simple_draw_descriptor_set_layout));
    }
    {
        const vkex::ShaderInterface& shader_interface = m_scaled_tex_copy_shader_program->GetInterface();
        vkex::DescriptorSetLayoutCreateInfo create_info = ToVkexCreateInfo(shader_interface.GetSet(0));
        VKEX_CALL(GetDevice()->CreateDescriptorSetLayout(create_info, &m_scaled_tex_copy_descriptor_set_layout));
    }

    // Pipeline layout
    {
        vkex::PipelineLayoutCreateInfo create_info = {};
        create_info.descriptor_set_layouts.push_back(vkex::ToVulkan(m_simple_draw_descriptor_set_layout));
        vkex::Result vkex_result = vkex::Result::Undefined;
        VKEX_CALL(GetDevice()->CreatePipelineLayout(create_info, &m_simple_draw_pipeline_layout));
    }
    {
        vkex::PipelineLayoutCreateInfo create_info = {};
        create_info.descriptor_set_layouts.push_back(vkex::ToVulkan(m_scaled_tex_copy_descriptor_set_layout));
        vkex::Result vkex_result = vkex::Result::Undefined;
        VKEX_CALL(GetDevice()->CreatePipelineLayout(create_info, &m_scaled_tex_copy_pipeline_layout));
    }

    // Pipeline
    {
        vkex::VertexBindingDescription vertex_binding_descriptions = p_vertex_buffer_data->GetVertexBindingDescription();

        vkex::GraphicsPipelineCreateInfo create_info = {};
        create_info.shader_program = m_simple_draw_shader_program;
        create_info.vertex_binding_descriptions = { vertex_binding_descriptions };
        create_info.samples = VK_SAMPLE_COUNT_1_BIT;
        create_info.depth_test_enable = true;
        create_info.depth_write_enable = true;
        create_info.pipeline_layout = m_simple_draw_pipeline_layout;
        create_info.rtv_formats = { m_target_res_draw.simple_render_pass.rtv->GetFormat() };
        create_info.dsv_format = m_target_res_draw.simple_render_pass.dsv->GetFormat();
        create_info.render_pass = m_target_res_draw.simple_render_pass.render_pass;
        vkex::Result vkex_result = vkex::Result::Undefined;
        VKEX_CALL(GetDevice()->CreateGraphicsPipeline(create_info, &m_simple_draw_pipeline));
    }
    {
        vkex::ComputePipelineCreateInfo create_info = {};
        create_info.shader_program = m_scaled_tex_copy_shader_program;
        create_info.pipeline_layout = m_scaled_tex_copy_pipeline_layout;

        vkex::Result vkex_result = vkex::Result::Undefined;
        VKEX_CALL(GetDevice()->CreateComputePipeline(create_info, &m_scaled_tex_copy_pipeline));
    }

    // Descriptor pool
    {
        const vkex::ShaderInterface& simple_draw_shader_interface = m_simple_draw_shader_program->GetInterface();
        const vkex::ShaderInterface& scaled_tex_copy_interface = m_scaled_tex_copy_shader_program->GetInterface();
        vkex::DescriptorPoolCreateInfo create_info = {};
        create_info.pool_sizes = simple_draw_shader_interface.GetDescriptorPoolSizes();

        auto scaled_tex_copy_descriptor_pool_sizes = scaled_tex_copy_interface.GetDescriptorPoolSizes();
        scaled_tex_copy_descriptor_pool_sizes *= 2; // One for half-res, one for full-res
        create_info.pool_sizes += scaled_tex_copy_descriptor_pool_sizes;

        VKEX_CALL(GetDevice()->CreateDescriptorPool(create_info, &m_descriptor_pool));

        // TODO: Refactor how shaders make their allocation requests for descriptors
        // For draws and upscales, they probably just need one descriptor set in place
        // unless something needs to be updated every frame (then one per swapchain image
        // synchronized on some submit fence). The target -> swapchain copy will need
        // one descriptor per swapchain image, since presents are synchronized on
        // acquire next image
    }

    // Descriptor sets
    {
        vkex::DescriptorSetAllocateInfo allocate_info = {};
        allocate_info.layouts.push_back(m_simple_draw_descriptor_set_layout);
        VKEX_CALL(m_descriptor_pool->AllocateDescriptorSets(allocate_info, &m_simple_draw_descriptor_set));
    }
    {
        vkex::DescriptorSetAllocateInfo allocate_info = {};
        allocate_info.layouts.push_back(m_scaled_tex_copy_descriptor_set_layout);
        VKEX_CALL(m_descriptor_pool->AllocateDescriptorSets(allocate_info, &m_target_res_draw.scaled_tex_copy_descriptor_set));
        VKEX_CALL(m_descriptor_pool->AllocateDescriptorSets(allocate_info, &m_half_res_draw.scaled_tex_copy_descriptor_set));
    }

    // Draw constants buffer, vertex buffer + binding
    // Constant buffer
    {
        VKEX_CALL(asset_util::CreateConstantBuffer(
            m_simple_draw_view_transform_constants.size,
            nullptr,
            GetGraphicsQueue(),
            asset_util::MEMORY_USAGE_CPU_TO_GPU,
            &m_simple_draw_constant_buffer));
    }

    // Vertex buffer
    {
        VKEX_CALL(asset_util::CreateVertexBuffer(
            p_vertex_buffer_data->GetDataSize(),
            p_vertex_buffer_data->GetData(),
            GetGraphicsQueue(),
            asset_util::MEMORY_USAGE_GPU_ONLY,
            &m_simple_draw_vertex_buffer));
    }

    // Update descriptors
    {
        m_simple_draw_descriptor_set->UpdateDescriptor(0, m_simple_draw_constant_buffer);
    }

    // Compute constants + descriptor updates
    {
        // TODO: Maybe set this with initial contents?
        // TODO: If we change to MEMORY_USAGE_GPU_ONLY, we can use vkex::CopyResource
        // and then have multiple constants buffers with this data floating around...
        VKEX_CALL(asset_util::CreateConstantBuffer(
            m_target_res_draw.scaled_tex_copy_dims_constants.size,
            nullptr,
            GetGraphicsQueue(),
            asset_util::MEMORY_USAGE_CPU_TO_GPU,
            &m_target_res_draw.scaled_tex_copy_constant_buffer));
    }
    {
        VKEX_CALL(asset_util::CreateConstantBuffer(
            m_half_res_draw.scaled_tex_copy_dims_constants.size,
            nullptr,
            GetGraphicsQueue(),
            asset_util::MEMORY_USAGE_CPU_TO_GPU,
            &m_half_res_draw.scaled_tex_copy_constant_buffer));
    }

    {
        m_target_res_draw.scaled_tex_copy_dims_constants.data.srcWidth = 1920;
        m_target_res_draw.scaled_tex_copy_dims_constants.data.srcHeight = 1080;
        m_target_res_draw.scaled_tex_copy_dims_constants.data.dstWidth = 1920;
        m_target_res_draw.scaled_tex_copy_dims_constants.data.dstHeight = 1080;

        VKEX_CALL(m_target_res_draw.scaled_tex_copy_constant_buffer->Copy(
            m_target_res_draw.scaled_tex_copy_dims_constants.size,
            &m_target_res_draw.scaled_tex_copy_dims_constants.data));
    }
    {
        m_half_res_draw.scaled_tex_copy_dims_constants.data.srcWidth = 960;
        m_half_res_draw.scaled_tex_copy_dims_constants.data.srcHeight = 540;
        m_half_res_draw.scaled_tex_copy_dims_constants.data.dstWidth = 1920;
        m_half_res_draw.scaled_tex_copy_dims_constants.data.dstHeight = 1080;

        VKEX_CALL(m_half_res_draw.scaled_tex_copy_constant_buffer->Copy(
            m_half_res_draw.scaled_tex_copy_dims_constants.size,
            &m_half_res_draw.scaled_tex_copy_dims_constants.data));
    }

    {
        // TODO: It would be nice to have this grab the binding via the name instead of magically knowing
        // the binding here :p (TBH, all of that could be done offline as well, but whatever)
        m_target_res_draw.scaled_tex_copy_descriptor_set->UpdateDescriptor(0, m_target_res_draw.scaled_tex_copy_constant_buffer);
        m_half_res_draw.scaled_tex_copy_descriptor_set->UpdateDescriptor(0, m_half_res_draw.scaled_tex_copy_constant_buffer);

        m_target_res_draw.scaled_tex_copy_descriptor_set->UpdateDescriptor(1, m_target_res_draw.simple_render_pass.rtv_texture);
        m_half_res_draw.scaled_tex_copy_descriptor_set->UpdateDescriptor(1, m_half_res_draw.simple_render_pass.rtv_texture);
    }

    // TODO: Consider having full res internal image, and use 
    // scissor/viewport to restrict rendered area. Fixed internal size
    // is fine for now, but not really realistic

    // TODO: Create separate internal + target images

    // Image transitions
    // TODO: When we create textures, can we specify an initial layout?
    {
        vkex::Result vkex_result = vkex::Result::Undefined;
        VKEX_CALL(vkex::TransitionImageLayout(GetGraphicsQueue(), 
            m_target_res_draw.simple_render_pass.rtv_texture,
            VK_IMAGE_LAYOUT_UNDEFINED, 
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT));

        vkex_result = vkex::Result::Undefined;
        VKEX_CALL(vkex::TransitionImageLayout(GetGraphicsQueue(),
            m_target_res_draw.simple_render_pass.dsv_texture,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)));
    }
    {
        vkex::Result vkex_result = vkex::Result::Undefined;
        VKEX_CALL(vkex::TransitionImageLayout(GetGraphicsQueue(),
            m_half_res_draw.simple_render_pass.rtv_texture,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT));

        vkex_result = vkex::Result::Undefined;
        VKEX_CALL(vkex::TransitionImageLayout(GetGraphicsQueue(),
            m_half_res_draw.simple_render_pass.dsv_texture,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)));
    }
}

void VkexInfoApp::Update(double frame_elapsed_time)
{
    if (m_internal_res_selector == InternalResolution::Full) {
        m_current_internal_draw = &m_target_res_draw;
    } else if (m_internal_res_selector == InternalResolution::Half) {
        m_current_internal_draw = &m_half_res_draw;
    }
}

void VkexInfoApp::Render(vkex::Application::RenderData* p_data)
{
    auto cmd = p_data->GetCommandBuffer();

    // TODO: Render to all internal resolutions in order to have delta visualization
    auto render_pass = m_current_internal_draw->simple_render_pass.render_pass;

    float3 eye = float3(0, 0, 2);
    float3 center = float3(0, 0, 0);
    float3 up = float3(0, 1, 0);
    float aspect = GetWindowAspect();
    vkex::PerspCamera camera(eye, center, up, 60.0f, aspect);

    float t = GetFrameStartTime();
    float4x4 M = glm::translate(float3(0, 0, 0)) * glm::rotate(t / 2.0f, float3(0, 1, 0)) * glm::rotate(t / 4.0f, float3(1, 0, 0));
    float4x4 V = camera.GetViewMatrix();
    float4x4 P = camera.GetProjectionMatrix();

    m_simple_draw_view_transform_constants.data.ModelViewProjectionMatrix = P * V*M;

    // TODO: These updates to the constant buffer are happening without very much
    // clear, explicit synchronization. Right now, the Application loop uses m_frame_fence
    // to signal the end of the Present command buffer submit. Then Application waits on 
    // ProcessFrameFence. Application is basically synchronizing the CPU on the return of every
    // Present command buffer, which is fine...these are simple samples.
    // Rather, it would be nice if this code allowed for a bit more CPU/GPU overlap.
    // We could move m_frame_fence into PresentData, and make sure we have as many constant buffers
    // as frames. We'd also need to update the descriptor sets every frame.
    VKEX_CALL(m_simple_draw_constant_buffer->Copy(m_simple_draw_view_transform_constants.size, &m_simple_draw_view_transform_constants.data));

    VkClearValue rtv_clear = {};
    VkClearValue dsv_clear = {};
    dsv_clear.depthStencil.depth = 1.0f;
    dsv_clear.depthStencil.stencil = 0xFF;
    std::vector<VkClearValue> clear_values = { rtv_clear, dsv_clear };
    cmd->Begin();
    cmd->CmdBeginRenderPass(render_pass, &clear_values);
    cmd->CmdSetViewport(render_pass->GetFullRenderArea());
    cmd->CmdSetScissor(render_pass->GetFullRenderArea());
    cmd->CmdBindPipeline(m_simple_draw_pipeline);
    cmd->CmdBindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, *m_simple_draw_pipeline_layout, 0, { *m_simple_draw_descriptor_set });
    cmd->CmdBindVertexBuffers(m_simple_draw_vertex_buffer);
    cmd->CmdDraw(36, 1, 0, 0);

    cmd->CmdEndRenderPass();

    // TODO: Perform upscale or visualization to 'final' target here
    // Final target will then be copied to swapchain in Present

    cmd->End();

    SubmitRender(p_data);
}

void VkexInfoApp::Present(vkex::Application::PresentData* p_data)
{
    auto cmd = p_data->GetCommandBuffer();

    auto present_render_pass = p_data->GetRenderPass();
    auto swapchain_image = present_render_pass->GetRtvs()[0]->GetResource()->GetImage();

    {
        // TODO: This isn't really a good way to do this, updating the descriptors on the fly
        // Might as well just have one set of descriptors per swapchain image
        // or maybe two descriptor sets per shader (one for fixed inputs, one for outputs)
        VkDescriptorImageInfo info = {};
        info.sampler = VK_NULL_HANDLE;
        info.imageView = *(present_render_pass->GetRtvs()[0]->GetResource());
        info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        m_current_internal_draw->scaled_tex_copy_descriptor_set->UpdateDescriptors(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 0, 1, &info);

        // TODO: Add UpdateDescriptor helper for ImageViews?
    }
    
    cmd->Begin();

    {
        cmd->CmdTransitionImageLayout(m_current_internal_draw->simple_render_pass.rtv_texture,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        cmd->CmdTransitionImageLayout(swapchain_image->GetVkObject(),
            swapchain_image->GetAspectFlags(),
            0, swapchain_image->GetMipLevels(),
            0, swapchain_image->GetArrayLayers(),
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        cmd->CmdBindPipeline(m_scaled_tex_copy_pipeline);
        cmd->CmdBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *m_scaled_tex_copy_pipeline_layout, 0, { *m_current_internal_draw->scaled_tex_copy_descriptor_set });
        
        // TODO: Select between visualizations or draws
        // TODO: Perhaps we should do the upscale or visualization to another internal
        //       full-res texture, and then do the copy from that to the swapchain.
        //       The reason being that the descriptor set complexity is greatly
        //       simplified if we don't have to worry about the swapchain images
        //       for all dispatches.

        // TODO: Automate (image size / thread group size)
        vkex::uint3 dispatchDims = { 120, 68, 1 };
        cmd->CmdDispatch(dispatchDims.x, dispatchDims.y, dispatchDims.z);

        cmd->CmdTransitionImageLayout(m_current_internal_draw->simple_render_pass.rtv_texture,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

        cmd->CmdTransitionImageLayout(swapchain_image->GetVkObject(),
            swapchain_image->GetAspectFlags(),
            0, swapchain_image->GetMipLevels(),
            0, swapchain_image->GetArrayLayers(),
            VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

        auto swapchain_render_pass = p_data->GetRenderPass();

        VkClearValue rtv_clear = {};
        VkClearValue dsv_clear = {};
        dsv_clear.depthStencil.depth = 1.0f;
        dsv_clear.depthStencil.stencil = 0xFF;
        std::vector<VkClearValue> clear_values = { rtv_clear, dsv_clear };

        cmd->CmdBeginRenderPass(swapchain_render_pass, &clear_values);
        cmd->CmdSetViewport(swapchain_render_pass->GetFullRenderArea());
        cmd->CmdSetScissor(swapchain_render_pass->GetFullRenderArea());
        
        // TODO: Make sure this size is multiplied for current target resolution
        // TODO: Figure out how to change FontSize?

        ImGui::SetNextWindowSize(ImVec2(400, 400), ImGuiCond_Once);
        this->DrawAppInfoGUI();
        this->DrawImGui(cmd);

        cmd->CmdEndRenderPass();

        cmd->CmdTransitionImageLayout(swapchain_image->GetVkObject(),
            swapchain_image->GetAspectFlags(),
            0, swapchain_image->GetMipLevels(),
            0, swapchain_image->GetArrayLayers(),
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }

    cmd->End();

    SubmitPresent(p_data);
}

int main(int argc, char** argv)
{
    // TODO: Drive resolution from input
    VkexInfoApp app;
    vkex::Result vkex_result = app.Run(argc, argv);
    if (!vkex_result) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}