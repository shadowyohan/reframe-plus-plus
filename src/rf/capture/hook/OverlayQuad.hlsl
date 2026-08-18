
struct VsOut {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VsOut VsMain(uint id : SV_VertexID) {
    VsOut o;

    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

Texture2D<float4> Overlay : register(t0);
SamplerState Sampler : register(s0);

float4 PsMain(VsOut i) : SV_TARGET {

    return Overlay.Sample(Sampler, i.uv);
}
