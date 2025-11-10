#include "Core/Core.h"
#include <format>

#include <imgui_internal.h>
#include <ImExtensions/ImGuizmo.h>
#include <backends/imgui_impl_glfw.cpp>

#include "Embeded/fonts/fa-regular-400.h"
#include "Embeded/fonts/fa-solid-900.h"
#include "Embeded/fonts/OpenSans-Bold.h"
#include "Embeded/fonts/OpenSans-Regular.h"

#if NVRHI_HAS_D3D11
#include "Embeded/dxbc/imgui_main_vs.bin.h"
#include "Embeded/dxbc/imgui_main_ps.bin.h"
#endif

#if NVRHI_HAS_D3D12
#include "Embeded/dxil/imgui_main_vs.bin.h"
#include "Embeded/dxil/imgui_main_ps.bin.h"
#endif

#if NVRHI_HAS_VULKAN
#include "Embeded/spirv/imgui_main_vs.bin.h"
#include "Embeded/spirv/imgui_main_ps.bin.h"
#endif

using namespace Core;

#define HE_PROFILE_IMGUI 0x4300FF

//////////////////////////////////////////////////////////////////////////
// ImGui Backend
//////////////////////////////////////////////////////////////////////////

struct ImGuiBackend
{
    nvrhi::DeviceHandle device;
    nvrhi::CommandListHandle commandList;

    nvrhi::ShaderHandle vertexShader;
    nvrhi::ShaderHandle pixelShader;
    nvrhi::InputLayoutHandle shaderAttribLayout;

    nvrhi::SamplerHandle fontSampler;

    nvrhi::BufferHandle vertexBuffer;
    nvrhi::BufferHandle indexBuffer;

    nvrhi::BindingLayoutHandle bindingLayout;
    nvrhi::GraphicsPipelineDesc basePSODesc;

    nvrhi::GraphicsPipelineHandle pso;
    std::unordered_map<nvrhi::ITexture*, nvrhi::BindingSetHandle> bindingsCache;

    std::vector<ImDrawVert> vtxBuffer;
    std::vector<ImDrawIdx> idxBuffer;

    struct PushConstants
    {
        ImVec2 scale;
        ImVec2 translate;
    };

    bool Init(nvrhi::DeviceHandle pDevice)
    {
        CORE_PROFILE_SCOPE_COLOR(HE_PROFILE_IMGUI);

        device = pDevice;

        nvrhi::CommandListParameters clp;
        clp.enableImmediateExecution = device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11;
        commandList = device->createCommandList(clp);

        {
            CORE_PROFILE_SCOPE_NC("Create Shaders", HE_PROFILE_IMGUI);

            nvrhi::ShaderDesc vsDesc;
            vsDesc.shaderType = nvrhi::ShaderType::Vertex;
            vsDesc.debugName = "imgui_vs";
            vsDesc.entryName = "main_vs";

            nvrhi::ShaderDesc psDesc;
            psDesc.shaderType = nvrhi::ShaderType::Pixel;
            psDesc.debugName = "imgui_ps";
            psDesc.entryName = "main_ps";

            vertexShader = RHI::CreateStaticShader(device, STATIC_SHADER(imgui_main_vs), nullptr, vsDesc);
            pixelShader = RHI::CreateStaticShader(device, STATIC_SHADER(imgui_main_ps), nullptr, psDesc);
            CORE_ASSERT(vertexShader);
            CORE_ASSERT(pixelShader);
        }

        {
            CORE_PROFILE_SCOPE_NC("Create Input Layout", HE_PROFILE_IMGUI);

            nvrhi::VertexAttributeDesc vertexAttribLayout[] = {
                { "POSITION", nvrhi::Format::RG32_FLOAT,  1, 0, offsetof(ImDrawVert,pos), sizeof(ImDrawVert), false },
                { "TEXCOORD", nvrhi::Format::RG32_FLOAT,  1, 0, offsetof(ImDrawVert,uv),  sizeof(ImDrawVert), false },
                { "COLOR",    nvrhi::Format::RGBA8_UNORM, 1, 0, offsetof(ImDrawVert,col), sizeof(ImDrawVert), false },
            };

            shaderAttribLayout = device->createInputLayout(vertexAttribLayout, sizeof(vertexAttribLayout) / sizeof(vertexAttribLayout[0]), vertexShader);
        }

        {
            CORE_PROFILE_SCOPE_NC("CreateBindingLayout and set PSO desc", HE_PROFILE_IMGUI);

            nvrhi::BlendState blendState;
            blendState.targets[0].setBlendEnable(true)
                .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
                .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
                .setSrcBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha)
                .setDestBlendAlpha(nvrhi::BlendFactor::Zero);

            auto rasterState = nvrhi::RasterState()
                .setFillSolid()
                .setCullNone()
                .setScissorEnable(true)
                .setDepthClipEnable(true);

            auto depthStencilState = nvrhi::DepthStencilState()
                .disableDepthTest()
                .enableDepthWrite()
                .disableStencil()
                .setDepthFunc(nvrhi::ComparisonFunc::Always);

            nvrhi::RenderState renderState;
            renderState.blendState = blendState;
            renderState.depthStencilState = depthStencilState;
            renderState.rasterState = rasterState;

            nvrhi::BindingLayoutDesc layoutDesc;
            layoutDesc.visibility = nvrhi::ShaderType::All;
            layoutDesc.bindings = {
                nvrhi::BindingLayoutItem::PushConstants(0, sizeof(PushConstants)),
                nvrhi::BindingLayoutItem::Texture_SRV(0),
                nvrhi::BindingLayoutItem::Sampler(0)
            };
            bindingLayout = device->createBindingLayout(layoutDesc);

            basePSODesc.primType = nvrhi::PrimitiveType::TriangleList;
            basePSODesc.inputLayout = shaderAttribLayout;
            basePSODesc.VS = vertexShader;
            basePSODesc.PS = pixelShader;
            basePSODesc.renderState = renderState;
            basePSODesc.bindingLayouts = { bindingLayout };
        }

        {
            CORE_PROFILE_SCOPE("Create Sampler");

            const auto desc = nvrhi::SamplerDesc()
                .setAllAddressModes(nvrhi::SamplerAddressMode::Wrap)
                .setAllFilters(true);

            fontSampler = device->createSampler(desc);

            if (fontSampler == nullptr)
                return false;
        }

        return true;
    }

    bool Render(ImDrawData* drawData, nvrhi::IFramebuffer* framebuffer)
    {
        CORE_PROFILE_SCOPE_COLOR(HE_PROFILE_IMGUI);

        CORE_ASSERT(framebuffer);

        for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
            if (tex->Status != ImTextureStatus_OK)
                UpdateTexture(tex);

        float fbWidth = (float)(drawData->DisplaySize.x * drawData->FramebufferScale.x);
        float fbHeight = (float)(drawData->DisplaySize.y * drawData->FramebufferScale.y);

        commandList->open();
        commandList->beginMarker("ImGui");
        BUILTIN_PROFILE_BEGIN(device, commandList, "ImGui Render");

        if (!UpdateGeometry(drawData, commandList))
        {
            commandList->close();
            return false;
        }

        // handle DPI scaling
        drawData->ScaleClipRects(drawData->FramebufferScale);

        PushConstants pushConstants;
        pushConstants.scale.x = 2 / drawData->DisplaySize.x;
        pushConstants.scale.y = 2 / drawData->DisplaySize.y;
        pushConstants.translate.x = -1 - drawData->DisplayPos.x * pushConstants.scale.x;
        pushConstants.translate.y = -1 - drawData->DisplayPos.y * pushConstants.scale.y;
        
        // set up graphics state
        nvrhi::GraphicsState drawState;
        drawState.framebuffer = framebuffer;
        drawState.pipeline = GetPSO(drawState.framebuffer);
        drawState.viewport.viewports.push_back(nvrhi::Viewport(fbWidth, fbHeight));
        drawState.viewport.scissorRects.resize(1);  // updated below

        nvrhi::VertexBufferBinding vbufBinding;
        vbufBinding.buffer = vertexBuffer;
        vbufBinding.slot = 0;
        vbufBinding.offset = 0;
        drawState.vertexBuffers.push_back(vbufBinding);

        drawState.indexBuffer.buffer = indexBuffer;
        drawState.indexBuffer.format = (sizeof(ImDrawIdx) == 2 ? nvrhi::Format::R16_UINT : nvrhi::Format::R32_UINT);
        drawState.indexBuffer.offset = 0;

        // Will project scissor/clipping rectangles into framebuffer space
        ImVec2 clipOff = drawData->DisplayPos;         // (0,0) unless using multi-viewports
        ImVec2 clipScale = drawData->FramebufferScale; // (1,1) unless using retina display which are often (2,2)

        // render command lists
        int vtxOffset = 0;
        int idxOffset = 0;
        for (int n = 0; n < drawData->CmdListsCount; n++)
        {
            const ImDrawList* cmdList = drawData->CmdLists[n];
            for (int i = 0; i < cmdList->CmdBuffer.Size; i++)
            {
                const ImDrawCmd* pCmd = &cmdList->CmdBuffer[i];

                if (pCmd->UserCallback)
                {
                    pCmd->UserCallback(cmdList, pCmd);
                }
                else
                {
                    drawState.bindings = { GetBindingSet((nvrhi::ITexture*)pCmd->GetTexID())};
                    CORE_ASSERT(drawState.bindings[0]);

                    ImVec2 clipMin((pCmd->ClipRect.x - clipOff.x) * clipScale.x, (pCmd->ClipRect.y - clipOff.y) * clipScale.y);
                    ImVec2 clipMax((pCmd->ClipRect.z - clipOff.x) * clipScale.x, (pCmd->ClipRect.w - clipOff.y) * clipScale.y);

                    if (clipMin.x < 0.0f) { clipMin.x = 0.0f; }
                    if (clipMin.y < 0.0f) { clipMin.y = 0.0f; }
                    if (clipMax.x > fbWidth) { clipMax.x = (float)fbWidth; }
                    if (clipMax.y > fbHeight) { clipMax.y = (float)fbHeight; }
                    if (clipMax.x <= clipMin.x || clipMax.y <= clipMin.y) continue;
                        
                    drawState.viewport.scissorRects[0] = nvrhi::Rect((int)clipMin.x, (int)clipMax.x, (int)clipMin.y, (int)clipMax.y);

                    nvrhi::DrawArguments drawArguments;
                    drawArguments.vertexCount = pCmd->ElemCount;
                    drawArguments.startIndexLocation = pCmd->IdxOffset + idxOffset;
                    drawArguments.startVertexLocation = pCmd->VtxOffset + vtxOffset;

                    commandList->setGraphicsState(drawState);
                    commandList->setPushConstants(&pushConstants, sizeof(PushConstants));
                    commandList->drawIndexed(drawArguments);
                }
            }

            idxOffset += cmdList->IdxBuffer.Size;
            vtxOffset += cmdList->VtxBuffer.Size;
        }

        BUILTIN_PROFILE_END();
        commandList->endMarker();
        commandList->close();
        device->executeCommandList(commandList);

        return true;
    }

    bool ReallocateBuffer(nvrhi::BufferHandle& buffer, size_t requiredSize, size_t reallocateSize, bool isIndexBuffer)
    {
        CORE_PROFILE_SCOPE_COLOR(HE_PROFILE_IMGUI);

        if (buffer == nullptr || size_t(buffer->getDesc().byteSize) < requiredSize)
        {
            nvrhi::BufferDesc desc;
            desc.byteSize = uint32_t(reallocateSize);
            desc.structStride = 0;
            desc.debugName = isIndexBuffer ? "ImGui index buffer" : "ImGui vertex buffer";
            desc.canHaveUAVs = false;
            desc.isVertexBuffer = !isIndexBuffer;
            desc.isIndexBuffer = isIndexBuffer;
            desc.isDrawIndirectArgs = false;
            desc.isVolatile = false;
            desc.initialState = isIndexBuffer ? nvrhi::ResourceStates::IndexBuffer : nvrhi::ResourceStates::VertexBuffer;
            desc.keepInitialState = true;

            buffer = device->createBuffer(desc);

            if (!buffer)
            {
                return false;
            }
        }

        return true;
    }

    void UpdateTextureRegion(
        nvrhi::ICommandList* commandList,
        nvrhi::ITexture* texture,
        uint32_t x, uint32_t y, uint32_t w, uint32_t h,
        uint8_t* bytes
    )
    {
        CORE_PROFILE_SCOPE_COLOR(HE_PROFILE_IMGUI);

        size_t outRowPitch = 0;
        nvrhi::TextureSlice desTc = { .x = x, .y = y, .width = w, .height = h };
        nvrhi::TextureSlice srcTc = { .width = w, .height = h, };

        auto desc = texture->getDesc();
        desc.debugName = "StagingTexture";
        desc.width = w;
        desc.height = h;

        nvrhi::StagingTextureHandle stagingTexture = device->createStagingTexture(desc, nvrhi::CpuAccessMode::Write);

        uint8_t* mapped = (uint8_t*)device->mapStagingTexture(stagingTexture, srcTc, nvrhi::CpuAccessMode::Write, &outRowPitch);

        for (uint32_t row = 0; row < h; ++row)
        {
            std::memcpy(
                mapped + row * outRowPitch,
                bytes + (row * (texture->getDesc().width) * 4),
                outRowPitch
            );
        }
        device->unmapStagingTexture(stagingTexture);

        commandList->copyTexture(texture, desTc, stagingTexture, srcTc);
    }

    void UpdateTexture(ImTextureData* tex)
    {
        CORE_PROFILE_SCOPE_COLOR(HE_PROFILE_IMGUI);

        if (tex->Status == ImTextureStatus_WantCreate)
        {
            CORE_ASSERT(tex->TexID == 0 && tex->BackendUserData == nullptr);
            CORE_ASSERT(tex->Format == ImTextureFormat_RGBA32);

            const void* pixels = tex->GetPixels();

            nvrhi::TextureDesc textureDesc;
            textureDesc.width = tex->Width;
            textureDesc.height = tex->Height;
            textureDesc.format = nvrhi::Format::RGBA8_UNORM;
            textureDesc.isUAV = true;
            textureDesc.initialState = nvrhi::ResourceStates::ShaderResource;
            textureDesc.keepInitialState = true;
            textureDesc.debugName = "ImGui font texture";

            nvrhi::ITexture* texture = device->createTexture(textureDesc).Detach();
            CORE_ASSERT(texture);

            auto cl = device->createCommandList();
            cl->open();
            cl->writeTexture(texture, 0, 0, pixels, tex->Width * 4);
            cl->close();
            device->executeCommandList(cl);

            tex->SetTexID(texture);
            tex->Status = ImTextureStatus_OK;

            //LOG_INFO("[ImGui] : ImTextureStatus_WantCreate : ({}, {}, {}), {}", tex->UniqueID, tex->Width, tex->Height, (uint64_t)(nvrhi::ITexture*)tex->GetTexID());
        }

        if (tex->Status == ImTextureStatus_WantUpdates)
        {
            //LOG_TRACE("[ImGui] : ImTextureStatus_WantUpdates : ({}, {}, {}), {}", tex->UniqueID, tex->Width, tex->Height, (uint64_t)(nvrhi::ITexture*)tex->GetTexID());
            nvrhi::ITexture* texture = (nvrhi::ITexture*)tex->TexID;
            CORE_ASSERT(texture);

            auto cl = device->createCommandList();
            cl->open();

            const int upload_x = (tex->Status == ImTextureStatus_WantCreate) ? 0 : tex->UpdateRect.x;
            const int upload_y = (tex->Status == ImTextureStatus_WantCreate) ? 0 : tex->UpdateRect.y;
            const int upload_w = (tex->Status == ImTextureStatus_WantCreate) ? tex->Width : tex->UpdateRect.w;
            const int upload_h = (tex->Status == ImTextureStatus_WantCreate) ? tex->Height : tex->UpdateRect.h;
            UpdateTextureRegion(cl, texture, upload_x, upload_y, upload_w, upload_h, tex->GetPixelsAt(upload_x, upload_y));

            cl->close();
            device->executeCommandList(cl);
            tex->Status = ImTextureStatus_OK;
        }

        if (tex->Status == ImTextureStatus_WantDestroy)
        {
            //LOG_ERROR("[ImGui] : ImTextureStatus_WantDestroy : ({}, {}, {}), {}", tex->UniqueID, tex->Width, tex->Height, (uint64_t)(nvrhi::ITexture*)tex->GetTexID());

            nvrhi::TextureHandle texture;
            texture.Attach((nvrhi::ITexture*)tex->GetTexID());

            tex->SetTexID(ImTextureID_Invalid);
            tex->Status = ImTextureStatus_Destroyed;
        }
    }

    nvrhi::IGraphicsPipeline* GetPSO(nvrhi::IFramebuffer* fb)
    {
        CORE_PROFILE_SCOPE_COLOR(HE_PROFILE_IMGUI);

        if (pso) return pso;

        pso = device->createGraphicsPipeline(basePSODesc, fb);
        CORE_ASSERT(pso);

        return pso;
    }

    nvrhi::IBindingSet* GetBindingSet(nvrhi::ITexture* texture)
    {
        if (bindingsCache.contains(texture))
            return bindingsCache.at(texture);

        for (auto it = bindingsCache.begin(); it != bindingsCache.end();)
        {
            if (it->first->GetRefCount() == 1)
                it = bindingsCache.erase(it);
            else
                ++it;
        }

        nvrhi::BindingSetDesc desc;

        desc.bindings = {
            nvrhi::BindingSetItem::PushConstants(0, sizeof(PushConstants)),
            nvrhi::BindingSetItem::Texture_SRV(0, texture),
            nvrhi::BindingSetItem::Sampler(0, fontSampler)
        };

        nvrhi::BindingSetHandle binding = device->createBindingSet(desc, bindingLayout);
        CORE_ASSERT(binding);

        bindingsCache[texture] = binding;

        return binding;
    }

    bool UpdateGeometry(ImDrawData* drawData, nvrhi::ICommandList* commandList)
    {
        CORE_PROFILE_SCOPE_COLOR(HE_PROFILE_IMGUI);

        // create/resize vertex and index buffers if needed
        if (!ReallocateBuffer(vertexBuffer, drawData->TotalVtxCount * sizeof(ImDrawVert), (drawData->TotalVtxCount + 5000) * sizeof(ImDrawVert), false))
            return false;


        if (!ReallocateBuffer(indexBuffer, drawData->TotalIdxCount * sizeof(ImDrawIdx), (drawData->TotalIdxCount + 5000) * sizeof(ImDrawIdx), true))
            return false;

        vtxBuffer.resize(vertexBuffer->getDesc().byteSize / sizeof(ImDrawVert));
        idxBuffer.resize(indexBuffer->getDesc().byteSize / sizeof(ImDrawIdx));

        // copy and convert all vertices into a single contiguous buffer
        ImDrawVert* vtxDst = &vtxBuffer[0];
        ImDrawIdx* idxDst = &idxBuffer[0];

        for (int n = 0; n < drawData->CmdListsCount; n++)
        {
            const ImDrawList* cmdList = drawData->CmdLists[n];

            memcpy(vtxDst, cmdList->VtxBuffer.Data, cmdList->VtxBuffer.Size * sizeof(ImDrawVert));
            memcpy(idxDst, cmdList->IdxBuffer.Data, cmdList->IdxBuffer.Size * sizeof(ImDrawIdx));

            vtxDst += cmdList->VtxBuffer.Size;
            idxDst += cmdList->IdxBuffer.Size;
        }

        commandList->writeBuffer(vertexBuffer, &vtxBuffer[0], vertexBuffer->getDesc().byteSize);
        commandList->writeBuffer(indexBuffer, &idxBuffer[0], indexBuffer->getDesc().byteSize);

        return true;
    }
};

//////////////////////////////////////////////////////////////////////////
// ImGui Layer
//////////////////////////////////////////////////////////////////////////

struct ViewportData
{
    Scope<SwapChain> sc;
};

struct ImGuiLayer : public Layer
{
    nvrhi::DeviceHandle device;
    bool blockEvents = true;
    ImGuiBackend imGuiBackend;

    ImGuiLayer(nvrhi::DeviceHandle pDevice) :device(pDevice) {}

    void Theme()
    {
        CORE_PROFILE_SCOPE_COLOR(HE_PROFILE_IMGUI);

        ImGuiStyle& style = ImGui::GetStyle();
        ImVec4* colors = ImGui::GetStyle().Colors;

        colors[ImGuiCol_Text] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
        colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
        colors[ImGuiCol_WindowBg] = ImVec4(0.18f, 0.18f, 0.18f, 1.0f);
        colors[ImGuiCol_ChildBg] = ImVec4(0.239216f, 0.239216f, 0.239216f, 1.0f);
        colors[ImGuiCol_PopupBg] = ImVec4(0.094118f, 0.094118f, 0.094118f, 1.00f);
        colors[ImGuiCol_Border] = ImVec4(0.05f, 0.05f, 0.05f, 1.0f);
        colors[ImGuiCol_BorderShadow] = ImVec4(0.05f, 0.05f, 0.05f, 1.0f);
        colors[ImGuiCol_FrameBg] = ImVec4(0.329412f, 0.329412f, 0.329412f, 1.0f);
        colors[ImGuiCol_FrameBgHovered] = ImVec4(0.429412f, 0.429412f, 0.429412f, 1.0f);
        colors[ImGuiCol_FrameBgActive] = ImVec4(0.133333f, 0.133333f, 0.133333f, 1.00f);
        colors[ImGuiCol_TitleBg] = ImVec4(0.113725f, 0.113725f, 0.113725f, 1.00f);
        colors[ImGuiCol_TitleBgActive] = ImVec4(0.113725f, 0.113725f, 0.113725f, 1.00f);
        colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.113725f, 0.113725f, 0.113725f, 1.00f);
        colors[ImGuiCol_MenuBarBg] = ImVec4(0.113725f, 0.113725f, 0.113725f, 1.00f);
        colors[ImGuiCol_ScrollbarBg] = ImVec4(0.18f, 0.18f, 0.18f, 1.0f);
        colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.329412f, 0.329412f, 0.329412f, 1.0f);
        colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.429412f, 0.429412f, 0.429412f, 1.0f);
        colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.278431f, 0.447059f, 0.701961f, 1.00f);
        colors[ImGuiCol_CheckMark] = ImVec4(0.278431f, 0.447059f, 0.701961f, 1.00f);
        colors[ImGuiCol_SliderGrab] = ImVec4(0.278431f, 0.547059f, 0.801961f, 1.00f);
        colors[ImGuiCol_SliderGrabActive] = ImVec4(0.278431f, 0.447059f, 0.701961f, 1.00f);
        colors[ImGuiCol_Button] = ImVec4(0.329412f, 0.329412f, 0.329412f, 1.0f);
        colors[ImGuiCol_ButtonHovered] = ImVec4(0.429412f, 0.429412f, 0.429412f, 1.0f);
        colors[ImGuiCol_ButtonActive] = ImVec4(0.278431f, 0.447059f, 0.701961f, 1.00f);
        colors[ImGuiCol_Header] = ImVec4(0.329412f, 0.329412f, 0.329412f, 1.0f);
        colors[ImGuiCol_HeaderHovered] = ImVec4(0.429412f, 0.429412f, 0.429412f, 1.0f);
        colors[ImGuiCol_HeaderActive] = ImVec4(0.278431f, 0.447059f, 0.701961f, 1.00f);
        colors[ImGuiCol_Separator] = ImVec4(0.329412f, 0.329412f, 0.329412f, 1.0f);
        colors[ImGuiCol_SeparatorHovered] = ImVec4(0.429412f, 0.429412f, 0.429412f, 1.0f);
        colors[ImGuiCol_SeparatorActive] = ImVec4(0.278431f, 0.447059f, 0.701961f, 1.00f);
        colors[ImGuiCol_ResizeGrip] = ImVec4(0.329412f, 0.329412f, 0.329412f, 1.0f);
        colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.429412f, 0.429412f, 0.429412f, 1.0f);
        colors[ImGuiCol_ResizeGripActive] = ImVec4(0.278431f, 0.447059f, 0.701961f, 1.00f);
        colors[ImGuiCol_Tab] = ImVec4(0.213725f, 0.213725f, 0.213725f, 1.0f);
        colors[ImGuiCol_TabHovered] = ImVec4(0.429412f, 0.429412f, 0.429412f, 1.0f);
        colors[ImGuiCol_TabSelected] = ImVec4(0.288235f, 0.288235f, 0.288235f, 1.00f);
        colors[ImGuiCol_TabSelectedOverline] = colors[ImGuiCol_HeaderActive];
        colors[ImGuiCol_TabDimmed] = ImLerp(colors[ImGuiCol_Tab], colors[ImGuiCol_TitleBg], 0.80f);
        colors[ImGuiCol_TabDimmedSelected] = ImLerp(colors[ImGuiCol_TabSelected], colors[ImGuiCol_TitleBg], 0.40f);
        colors[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.50f, 0.50f, 0.50f, 0.00f);
        colors[ImGuiCol_DockingPreview] = colors[ImGuiCol_HeaderActive] * ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
        colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
        colors[ImGuiCol_PlotLines] = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
        colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
        colors[ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
        colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
        colors[ImGuiCol_TableHeaderBg] = ImVec4(0.19f, 0.19f, 0.20f, 1.00f);
        colors[ImGuiCol_TableBorderStrong] = ImVec4(0.31f, 0.31f, 0.35f, 1.00f);   // Prefer using Alpha=1.0 here
        colors[ImGuiCol_TableBorderLight] = ImVec4(0.23f, 0.23f, 0.25f, 1.00f);   // Prefer using Alpha=1.0 here
        colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);
        colors[ImGuiCol_TextLink] = ImVec4(0.26f, 0.59f, 0.98f, 1.0f);
        colors[ImGuiCol_TextSelectedBg] = ImVec4(0.26f, 0.59f, 0.98f, 0.35f);
        colors[ImGuiCol_DragDropTarget] = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
        colors[ImGuiCol_NavCursor] = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
        colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
        colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
        colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);

        style.WindowRounding = 3.0f;
        style.ChildRounding = 3.0f;
        style.FramePadding = ImVec2(4, 4);
        style.FrameRounding = 2.0f;
        style.PopupRounding = 2.0f;
        style.GrabRounding = 3.0f;
        style.TabRounding = 3.0f;
        style.HoverStationaryDelay = 0.4f;
        style.WindowBorderSize = 1.0f;
        style.ScrollbarSize = 10.0f;
        style.ScrollbarRounding = 4.0f;
        style.FrameBorderSize = 0.0f;
    }

    void CreateDefultFont()
    {
        CORE_PROFILE_SCOPE_COLOR(HE_PROFILE_IMGUI);

        ImGuiIO& io = ImGui::GetIO();

        auto& w = Application::GetWindow();
        auto [sx, sy] = w.GetWindowContentScale();

        {
            float fontSize = 16.0f;

            const ImWchar min_fa = 0xe005;
            const ImWchar max_fa = 0xf8ff;

            {
                ImFontConfig config;

                config.FontDataOwnedByAtlas = false;
                config.SizePixels = fontSize * sx;
                strcpy_s(config.Name, "OpenSans-Regular + icons");
                io.FontDefault = io.Fonts->AddFontFromMemoryCompressedTTF((void*)OpenSans_Regular_compressed_data, OpenSans_Regular_compressed_size, 0, &config);

                {
                    // Icons Fonts
                    config.MergeMode = true;
                    config.GlyphMinAdvanceX = 13.0f;
                    const ImWchar icon_ranges[] = { min_fa, max_fa, 0 };
                    io.Fonts->AddFontFromMemoryCompressedTTF((void*)fa_regular_400_compressed_data, fa_regular_400_compressed_size, 0, &config, icon_ranges);
                    io.Fonts->AddFontFromMemoryCompressedTTF((void*)fa_solid_900_compressed_data, fa_solid_900_compressed_size, 0, &config, icon_ranges);
                }
            }

            {
                ImFontConfig config;
                config.FontDataOwnedByAtlas = false;
                config.SizePixels = fontSize * sx;
                strcpy_s(config.Name, "OpenSans-Bold");
                io.Fonts->AddFontFromMemoryCompressedTTF((void*)OpenSans_Bold_compressed_data, OpenSans_Bold_compressed_size, 0, &config);

                // Icons Fonts
                config.MergeMode = true;
                config.GlyphMinAdvanceX = 13.0f;
                const ImWchar icon_ranges[] = { min_fa, max_fa, 0 };
                io.Fonts->AddFontFromMemoryCompressedTTF((void*)fa_regular_400_compressed_data, fa_regular_400_compressed_size, 0, &config, icon_ranges);
                io.Fonts->AddFontFromMemoryCompressedTTF((void*)fa_solid_900_compressed_data, fa_solid_900_compressed_size, 0, &config, icon_ranges);
            }
        }

        io.DisplayFramebufferScale.x = sx;
        io.DisplayFramebufferScale.y = sy;

        ImGui::GetStyle() = ImGuiStyle();
        ImGui::GetStyle().ScaleAllSizes(sx);
        Theme();
    }

    void OnAttach() override
    {
        CORE_PROFILE_SCOPE_NC("ImGuiLayer::OnAttach", HE_PROFILE_IMGUI);

        ImGui::CreateContext();

        auto& w = Application::GetWindow();

        ImGuiIO& io = ImGui::GetIO();
        io.BackendRendererUserData = &imGuiBackend;
        io.BackendRendererName = "HEImGui-NVRHI";
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;
        io.BackendFlags |= ImGuiBackendFlags_PlatformHasViewports;
        io.ConfigDockingTransparentPayload = true;
        //io.ConfigViewportsNoDecoration = false;

        Theme();

        GLFWwindow* window = static_cast<GLFWwindow*>(w.handle);
        ImGui_ImplGlfw_InitForOther(window, true);

        ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
        if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            IM_ASSERT(platform_io.Platform_CreateVkSurface != nullptr && "Platform needs to setup the CreateVkSurface handler.");

            platform_io.Renderer_CreateWindow = [](ImGuiViewport* viewport) {

                CORE_PROFILE_SCOPE_NC("Renderer_CreateWindow", HE_PROFILE_IMGUI);

                ViewportData* data = IM_NEW(ViewportData)();
                viewport->RendererUserData = data;

                SwapChainDesc swapChainDesc = Application::GetWindow().desc.swapChainDesc;
                auto ptr = RHI::GetDeviceManager()->CreateSwapChain(swapChainDesc, viewport->PlatformHandle);
               
                std::unique_ptr<SwapChain> scope(ptr);
                data->sc = std::move(scope);
            };

            platform_io.Renderer_DestroyWindow = [](ImGuiViewport* viewport) {

                CORE_PROFILE_SCOPE_NC("Renderer_DestroyWindow", HE_PROFILE_IMGUI);

                ViewportData* data = (ViewportData*)viewport->RendererUserData;
                IM_DELETE(data);
                viewport->RendererUserData = nullptr;
            };

            platform_io.Renderer_SetWindowSize = [](ImGuiViewport* viewport, ImVec2 size) {

                CORE_PROFILE_SCOPE_NC("Renderer_SetWindowSize", HE_PROFILE_IMGUI);

                ViewportData* data = (ViewportData*)viewport->RendererUserData;
            };

            platform_io.Renderer_RenderWindow = [](ImGuiViewport* viewport, void* backend) {

                CORE_PROFILE_SCOPE_NC("Renderer_RenderWindow", HE_PROFILE_IMGUI);

                ViewportData* data = (ViewportData*)viewport->RendererUserData;
                ImGuiBackend* imGuiBackend = (ImGuiBackend*)ImGui::GetIO().BackendRendererUserData;

                data->sc->UpdateSize();
                data->sc->BeginFrame();

                imGuiBackend->Render(viewport->DrawData, data->sc->GetCurrentFramebuffer());
            };

            platform_io.Renderer_SwapBuffers = [](ImGuiViewport* viewport, void*) {

                CORE_PROFILE_SCOPE_NC("Renderer_SwapBuffers", HE_PROFILE_IMGUI);

                ViewportData* data = (ViewportData*)viewport->RendererUserData;
                data->sc->Present();
            };
        }

        imGuiBackend.Init(device);

        CreateDefultFont();
    }

    void OnDetach() override
    {
        CORE_PROFILE_SCOPE_NC("ImGuiLayer::OnDetach", HE_PROFILE_IMGUI);

        for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
        {
            if (tex->RefCount == 1)
            {
                tex->Status = ImTextureStatus_WantDestroy;
                imGuiBackend.UpdateTexture(tex);
            }
        }

        ImGui::GetIO().BackendRendererUserData = nullptr;
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }

    void OnBegin(const FrameInfo& info) override
    {
        CORE_PROFILE_SCOPE_NC("ImGuiLayer::OnBegin", HE_PROFILE_IMGUI);

        ImGuiIO& io = ImGui::GetIO();
        auto& w = Application::GetWindow();

        io.DisplaySize = ImVec2((float)w.GetWidth(), (float)w.GetHeight());
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGuizmo::BeginFrame();
    }

    void OnEnd(const FrameInfo& info) override
    {
        CORE_PROFILE_SCOPE_NC("ImGuiLayer::OnEnd", HE_PROFILE_IMGUI);

        {
            BUILTIN_PROFILE_CPU("ImGui");
            ImGui::Render();
            imGuiBackend.Render(ImGui::GetMainViewport()->DrawData, info.fb);
        }

        ImGuiIO& io = ImGui::GetIO();

        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            {
                CORE_PROFILE_SCOPE_NC("ImGui::UpdatePlatformWindows", HE_PROFILE_IMGUI);
                ImGui::UpdatePlatformWindows();
            }
            
            {
                CORE_PROFILE_SCOPE_NC("ImGui::RenderPlatformWindowsDefault", HE_PROFILE_IMGUI);
                ImGui::RenderPlatformWindowsDefault();
            }
        }
    }

    void OnEvent(Event& e) override
    {
        CORE_PROFILE_SCOPE_NC("ImGuiLayer::OnEvent", HE_PROFILE_IMGUI);

        if (blockEvents)
        {
            ImGuiIO& io = ImGui::GetIO();
            e.handled |= e.GetCategory() == Core::EventCategory::Keyboard && io.WantCaptureKeyboard;
            e.handled |= e.GetCategory() == Core::EventCategory::Mouse && io.WantCaptureMouse;
        }

        DispatchEvent<WindowContentScaleEvent>(e, [this](WindowContentScaleEvent& e) {

            CreateDefultFont();
            return false;
        });
    }
};

static ImGuiLayer* g_imGuiLayer = nullptr;

EXPORT void OnModuleLoaded()
{
    g_imGuiLayer = new ImGuiLayer(RHI::GetDevice());
    Application::PushOverlay(g_imGuiLayer);
}

EXPORT void OnModuleShutdown()
{
    Application::PopOverlay(g_imGuiLayer);
    delete g_imGuiLayer;
}
