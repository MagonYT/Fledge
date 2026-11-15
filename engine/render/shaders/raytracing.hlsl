// Fledge DXR shader library (lib_6_3). Compiled at build time with DXC into an embedded DXIL blob
// (see engine/render/CMakeLists.txt); bindings must match d3d12_raytracing.cpp.
//
// Effects (one dispatch, selected by Flags.x): sun shadows, ambient occlusion, reflections,
// one-bounce diffuse GI, plus a separate progressive path tracer (reference mode).
// Hit shading uses material factors (base color, roughness, metallic, emissive) and geometric
// normals - textures are not sampled in hit shaders.

struct Light { float4 posType; float4 dirRange; float4 colorIntensity; float4 cone; };
struct Material { float4 baseColor; float4 emissiveRoughness; float4 params; };

RaytracingAccelerationStructure Scene : register(t0);
Texture2D<float> DepthTex : register(t1);
Texture2D<float4> NormalTex : register(t2);
TextureCube<float4> EnvTex : register(t3);
StructuredBuffer<Material> Materials : register(t4);
RWTexture2D<float> OutShadow : register(u0);
RWTexture2D<float> OutAo : register(u1);
RWTexture2D<float4> OutReflection : register(u2);
RWTexture2D<float4> OutGi : register(u3);
RWTexture2D<float4> OutPathTrace : register(u4);
RWStructuredBuffer<float4> Accum : register(u5);
SamplerState LinearClamp : register(s0);

cbuffer RtConstants : register(b0) {
    float4x4 InvViewProj;
    float4 CameraPos;   // xyz
    float4 SunDir;      // xyz towards the sun, w angular radius
    float4 SunColor;    // rgb color*intensity, w environment intensity
    float4 Params;      // x ao radius, y rays per pixel, z max bounces, w quality (0 low .. 3 ultra)
    uint4 Flags;        // x effect bits, y frame index, z accumulated frames
    uint4 Size;         // x width, y height
    uint4 LightCount;   // x
    Light Lights[8];
};

// local root signature (per hit group record)
ByteAddressBuffer Vertices : register(t0, space1);
ByteAddressBuffer Indices : register(t1, space1);
cbuffer GeometryConstants : register(b0, space1) { uint MaterialIndex; uint VertexStride; };

static const float PI = 3.14159265359;
// 1.3.3 robustness (same rules as engine/render/src/shaders_robust.h; bit tests, compilers may fold isnan())
bool fl_finite(float x) { return (asuint(x) & 0x7f800000u) != 0x7f800000u; }
float3 fl_finiteOr0(float3 c) { return float3(fl_finite(c.x) ? c.x : 0.0, fl_finite(c.y) ? c.y : 0.0, fl_finite(c.z) ? c.z : 0.0); }
bool fl_usable(float3 v) { float l2 = dot(v, v); return l2 > 1e-30 && fl_finite(l2); }
static const uint FX_SHADOW = 1, FX_AO = 2, FX_REFLECTION = 4, FX_GI = 8, FX_PATHTRACE = 16;

struct HitInfo {
    float3 normal; float t;
    float3 albedo; float roughness;
    float3 emissive; float metallic;
};
struct ShadowPayload { float visible; };

// ------------------------------------------------------------------ random ---
uint pcg(inout uint s) {
    s = s * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}
float rnd(inout uint s) { return float(pcg(s)) * (1.0 / 4294967296.0); }
float3 tangentToWorld(float3 v, float3 n) {
    float3 up = abs(n.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 t = normalize(cross(up, n));
    float3 b = cross(n, t);
    return t * v.x + b * v.y + n * v.z;
}
float3 cosineSample(float3 n, inout uint s) {
    float u1 = rnd(s), u2 = rnd(s);
    float r = sqrt(u1), phi = 2.0 * PI * u2;
    return tangentToWorld(float3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - u1))), n);
}
float3 coneSample(float3 dir, float angle, inout uint s) {
    float cosMax = cos(angle);
    float u1 = rnd(s), u2 = rnd(s);
    float c = 1.0 - u1 * (1.0 - cosMax), sn = sqrt(max(0.0, 1.0 - c * c)), phi = 2.0 * PI * u2;
    return tangentToWorld(float3(sn * cos(phi), sn * sin(phi), c), dir);
}
float3 ggxHalfVector(float3 n, float roughness, inout uint s) {
    float a = max(roughness * roughness, 1e-3);
    float u1 = rnd(s), u2 = rnd(s);
    float cosT = sqrt((1.0 - u1) / (1.0 + (a * a - 1.0) * u1)), sinT = sqrt(max(0.0, 1.0 - cosT * cosT)), phi = 2.0 * PI * u2;
    return tangentToWorld(float3(sinT * cos(phi), sinT * sin(phi), cosT), n);
}

// ------------------------------------------------------------------ tracing ---
float traceVisibility(float3 origin, float3 dir, float tMax) {
    RayDesc r;
    r.Origin = origin;
    r.Direction = dir;
    r.TMin = 0.0;
    r.TMax = tMax;
    ShadowPayload p;
    p.visible = 0.0;
    TraceRay(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_FORCE_OPAQUE, 0xFF, 0, 1, 1, r, p);
    return p.visible;
}
HitInfo traceRadiance(float3 origin, float3 dir) {
    RayDesc r;
    r.Origin = origin;
    r.Direction = dir;
    r.TMin = 0.0;
    r.TMax = 1e5;
    HitInfo h = (HitInfo)0;
    h.t = -1.0;
    TraceRay(Scene, RAY_FLAG_FORCE_OPAQUE, 0xFF, 0, 1, 0, r, h);
    return h;
}
float3 environment(float3 dir, float lod) { return EnvTex.SampleLevel(LinearClamp, dir, lod).rgb * SunColor.w; }

// Simple lighting at a secondary hit: sun (optionally shadowed) + coarse sky ambient + emissive.
float3 shadeHit(HitInfo h, float3 pos, float3 inDir, bool shadowed) {
    float3 diffuse = h.albedo * (1.0 - h.metallic);
    float ndl = saturate(dot(h.normal, SunDir.xyz));
    float vis = 1.0;
    if (shadowed && ndl > 0.0) vis = traceVisibility(pos + h.normal * 0.01, SunDir.xyz, 1e4);
    float3 sun = SunColor.rgb * ndl * vis;
    float3 ambient = environment(h.normal, 6.0);
    float3 spec = lerp((0.04).xxx, h.albedo, h.metallic) * environment(reflect(inDir, h.normal), h.roughness * 6.0);
    return diffuse / PI * sun + diffuse * ambient + spec * (1.0 - h.roughness) + h.emissive;
}

// Firefly suppression for 1-4 sample estimates (the sky cube contains the sun disc).
float3 clampSample(float3 c, float maxLuma) {
    c = fl_finiteOr0(c);  // 1.3.3: a NaN sample must not reach the accumulation / denoiser
    float l = dot(c, float3(0.2126, 0.7152, 0.0722));
    return l > maxLuma ? c * (maxLuma / l) : c;
}

bool reconstruct(uint2 px, out float3 pos, out float3 n, out float roughness) {
    float d = DepthTex[px];
    pos = 0;
    n = float3(0, 1, 0);
    roughness = 1.0;
    if (d >= 1.0) return false;
    float2 uv = (float2(px) + 0.5) / float2(Size.xy);
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 w = mul(InvViewProj, float4(ndc, d, 1.0));
    pos = w.xyz / w.w;
    float4 nr = NormalTex[px];
    n = fl_usable(nr.xyz) ? normalize(nr.xyz) : float3(0, 1, 0);
    roughness = nr.w;
    return true;
}

[shader("raygeneration")]
void RayGen() {
    uint2 px = DispatchRaysIndex().xy;
    uint seed = (px.x * 1973u + px.y * 9277u + Flags.y * 26699u) | 1u;
    float3 pos, n;
    float rough;
    bool geo = reconstruct(px, pos, n, rough);
    uint fx = Flags.x;
    if (!geo) {
        if (fx & FX_SHADOW) OutShadow[px] = 1.0;
        if (fx & FX_AO) OutAo[px] = 1.0;
        if (fx & FX_REFLECTION) OutReflection[px] = float4(0, 0, 0, 0);
        if (fx & FX_GI) OutGi[px] = float4(0, 0, 0, 0);
        return;
    }
    float dist = length(pos - CameraPos.xyz);
    float3 origin = pos + n * (0.002 * dist + 0.01);
    uint rays = max(1u, (uint)Params.y);
    bool hq = Params.w >= 2.0;
    if (fx & FX_SHADOW) {
        float vis = 0.0;
        if (dot(n, SunDir.xyz) > 0.0)
            for (uint i = 0; i < rays; ++i) vis += traceVisibility(origin, coneSample(SunDir.xyz, SunDir.w, seed), 1e4);
        OutShadow[px] = vis / rays;
    }
    if (fx & FX_AO) {
        float open = 0.0;
        for (uint i = 0; i < rays; ++i) open += traceVisibility(origin, cosineSample(n, seed), Params.x);
        OutAo[px] = open / rays;
    }
    float3 V = normalize(CameraPos.xyz - pos);
    if (fx & FX_REFLECTION) {
        float3 sum = 0;
        float hitT = 0;
        for (uint i = 0; i < rays; ++i) {
            float3 h = ggxHalfVector(n, rough, seed);
            float3 R = reflect(-V, h);
            if (dot(R, n) <= 0.0) R = reflect(-V, n);
            HitInfo hit = traceRadiance(origin, R);
            if (hit.t < 0.0) {
                sum += clampSample(environment(R, rough * 4.0), 24.0);
                hitT += 1e4;
            } else {
                sum += clampSample(shadeHit(hit, origin + R * hit.t, R, hq), 24.0);
                hitT += hit.t;
            }
        }
        OutReflection[px] = float4(sum / rays, hitT / rays);
    }
    if (fx & FX_GI) {
        float3 sum = 0;
        for (uint i = 0; i < rays; ++i) {
            float3 d = cosineSample(n, seed);
            HitInfo hit = traceRadiance(origin, d);
            sum += clampSample(hit.t < 0.0 ? environment(d, 3.0) : shadeHit(hit, origin + d * hit.t, d, hq), 6.0);
        }
        OutGi[px] = float4(sum / rays, 1.0);
    }
}

// Progressive path tracer (reference mode): accumulates into Accum, writes the average.
[shader("raygeneration")]
void RayGenPathTrace() {
    uint2 px = DispatchRaysIndex().xy;
    uint idx = px.y * Size.x + px.x;
    uint seed = (px.x * 7919u + px.y * 104729u + Flags.y * 15485863u) | 1u;
    float2 uv = (float2(px) + float2(rnd(seed), rnd(seed))) / float2(Size.xy);
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 nearP = mul(InvViewProj, float4(ndc, 0.0, 1.0));  // a point on the near plane (D3D clip z = 0): precise direction
    float3 origin = CameraPos.xyz;
    float3 dir = normalize(nearP.xyz / nearP.w - origin);
    float3 radiance = 0, throughput = 1;
    uint bounces = max(1u, (uint)Params.z);
    for (uint b = 0; b <= bounces; ++b) {
        HitInfo h = traceRadiance(origin, dir);
        if (h.t < 0.0) {
            radiance += throughput * environment(dir, 0.0);
            break;
        }
        float3 p = origin + dir * h.t;
        radiance += throughput * h.emissive;
        float3 nrm = h.normal;
        float3 diffuse = h.albedo * (1.0 - h.metallic);
        float3 f0 = lerp((0.04).xxx, h.albedo, h.metallic);
        // next event estimation: the sun
        float ndl = dot(nrm, SunDir.xyz);
        if (ndl > 0.0) {
            float3 l = coneSample(SunDir.xyz, SunDir.w, seed);
            radiance += throughput * diffuse / PI * SunColor.rgb * ndl * traceVisibility(p + nrm * 0.01, l, 1e4);
        }
        // choose a lobe
        float specProb = clamp(0.5 * (f0.r + f0.g + f0.b) / 3.0 + 0.5 * h.metallic, 0.05, 0.95);
        if (rnd(seed) < specProb) {
            float3 hv = ggxHalfVector(nrm, h.roughness, seed);
            float3 r = reflect(dir, hv);
            if (dot(r, nrm) <= 0.0) break;
            throughput *= f0 / specProb;
            dir = r;
        } else {
            dir = cosineSample(nrm, seed);
            throughput *= diffuse / (1.0 - specProb);
        }
        origin = p + nrm * 0.01;
        if (b >= 3) {  // russian roulette
            float q = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.05, 0.95);
            if (rnd(seed) > q) break;
            throughput /= q;
        }
    }
    radiance = min(fl_finiteOr0(radiance), (16.0).xxx);  // firefly clamp (1.3.3: NaN -> 0 first; min(NaN, 16) would be 16) (the sky cube also contains the sun disc)
    float n = (float)Flags.z;
    float4 acc = Flags.z == 0 ? float4(radiance, 1.0) : float4((Accum[idx].rgb * n + radiance) / (n + 1.0), 1.0);
    Accum[idx] = acc;
    OutPathTrace[px] = acc;
}

[shader("closesthit")]
void ClosestHit(inout HitInfo p, in BuiltInTriangleIntersectionAttributes attr) {
    uint3 tri = Indices.Load3(PrimitiveIndex() * 12);
    float3 n0 = asfloat(Vertices.Load3(tri.x * VertexStride + 12));
    float3 n1 = asfloat(Vertices.Load3(tri.y * VertexStride + 12));
    float3 n2 = asfloat(Vertices.Load3(tri.z * VertexStride + 12));
    float3 bc = float3(1.0 - attr.barycentrics.x - attr.barycentrics.y, attr.barycentrics.x, attr.barycentrics.y);
    float3 n = mul(n0 * bc.x + n1 * bc.y + n2 * bc.z, (float3x3)WorldToObject3x4());  // inverse transpose
    if (!fl_usable(n)) {  // 1.3.3: zero-length / NaN vertex normals -> the triangle's geometric normal
        float3 p0 = asfloat(Vertices.Load3(tri.x * VertexStride));
        float3 p1 = asfloat(Vertices.Load3(tri.y * VertexStride));
        float3 p2 = asfloat(Vertices.Load3(tri.z * VertexStride));
        n = mul(cross(p1 - p0, p2 - p0), (float3x3)WorldToObject3x4());
    }
    n = fl_usable(n) ? normalize(n) : -WorldRayDirection();
    if (dot(n, WorldRayDirection()) > 0.0) n = -n;          // two-sided
    Material m = Materials[MaterialIndex];
    p.normal = n;
    p.t = RayTCurrent();
    p.albedo = m.baseColor.rgb;
    p.roughness = clamp(m.emissiveRoughness.w, 0.04, 1.0);
    p.emissive = m.emissiveRoughness.rgb;
    p.metallic = saturate(m.params.x);
}

[shader("miss")]
void Miss(inout HitInfo p) { p.t = -1.0; }

[shader("miss")]
void MissShadow(inout ShadowPayload p) { p.visible = 1.0; }
