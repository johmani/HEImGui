struct Constants
{
    float2 scale;
    float2 translate;
};

#ifdef SPIRV
    [[vk::push_constant]] ConstantBuffer<Constants> g_Const;
#else
    cbuffer g_Const : register(b0) { Constants g_Const; }
#endif

struct VertexInput
{
    float2 position : POSITION;
    float2 uv : TEXCOORD;
    float4 color : COLOR;
};

struct PixelInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
    float4 color : COLOR;
};


PixelInput main_vs(VertexInput input)
{
    PixelInput output;
    output.position.xy = input.position.xy * g_Const.scale + g_Const.translate;
    output.position.y *= -1;
    output.position.zw = float2(0, 1);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

sampler sampler0 : register(s0);
Texture2D texture0 : register(t0);

float4 main_ps(PixelInput input) : SV_Target
{
    return input.color * texture0.Sample(sampler0, input.uv);
}
