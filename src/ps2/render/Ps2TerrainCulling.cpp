#include "ps2/render/Ps2TerrainCulling.h"

#ifdef PS2_PLATFORM

#include "platform/PlatformTuning.h"
#include "ps2/render/Ps2ClipGuard.h"

namespace
{
constexpr float kClusterPlaneEpsilon = 1e-4f;

#if PS2_VU0_CLUSTER_CULL
// Risk bit each lane of Ps2TerrainCullingContext::riskBlock contributes. The
// block is ordered by this table rather than by source array, so the classifier
// never has to map a lane back to frustum[] or guardBand[]. Padding lanes carry
// 0 and are additionally neutral by construction.
constexpr int kRiskLaneBits[2][4] = {
    { PS2_CLUSTER_GUARD_NEAR, PS2_CLUSTER_GUARD_NEAR,
      PS2_CLUSTER_GUARD_SIDE, PS2_CLUSTER_GUARD_SIDE },
    { PS2_CLUSTER_GUARD_SIDE, PS2_CLUSTER_GUARD_SIDE, 0, 0 }
};
#endif

void setPlane(float* out, const float* mvp, int row, float sign)
{
    out[0] = mvp[3]  + sign * mvp[row];
    out[1] = mvp[7]  + sign * mvp[4 + row];
    out[2] = mvp[11] + sign * mvp[8 + row];
    out[3] = mvp[15] + sign * mvp[12 + row];
}

void buildClusterFrustum(Ps2TerrainCullingContext& out, const float* mvp)
{
    setPlane(out.frustum[0], mvp, 0,  1.0f);
    setPlane(out.frustum[1], mvp, 0, -1.0f);
    setPlane(out.frustum[2], mvp, 1,  1.0f);
    setPlane(out.frustum[3], mvp, 1, -1.0f);
    setPlane(out.frustum[4], mvp, 2,  1.0f);
    setPlane(out.frustum[5], mvp, 2, -1.0f);
}

void setGuardPlane(float* out, const float* mvp,
                   int row, float sign, float wScale)
{
    out[0] = wScale * mvp[3]  + sign * mvp[row];
    out[1] = wScale * mvp[7]  + sign * mvp[4 + row];
    out[2] = wScale * mvp[11] + sign * mvp[8 + row];
    out[3] = wScale * mvp[15] + sign * mvp[12 + row];
}

void buildClusterGuardBand(Ps2TerrainCullingContext& out,
                           const float* mvp,
                           float viewW,
                           float viewH)
{
    const float gx = ps2_guard_clip_scale(viewW);
    const float gy = ps2_guard_clip_scale(viewH);

    out.guardBand[0][0] = mvp[3];
    out.guardBand[0][1] = mvp[7];
    out.guardBand[0][2] = mvp[11];
    out.guardBand[0][3] = mvp[15] - PS2_NEAR_CLIP_W;
    setGuardPlane(out.guardBand[1], mvp, 0,  1.0f, gx);
    setGuardPlane(out.guardBand[2], mvp, 0, -1.0f, gx);
    setGuardPlane(out.guardBand[3], mvp, 1,  1.0f, gy);
    setGuardPlane(out.guardBand[4], mvp, 1, -1.0f, gy);
}

#if PS2_VU0_CLUSTER_CULL
void buildFrustumBlocks(Ps2TerrainCullingContext& out)
{
    ps2_vu0_plane_block_build(out.frustumBlock[0], &out.frustum[0], 4);
    ps2_vu0_plane_block_build(out.frustumBlock[1], &out.frustum[4], 2);
}

void buildRiskBlocks(Ps2TerrainCullingContext& out)
{
    // The clipped VU1 entry has a w>=epsilon plane, but it does NOT clip the
    // OpenGL homogeneous near plane z+w>=0. Both therefore contribute NEAR, and
    // they lead the block so kRiskLaneBits stays a compile-time constant.
    const float risk[6][4] = {
        { out.frustum[4][0], out.frustum[4][1], out.frustum[4][2], out.frustum[4][3] },
        { out.guardBand[0][0], out.guardBand[0][1], out.guardBand[0][2], out.guardBand[0][3] },
        { out.guardBand[1][0], out.guardBand[1][1], out.guardBand[1][2], out.guardBand[1][3] },
        { out.guardBand[2][0], out.guardBand[2][1], out.guardBand[2][2], out.guardBand[2][3] },
        { out.guardBand[3][0], out.guardBand[3][1], out.guardBand[3][2], out.guardBand[3][3] },
        { out.guardBand[4][0], out.guardBand[4][1], out.guardBand[4][2], out.guardBand[4][3] }
    };
    ps2_vu0_plane_block_build(out.riskBlock[0], &risk[0], 4);
    ps2_vu0_plane_block_build(out.riskBlock[1], &risk[4], 2);
}

// Cluster AABB in the form the COP2 kernel consumes. The scalar classifier
// rebuilt this per plane; here it is built once and reused by every block.
void clusterCenterExtent(const Ps2MeshCluster& cluster,
                         VU_VECTOR& center,
                         VU_VECTOR& extent)
{
    center.x = (cluster.minX + cluster.maxX) * 0.5f;
    center.y = (cluster.minY + cluster.maxY) * 0.5f;
    center.z = (cluster.minZ + cluster.maxZ) * 0.5f;
    center.w = 1.0f;
    extent.x = (cluster.maxX - cluster.minX) * 0.5f;
    extent.y = (cluster.maxY - cluster.minY) * 0.5f;
    extent.z = (cluster.maxZ - cluster.minZ) * 0.5f;
    extent.w = 0.0f;
}
#endif

#if !PS2_VU0_CLUSTER_CULL
void clusterPlaneDistance(const Ps2MeshCluster& cluster,
                          const float* plane,
                          float& distance,
                          float& radius)
{
    const float centerX = (cluster.minX + cluster.maxX) * 0.5f;
    const float centerY = (cluster.minY + cluster.maxY) * 0.5f;
    const float centerZ = (cluster.minZ + cluster.maxZ) * 0.5f;
    const float extentX = (cluster.maxX - cluster.minX) * 0.5f;
    const float extentY = (cluster.maxY - cluster.minY) * 0.5f;
    const float extentZ = (cluster.maxZ - cluster.minZ) * 0.5f;

    distance = plane[0] * centerX + plane[1] * centerY +
        plane[2] * centerZ + plane[3];
    radius =
        (plane[0] < 0.0f ? -plane[0] : plane[0]) * extentX +
        (plane[1] < 0.0f ? -plane[1] : plane[1]) * extentY +
        (plane[2] < 0.0f ? -plane[2] : plane[2]) * extentZ;
}
#endif
}

void ps2_terrain_build_culling_context(Ps2TerrainCullingContext& out,
                                       const float* mvp,
                                       float viewW,
                                       float viewH)
{
    buildClusterFrustum(out, mvp);
    out.guardBandValid = viewW > 0.0f && viewH > 0.0f;
    if (out.guardBandValid)
        buildClusterGuardBand(out, mvp, viewW, viewH);
#if PS2_VU0_CLUSTER_CULL
    buildFrustumBlocks(out);
    // Mirrors the guard band itself: an invalid band is rejected before any
    // cluster reads the blocks, so there is nothing to transpose.
    if (out.guardBandValid)
        buildRiskBlocks(out);
#endif
}

int ps2_terrain_classify_cluster_visibility(const Ps2MeshCluster& cluster,
                                            const Ps2TerrainCullingContext& context)
{
    if (!cluster.valid())
        return 0;

    bool fullyInside = true;
#if PS2_VU0_CLUSTER_CULL
    VU_VECTOR center __attribute__((aligned(16)));
    VU_VECTOR extent __attribute__((aligned(16)));
    clusterCenterExtent(cluster, center, extent);

    for (int block = 0; block < 2; ++block)
    {
        alignas(16) float distance[4];
        alignas(16) float radius[4];
        ps2_vu0_plane_block_eval(context.frustumBlock[block], &center, &extent,
                                 distance, radius);
        for (int lane = 0; lane < 4; ++lane)
        {
            if (distance[lane] + radius[lane] < -kClusterPlaneEpsilon)
                return 0;
            if (distance[lane] - radius[lane] <= kClusterPlaneEpsilon)
                fullyInside = false;
        }
    }
#else
    for (int i = 0; i < 6; ++i)
    {
        float distance;
        float radius;
        clusterPlaneDistance(cluster, context.frustum[i], distance, radius);
        if (distance + radius < -kClusterPlaneEpsilon)
            return 0;
        if (distance - radius <= kClusterPlaneEpsilon)
            fullyInside = false;
    }
#endif

    return fullyInside ? 2 : 1;
}

int ps2_terrain_classify_cluster_guard_risk(const Ps2MeshCluster& cluster,
                                            const Ps2TerrainCullingContext& context)
{
    if (!cluster.valid() || !context.guardBandValid)
        return PS2_CLUSTER_GUARD_NEAR | PS2_CLUSTER_GUARD_SIDE;

    int risk = PS2_CLUSTER_GUARD_SAFE;

#if PS2_VU0_CLUSTER_CULL
    VU_VECTOR center __attribute__((aligned(16)));
    VU_VECTOR extent __attribute__((aligned(16)));
    clusterCenterExtent(cluster, center, extent);

    for (int block = 0; block < 2; ++block)
    {
        alignas(16) float distance[4];
        alignas(16) float radius[4];
        ps2_vu0_plane_block_eval(context.riskBlock[block], &center, &extent,
                                 distance, radius);
        for (int lane = 0; lane < 4; ++lane)
        {
            if (distance[lane] - radius[lane] <= kClusterPlaneEpsilon)
                risk |= kRiskLaneBits[block][lane];
        }
    }
#else
    // The clipped VU1 entry has a w>=epsilon plane, but it does NOT clip the
    // OpenGL homogeneous near plane z+w>=0. Treat intersection with either as
    // NEAR risk so a SIDE-only classification is a real proof that the side
    // clipper can run without ever seeing unsafe near-plane geometry.
    float nearDistance;
    float nearRadius;
    clusterPlaneDistance(cluster, context.frustum[4], nearDistance, nearRadius);
    if (nearDistance - nearRadius <= kClusterPlaneEpsilon)
        risk |= PS2_CLUSTER_GUARD_NEAR;

    for (int i = 0; i < 5; ++i)
    {
        float distance;
        float radius;
        clusterPlaneDistance(cluster, context.guardBand[i], distance, radius);
        if (distance - radius <= kClusterPlaneEpsilon)
        {
            risk |= i == 0
                ? PS2_CLUSTER_GUARD_NEAR
                : PS2_CLUSTER_GUARD_SIDE;
        }
    }
#endif
    return risk;
}

void ps2_terrain_classify_clusters(const Ps2MeshCluster* clusters,
                                   bool sectionFullyInside,
                                   bool nativePathUsable,
                                   const Ps2TerrainCullingContext* context,
                                   int* visibility,
                                   int* guardRisk)
{
    if (!clusters || !visibility || !guardRisk)
        return;

    for (int cluster = 0; cluster < PS2_MESH_CLUSTER_COUNT; ++cluster)
    {
        if (sectionFullyInside)
        {
            visibility[cluster] = clusters[cluster].valid() ? 2 : 0;
        }
        else if (nativePathUsable && context != nullptr)
        {
            visibility[cluster] = ps2_terrain_classify_cluster_visibility(
                clusters[cluster], *context);
        }
        else
        {
            visibility[cluster] = clusters[cluster].valid() ? 1 : 0;
        }

        // Rejected clusters cannot be submitted by either terrain path.
        // Keep a conservative risk value without testing more clip planes;
        // visibility and guard risk are recomputed together on the next call.
        if (visibility[cluster] == 0)
        {
            guardRisk[cluster] = PS2_CLUSTER_GUARD_NEAR | PS2_CLUSTER_GUARD_SIDE;
            continue;
        }

        if (nativePathUsable && context != nullptr && context->guardBandValid)
        {
            guardRisk[cluster] = ps2_terrain_classify_cluster_guard_risk(
                clusters[cluster], *context);
        }
        else
        {
            guardRisk[cluster] = PS2_CLUSTER_GUARD_NEAR | PS2_CLUSTER_GUARD_SIDE;
        }
    }
}

bool ps2_terrain_cluster_can_skip_clip(int visibilityClass, int guardRisk)
{
    if (visibilityClass == 0 || guardRisk != PS2_CLUSTER_GUARD_SAFE)
        return false;
#if PS2_VU1_GUARD_BAND_PARTIALS
    return true;
#else
    return visibilityClass == 2;
#endif
}

#endif
