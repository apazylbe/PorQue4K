/*
 Copyright 2020 Google Inc.
 
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

// Heavily borrowed from reference implementation from glTF-Sample-Viewer
// https://github.com/KhronosGroup/glTF-Sample-Viewer/blob/master/src/shaders/metallic-roughness.frag
// with contributions from glTF spec + Filament documentations

// References:
// [1] Real Shading in Unreal Engine 4
//     http://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf
// [2] Physically Based Shading at Disney
//     http://blog.selfshadow.com/publications/s2012-shading-course/burley/s2012_pbs_disney_brdf_notes_v3.pdf
// [3] README.md - Environment Maps
//     https://github.com/KhronosGroup/glTF-WebGL-PBR/#environment-maps
// [4] "An Inexpensive BRDF Model for Physically based Rendering" by Christophe Schlick
//     https://www.cs.virginia.edu/~jdl/bib/appearance/analytic%20models/schlick94b.pdf
// [5] glTF Specification Version 2.0
//     https://github.com/KhronosGroup/glTF/tree/master/specification/2.0
// [6] Physically Based Rendering in Filament
//     https://google.github.io/filament/Filament.html

#include "ConstantBufferStructs.h"
#include "SharedShaderConstants.h"

// TODO: I need to figure out how to trigger re-compiles
// when headers change. The custom build rules don't seem
// to exercise the same dependency analysis...

// TODO: Do I want to hoist texture access into a shared function?

struct VSInput
{
    float3 PositionOS : POSITION;
    float3 Normal : NORMAL;
    float2 UV0 : TEXCOORD0;
};

struct VSOutput
{
    float4 PositionCS : SV_Position;
    float4 CurrentClipPos : TEXCOORD1;
    float4 PreviousClipPos : TEXCOORD2;
    float3 PositionWS : WORLDPOS;
    float3 Normal : NORMAL;
    float2 UV0 : TEXCOORD0;
};

#if defined(ENABLE_SAMPLE_LOCATION_SHADING)
struct PSInput
{
    // These parameters require the `sample`
    // interpolation modifier in order to generate
    // the correct values for checkerboard.

    // VK_EXT_sample_location seems to forbid the
    // use of 'sample' interpolation...

    sample float4 PositionSS : SV_Position;
    sample float4 CurrentClipPos : TEXCOORD1;
    sample float4 PreviousClipPos : TEXCOORD2;
    sample float3 PositionWS : WORLDPOS;
    sample float3 Normal : NORMAL;
    sample float2 UV0 : TEXCOORD0;

    uint sampleIndex : SV_SampleIndex;
};
#else  //defined (ENABLE_SAMPLE_LOCATION_SHADING)
struct PSInput
{
    float4 PositionSS : SV_Position;
    float4 CurrentClipPos : TEXCOORD1;
    float4 PreviousClipPos : TEXCOORD2;
    float3 PositionWS : WORLDPOS;
    float3 Normal : NORMAL;
    float2 UV0 : TEXCOORD0;
};
#endif //defined (ENABLE_SAMPLE_LOCATION_SHADING)

struct PSOutput
{
    float4 color : SV_Target0;
    float2 velocity : SV_Target1;
};

ConstantBuffer<PerFrameConstantData> PerFrame : register(b0);
ConstantBuffer<PerObjectConstantData> PerObject : register(b1);

VSOutput vsmain(VSInput input)
{
    VSOutput output = (VSOutput)0;
    output.PositionWS = mul(PerObject.worldMatrix, float4(input.PositionOS, 1)).xyz;
    output.PositionCS = mul(PerFrame.viewProjectionMatrix, float4(output.PositionWS, 1));
    output.CurrentClipPos = output.PositionCS;

    float3 previousWorldSpace = mul(PerObject.prevWorldMatrix, float4(input.PositionOS, 1)).xyz;
    output.PreviousClipPos = mul(PerFrame.prevViewProjectionMatrix, float4(previousWorldSpace, 1));

    output.Normal = normalize(mul(PerObject.worldMatrix, float4(input.Normal, 0.f)).xyz);
    output.UV0 = input.UV0;

    return output;
}

struct MaterialInfo
{
    float perceptualRoughness; // roughness value, as authored by the model creator (input to shader)
    float3 reflectance0;       // full reflectance color (normal incidence angle)

    float alphaRoughness; // roughness mapped to a more linear change in the roughness (proposed by [2])
    float3 diffuseColor;  // color contribution from diffuse lighting

    float3 reflectance90; // reflectance color at grazing angle
    float3 specularColor; // color contribution from specular lighting
};

struct AngularInfo
{
    float NdotL; // cos angle between normal and light direction
    float NdotV; // cos angle between normal and view direction
    float NdotH; // cos angle between normal and half vector
    float LdotH; // cos angle between light direction and half vector

    float VdotH; // cos angle between view direction and half vector

    // There might be need in the future to pad this struct, but
    // everything seems ok for now...
};

static const float c_PI = 3.141592653589793;
static const float c_MinReflectance = 0.04;

// TODO: Can I use offsets in the future?
// TODO: Can I build SamplerState in shader?
// TODO: Different samplers for different textures?
// TODO: Support different texture coordinates for different textures?
SamplerState texSampler : register(s2);
Texture2D<float4> baseColorTexture : register(t3);
Texture2D<float4> metallicRoughnessTexture : register(t4);
Texture2D<float4> emissiveTexture : register(t5);
Texture2D<float4> occlusionTexture : register(t6);
Texture2D<float4> normalTexture : register(t7);

float3 GetNormal(PSInput input, float2 tdx, float2 tdy)
{
    // TODO: Use input tangent space if available
    // TODO: Generate basis  normal without vertex normals
    // TODO: Add path to just use vertex normal?

    float2 nUV = input.UV0;

    float3 pos_dx = ddx(input.PositionWS);
    float3 pos_dy = ddy(input.PositionWS);
    float3 tex_dx = ddx(float3(nUV, 0.0));
    float3 tex_dy = ddy(float3(nUV, 0.0));
    float3 t = (tex_dy.y * pos_dx - tex_dx.y * pos_dy) / (tex_dx.x * tex_dy.y - tex_dy.x * tex_dx.y);

    float3 ng = normalize(input.Normal);

    t = normalize(t - ng * dot(ng, t));
    float3 b = normalize(cross(ng, t));
    float3x3 tbn = float3x3(t, b, ng);

    // if we generate ng from derivatives, normalize
    //float3 n = normalize(tbn[2].xyz);
#if defined(ENABLE_MANUAL_GRADIENTS)
    float3 n = normalTexture.SampleGrad(texSampler, input.UV0, tdx, tdy).rgb;
#else
    float3 n = normalTexture.Sample(texSampler, input.UV0).rgb;
#endif
    n = normalize(mul(transpose(tbn), ((2.0 * n - 1.0))));

    return n;
}

AngularInfo GetAngularInfo(float3 pointToLight, float3 normal, float3 view)
{
    float3 n = normalize(normal);       // Outward direction of surface point
    float3 v = normalize(view);         // Direction from surface point to view
    float3 l = normalize(pointToLight); // Direction from surface point to light
    float3 h = normalize(l + v);        // Direction of the vector between l and v

    float NdotL = clamp(dot(n, l), 0.0, 1.0);
    float NdotV = clamp(dot(n, v), 0.0, 1.0);
    float NdotH = clamp(dot(n, h), 0.0, 1.0);
    float LdotH = clamp(dot(l, h), 0.0, 1.0);
    float VdotH = clamp(dot(v, h), 0.0, 1.0);

    AngularInfo angularInfo =
        {
            NdotL,
            NdotV,
            NdotH,
            LdotH,
            VdotH,
        };

    return angularInfo;
}

// Schlick specular reflectance, with clamping for the v.h term
// https://google.github.io/filament/Filament.html#materialsystem/specularbrdf/fresnel(specularf)
float3 SpecularReflection(MaterialInfo materialInfo, AngularInfo angularInfo)
{
    return materialInfo.reflectance0 +
           ((materialInfo.reflectance90 - materialInfo.reflectance0) *
            pow(clamp(1.0 - angularInfo.VdotH, 0.0, 1.0), 5.0));
}

// Smith-GGX specular visibility function
// Could use faster version referenced here:
// https://google.github.io/filament/Filament.html#materialsystem/specularbrdf/geometricshadowing(specularg)
float VisibilityOcclusion(MaterialInfo materialInfo, AngularInfo angularInfo)
{
    float NdotL = angularInfo.NdotL;
    float NdotV = angularInfo.NdotV;
    float alphaRoughnessSq = materialInfo.alphaRoughness * materialInfo.alphaRoughness;

    float GGXV = NdotL * sqrt(NdotV * NdotV * (1.0 - alphaRoughnessSq) + alphaRoughnessSq);
    float GGXL = NdotV * sqrt(NdotL * NdotL * (1.0 - alphaRoughnessSq) + alphaRoughnessSq);

    float GGX = GGXV + GGXL;
    if (GGX > 0.0)
    {
        return 0.5 / GGX;
    }
    return 0.0;
}

// GGX normal distribution function
// Alt implementations listed:
// https://google.github.io/filament/Filament.html#materialsystem/specularbrdf/normaldistributionfunction(speculard)
float MicrofacetDistribution(MaterialInfo materialInfo, AngularInfo angularInfo)
{
    float alphaRoughnessSq = materialInfo.alphaRoughness * materialInfo.alphaRoughness;
    float f = (angularInfo.NdotH * alphaRoughnessSq - angularInfo.NdotH) * angularInfo.NdotH + 1.0;
    return alphaRoughnessSq / (c_PI * f * f + 0.000001f);
}

// Simplified Lambertian diffuse reflectance
// Could extend to support other Fd functions in
// the future? Such as Burley12?
// https://google.github.io/filament/Filament.html#materialsystem/diffusebrdf
float3 Diffuse(MaterialInfo materialInfo)
{
    return materialInfo.diffuseColor / c_PI;
}

float3 GetPointShade(float3 pointToLight, MaterialInfo materialInfo, float3 normal, float3 view)
{
    AngularInfo angularInfo = GetAngularInfo(pointToLight, normal, view);

    if (angularInfo.NdotL > 0.0 || angularInfo.NdotV > 0.0)
    {
        // Calculate the shading terms for the microfacet specular shading model
        float3 F = SpecularReflection(materialInfo, angularInfo);
        float Vis = VisibilityOcclusion(materialInfo, angularInfo);
        float D = MicrofacetDistribution(materialInfo, angularInfo);

        // Calculation of analytical lighting contribution
        float3 diffuseContrib = (1.0 - F) * Diffuse(materialInfo);
        float3 specContrib = F * Vis * D;

        // Obtain final intensity as reflectance (BRDF) scaled by the energy of the light (cosine law)
        return angularInfo.NdotL * (diffuseContrib + specContrib);
    }

    return float3(0.0, 0.0, 0.0);
}

float3 CalcDirectionalLightContrib(PSInput input, GPULightInfo light, MaterialInfo materialInfo, float3 normal, float3 view)
{
    float3 pointToLight = light.direction;
    float3 shade = GetPointShade(pointToLight, materialInfo, normal, view);
    return light.intensity * light.color * shade;
}

PSOutput psmain(PSInput input)
{
    float perceptualRoughness = 0.0;
    float metallic = 0.0;
    float4 baseColor = float4(0.0, 0.0, 0.0, 1.0);
    float3 diffuseColor = float3(0.0, 0.0, 0.0);
    float3 specularColor = float3(0.0, 0.0, 0.0);
    // TODO: Manipulate via constant if not available from material?
    float3 f0 = float3(0.04, 0.04, 0.04);

    float2 tdx = float2(0.0f, 0.0f);
    float2 tdy = float2(0.0f, 0.0f);
#if defined(ENABLE_MANUAL_GRADIENTS)
#if (CB_RESOLVE_DEBUG > 0)
    tdx = ddx_fine(input.UV0) * PerFrame.texGradScaler;
    tdy = ddy_fine(input.UV0) * PerFrame.texGradScaler;
#else
    tdx = ddx_fine(input.UV0) * GRADIENT_SCALING_FACTOR;
    tdy = ddy_fine(input.UV0) * GRADIENT_SCALING_FACTOR;
#endif
#endif // defined(ENABLE_MANUAL_GRADIENTS)

    // TODO: Create function to route samples through

    // TODO: Support KHR_materials_pbrSpecularGlossiness?

    const float metallicFactor = PerObject.metallicFactor;
    const float roughnessFactor = PerObject.roughnessFactor;

    // https://github.com/KhronosGroup/glTF/tree/master/specification/2.0#pbrmetallicroughnessmetallicroughnesstexture
    // "The metalness values are sampled from the B channel. The roughness values are sampled from the G channel."
#if defined(ENABLE_MANUAL_GRADIENTS)
    float4 metallicRoughness = metallicRoughnessTexture.SampleGrad(texSampler, input.UV0, tdx, tdy);
#else
    float4 metallicRoughness = metallicRoughnessTexture.Sample(texSampler, input.UV0);
#endif
    perceptualRoughness = metallicRoughness.g * roughnessFactor;
    metallic = metallicRoughness.b * metallicFactor;

    perceptualRoughness = saturate(perceptualRoughness);
    metallic = saturate(metallic);

    const float4 baseColorFactor = PerObject.baseColorFactor;
#if defined(ENABLE_MANUAL_GRADIENTS)
    baseColor = baseColorTexture.SampleGrad(texSampler, input.UV0, tdx, tdy);
#else
    baseColor = baseColorTexture.Sample(texSampler, input.UV0);
#endif
    baseColor *= baseColorFactor;

    // https://github.com/KhronosGroup/glTF/tree/master/specification/2.0#metallic-roughness-material
    diffuseColor = baseColor.rgb * (float3(1.0, 1.0, 1.0) - f0) * (1.0 - metallic);
    specularColor = lerp(f0, baseColor.rgb, metallic);

    // TODO: Alpha cutoff?
    baseColor.a = 1.0;

    // Roughness remapping
    // https://google.github.io/filament/Filament.html#materialsystem/parameterization/remapping
    float alphaRoughness = perceptualRoughness * perceptualRoughness;

    float reflectance = max(max(specularColor.r, specularColor.g), specularColor.b);
    float3 specularEnvironmentR0 = specularColor.rgb;
    // https://google.github.io/filament/Filament.html#lighting/occlusion/specularocclusion
    float3 specularEnvironmentR90 = float3(1.0, 1.0, 1.0) * clamp(reflectance * 50.0, 0.0, 1.0);

    float3 normal = GetNormal(input, tdx, tdy);
    float3 view = normalize(PerFrame.cameraPos.xyz - input.PositionWS);
    float3 color = float3(0.0, 0.0, 0.0);

    // TODO: This fails ShaderModule creation somehow...
    // Try a newer version of DXC or file a bug!
    //GPULightInfo light = PerFrame.dirLight;

    GPULightInfo light;
    light.direction = PerFrame.dirLight.direction;
    light.color = PerFrame.dirLight.color;
    light.intensity = PerFrame.dirLight.intensity;

    MaterialInfo materialInfo =
        {
            perceptualRoughness,
            specularEnvironmentR0,
            alphaRoughness,
            diffuseColor,
            specularEnvironmentR90,
            specularColor};

    // TODO: Handle other light types
    color += CalcDirectionalLightContrib(input, light, materialInfo, normal, view);

    // TODO: Handle IBL?

    // TODO: occlusion strength constant, controlled by debug slider
    float ao = 1.0f;
#if defined(ENABLE_MANUAL_GRADIENTS)
    ao = occlusionTexture.SampleGrad(texSampler, input.UV0, tdx, tdy).r;
#else
    ao = occlusionTexture.Sample(texSampler, input.UV0).r;
#endif
    color *= ao;

    // TODO: Load frame emissive factors
    const float3 objectEmissiveFactor = PerObject.emissiveFactor;
    const float frameEmissiveFactor = 1.f;
#if defined(ENABLE_MANUAL_GRADIENTS)
    float3 emissive = (emissiveTexture.SampleGrad(texSampler, input.UV0, tdx, tdy)).rgb * objectEmissiveFactor.rgb * frameEmissiveFactor;
#else
    float3 emissive = (emissiveTexture.Sample(texSampler, input.UV0)).rgb * objectEmissiveFactor.rgb * frameEmissiveFactor;
#endif
    color += emissive;

    float4 outColor = float4(color, baseColor.a);

    // TODO: Define debug outputs
    //{
    // Texture coordinates
    //return float4(input.UV0.x, input.UV0.y, 0.f, 1);

    // Texture gradients
    //#if defined(ENABLE_MANUAL_GRADIENTS)
    //return float4(tdy.x * 100.f, tdy.y * 100.f, 0.f, 1.f);
    //#endif

    // Input vertex normal
    //return float4(input.Normal.x, input.Normal.y, input.Normal.z, 1);

    // Derived normal
    //return float4(normal.rgb, 1.f);

    // View
    //return float4(view.rgb, 1.f);

    // Base color
    //return float4(baseColor.rgb, 1.f);

    // Metallic roughness
    //return float4(metallicRoughness.rgb, 1.f);

    // Occlusion texture
    //return float4(ao, ao, ao, 1.f);

    // Emissive
    //return float4(emissive, 1.f);

    // Material based diffuse and specular contributions
    //return float4(diffuseColor.rgb, 1.f);
    //return float4(specularColor.rgb, 1.f);

    // Verify that pixel quads share the same sample index
    //#if defined(ENABLE_SAMPLE_LOCATION_SHADING)
    //    float smpIndexGradX = ddx_fine(input.sampleIndex);
    //    float smpIndexGradY = ddy_fine(input.sampleIndex);
    //    return float4(smpIndexGradX, smpIndexGradY, 0.f, 1.0f);
    //#endif

    //}
    PSOutput output;
    output.color = outColor;

    // Generate velocity in NDC space to avoid screen-size issues
    float2 currentNDC = input.CurrentClipPos.xy / input.CurrentClipPos.w;
    float2 previousNDC = input.PreviousClipPos.xy / input.PreviousClipPos.w;
    output.velocity = currentNDC - previousNDC;

    return output;
}