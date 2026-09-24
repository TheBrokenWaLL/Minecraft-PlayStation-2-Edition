#ifdef PS2_PLATFORM

#include "ps2/render/Ps2MeshSort.h"

#include "ps2/render/Ps2CaptureLayout.h"
#include "platform/PlatformTuning.h"

#include <algorithm>
#include <math.h>
#include <string.h>

namespace
{
	// Capture layout: x y z u v rgba, one int_t each.
	const int kSlots = (int)Ps2CaptureLayout::Slots;
	const int kQuadInts = kSlots * 4;

	// Atlas cells along one axis of the 256x256 terrain atlas. This has to match
	// the granularity ps2_select_clamp keys REGION_REPEAT on (ufix = texel & ~15),
	// because the whole point of the grouping is that quads sharing a group need
	// no clamp change between them.
	const int kTilesPerAxis = 16;
	const int kTileCount = kTilesPerAxis * kTilesPerAxis; // 256
	const int kKeyCount =
		kTileCount * PS2_MESH_CLUSTER_COUNT * PS2_FACE_GROUP_COUNT;
	const float kSectionSize = 16.0f;

	static_assert(PS2_MESH_CLUSTER_AXIS > 0,
		"PS2 mesh clustering needs at least one cell per axis");
	static_assert((16 % PS2_MESH_CLUSTER_AXIS) == 0,
		"PS2 mesh clusters must divide a 16-block section evenly");
	// The full sort key includes the 256 atlas tiles and therefore exceeds
	// 16 bits with 4x4x4 clusters. Only the temporary sorter needs that full
	// key; retained Ps2MeshRange metadata stores face+cluster separately below.
	static_assert(PS2_FACE_GROUP_COUNT * PS2_MESH_CLUSTER_COUNT <= 65536,
		"PS2 retained face/cluster key must fit in unsigned short");
	static_assert(PLATFORM_MESH_SORT_MAX_QUADS <= 65535,
		"PS2 quad indices and radix cursors must fit in unsigned short");
	static_assert(sizeof(Ps2MeshRange) == 8,
		"PS2 mesh range metadata must remain compact");

	static_assert(kKeyCount > 0, "PS2 sort key space must be non-empty");

	// Single-threaded section finalization: keep bounded scratch off the EE
	// stack. Stable byte-wise radix sorting preserves source order for equal
	// tile/face/cluster keys without a dense key-space histogram or per-quad
	// binary search. At 4096 quads this uses 33280 bytes instead of 40960.
	unsigned int s_key[PLATFORM_MESH_SORT_MAX_QUADS];
	unsigned short s_order[2][PLATFORM_MESH_SORT_MAX_QUADS];
	unsigned short s_cursor[256];

	const unsigned short* sortQuadIndices(int_t quadCount)
	{
		unsigned short* input = s_order[0];
		unsigned short* output = s_order[1];
		for (unsigned int shift = 0, remaining = kKeyCount - 1;
		     remaining != 0; shift += 8, remaining >>= 8)
		{
			memset(s_cursor, 0, sizeof(s_cursor));
			for (int_t q = 0; q < quadCount; ++q)
				++s_cursor[(s_key[input[q]] >> shift) & 255u];
			unsigned int base = 0;
			for (int bucket = 0; bucket < 256; ++bucket)
			{
				const unsigned int count = s_cursor[bucket];
				s_cursor[bucket] = (unsigned short)base;
				base += count;
			}
			for (int_t q = 0; q < quadCount; ++q)
			{
				const unsigned short source = input[q];
				output[s_cursor[(s_key[source] >> shift) & 255u]++] = source;
			}
			std::swap(input, output);
		}
		return input;
	}

	inline float asFloat(int_t bits)
	{
		float f;
		__builtin_memcpy(&f, &bits, sizeof(f));
		return f;
	}

	// Which 16x16 atlas cell a quad samples, from the minimum corner of its UV
	// box -- the same corner ps2_select_clamp truncates to build ufix/vfix.
	inline int tileIndex(float minU, float minV)
	{
		int tu = (int)(minU * 256.0f) >> 4;
		int tv = (int)(minV * 256.0f) >> 4;
		if (tu < 0) tu = 0; else if (tu >= kTilesPerAxis) tu = kTilesPerAxis - 1;
		if (tv < 0) tv = 0; else if (tv >= kTilesPerAxis) tv = kTilesPerAxis - 1;
		return tv * kTilesPerAxis + tu;
	}

	inline int clusterCoord(float center)
	{
		int cell = (int)(center * ((float)PS2_MESH_CLUSTER_AXIS / kSectionSize));
		if (cell < 0) cell = 0;
		if (cell >= PS2_MESH_CLUSTER_AXIS) cell = PS2_MESH_CLUSTER_AXIS - 1;
		return cell;
	}

	inline int clusterIndex(float minX, float minY, float minZ,
	                       float maxX, float maxY, float maxZ)
	{
		const int x = clusterCoord((minX + maxX) * 0.5f);
		const int y = clusterCoord((minY + maxY) * 0.5f);
		const int z = clusterCoord((minZ + maxZ) * 0.5f);
		return (z * PS2_MESH_CLUSTER_AXIS + y) * PS2_MESH_CLUSTER_AXIS + x;
	}
}

void Ps2FaceGroups::reset()
{
	valid = false;
	ranges.clear();
	for (int g = 0; g < PS2_FACE_GROUP_COUNT; g++)
	{
		planeMin[g] = 0.0f;
		planeMax[g] = 0.0f;
	}
	for (int c = 0; c < PS2_MESH_CLUSTER_COUNT; c++)
	{
		clusters[c].minX = clusters[c].minY = clusters[c].minZ = 1e30f;
		clusters[c].maxX = clusters[c].maxY = clusters[c].maxZ = -1e30f;
	}
}

void Ps2FaceGroups::release()
{
	std::vector<Ps2MeshRange>().swap(ranges);
	reset();
}

bool ps2_mesh_sort_faces(const int_t *src, int_t *dst, int_t quadCount, Ps2FaceGroups &groups)
{
	groups.reset();

	if (src == nullptr || dst == nullptr || quadCount <= 0)
		return false;
	if (quadCount > (int_t)PLATFORM_MESH_SORT_MAX_QUADS)
		return false;

	float planeMin[PS2_FACE_GROUP_COUNT];
	float planeMax[PS2_FACE_GROUP_COUNT];
	for (int g = 0; g < PS2_FACE_GROUP_COUNT; g++)
	{
		planeMin[g] = 1e30f;
		planeMax[g] = -1e30f;
	}

	// ---- Pass 1: classify every quad ----
	for (int_t q = 0; q < quadCount; q++)
	{
		const int_t *v = src + (size_t)q * kQuadInts;

		const float x0 = asFloat(v[0]),               y0 = asFloat(v[1]),               z0 = asFloat(v[2]);
		const float x1 = asFloat(v[kSlots + 0]),      y1 = asFloat(v[kSlots + 1]),      z1 = asFloat(v[kSlots + 2]);
		const float x2 = asFloat(v[2 * kSlots + 0]),  y2 = asFloat(v[2 * kSlots + 1]),  z2 = asFloat(v[2 * kSlots + 2]);
		const float x3 = asFloat(v[3 * kSlots + 0]),  y3 = asFloat(v[3 * kSlots + 1]),  z3 = asFloat(v[3 * kSlots + 2]);
		float minX = x0, minY = y0, minZ = z0;
		float maxX = x0, maxY = y0, maxZ = z0;
		const float quadX[3] = { x1, x2, x3 };
		const float quadY[3] = { y1, y2, y3 };
		const float quadZ[3] = { z1, z2, z3 };
		for (int corner = 0; corner < 3; corner++)
		{
			if (quadX[corner] < minX) minX = quadX[corner];
			if (quadX[corner] > maxX) maxX = quadX[corner];
			if (quadY[corner] < minY) minY = quadY[corner];
			if (quadY[corner] > maxY) maxY = quadY[corner];
			if (quadZ[corner] < minZ) minZ = quadZ[corner];
			if (quadZ[corner] > maxZ) maxZ = quadZ[corner];
		}

		// Outward normal. Minecraft winds a face so that (p1-p0)x(p2-p0) points
		// away from the block -- verified against RenderBlocks::renderTopFace,
		// whose vertex order (x1,y1,z1)(x1,y1,z0)(x0,y1,z0)(x0,y1,z1) yields
		// +Y, and renderBottomFace, which yields -Y. The GL winding convention
		// is not involved: this is the geometric normal, and the visibility test
		// below is pure geometry.
		const float ax = x1 - x0, ay = y1 - y0, az = z1 - z0;
		const float bx = x2 - x0, by = y2 - y0, bz = z2 - z0;
		const float nx = ay * bz - az * by;
		const float ny = az * bx - ax * bz;
		const float nz = ax * by - ay * bx;

		const float anx = fabsf(nx), any = fabsf(ny), anz = fabsf(nz);

		int bucket = PS2_FACE_OTHER;
		float plane = 0.0f;

		// Dominant axis, then confirm the face really is axis-aligned: the other
		// two normal components negligible AND all four corners on one plane.
		// Anything else (crossed squares, torches, rails, ladders, the fluid
		// surface slope) goes to PS2_FACE_OTHER and is always drawn.
		if (anx >= any && anx >= anz)
		{
			if (anx > 1e-6f && any <= 1e-3f * anx && anz <= 1e-3f * anx &&
			    fabsf(x1 - x0) <= 1e-4f && fabsf(x2 - x0) <= 1e-4f && fabsf(x3 - x0) <= 1e-4f)
			{
				bucket = (nx > 0.0f) ? PS2_FACE_XP : PS2_FACE_XN;
				plane = x0;
			}
		}
		else if (any >= anz)
		{
			if (any > 1e-6f && anx <= 1e-3f * any && anz <= 1e-3f * any &&
			    fabsf(y1 - y0) <= 1e-4f && fabsf(y2 - y0) <= 1e-4f && fabsf(y3 - y0) <= 1e-4f)
			{
				bucket = (ny > 0.0f) ? PS2_FACE_YP : PS2_FACE_YN;
				plane = y0;
			}
		}
		else
		{
			if (anz > 1e-6f && anx <= 1e-3f * anz && any <= 1e-3f * anz &&
			    fabsf(z1 - z0) <= 1e-4f && fabsf(z2 - z0) <= 1e-4f && fabsf(z3 - z0) <= 1e-4f)
			{
				bucket = (nz > 0.0f) ? PS2_FACE_ZP : PS2_FACE_ZN;
				plane = z0;
			}
		}

		if (bucket != PS2_FACE_OTHER)
		{
			if (plane < planeMin[bucket]) planeMin[bucket] = plane;
			if (plane > planeMax[bucket]) planeMax[bucket] = plane;
		}

		// UV box minimum over the four corners, matching ensureClamp.
		float minU = asFloat(v[3]), minV = asFloat(v[4]);
		for (int c = 1; c < 4; c++)
		{
			const float u = asFloat(v[c * kSlots + 3]);
			const float w = asFloat(v[c * kSlots + 4]);
			if (u < minU) minU = u;
			if (w < minV) minV = w;
		}

		const int cluster = clusterIndex(minX, minY, minZ, maxX, maxY, maxZ);
		Ps2MeshCluster &clusterBounds = groups.clusters[cluster];
		if (minX < clusterBounds.minX) clusterBounds.minX = minX;
		if (minY < clusterBounds.minY) clusterBounds.minY = minY;
		if (minZ < clusterBounds.minZ) clusterBounds.minZ = minZ;
		if (maxX > clusterBounds.maxX) clusterBounds.maxX = maxX;
		if (maxY > clusterBounds.maxY) clusterBounds.maxY = maxY;
		if (maxZ > clusterBounds.maxZ) clusterBounds.maxZ = maxZ;

		const int tile = tileIndex(minU, minV);
		const unsigned int key = (unsigned int)(
			(tile * PS2_FACE_GROUP_COUNT + bucket) * PS2_MESH_CLUSTER_COUNT + cluster);
		s_key[q] = key;
		s_order[0][q] = (unsigned short)q;
	}

	for (int g = 0; g < PS2_FACE_GROUP_COUNT; g++)
	{
		if (planeMin[g] <= planeMax[g] && g != PS2_FACE_OTHER)
		{
			groups.planeMin[g] = planeMin[g];
			groups.planeMax[g] = planeMax[g];
		}
	}

	// Sort indices, then emit ranges and copy quads in destination order.
	// Stable ordering is identical to the old source-order scatter.
	const unsigned short* order = sortQuadIndices(quadCount);
	int_t running = 0;
	for (int_t q = 0; q < quadCount; )
	{
		const unsigned int key = s_key[order[q]];
		int_t end = q + 1;
		while (end < quadCount && s_key[order[end]] == key)
			++end;
		const int_t n = end - q;

		Ps2MeshRange range;
		range.firstVertex = running * 4;
		range.quadCount = (unsigned short)n;
		// Tile is needed only for ordering. Draw-time classification needs
		// face+cluster, which remains compact even with 64 clusters, and the
		// key was built as (tile * groups + face) * clusters + cluster -- so
		// dropping the tile is one modulo.
		range.sortKey = (unsigned short)(
			key % (unsigned int)(PS2_FACE_GROUP_COUNT * PS2_MESH_CLUSTER_COUNT));
		groups.ranges.push_back(range);


		running += n;
		q = end;
	}

	for (int_t q = 0; q < quadCount; ++q)
	{
		memcpy(dst + (size_t)q * kQuadInts,
		       src + (size_t)order[q] * kQuadInts,
		       (size_t)kQuadInts * sizeof(int_t));
	}

	groups.valid = running == quadCount && !groups.ranges.empty();
	return groups.valid;
}

#endif // PS2_PLATFORM
