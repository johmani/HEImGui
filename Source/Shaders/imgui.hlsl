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

static const float c_DarkTextBoost = 2.4;

float4 main_ps(PixelInput input) : SV_Target
{
    const float4 tex = texture0.Sample(sampler0, input.uv);

    const float coverageIn = saturate(tex.a);
    const float lum = dot(input.color.rgb, float3(0.299, 0.587, 0.114));
    const float darkCoverage = 1.0 - pow(1.0 - coverageIn, c_DarkTextBoost);
    const float coverage = lerp(darkCoverage, coverageIn, lum);
    const float3 linearColor = pow(abs(input.color.rgb), 2.2) * tex.rgb;
    
    return float4(linearColor, input.color.a * coverage);
}