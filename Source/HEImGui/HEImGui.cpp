#include "HydraEngine/Base.h"
#include <format>

#include <imgui_internal.h>
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

import HE;
import nvrhi;
import std;

using namespace HE;

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

    bool Init(nvrhi::DeviceHandle pDevice)
    {
        device = pDevice;

        nvrhi::CommandListParameters clp;
        clp.enableImmediateExecution = device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11;
        commandList = device->createCommandList(clp);

        {
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
            HE_ASSERT(vertexShader);
            HE_ASSERT(pixelShader);
        }

        nvrhi::VertexAttributeDesc vertexAttribLayout[] = {
            { "POSITION", nvrhi::Format::RG32_FLOAT,  1, 0, offsetof(ImDrawVert,pos), sizeof(ImDrawVert), false },
            { "TEXCOORD", nvrhi::Format::RG32_FLOAT,  1, 0, offsetof(ImDrawVert,uv),  sizeof(ImDrawVert), false },
            { "COLOR",    nvrhi::Format::RGBA8_UNORM, 1, 0, offsetof(ImDrawVert,col), sizeof(ImDrawVert), false },
        };

        shaderAttribLayout = device->createInputLayout(vertexAttribLayout, sizeof(vertexAttribLayout) / sizeof(vertexAttribLayout[0]), vertexShader);

        // create PSO
        {
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
                nvrhi::BindingLayoutItem::PushConstants(0, sizeof(float) * 2),
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
            const auto desc = nvrhi::SamplerDesc()
                .setAllAddressModes(nvrhi::SamplerAddressMode::Wrap)
                .setAllFilters(true);

            fontSampler = device->createSampler(desc);

            if (fontSampler == nullptr)
                return false;
        }

        return true;
    }

    bool Render(nvrhi::IFramebuffer* framebuffer)
    {
        for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
            if (tex->Status != ImTextureStatus_OK)
                UpdateTexture(tex);

        ImDrawData* drawData = ImGui::GetDrawData();
        const auto& io = ImGui::GetIO();

        commandList->open();
        commandList->beginMarker("ImGui");

        if (!UpdateGeometry(commandList))
        {
            commandList->close();
            return false;
        }

        // handle DPI scaling
        drawData->ScaleClipRects(io.DisplayFramebufferScale);

        float invDisplaySize[2] = { 1.f / io.DisplaySize.x, 1.f / io.DisplaySize.y };

        // set up graphics state
        nvrhi::GraphicsState drawState;

        drawState.framebuffer = framebuffer;
        HE_ASSERT(drawState.framebuffer);

        drawState.pipeline = GetPSO(drawState.framebuffer);

        drawState.viewport.viewports.push_back(nvrhi::Viewport(io.DisplaySize.x * io.DisplayFramebufferScale.x, io.DisplaySize.y * io.DisplayFramebufferScale.y));
        drawState.viewport.scissorRects.resize(1);  // updated below

        nvrhi::VertexBufferBinding vbufBinding;
        vbufBinding.buffer = vertexBuffer;
        vbufBinding.slot = 0;
        vbufBinding.offset = 0;
        drawState.vertexBuffers.push_back(vbufBinding);

        drawState.indexBuffer.buffer = indexBuffer;
        drawState.indexBuffer.format = (sizeof(ImDrawIdx) == 2 ? nvrhi::Format::R16_UINT : nvrhi::Format::R32_UINT);
        drawState.indexBuffer.offset = 0;

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
                    drawState.bindings = { GetBindingSet((nvrhi::ITexture*)pCmd->GetTexID()) };
                    HE_ASSERT(drawState.bindings[0]);

                    drawState.viewport.scissorRects[0] = nvrhi::Rect(
                        int(pCmd->ClipRect.x),
                        int(pCmd->ClipRect.z),
                        int(pCmd->ClipRect.y),
                        int(pCmd->ClipRect.w)
                    );

                    nvrhi::DrawArguments drawArguments;
                    drawArguments.vertexCount = pCmd->ElemCount;
                    drawArguments.startIndexLocation = idxOffset;
                    drawArguments.startVertexLocation = vtxOffset;

                    commandList->setGraphicsState(drawState);
                    commandList->setPushConstants(invDisplaySize, sizeof(invDisplaySize));
                    commandList->drawIndexed(drawArguments);
                }

                idxOffset += pCmd->ElemCount;
            }

            vtxOffset += cmdList->VtxBuffer.Size;
        }

        commandList->endMarker();
        commandList->close();
        device->executeCommandList(commandList);

        return true;
    }

    void BackbufferResizing() { pso = nullptr; }

    bool ReallocateBuffer(nvrhi::BufferHandle& buffer, size_t requiredSize, size_t reallocateSize, bool isIndexBuffer)
    {
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
        size_t outRowPitch = 0;
        nvrhi::TextureSlice desTc = {
            .x = x,
            .y = y,
            .width = w,
            .height = h,
        };

        nvrhi::TextureSlice srcTc = {
            .width = w,
            .height = h,
        };

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
        if (tex->Status == ImTextureStatus_WantCreate)
        {
            HE_ASSERT(tex->TexID == 0 && tex->BackendUserData == nullptr);
            HE_ASSERT(tex->Format == ImTextureFormat_RGBA32);

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
            HE_ASSERT(texture);

            auto cl = device->createCommandList();
            cl->open();
            cl->writeTexture(texture, 0, 0, pixels, tex->Width * 4);
            cl->close();
            device->executeCommandList(cl);

            tex->SetTexID(texture);
            tex->Status = ImTextureStatus_OK;

            //HE_INFO("[ImGui] : ImTextureStatus_WantCreate : ({}, {}, {}), {}", tex->UniqueID, tex->Width, tex->Height, (uint64_t)(nvrhi::ITexture*)tex->GetTexID());
        }

        if (tex->Status == ImTextureStatus_WantUpdates || tex->Status == ImTextureStatus_WantCreate)
        {
            //HE_TRACE("[ImGui] : ImTextureStatus_WantUpdates : ({}, {}, {}), {}", tex->UniqueID, tex->Width, tex->Height, (uint64_t)(nvrhi::ITexture*)tex->GetTexID());
            nvrhi::ITexture* texture = (nvrhi::ITexture*)tex->TexID;
            HE_ASSERT(texture);

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
            //HE_ERROR("[ImGui] : ImTextureStatus_WantDestroy : ({}, {}, {}), {}", tex->UniqueID, tex->Width, tex->Height, (uint64_t)(nvrhi::ITexture*)tex->GetTexID());

            nvrhi::TextureHandle texture;
            texture.Attach((nvrhi::ITexture*)tex->GetTexID());

            tex->SetTexID(ImTextureID_Invalid);
            tex->Status = ImTextureStatus_Destroyed;
        }
    }

    nvrhi::IGraphicsPipeline* GetPSO(nvrhi::IFramebuffer* fb)
    {
        if (pso) return pso;

        pso = device->createGraphicsPipeline(basePSODesc, fb);
        HE_ASSERT(pso);

        return pso;
    }

    nvrhi::IBindingSet* GetBindingSet(nvrhi::ITexture* texture)
    {
        if (bindingsCache.contains(texture))
            return bindingsCache.at(texture);

        nvrhi::BindingSetDesc desc;

        desc.bindings = {
            nvrhi::BindingSetItem::PushConstants(0, sizeof(float) * 2),
            nvrhi::BindingSetItem::Texture_SRV(0, texture),
            nvrhi::BindingSetItem::Sampler(0, fontSampler)
        };

        nvrhi::BindingSetHandle binding = device->createBindingSet(desc, bindingLayout);
        HE_ASSERT(binding);

        bindingsCache[texture] = binding;
        return binding;
    }

    bool UpdateGeometry(nvrhi::ICommandList* commandList)
    {
        ImDrawData* drawData = ImGui::GetDrawData();

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

class ImGuiLayer : public Layer
{
public:
    nvrhi::DeviceHandle device;
    bool m_BlockEvents = true;
    ImGuiBackend imGuiBackend;

    ImGuiLayer(nvrhi::DeviceHandle pDevice) :device(pDevice) {}

    void Theme()
    {
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

    virtual void OnAttach() override
    {
        HE_PROFILE_SCOPE("ImGuiLayer::OnAttach");

        ImGui::CreateContext();

        auto& w = Application::GetWindow();

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

        Theme();

        GLFWwindow* window = static_cast<GLFWwindow*>(w.GetWindowHandle());
        ImGui_ImplGlfw_InitForOther(window, true);

        imGuiBackend.Init(device);

        CreateDefultFont();
    }

    virtual void OnDetach() override
    {
        HE_PROFILE_SCOPE("ImGuiLayer::OnDetach");

        for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
        {
            if (tex->RefCount == 1)
            {
                tex->Status = ImTextureStatus_WantDestroy;
                imGuiBackend.UpdateTexture(tex);
            }
        }

        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }

    virtual void OnBegin(const FrameInfo& info)
    {
        HE_PROFILE_SCOPE("ImGuiLayer::OnBegin");

        ImGuiIO& io = ImGui::GetIO();
        auto& w = Application::GetWindow();

        io.DisplaySize = ImVec2((float)w.GetWidth(), (float)w.GetHeight());
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
    }

    virtual void OnEnd(const FrameInfo& info)
    {
        HE_PROFILE_SCOPE_NC("ImGuiLayer::OnEnd", HE_PROFILE_IMGUI);

        ImGui::Render();
        imGuiBackend.Render(info.fb);

        ImGuiIO& io = ImGui::GetIO();

        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }
    }

    virtual void OnEvent(Event& e) override
    {
        HE_PROFILE_SCOPE_NC("ImGuiLayer::OnEvent", HE_PROFILE_IMGUI);

        if (m_BlockEvents)
        {
            ImGuiIO& io = ImGui::GetIO();
            e.Handled |= e.IsInCategory(EventCategoryMouse) & io.WantCaptureMouse;
            e.Handled |= e.IsInCategory(EventCategoryKeyboard) & io.WantCaptureKeyboard;
        }

        EventDispatcher dispatcher(e);

        dispatcher.Dispatch<WindowResizeEvent>([this](Event& e) {

            imGuiBackend.BackbufferResizing();
            return false;

            });

        dispatcher.Dispatch<WindowContentScaleEvent>([this](Event& e) {

            CreateDefultFont();
            return false;

            });
    }

    void BlockEvents(bool block) { m_BlockEvents = block; }
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
