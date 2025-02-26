//#define NOMINMAX

#include "HydraEngine/Base.h"
#include <format>

#include <backends/imgui_impl_glfw.cpp>
#include <extensions/imnodes/imnodes.h>

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

import HydraEngine;
import nvrhi;
import std;

using namespace HydraEngine;

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

	nvrhi::TextureHandle fontTexture;
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

		commandList = device->createCommandList();
		commandList->open();

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

		if (!CreateFontTexture(commandList))
			return false;

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

		commandList->close();
		device->executeCommandList(commandList);

		return true;
	}

	bool Render(nvrhi::IFramebuffer* framebuffer)
	{
		ImDrawData* drawData = ImGui::GetDrawData();
		const auto& io = ImGui::GetIO();

		commandList->open();
		commandList->beginMarker("ImGUI");

		if (!UpdateGeometry(commandList))
			return false;

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
					drawState.bindings = { GetBindingSet((nvrhi::ITexture*)pCmd->TextureId) };
					HE_ASSERT(drawState.bindings[0]);

					drawState.viewport.scissorRects[0] = nvrhi::Rect(int(pCmd->ClipRect.x),
						int(pCmd->ClipRect.z),
						int(pCmd->ClipRect.y),
						int(pCmd->ClipRect.w));

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

private:

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

	bool CreateFontTexture(nvrhi::ICommandList* commandList)
	{
		ImGuiIO& io = ImGui::GetIO();
		unsigned char* pixels;
		int width, height;

		io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

		{
			nvrhi::TextureDesc desc;
			desc.width = width;
			desc.height = height;
			desc.format = nvrhi::Format::RGBA8_UNORM;
			desc.debugName = "ImGui font texture";

			fontTexture = device->createTexture(desc);

			commandList->beginTrackingTextureState(fontTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);

			if (fontTexture == nullptr)
				return false;

			commandList->writeTexture(fontTexture, 0, 0, pixels, width * 4);

			commandList->setPermanentTextureState(fontTexture, nvrhi::ResourceStates::ShaderResource);
			commandList->commitBarriers();

			io.Fonts->TexID = fontTexture;
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

	nvrhi::IGraphicsPipeline* GetPSO(nvrhi::IFramebuffer* fb)
	{
		if (pso)
		{
			return pso;
		}

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

		nvrhi::BindingSetHandle binding;
		binding = device->createBindingSet(desc, bindingLayout);
		HE_ASSERT(binding);

		bindingsCache[texture] = binding;
		return binding;
	}

	bool UpdateGeometry(nvrhi::ICommandList* commandList)
	{
		ImDrawData* drawData = ImGui::GetDrawData();

		// create/resize vertex and index buffers if needed
		if (!ReallocateBuffer(vertexBuffer, drawData->TotalVtxCount * sizeof(ImDrawVert), (drawData->TotalVtxCount + 5000) * sizeof(ImDrawVert), false))
		{
			return false;
		}

		if (!ReallocateBuffer(indexBuffer, drawData->TotalIdxCount * sizeof(ImDrawIdx), (drawData->TotalIdxCount + 5000) * sizeof(ImDrawIdx), true))
		{
			return false;
		}

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
		ImVec4* colors = style.Colors;

		colors[ImGuiCol_Text] = ImVec4(1.000f, 1.000f, 1.000f, 1.000f);
		colors[ImGuiCol_TextDisabled] = ImVec4(0.500f, 0.500f, 0.500f, 1.000f);
		colors[ImGuiCol_WindowBg] = ImVec4(0.090f, 0.090f, 0.090f, 1.000f);
		colors[ImGuiCol_ChildBg] = ImVec4(0.150f, 0.150f, 0.150f, 1.000f);
		colors[ImGuiCol_PopupBg] = ImVec4(0.110f, 0.110f, 0.110f, 1.000f);
		colors[ImGuiCol_Border] = ImVec4(0.200f, 0.200f, 0.200f, 1.000f);
		colors[ImGuiCol_BorderShadow] = ImVec4(0.000f, 0.000f, 0.000f, 0.000f);
		colors[ImGuiCol_FrameBg] = ImVec4(0.200f, 0.200f, 0.200f, 1.000f);
		colors[ImGuiCol_FrameBgHovered] = ImVec4(0.250f, 0.250f, 0.250f, 1.000f);
		colors[ImGuiCol_FrameBgActive] = ImVec4(0.300f, 0.300f, 0.300f, 1.000f);
		colors[ImGuiCol_TitleBg] = ImVec4(0.090f, 0.090f, 0.090f, 1.000f);
		colors[ImGuiCol_TitleBgActive] = ImVec4(0.150f, 0.150f, 0.150f, 1.000f);
		colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.050f, 0.050f, 0.050f, 1.000f);
		colors[ImGuiCol_MenuBarBg] = ImVec4(0.100f, 0.100f, 0.100f, 1.000f);
		colors[ImGuiCol_ScrollbarBg] = ImVec4(0.090f, 0.090f, 0.090f, 1.000f);
		colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.200f, 0.200f, 0.200f, 1.000f);
		colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.250f, 0.250f, 0.250f, 1.000f);
		colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.300f, 0.300f, 0.300f, 1.000f);
		colors[ImGuiCol_CheckMark] = ImVec4(0.500f, 1.000f, 0.500f, 1.000f);
		colors[ImGuiCol_SliderGrab] = ImVec4(0.400f, 0.400f, 0.400f, 1.000f);
		colors[ImGuiCol_SliderGrabActive] = ImVec4(0.600f, 0.600f, 0.600f, 1.000f);
		colors[ImGuiCol_Button] = ImVec4(0.150f, 0.150f, 0.150f, 1.000f);
		colors[ImGuiCol_ButtonHovered] = ImVec4(0.200f, 0.200f, 0.200f, 1.000f);
		colors[ImGuiCol_ButtonActive] = ImVec4(0.250f, 0.250f, 0.250f, 1.000f);
		colors[ImGuiCol_Header] = ImVec4(0.200f, 0.200f, 0.200f, 1.000f);
		colors[ImGuiCol_HeaderHovered] = ImVec4(0.250f, 0.250f, 0.250f, 1.000f);
		colors[ImGuiCol_HeaderActive] = ImVec4(0.300f, 0.300f, 0.300f, 1.000f);
		colors[ImGuiCol_Separator] = ImVec4(0.200f, 0.200f, 0.200f, 1.000f);
		colors[ImGuiCol_SeparatorHovered] = ImVec4(0.250f, 0.250f, 0.250f, 1.000f);
		colors[ImGuiCol_SeparatorActive] = ImVec4(0.300f, 0.300f, 0.300f, 1.000f);
		colors[ImGuiCol_ResizeGrip] = ImVec4(0.200f, 0.200f, 0.200f, 1.000f);
		colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.250f, 0.250f, 0.250f, 1.000f);
		colors[ImGuiCol_ResizeGripActive] = ImVec4(0.300f, 0.300f, 0.300f, 1.000f);
		colors[ImGuiCol_Tab] = ImVec4(0.150f, 0.150f, 0.150f, 1.000f);
		colors[ImGuiCol_TabHovered] = ImVec4(0.200f, 0.200f, 0.200f, 1.000f);
		colors[ImGuiCol_TabActive] = ImVec4(0.250f, 0.250f, 0.250f, 1.000f);
		colors[ImGuiCol_TabUnfocused] = ImVec4(0.100f, 0.100f, 0.100f, 1.000f);
		colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.150f, 0.150f, 0.150f, 1.000f);
		colors[ImGuiCol_DockingPreview] = ImVec4(0.500f, 0.500f, 1.000f, 0.500f);
		colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.100f, 0.100f, 0.100f, 1.000f);
		colors[ImGuiCol_PlotLines] = ImVec4(1.000f, 1.000f, 1.000f, 1.000f);
		colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.000f, 0.500f, 0.500f, 1.000f);
		colors[ImGuiCol_PlotHistogram] = ImVec4(1.000f, 1.000f, 0.000f, 1.000f);
		colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.000f, 0.500f, 0.000f, 1.000f);
		colors[ImGuiCol_TextSelectedBg] = ImVec4(0.500f, 1.000f, 0.500f, 0.500f);
		colors[ImGuiCol_DragDropTarget] = ImVec4(1.000f, 1.000f, 0.000f, 1.000f);
		colors[ImGuiCol_NavHighlight] = ImVec4(0.500f, 0.500f, 1.000f, 1.000f);
		colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.000f, 1.000f, 1.000f, 1.000f);
		colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.000f, 0.000f, 0.000f, 0.500f);
		colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.000f, 0.000f, 0.000f, 0.500f);

		style.WindowRounding = 4.0f;
		style.ChildRounding = 4.0f;
		style.FrameRounding = 4.0f;
		style.PopupRounding = 4.0f;
		style.ScrollbarRounding = 4.0f;
		style.GrabRounding = 4.0f;
		style.TabRounding = 4.0f;
	}

	virtual void OnAttach() override
	{
		ImGui::CreateContext();
		ImNodes::CreateContext();

		ImGuiIO& io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;  // Enable Gamepad Controls
		io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;     // Enable Docking
		io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;   // Enable Multi-Viewport / Platform Windows

		float fontSize = 18.0f;

		{
			ImFontConfig config;
			strcpy_s(config.Name, "OpenSans-Regular");
			config.FontDataOwnedByAtlas = false;
			io.FontDefault = io.Fonts->AddFontFromMemoryTTF((void*)OpenSans_Regular_ttf, sizeof(OpenSans_Regular_ttf), fontSize, &config);
		}

		ImGuiStyle& style = ImGui::GetStyle();
		if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
		{
			style.WindowRounding = 0.0f;
			style.Colors[ImGuiCol_WindowBg].w = 1.0f;
			style.HoverStationaryDelay = 0.7f;
		}

		Theme();

		auto& w = GetAppContext().mainWindow;
		GLFWwindow* window = static_cast<GLFWwindow*>(w.GetWindowHandle());
		ImGui_ImplGlfw_InitForOther(window, true);

		imGuiBackend.Init(device);
	}

	virtual void OnDetach() override
	{
		ImGui_ImplGlfw_Shutdown();
		ImGui::DestroyContext();
		ImNodes::DestroyContext();
	}

	virtual void OnBegin(const FrameInfo& info)
	{
		ImGuiIO& io = ImGui::GetIO();
		auto& w = GetAppContext().mainWindow;
		io.DisplaySize = ImVec2((float)w.GetWidth(), (float)w.GetHeight());
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();
	}

	virtual void OnEnd(const FrameInfo& info)
	{
		HE_PROFILE_SCOPE_NC("ImGuiLayer::End", HE_PROFILE_IMGUI);

		ImGui::Render();
		imGuiBackend.Render(info.fb);

		ImGuiIO& io = ImGui::GetIO();

		if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
		{
			ImGui::UpdatePlatformWindows();
			ImGui::RenderPlatformWindowsDefault();
		}
	}

	bool OnWindowResize(WindowResizeEvent& e)
	{
		imGuiBackend.BackbufferResizing();
		return false;
	}

	virtual void OnEvent(Event& e) override
	{
		if (m_BlockEvents)
		{
			ImGuiIO& io = ImGui::GetIO();
			e.Handled |= e.IsInCategory(EventCategoryMouse) & io.WantCaptureMouse;
			e.Handled |= e.IsInCategory(EventCategoryKeyboard) & io.WantCaptureKeyboard;
		}

		EventDispatcher dispatcher(e);
		dispatcher.Dispatch<WindowResizeEvent>(HE_BIND_EVENT_FN(ImGuiLayer::OnWindowResize));
	}

	void BlockEvents(bool block) { m_BlockEvents = block; }
};

static ImGuiLayer* g_imGuiLayer = nullptr;

EXPORT void OnModuleLoaded()
{
	HE_TRACE("ImGui Layer: OnModuleLoaded");
	g_imGuiLayer = new ImGuiLayer(RHI::GetDevice());
	Application::PushOverlay(g_imGuiLayer);
}

EXPORT void OnModuleShutdown()
{
	HE_TRACE("ImGuiLayer: OnModuleShutdown");
	Application::PopOverlay(g_imGuiLayer);
	delete g_imGuiLayer;
}