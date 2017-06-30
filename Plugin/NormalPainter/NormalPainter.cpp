#include "pch.h"
#include "NormalPainter.h"

using namespace mu;

inline static int Raycast(
    const float3 pos, const float3 dir, const float3 vertices[], const int indices[], int num_triangles,
    int& tindex, float& distance)
{
    float d;
    int hit = RayTrianglesIntersection(pos, dir, vertices, indices, num_triangles, tindex, d);
    if (hit) {
        float3 hpos = pos + dir * d;
        distance = length(hpos - pos);
    }
    return hit;
}
inline static int Raycast(
    const float3 pos, const float3 dir, const float3 vertices[], const int indices[], int num_triangles, const float4x4& trans,
    int& tindex, float& distance)
{
    float4x4 itrans = invert(trans);
    float3 rpos = mul_p(itrans, pos);
    float3 rdir = normalize(mul_v(itrans, dir));
    float d;
    int hit = RayTrianglesIntersection(rpos, rdir, vertices, indices, num_triangles, tindex, d);
    if (hit) {
        float3 hpos = rpos + rdir * d;
        distance = length(mul_p(trans, hpos) - pos);
    }
    return hit;
}

template<class Body>
inline static int SelectInside(float3 pos, float radius, const float3 vertices[], int num_vertices, const float4x4& trans, const Body& body)
{
    int ret = 0;
    float rq = radius * radius;
    for (int vi = 0; vi < num_vertices; ++vi) {
        float3 p = mul_p(trans, vertices[vi]);
        float dsq = length_sq(p - pos);
        if (dsq <= rq) {
            body(vi, std::sqrt(dsq), p);
            ++ret;
        }
    }
    return ret;
}

static bool GetFurthestDistance(const float3 vertices[], const float selection[], int num_vertices, float3 pos, const float4x4& trans, int &vidx, float &dist)
{
    float furthest_sq = FLT_MIN;
    int furthest_vi;

    float3 lpos = mul_p(invert(trans), pos);
    for (int vi = 0; vi < num_vertices; ++vi) {
        if (selection[vi] > 0.0f) {
            float dsq = length_sq(vertices[vi] - lpos);
            if (dsq > furthest_sq) {
                furthest_sq = dsq;
                furthest_vi = vi;
            }
        }
    }

    if (furthest_sq > FLT_MIN) {
        dist = length(mul_p(trans, vertices[furthest_vi]) - pos);
        vidx = furthest_vi;
        return true;
    }
    return false;
}

static inline int GetBrushSampleIndex(float distance, float bradius, int num_bsamples)
{
    return int(clamp01(1.0f - distance / bradius) * (num_bsamples - 1));
}
static inline float GetBrushSample(float distance, float bradius, float bsamples[], int num_bsamples)
{
    return bsamples[GetBrushSampleIndex(distance, bradius, num_bsamples)];
}


npAPI int npRaycast(
    int num_triangles, const float3 pos, const float3 dir, const float3 vertices[], const int indices[],
    int *tindex, float *distance, const float4x4 *trans)
{
    return Raycast(pos, dir, vertices, indices, num_triangles, *trans, *tindex, *distance);
}

npAPI float3 npPickNormal(
    const float3 vertices[], const int indices[], const float3 normals[], const float4x4 *trans,
    const float3 pos, int ti)
{
    float3 p[3]{ vertices[indices[ti * 3 + 0]], vertices[indices[ti * 3 + 1]], vertices[indices[ti * 3 + 2]] };
    float3 n[3]{ normals[indices[ti * 3 + 0]], normals[indices[ti * 3 + 1]], normals[indices[ti * 3 + 2]] };
    float3 lpos = mul_p(invert(*trans), pos);
    float3 r = triangle_interpolation(lpos, p[0], p[1], p[2], n[0], n[1], n[2]);
    return normalize(mul_v(*trans, r));
}

npAPI int npSelectSingle(
    int num_vertices, int num_triangles, const float3 vertices[], const float3 normals[], const int indices[], float selection[], float strength,
    const float4x4 *mvp_, const float4x4 *trans_, float2 rmin, float2 rmax, float3 campos, int frontface_only)
{
    float4x4 mvp = *mvp_;
    float4x4 trans = *trans_;
    float3 lcampos = mul_p(invert(trans), campos);
    float2 rcenter = (rmin + rmax) * 0.5f;

    const int MaxInsider = 64;
    std::pair<int, float> insider[MaxInsider];
    int num_inside = 0;

    {
        // gather vertices inside rect
        std::atomic_int num_inside_a{ 0 };
        parallel_for(0, num_vertices, [&](int vi) {
            float4 vp = mul4(mvp, vertices[vi]);
            float2 sp = float2{ vp.x, vp.y } / vp.w;
            if (sp.x >= rmin.x && sp.x <= rmax.x &&
                sp.y >= rmin.y && sp.y <= rmax.y && vp.z > 0.0f)
            {
                bool hit = false;
                if (frontface_only) {
                    float3 vpos = vertices[vi];
                    float3 dir = normalize(vpos - lcampos);
                    int ti;
                    float distance;
                    if (Raycast(lcampos, dir, vertices, indices, num_triangles, ti, distance)) {
                        float3 hitpos = lcampos + dir * distance;
                        if (length(vpos - hitpos) < 0.01f) {
                            hit = true;
                        }
                    }
                }
                else {
                    hit = true;
                }

                if (hit) {
                    int ii = num_inside_a++;
                    if (ii < MaxInsider) {
                        insider[ii].first = vi;
                        insider[ii].second = length(sp - rcenter);
                    }
                }
            }
        });
        num_inside = std::min<int>(num_inside_a, MaxInsider);
    }

    if (num_inside > 0) {
        // search nearest from center of rect
        int nearest_index = 0;
        float nearest_distance = FLT_MAX;
        float nearest_facing = 1.0f;

        for (int ii = 0; ii < num_inside; ++ii) {
            int vi = insider[ii].first;
            float distance = insider[ii].second;
            float3 dir = normalize(vertices[vi] - lcampos);
            
            // if there are vertices with identical position, pick most camera-facing one 
            if (near_equal(distance, nearest_distance)) {
                float facing = dot(normals[vi], dir);
                if (facing < nearest_facing) {
                    nearest_index = vi;
                    nearest_distance = distance;
                    nearest_facing = facing;
                }
            }
            else if (distance < nearest_distance) {
                nearest_index = vi;
                nearest_distance = distance;
                nearest_facing = dot(normals[vi], dir);
            }
        }

        selection[nearest_index] = clamp01(selection[nearest_index] + strength);
        return 1;
    }
    return 0;
}


npAPI int npSelectTriangle(
    int num_triangles, const float3 vertices[], const int indices[], float selection[], float strength,
    const float4x4 *trans, const float3 pos, const float3 dir)
{
    int ti;
    float distance;
    if (Raycast(pos, dir, vertices, indices, num_triangles, *trans, ti, distance)) {
        for (int i = 0; i < 3; ++i) {
            selection[indices[ti * 3 + i]] = clamp01(selection[indices[ti * 3 + i]] + strength);
        }
        return 1;
    }
    return 0;
}

npAPI int npSelectEdge(
    int num_vertices, int num_triangles, const float3 vertices_[], const int indices_[], float selection[], float strength, int clear)
{
    auto indices = IArray<int>(indices_, num_triangles * 3);
    auto vertices = IArray<float3>(vertices_, num_vertices);

    RawVector<int> targets;
    targets.reserve(num_vertices);
    for (int vi = 0; vi < num_vertices; ++vi) {
        if (selection[vi] > 0.0f) {
            targets.push_back(vi);
        }
    }
    if (targets.size() == 0) {
        targets.resize(num_vertices);
        for (int vi = 0; vi < num_vertices; ++vi)
            targets[vi] = vi;
    }

    if (clear) { memset(selection, 0, num_vertices * 4); }

    int ret = 0;
    SelectEdge(indices, 3, vertices, targets, [&](int vi) {
        selection[vi] = clamp01(selection[vi] + strength);
        ++ret;
    });
    return ret;
}

npAPI int npSelectHole(
    int num_vertices, int num_triangles, const float3 vertices_[], const int indices_[], float selection[], float strength, int clear)
{
    auto indices = IArray<int>(indices_, num_triangles * 3);
    auto vertices = IArray<float3>(vertices_, num_vertices);

    RawVector<int> targets;
    targets.reserve(num_vertices);
    for (int vi = 0; vi < num_vertices; ++vi) {
        if (selection[vi] > 0.0f) {
            targets.push_back(vi);
        }
    }
    if (targets.size() == 0) {
        targets.resize(num_vertices);
        for (int vi = 0; vi < num_vertices; ++vi)
            targets[vi] = vi;
    }

    if (clear) { memset(selection, 0, num_vertices * 4); }

    int ret = 0;
    SelectHole(indices, 3, vertices, targets, [&](int vi) {
        selection[vi] = clamp01(selection[vi] + strength);
        ++ret;
    });
    return ret;
}

npAPI int npSelectConnected(
    int num_vertices, int num_triangles, const float3 vertices_[], const int indices_[], float selection[], float strength, int clear)
{
    auto indices = IArray<int>(indices_, num_triangles * 3);
    auto vertices = IArray<float3>(vertices_, num_vertices);

    RawVector<int> targets;
    targets.reserve(num_vertices);
    for (int vi = 0; vi < num_vertices; ++vi) {
        if (selection[vi] > 0.0f) {
            targets.push_back(vi);
        }
    }
    if (targets.size() == 0) {
        targets.resize(num_vertices);
        for (int vi = 0; vi < num_vertices; ++vi)
            targets[vi] = vi;
    }

    if (clear) { memset(selection, 0, num_vertices * 4); }

    int ret = 0;
    SelectConnected(indices, 3, vertices, targets, [&](int vi) {
        selection[vi] = clamp01(selection[vi] + strength);
        ++ret;
    });
    return ret;
}

npAPI int npSelectRect(
    int num_vertices, int num_triangles, const float3 vertices[], const int indices[], float selection[], float strength,
    const float4x4 *mvp_, const float4x4 *trans_, float2 rmin, float2 rmax, float3 campos, int frontface_only)
{
    float4x4 mvp = *mvp_;
    float4x4 trans = *trans_;
    float3 lcampos = mul_p(invert(trans), campos);

    std::atomic_int ret{ 0 };
    parallel_for(0, num_vertices, [&](int vi) {
        float4 vp = mul4(mvp, vertices[vi]);
        float2 sp = float2{ vp.x, vp.y } / vp.w;
        if (sp.x >= rmin.x && sp.x <= rmax.x &&
            sp.y >= rmin.y && sp.y <= rmax.y && vp.z > 0.0f)
        {
            bool hit = false;
            if (frontface_only) {
                float3 vpos = vertices[vi];
                float3 dir = normalize(vpos - lcampos);
                int ti;
                float distance;
                if (Raycast(lcampos, dir, vertices, indices, num_triangles, ti, distance)) {
                    float3 hitpos = lcampos + dir * distance;
                    if (length(vpos - hitpos) < 0.01f) {
                        hit = true;
                    }
                }
            }
            else {
                hit = true;
            }

            if (hit) {
                selection[vi] = clamp01(selection[vi] + strength);
                ++ret;
            }
        }
    });
    return ret;
}

npAPI int npSelectLasso(
    int num_vertices, int num_triangles, const float3 vertices[], const int indices[], float selection[], float strength,
    const float4x4 *mvp_, const float4x4 *trans_, const float2 poly[], int ngon, float3 campos, int frontface_only)
{
    if (ngon < 3) { return 0; }

    float4x4 mvp = *mvp_;
    float4x4 trans = *trans_;
    float3 lcampos = mul_p(invert(trans), campos);

    float2 minp, maxp;
    MinMax(poly, ngon, minp, maxp);

    RawVector<float> polyx, polyy;
    polyx.resize(ngon); polyy.resize(ngon);
    for (int i = 0; i < ngon; ++i) {
        polyx[i] = poly[i].x;
        polyy[i] = poly[i].y;
    }

    std::atomic_int ret{ 0 };
    parallel_for(0, num_vertices, [&](int vi) {
        float4 vp = mul4(mvp, vertices[vi]);
        float2 sp = float2{ vp.x, vp.y } / vp.w;
        if (PolyInside(polyx.data(), polyy.data(), ngon, minp, maxp, sp)) {
            bool hit = false;
            if (frontface_only) {
                float3 vpos = vertices[vi];
                float3 dir = normalize(vpos - lcampos);
                int ti;
                float distance;
                if (Raycast(lcampos, dir, vertices, indices, num_triangles, ti, distance)) {
                    float3 hitpos = lcampos + dir * distance;
                    if (length(vpos - hitpos) < 0.01f) {
                        hit = true;
                    }
                }
            }
            else {
                hit = true;
            }

            if (hit) {
                selection[vi] = clamp01(selection[vi] + strength);
                ++ret;
            }
        }
    });
    return ret;
}

npAPI int npSelectBrush(
    int num_vertices, const float3 vertices[], const float4x4 *trans,
    const float3 pos, float radius, float strength, float bsamples[], int num_bsamples, float selection[])
{
    return SelectInside(pos, radius, vertices, num_vertices, *trans, [&](int vi, float d, float3 p) {
        float s = GetBrushSample(d, radius, bsamples, num_bsamples) * strength;
        selection[vi] = clamp01(selection[vi] + s);
    });
}

npAPI int npUpdateSelection(
    int num_vertices, const float3 vertices[], const float3 normals[], const float selection[], const float4x4 *trans,
    float3 *selection_pos, float3 *selection_normal)
{
    float st = 0.0f;
    int num_selected = 0;
    float3 spos = float3::zero();
    float3 snormal = float3::zero();
    quatf srot = quatf::identity();

    for (int vi = 0; vi < num_vertices; ++vi) {
        float s = selection[vi];
        if (s > 0.0f) {
            spos += vertices[vi] * s;
            snormal += normals[vi] * s;
            ++num_selected;
            st += s;
        }
    }

    if (num_selected > 0) {
        spos /= st;
        spos = mul_p(*trans, spos);
        snormal = normalize(mul_v(*trans, snormal));
        srot = to_quat(look33(snormal, {0,1,0}));
    }

    *selection_pos = spos;
    *selection_normal = snormal;
    return num_selected;
}


npAPI void npAssign(
    int num_vertices, const float selection[], const float4x4 *trans_,
    float3 v, float3 normals[])
{
    v = mul_v(invert(*trans_), v);
    for (int vi = 0; vi < num_vertices; ++vi) {
        float s = selection[vi];
        if (s == 0.0f) continue;

        normals[vi] = normalize(lerp(normals[vi], v, s));
    }
}

npAPI void npMove(
    int num_vertices, const float selection[], const float4x4 *trans_,
    float3 amount, float3 normals[])
{
    amount = mul_v(invert(*trans_), amount);
    for (int vi = 0; vi < num_vertices; ++vi) {
        float s = selection[vi];
        if (s == 0.0f) continue;

        normals[vi] = normalize(normals[vi] + amount * s);
    }
}

npAPI void npRotate(
    int num_vertices, const float3 vertices[], const float selection[], const float4x4 *trans_,
    quatf amount, quatf pivot_rot, float3 normals[])
{
    float3 axis;
    float angle;
    to_axis_angle(amount, axis, angle);
    if (near_equal(angle, 0.0f) || std::isnan(angle)) {
        return;
    }

    auto ptrans = to_float4x4(invert(pivot_rot));
    auto iptrans = invert(ptrans);
    auto trans = *trans_;
    auto itrans = invert(trans);
    auto rot = to_float4x4(invert(amount));

    auto to_lspace = trans * iptrans * rot * ptrans * itrans;

    for (int vi = 0; vi < num_vertices; ++vi) {
        float s = selection[vi];
        if (s == 0.0f) continue;

        float3 n = normals[vi];
        float3 v = normalize(mul_v(to_lspace, n));
        normals[vi] = normalize(lerp(n, v, s));
    }
}

npAPI void npRotatePivot(
    int num_vertices, const float3 vertices[], const float selection[], const float4x4 *trans_,
    quatf amount, float3 pivot_pos, quatf pivot_rot, float3 normals[])
{
    float3 axis;
    float angle;
    to_axis_angle(amount, axis, angle);
    if (near_equal(angle, 0.0f) || std::isnan(angle)) {
        return;
    }

    float furthest;
    int furthest_idx;
    if (!GetFurthestDistance(vertices, selection, num_vertices, pivot_pos, *trans_, furthest_idx, furthest)) {
        return;
    }

    auto ptrans = to_float4x4(invert(pivot_rot)) * translate(pivot_pos);
    auto iptrans = invert(ptrans);
    auto trans = *trans_;
    auto itrans = invert(trans);

    auto to_pspace = trans * iptrans;
    auto to_lspace = ptrans * itrans;
    auto rot = to_float3x3(amount);

    for (int vi = 0; vi < num_vertices; ++vi) {
        float s = selection[vi];
        if (s == 0.0f) continue;

        float3 vpos = mul_p(to_pspace, vertices[vi]);
        float d = length(vpos);
        float3 v = vpos - (rot * vpos);
        if(near_equal(length(v), 0.0f)) { continue; }
        v = normalize(mul_v(to_lspace, v));
        normals[vi] = normalize(normals[vi] + v * (d / furthest * angle * s));
    }
}

npAPI void npScale(
    int num_vertices, const float3 vertices[], const float selection[], const float4x4 *trans_,
    float3 amount, float3 pivot_pos, quatf pivot_rot, float3 normals[])
{
    float furthest;
    int furthest_idx;
    if (!GetFurthestDistance(vertices, selection, num_vertices, pivot_pos, *trans_, furthest_idx, furthest)) {
        return;
    }

    auto ptrans = to_float4x4(invert(pivot_rot)) * translate(pivot_pos);
    auto iptrans = invert(ptrans);
    auto trans = *trans_;
    auto itrans = invert(trans);

    auto to_pspace = trans * iptrans;
    auto to_lspace = ptrans * itrans;

    for (int vi = 0; vi < num_vertices; ++vi) {
        float s = selection[vi];
        if (s == 0.0f) continue;

        float3 vpos = mul_p(to_pspace, vertices[vi]);
        float d = length(vpos);
        float3 v = mul_v(to_lspace, (vpos / d) * amount);
        normals[vi] = normalize(normals[vi] + v * (d / furthest * s));
    }
}

npAPI void npSmooth(
    int num_vertices, const float3 vertices[], const float selection[], const float4x4 *trans,
    float radius, float strength, float3 normals[])
{
    RawVector<float3> tvertices;
    tvertices.resize(num_vertices);
    parallel_for(0, num_vertices, [&](int vi) {
        tvertices[vi] = mul_p(*trans, vertices[vi]);
    });

    float rsq = radius * radius;
    parallel_for(0, num_vertices, [&](int vi) {
        float s = selection ? selection[vi] : 1.0f;
        if (s == 0.0f) { return; }

        float3 p = tvertices[vi];
        float3 average = float3::zero();
        for (int i = 0; i < num_vertices; ++i) {
            float s2 = selection ? selection[i] : 1.0f;
            float dsq = length_sq(tvertices[i] - p);
            if (dsq <= rsq) {
                average += normals[i] * s2;
            }
        }
        average = normalize(average);
        normals[vi] = normalize(normals[vi] + average * (strength * s));
    });
}

npAPI int npWeld(int num_vertices, const float3 vertices[], const float selection[], float3 normals[]
    , int smoothing, float weld_angle)
{
    RawVector<bool> checked;
    checked.resize(num_vertices);
    checked.zeroclear();

    int ret = 0;
    RawVector<int> shared;
    for (int vi = 0; vi < num_vertices; ++vi) {
        if (checked[vi]) { continue; }
        float s = selection ? selection[vi] : 1.0f;
        if (s == 0.0f) { continue; }

        float3 p = vertices[vi];
        float3 n = normals[vi];
        for (int i = 0; i < num_vertices; ++i) {
            if (vi != i && !checked[i] &&
                near_equal(length(vertices[i] - p), 0.0f) &&
                angle_between(n, normals[i]) * Rad2Deg <= weld_angle)
            {
                if (smoothing) n += normals[i];
                shared.push_back(i);
                checked[i] = true;
            }
        }

        if (!shared.empty()) {
            n = normalize(n);
            normals[vi] = n;
            for (int si : shared) {
                normals[si] = n;
            }
            shared.clear();
            ++ret;
        }
    }

    return ret;
}


npAPI int npWeld2(int num_vertices, const float3 vertices[], const float selection[], float3 normals[], const float4x4 *trans_,
    int num_targets, const int tnum_vertices[], const float3 *tvertices[], float3 *tnormals[], const float4x4 ttrans[],
    int weld_mode, float weld_angle)
{
    float4x4 trans = *trans_;
    float4x4 itrans = invert(trans);

    RawVector<float4x4> titrans;
    RawVector<float3> wvertices, wnormals;
    std::vector<RawVector<float3>> twvertices, twnormals;

    // generate world space vertices
    wvertices.resize(num_vertices);
    wnormals.resize(num_vertices);
    for (int vi = 0; vi < num_vertices; ++vi) {
        wvertices[vi] = mul_p(trans, vertices[vi]);
        wnormals[vi] = mul_v(trans, normals[vi]);
    }

    titrans.resize(num_targets);
    twvertices.resize(num_targets);
    twnormals.resize(num_targets);
    for (int ti = 0; ti < num_targets; ++ti) {
        auto tt = ttrans[ti];
        titrans[ti] = invert(tt);

        auto tva = tvertices[ti];
        auto tna = tnormals[ti];
        auto& twva = twvertices[ti];
        auto& twna = twnormals[ti];
        int num_tv = tnum_vertices[ti];
        twva.resize(num_tv);
        twna.resize(num_tv);
        for (int tvi = 0; tvi < num_tv; ++tvi) {
            twva[tvi] = mul_p(tt, tva[tvi]);
            twna[tvi] = mul_v(tt, tna[tvi]);
        }
    }

    std::vector<RawVector<std::pair<int, int>>> weld_maps;
    weld_maps.resize(num_targets);

    // generate weld maps
    for (int ti = 0; ti < num_targets; ++ti) {
        auto& weld_map = weld_maps[ti];

        for (int vi = 0; vi < num_vertices; ++vi) {
            if (selection && selection[vi] == 0.0f) { continue; }

            auto p = wvertices[vi];
            auto n = wnormals[vi];
            auto& twva = twvertices[ti];
            auto& twna = twnormals[ti];
            int num_tv = tnum_vertices[ti];
            for (int tvi = 0; tvi < num_tv; ++tvi) {
                if (near_equal(length_sq(twva[tvi] - p), 0.0f) && angle_between(n, twna[tvi]) * Rad2Deg <= weld_angle) {
                    weld_map.push_back({ vi, tvi });
                }
            }
        }
    }

    int ret = 0;
    for (auto& map : weld_maps) { ret += (int)map.size(); }
    if (ret == 0) { return 0; } // no vertices to weld


    if (weld_mode == 0) {
        // copy to targets
        for (int ti = 0; ti < num_targets; ++ti) {
            auto& weld_map = weld_maps[ti];
            auto it = titrans[ti];
            auto tna = tnormals[ti];
            for (auto& rel : weld_map) {
                tna[rel.second] = mul_v(it, wnormals[rel.first]);
            }
        }
    }
    else if (weld_mode == 1) {
        // copy from targets
        for (int ti = 0; ti < num_targets; ++ti) {
            auto& weld_map = weld_maps[ti];
            auto& twna = twnormals[ti];
            for (auto& rel : weld_map) {
                normals[rel.first] = mul_v(itrans, twna[rel.second]);
            }
        }
    }
    else if (weld_mode == 2) {
        // smooth
        RawVector<float3> tmp_wnormals = wnormals;

        for (int ti = 0; ti < num_targets; ++ti) {
            auto& weld_map = weld_maps[ti];
            auto& twna = twnormals[ti];
            for (auto& rel : weld_map) {
                tmp_wnormals[rel.first] += twna[rel.second];
            }
        }
        for (auto& n : tmp_wnormals) {
            n = normalize(n);
        }

        for (int ti = 0; ti < num_targets; ++ti) {
            auto& weld_map = weld_maps[ti];
            auto it = titrans[ti];
            auto tna = tnormals[ti];
            for (auto& rel : weld_map) {
                normals[rel.first] = mul_v(itrans, tmp_wnormals[rel.first]);
                tna[rel.second] = mul_v(it, tmp_wnormals[rel.first]);
            }
        }
    }
    return ret;
}


npAPI int npBrushReplace(
    int num_vertices, const float3 vertices[], const float selection[], const float4x4 *trans,
    const float3 pos, float radius, float strength, float bsamples[], int num_bsamples, float3 amount, float3 normals[])
{
    return SelectInside(pos, radius, vertices, num_vertices, *trans, [&](int vi, float d, float3 p) {
        float s = GetBrushSample(d, radius, bsamples, num_bsamples) * strength;
        if (selection) s *= selection[vi];

        normals[vi] = normalize(normals[vi] + amount * s);
    });
    return 0;
}

npAPI int npBrushPaint(
    int num_vertices, const float3 vertices[], const float selection[], const float4x4 *trans,
    const float3 pos, float radius, float strength, float bsamples[], int num_bsamples, float3 n, int blend_mode, float3 normals[])
{
    float3 ln = n;
    n = normalize(mul_v(*trans, n));
    auto itrans = invert(*trans);
    return SelectInside(pos, radius, vertices, num_vertices, *trans, [&](int vi, float d, float3 p) {
        int bsi = GetBrushSampleIndex(d, radius, num_bsamples);
        float s = clamp11(bsamples[bsi] * strength * 2.0f);
        if (selection) s *= selection[vi];

        float slope;
        if (bsi == 0) {
            slope = (bsamples[bsi+1] - bsamples[bsi  ]) / (1.0f / (num_bsamples - 1));
        }
        else if (bsi == num_bsamples - 1) {
            slope = (bsamples[bsi  ] - bsamples[bsi-1]) / (1.0f / (num_bsamples - 1));
        }
        else {
            slope = (bsamples[bsi+1] - bsamples[bsi-1]) / (1.0f / (num_bsamples - 1) * 2.0f);
        }

        float3 t;
        {
            float3 p1 = pos - n * plane_distance(pos, n);
            float3 p2 = p - n * plane_distance(p, n);
            t = normalize(p2 - p1);
        }
        if (slope < 0.0f) {
            t *= -1.0f;
            slope *= -1.0f;
        }
        if (s < 0.0f) {
            t *= -1.0f;
            s *= -1.0f;
        }

        float3 vn = normals[vi];
        float3 r = lerp(n, t, clamp01(slope * 0.5f));
        r = normalize(mul_v(itrans, r));

        // maybe add something here later
        //switch (blend_mode) {
        //}
        r = lerp(vn, r, s);

        normals[vi] = normalize(vn + r * s);
    });
}

npAPI int npBrushLerp(
    int num_vertices, const float3 vertices[], const float selection[], const float4x4 *trans,
    const float3 pos, float radius, float strength, float bsamples[], int num_bsamples, const float3 base[], float3 normals[])
{
    return SelectInside(pos, radius, vertices, num_vertices, *trans, [&](int vi, float d, float3 p) {
        float s = GetBrushSample(d, radius, bsamples, num_bsamples) * strength;
        if (selection) s *= selection[vi];

        float sign = strength < 0.0f ? -1.0f : 1.0f;
        normals[vi] = normalize(lerp(normals[vi], base[vi] * sign, s));
    });
}

npAPI int npBrushSmooth(
    int num_vertices, const float3 vertices[], const float selection[], const float4x4 *trans,
    const float3 pos, float radius, float strength, float bsamples[], int num_bsamples, float3 normals[])
{
    RawVector<std::pair<int, float>> inside;
    SelectInside(pos, radius, vertices, num_vertices, *trans, [&](int vi, float d, float3 p) {
        inside.push_back({ vi, d });
    });

    float3 average = float3::zero();
    for (auto& p : inside) {
        average += normals[p.first];
    }
    average = normalize(average);
    for (auto& p : inside) {
        float s = GetBrushSample(p.second, radius, bsamples, num_bsamples) * strength;
        if (selection) s *= selection[p.first];

        normals[p.first] = normalize(normals[p.first] + average * s);
    }
    return (int)inside.size();
}


npAPI int npBuildMirroringRelation(
    int num_vertices, const float3 vertices[], const float3 normals[], 
    float3 plane_normal, float epsilon, int relation[])
{
    RawVector<float> distances;
    distances.resize(num_vertices);
    parallel_for(0, num_vertices, [&](int vi) {
        distances[vi] = plane_distance(vertices[vi], plane_normal);
    });

    std::atomic_int ret{ 0 };
    parallel_for(0, num_vertices, [&](int vi) {
        int rel = -1;
        float d1 = distances[vi];
        if (d1 < 0.0f) {
            for (int i = 0; i < num_vertices; ++i) {
                float d2 = distances[i];
                if (d2 > 0.0f && near_equal(vertices[vi], vertices[i] - plane_normal * (d2 * 2.0f))) {
                    float3 n1 = normals[vi];
                    float3 n2 = plane_mirror(normals[i], plane_normal);
                    if (dot(n1, n2) >= 0.99f) {
                        rel = i;
                        ++ret;
                        break;
                    }
                }
            }
        }
        relation[vi] = rel;
    });
    return ret;
}

npAPI void npApplyMirroring(int num_vertices, const int relation[], float3 plane_normal, float3 normals[])
{
    parallel_for(0, num_vertices, [&](int vi) {
        if (relation[vi] != -1) {
            normals[relation[vi]] = plane_mirror(normals[vi], plane_normal);
        }
    });
}


npAPI void npProjectNormals(
    int num_vertices, int num_triangles, const float3 vertices[], const float3 normals[], float selection[], const float4x4 *trans,
    const float3 pvertices[], const float3 pnormals[], const int pindices[], const float4x4 *ptrans,
    float3 dst[])
{
    auto mat = *ptrans * invert(*trans);
    RawVector<float> soa[9]; // flattened + SoA-nized vertices (faster on CPU)

    // flatten + SoA-nize
    {
        for (int i = 0; i < 9; ++i) {
            soa[i].resize(num_triangles);
        }
        for (int ti = 0; ti < num_triangles; ++ti) {
            for (int i = 0; i < 3; ++i) {
                auto p = mul_p(mat, pvertices[pindices[ti * 3 + i]]);
                soa[i * 3 + 0][ti] = p.x;
                soa[i * 3 + 1][ti] = p.y;
                soa[i * 3 + 2][ti] = p.z;
            }
        }
    }

    parallel_for(0, num_vertices, [&](int ri) {
        float3 rpos = vertices[ri];
        float3 rdir = normals[ri];
        int ti;
        float distance;
        int num_hit = RayTrianglesIntersection(rpos, rdir,
            soa[0].data(), soa[1].data(), soa[2].data(),
            soa[3].data(), soa[4].data(), soa[5].data(),
            soa[6].data(), soa[7].data(), soa[8].data(),
            num_triangles, ti, distance);

        if (num_hit > 0) {
            float3 result = triangle_interpolation(
                rpos + rdir * distance,
                { soa[0][ti], soa[1][ti], soa[2][ti] },
                { soa[3][ti], soa[4][ti], soa[5][ti] },
                { soa[6][ti], soa[7][ti], soa[8][ti] },
                pnormals[pindices[ti * 3 + 0]],
                pnormals[pindices[ti * 3 + 1]],
                pnormals[pindices[ti * 3 + 2]]);

            result = normalize(mul_v(mat, result));
            float s = selection ? selection[ri] : 1.0f;
            dst[ri] = normalize(lerp(dst[ri], result, s));
        }
    });
}


template<int NumInfluence>
static void SkinningImpl(
    int num_vertices, const RawVector<float4x4>& poses, const Weights<NumInfluence> weights[],
    const float3 ipoints[], const float3 inormals[], const float4 itangents[],
    float3 opoints[], float3 onormals[], float4 otangents[])
{
    parallel_invoke(
    [&]() {
        if (ipoints && opoints) {
            for (int vi = 0; vi < num_vertices; ++vi) {
                const auto& w = weights[vi];
                float3 p = ipoints[vi];
                float3 rp = float3::zero();
                for (int bi = 0; bi < NumInfluence; ++bi) {
                    rp += mul_p(poses[w.indices[bi]], p) * w.weights[bi];
                }
                opoints[vi] = rp;
            }
        }
    },
    [&]() {
        if (inormals && onormals) {
            for (int vi = 0; vi < num_vertices; ++vi) {
                const auto& w = weights[vi];
                float3 n = inormals[vi];
                float3 rn = float3::zero();
                for (int bi = 0; bi < NumInfluence; ++bi) {
                    rn += mul_v(poses[w.indices[bi]], n) * w.weights[bi];
                }
                onormals[vi] = normalize(rn);
            }
        }
    },
    [&]() {
        if (itangents && otangents) {
            for (int vi = 0; vi < num_vertices; ++vi) {
                const auto& w = weights[vi];
                float4 t = itangents[vi];
                float4 rt = float4::zero();
                for (int bi = 0; bi < NumInfluence; ++bi) {
                    rt += mul_v(poses[w.indices[bi]], t) * w.weights[bi];
                }
                otangents[vi] = rt;
            }
        }
    });
}

npAPI void npApplySkinning(
    int num_vertices, const Weights4 weights[], int num_bones, const float4x4 bones[], const float4x4 bindposes[], const float4x4 *root,
    const float3 ipoints[], const float3 inormals[], const float4 itangents[],
    float3 opoints[], float3 onormals[], float4 otangents[])
{
    RawVector<float4x4> poses;
    poses.resize(num_bones);

    auto iroot = invert(*root);
    for (int bi = 0; bi < num_bones; ++bi) {
        poses[bi] = bindposes[bi] * bones[bi] * iroot;
    }
    SkinningImpl(num_vertices, poses, weights, ipoints, inormals, itangents, opoints, onormals, otangents);
}

npAPI void npApplyReverseSkinning(
    int num_vertices, const Weights4 weights[], int num_bones, const float4x4 bones[], const float4x4 bindposes[], const float4x4 *root,
    const float3 ipoints[], const float3 inormals[], const float4 itangents[],
    float3 opoints[], float3 onormals[], float4 otangents[])
{
    RawVector<float4x4> poses;
    poses.resize(num_bones);

    auto iroot = invert(*root);
    for (int bi = 0; bi < num_bones; ++bi) {
        poses[bi] = invert(bindposes[bi] * bones[bi] * iroot);
    }
    SkinningImpl(num_vertices, poses, weights, ipoints, inormals, itangents, opoints, onormals, otangents);
}



float g_pen_pressure = 1.0f;

npAPI float npGetPenPressure()
{
    return g_pen_pressure;
}

void npInitializePenInput_Win();

npAPI void npInitializePenInput()
{
#ifdef npEnablePenTablet
#ifdef _WIN32
    npInitializePenInput_Win();
#endif
#endif
}

