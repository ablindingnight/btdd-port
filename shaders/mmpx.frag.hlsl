// MMPX 2x pixel-art magnification (Morgan McGuire and Mara Gagiu, "MMPX Style-Preserving Pixel-Art Magnification",
// JCGT 10(2), 2021; reference code MIT licence, Copyright 2020 Morgan McGuire & Mara Gagiu, notice in
// third_party/README.md).
// HLSL port for this project: each source pixel E becomes 2x2 (J K / L M); the rules keep 1:1 and 2:1 lines, line
// intersections and triangle tips, never blend or invent colors. Renders into a texture twice the source size.
#include "common.hlsli"

float3 src(int2 p)
{
    p = clamp(p, int2(0, 0), int2(src_size.xy) - 1);
    return src_tex.Load(int3(p, 0)).rgb;
}

bool eq(float3 a, float3 b)
{
    return all(abs(a - b) < 1.0 / 512.0);
}
bool ne(float3 a, float3 b)
{
    return !eq(a, b);
}
float luma(float3 c)
{
    return dot(c, float3(0.299, 0.587, 0.114)); // order only matters, not the exact weights
}
bool all_eq2(float3 b, float3 a0, float3 a1)
{
    return eq(b, a0) && eq(b, a1);
}
bool all_eq3(float3 b, float3 a0, float3 a1, float3 a2)
{
    return eq(b, a0) && eq(b, a1) && eq(b, a2);
}
bool all_eq4(float3 b, float3 a0, float3 a1, float3 a2, float3 a3)
{
    return eq(b, a0) && eq(b, a1) && eq(b, a2) && eq(b, a3);
}
bool any_eq3(float3 b, float3 a0, float3 a1, float3 a2)
{
    return eq(b, a0) || eq(b, a1) || eq(b, a2);
}
bool none_eq2(float3 b, float3 a0, float3 a1)
{
    return ne(b, a0) && ne(b, a1);
}
bool none_eq4(float3 b, float3 a0, float3 a1, float3 a2, float3 a3)
{
    return ne(b, a0) && ne(b, a1) && ne(b, a2) && ne(b, a3);
}

float4 main(PSIn i) : SV_Target0
{
    float2 t = i.uv * src_size.xy;
    int2 p = int2(floor(t));
    int2 q = int2(floor(frac(t) * 2.0)); // which output pixel: J (0,0) K (1,0) L (0,1) M (1,1)
    int x = p.x, y = p.y;
    float3 A = src(int2(x - 1, y - 1)), B = src(int2(x, y - 1)), C = src(int2(x + 1, y - 1));
    float3 D = src(int2(x - 1, y)), E = src(int2(x, y)), F = src(int2(x + 1, y));
    float3 G = src(int2(x - 1, y + 1)), H = src(int2(x, y + 1)), I = src(int2(x + 1, y + 1));
    float3 J = E, K = E, L = E, M = E;

    if (ne(A, E) || ne(B, E) || ne(C, E) || ne(D, E) || ne(F, E) || ne(G, E) || ne(H, E) || ne(I, E)) {
        float3 P = src(int2(x, y - 2)), Q = src(int2(x - 2, y)), R = src(int2(x + 2, y)), S = src(int2(x, y + 2));
        float Bl = luma(B), Dl = luma(D), El = luma(E), Fl = luma(F), Hl = luma(H);

        // 1:1 slope rules
        if ((eq(D, B) && ne(D, H) && ne(D, F)) && (El >= Dl || eq(E, A)) && any_eq3(E, A, C, G) &&
            ((El < Dl) || ne(A, D) || ne(E, P) || ne(E, Q)))
            J = D;
        if ((eq(B, F) && ne(B, D) && ne(B, H)) && (El >= Bl || eq(E, C)) && any_eq3(E, A, C, I) &&
            ((El < Bl) || ne(C, B) || ne(E, P) || ne(E, R)))
            K = B;
        if ((eq(H, D) && ne(H, F) && ne(H, B)) && (El >= Hl || eq(E, G)) && any_eq3(E, A, G, I) &&
            ((El < Hl) || ne(G, H) || ne(E, S) || ne(E, Q)))
            L = H;
        if ((eq(F, H) && ne(F, B) && ne(F, D)) && (El >= Fl || eq(E, I)) && any_eq3(E, C, G, I) &&
            ((El < Fl) || ne(I, H) || ne(E, R) || ne(E, S)))
            M = F;

        // intersection rules
        if ((ne(E, F) && all_eq4(E, C, I, D, Q) && all_eq2(F, B, H)) && ne(F, src(int2(x + 3, y))))
            K = M = F;
        if ((ne(E, D) && all_eq4(E, A, G, F, R) && all_eq2(D, B, H)) && ne(D, src(int2(x - 3, y))))
            J = L = D;
        if ((ne(E, H) && all_eq4(E, G, I, B, P) && all_eq2(H, D, F)) && ne(H, src(int2(x, y + 3))))
            L = M = H;
        if ((ne(E, B) && all_eq4(E, A, C, H, S) && all_eq2(B, D, F)) && ne(B, src(int2(x, y - 3))))
            J = K = B;

        // triangle tip rules
        if (Bl < El && all_eq4(E, G, H, I, S) && none_eq4(E, A, D, C, F))
            J = K = B;
        if (Hl < El && all_eq4(E, A, B, C, P) && none_eq4(E, D, G, I, F))
            L = M = H;
        if (Fl < El && all_eq4(E, A, D, G, Q) && none_eq4(E, B, C, I, H))
            K = M = F;
        if (Dl < El && all_eq4(E, C, F, I, R) && none_eq4(E, B, A, G, H))
            J = L = D;

        // 2:1 slope rules
        if (ne(H, B)) {
            if (ne(H, A) && ne(H, E) && ne(H, C)) {
                if (all_eq3(H, G, F, R) && none_eq2(H, D, src(int2(x + 2, y - 1))))
                    L = M;
                if (all_eq3(H, I, D, Q) && none_eq2(H, F, src(int2(x - 2, y - 1))))
                    M = L;
            }
            if (ne(B, I) && ne(B, G) && ne(B, E)) {
                if (all_eq3(B, A, F, R) && none_eq2(B, D, src(int2(x + 2, y + 1))))
                    J = K;
                if (all_eq3(B, C, D, Q) && none_eq2(B, F, src(int2(x - 2, y + 1))))
                    K = J;
            }
        }
        if (ne(F, D)) {
            if (ne(D, I) && ne(D, E) && ne(D, C)) {
                if (all_eq3(D, A, H, S) && none_eq2(D, B, src(int2(x + 1, y + 2))))
                    J = L;
                if (all_eq3(D, G, B, P) && none_eq2(D, H, src(int2(x + 1, y - 2))))
                    L = J;
            }
            if (ne(F, E) && ne(F, A) && ne(F, G)) {
                if (all_eq3(F, C, H, S) && none_eq2(F, B, src(int2(x - 1, y + 2))))
                    K = M;
                if (all_eq3(F, I, B, P) && none_eq2(F, H, src(int2(x - 1, y - 2))))
                    M = K;
            }
        }
    }
    float3 c = q.y == 0 ? (q.x == 0 ? J : K) : (q.x == 0 ? L : M);
    return float4(c, 1.0);
}
