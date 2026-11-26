#version 450
// Temporal upscaler (settings fsr3), written for this port in the structure of the FSR 2 / FSR 3 upscaler's
// accumulate pass: the 3D picture is rendered at a low resolution with a sub-pixel jitter (the viewport of the scene's
// draws is shifted, see the backend's FsrJitter), each frame is reconstructed at the output resolution around the
// jitter offset, clipped history is blended in, and RCAS sharpens the result afterwards. It is NOT AMD's code and it
// has no motion vectors (the game's shaders are not touched): the history is reprojected as static and any
// disagreement with the new frame's neighbourhood (camera or object motion) is treated as disocclusion, so moving
// parts fall back to the spatial reconstruction while still parts accumulate detail.
//
// binding 0: the jittered frame at render size; binding 1: the previous output at output size.
layout(set = 0, binding = 0) uniform sampler2D uCur;
layout(set = 0, binding = 1) uniform sampler2D uHist;
layout(push_constant) uniform PC {
    vec2 jitter;      // render pixels: the frame's geometry was shifted by this (a point at x shows up at x + jitter)
    vec2 in_size;     // render size
    vec2 out_size;    // output size
    float blend;      // weight of the new frame when the history agrees with it (about 0.1)
    uint flags;       // 1: perceptual (the image is linear: work in gamma 2.0), 2: no history (first frame, resized)
    float clip_gamma; // width of the neighbourhood box in standard deviations (about 1.2)
    float motion_k;   // how fast disagreement raises the new frame's weight
} pc;
layout(location = 0) out vec4 outColor;

vec3 enc(vec3 c) {
    c = max(c, vec3(0.0));
    return (pc.flags & 1u) != 0u ? sqrt(c) : c;
}
vec3 dec(vec3 c) { return (pc.flags & 1u) != 0u ? c * c : c; }
vec3 fetchIn(ivec2 p) {
    return enc(texelFetch(uCur, clamp(p, ivec2(0), ivec2(pc.in_size) - ivec2(1)), 0).rgb);
}

void main() {
    const vec2 op = gl_FragCoord.xy;
    // The same point of the scene in render pixels, and where the jittered frame holds it.
    const vec2 s = op / pc.out_size * pc.in_size + pc.jitter;
    const ivec2 base = ivec2(floor(s));

    // 3x3 neighbourhood: weighted reconstruction and the statistics of the box the history is clipped to.
    vec3 sum = vec3(0.0), m1 = vec3(0.0), m2 = vec3(0.0), mn = vec3(1e9), mx = vec3(-1e9);
    float wsum = 0.0;
    for (int j = -1; j <= 1; j++) {
        for (int i = -1; i <= 1; i++) {
            const ivec2 p = base + ivec2(i, j);
            const vec3 c = fetchIn(p);
            const vec2 d = vec2(p) + 0.5 - s;
            const float w = exp(-3.0 * dot(d, d));
            sum += c * w;
            wsum += w;
            m1 += c;
            m2 += c * c;
            mn = min(mn, c);
            mx = max(mx, c);
        }
    }
    const vec3 cur = sum / max(wsum, 1e-4);
    if ((pc.flags & 2u) != 0u) {
        outColor = vec4(dec(cur), 1.0);
        return;
    }
    const vec3 mean = m1 / 9.0;
    const vec3 sd = sqrt(max(m2 / 9.0 - mean * mean, vec3(0.0)));
    const vec3 lo = max(mn, mean - pc.clip_gamma * sd);
    const vec3 hi = min(mx, mean + pc.clip_gamma * sd);

    vec3 h = enc(texture(uHist, op / pc.out_size).rgb);
    if (any(isnan(h)) || any(isinf(h))) h = cur;
    const vec3 hc = clamp(h, lo, hi);
    // How far the history had to be pulled: static parts barely move it, moving parts move it a lot.
    const float miss = length(h - hc) / (0.1 + length(mean));
    const float a = clamp(pc.blend + (1.0 - pc.blend) * clamp(miss * pc.motion_k, 0.0, 1.0), 0.0, 1.0);
    outColor = vec4(dec(mix(hc, cur, a)), 1.0);
}
