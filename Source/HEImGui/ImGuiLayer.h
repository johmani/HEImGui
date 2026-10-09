#pragma once
#include "Core/Core.h"

namespace ImGuiBackend { struct Data; }

struct ImGuiLayer : public Core::Layer
{
    nvrhi::DeviceHandle device;
    ImGuiBackend::Data* backend;
    bool blockEvents = true;

    ImGuiLayer(nvrhi::DeviceHandle device);
    ~ImGuiLayer();

    void OnAttach() override;
    void OnDetach() override;
    void OnBegin(const Core::FrameInfo& info) override;
    void OnEnd(const Core::FrameInfo& info) override;
    void OnEvent(Core::Event& e) override;
};
