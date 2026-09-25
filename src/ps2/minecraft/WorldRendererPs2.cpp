#ifdef PS2_PLATFORM

#include "net/minecraft/src/WorldRenderer.h"
#include "java/Arithmetic.h"

#include "platform/RenderAPI.h"
#include "platform/Log.h"
#include "platform/RenderTerrainAPI.h"
#include "net/minecraft/src/World.h"
#include "net/minecraft/src/Config.h"
#include "net/minecraft/src/ConnectedTextures.h"
#include "net/minecraft/src/Block.h"
#ifdef PS2_MERGE_WATER_TOPS
#include "net/minecraft/src/CustomColorizer.h"
#endif
#include "net/minecraft/src/RenderBlocks.h"
#include "net/minecraft/src/Tessellator.h"
#include "net/minecraft/src/Chunk.h"
#include "net/minecraft/src/ChunkCache.h"
#include "net/minecraft/src/ExtendedBlockStorage.h"
#include "net/minecraft/src/TileEntity.h"
#include "net/minecraft/src/TileEntityRenderer.h"
#include "platform/PlatformTuning.h"
#include "platform/PlatformCompat.h"

#include <algorithm>
#include <cstdint>
#include <utility>

#include "platform/Profiler.h"
#include "platform/ExtendedProfiler.h"
#include "platform/WorkProfiler.h"
#include "platform/RenderTerrainStaging.h"
#include "ps2/render/Ps2TerrainMesh.h"
#include "ps2/render/Ps2BlockRenderInfo.h"
#include "ps2/render/Ps2CubeFaceMask.h"
#include "ps2/render/Ps2GreedyMesh.h"
#ifdef PS2_MERGE_WATER_TOPS
#include "ps2/render/Ps2WaterSurfaceMerge.h"
#endif
#include "ps2/render/Ps2MeshStagingPool.h"
#include "ps2/render/Ps2SectionVisibility.h"

namespace
{
    static_assert(PLATFORM_CHUNK_BUILD_TIME_CHECK_BLOCKS > 0,
        "PS2 chunk build timer interval must be positive");

    // Interpolated eye position for the frame being drawn, world coordinates.
    // Written once per frame by RenderGlobal::renderSortedRenderers.
    static double s_ps2ViewX = 0.0;
    static double s_ps2ViewY = 0.0;
    static double s_ps2ViewZ = 0.0;
    static RenderTerrainFrame s_ps2RendererFrame = {};
    static constexpr size_t kPs2CapturedSlots = 6u; // xyz, uv, rgba (24-byte backend capture)

    // One shared publish scratch for opaque face sorting. The live opaque AoS
    // buffer is not needed after Ps2TerrainMesh has packed the section, so keeping
    // one 24-byte/vertex vector per WorldRenderer wasted several megabytes. A
    // completed section is published on the render thread, one at a time, which
    // makes a single retained high-water scratch sufficient for the whole grid.
    static std::vector<int_t> s_ps2OpaquePublishScratch;
    static RenderTerrainBackendCache s_ps2PublishCache = {};
    static WorldRenderer *s_ps2PublishOwner = nullptr;
    static bool s_ps2PublishHandedOver = false;
    static unsigned int s_ps2PackedFallbackLogCount = 0;

    static void clearOrReleaseOversizedRaw(std::vector<int_t> &buffer)
    {
        if (buffer.capacity() * sizeof(int_t) > PS2_MAX_RETAINED_RAW_MESH_BYTES)
            std::vector<int_t>().swap(buffer);
        else
            buffer.clear();
    }

    static void reserveRawAppendCapacity(std::vector<int_t> &buffer, std::size_t appendCount)
    {
        const std::size_t required = buffer.size() + appendCount;
        if (required <= buffer.capacity())
            return;

        // Grow in 16 KB raw-capture steps instead of std::vector's geometric
        // doubling. The OOM trace failed a 169 KB allocation while several MB
        // were fragmented but free; bounding each growth step avoids asking the
        // allocator for a block nearly twice the mesh actually needs.
        const std::size_t growthInts = (16u * 1024u) / sizeof(int_t);
        const std::size_t target = ((required + growthInts - 1u) / growthInts) * growthInts;
        buffer.reserve(target);
    }

#ifdef PS2_MERGE_WATER_TOPS
    static int_t ps2SectionBlockIndex(int_t x, int_t y, int_t z)
    {
        return x | (z << 4) | (y << 8);
    }

    // Fast path for the interior of a flat source-water surface. The generic
    // fluid renderer evaluates six faces, four interpolated corner heights and
    // flow direction for every block, even though a deep, uninterrupted ocean
    // source has only one visible face and all four heights are identical.
    //
    // Keep the predicate intentionally strict: section borders, shallow water,
    // flowing water, coastlines and any surface with water above fall back to
    // RenderBlocks unchanged. The emitted quad matches the vanilla still-water
    // top layout, so the existing bounded 2x2 merge can consume it normally.
    static bool ps2RenderFlatStillWaterTop(const Ps2MeshSectionCache *sectionCache,
                                           ChunkCache &chunkcache, Tessellator *tessellator,
                                           Block *block, int_t local,
                                           int_t x, int_t y, int_t z)
    {
        if (sectionCache == nullptr || !sectionCache->valid ||
            block == nullptr || block != Block::waterStill ||
            Block::waterStill == nullptr || Block::waterMoving == nullptr)
            return false;

        const int_t lx = local & 15;
        const int_t lz = (local >> 4) & 15;
        const int_t ly = (local >> 8) & 15;
        if (lx <= 0 || lx >= 15 || lz <= 0 || lz >= 15 || ly <= 0 || ly >= 15)
            return false;

        // Only source blocks use the static top texture and zero-flow UVs.
        if (chunkcache.getBlockMetadata(x, y, z) != 0)
            return false;

        const int_t stillId = Block::waterStill->blockID;
        const int_t movingId = Block::waterMoving->blockID;
        const auto blockIdAt = [&](int_t sx, int_t sy, int_t sz) -> int_t {
            return sectionCache->blockIds[static_cast<std::size_t>(
                ps2SectionBlockIndex(sx, sy, sz))];
        };

        // A water block below guarantees the bottom face is hidden. Requiring a
        // full 3x3 source-water neighborhood makes every corner height exactly
        // the same as the generic renderer. No water may sit above any of those
        // samples, otherwise getFluidHeight() would raise that corner to 1.0.
        if (blockIdAt(lx, ly - 1, lz) != stillId)
            return false;
        for (int_t dz = -1; dz <= 1; ++dz)
        {
            for (int_t dx = -1; dx <= 1; ++dx)
            {
                if (blockIdAt(lx + dx, ly, lz + dz) != stillId)
                    return false;
                const int_t above = blockIdAt(lx + dx, ly + 1, lz + dz);
                if (above == stillId || above == movingId)
                    return false;
            }
        }

        const int_t color = CustomColorizer::getFluidColor(block, &chunkcache, x, y, z);
        const float red = (float)(color >> 16 & 0xff) / 255.0f;
        const float green = (float)(color >> 8 & 0xff) / 255.0f;
        const float blue = (float)(color & 0xff) / 255.0f;

        const int_t tile = block->getBlockTextureFromSideAndMetadata(1, 0);
        const tess_coord_t u0 = (tess_coord_t)((tile & 0xf) << 4) / 256.0f;
        const tess_coord_t v0 = (tess_coord_t)(tile & 0xf0) / 256.0f;
        const tess_coord_t u1 = u0 + (tess_coord_t)(16.0f / 256.0f);
        const tess_coord_t v1 = v0 + (tess_coord_t)(16.0f / 256.0f);

        // Four metadata-0 source samples produce the exact 8/9 surface height
        // in RenderBlocks::getFluidHeight(). Use the same float ratio directly
        // so this hot path does not repeat the weighted-height loop per block.
        constexpr float kFlatSourceHeight = 8.0f / 9.0f;
        const float topY = (float)y + kFlatSourceHeight;

        tessellator->setBrightness(block->getMixedBrightnessForBlock(&chunkcache, x, y, z));
        tessellator->setColorOpaque_F(red, green, blue);
        tessellator->addVertexWithUV(x + 0, topY, z + 0, u0, v0);
        tessellator->addVertexWithUV(x + 0, topY, z + 1, u0, v1);
        tessellator->addVertexWithUV(x + 1, topY, z + 1, u1, v1);
        tessellator->addVertexWithUV(x + 1, topY, z + 0, u1, v0);
        return true;
    }
#endif

    static void logPackedFallbackStats()
    {
        const Ps2TerrainMeshBuildStats stats = ps2TerrainMeshBuildStats();
        const unsigned int failures = stats.invalidInput + stats.allocationFailed + stats.emptyRuns;
        if (failures == s_ps2PackedFallbackLogCount)
            return;

        s_ps2PackedFallbackLogCount = failures;
        if (failures <= 4u || (failures & 63u) == 0u)
        {
            MC_LOG_DEBUG("render",
                "[PS2] packed terrain fallback total=%u invalid=%u alloc=%u empty=%u packed=%u\n",
                failures, stats.invalidInput, stats.allocationFailed, stats.emptyRuns, stats.successful);
        }
    }

    // True when every block in this 16x16x16 section is air.
    //
    // Worth asking before sweeping the section, because the sweep is 4096
    // iterations of ChunkCache::getBlockId() -- two indirect calls each, since
    // that one then dispatches to Chunk::getBlockID -- to emit nothing at all.
    // Reading the chunk's block array directly answers the same question with
    // plain byte loads and an early exit on the first solid block.
    //
    // Exact, not a heuristic: it inspects the blocks themselves rather than
    // heightMap, which records the highest LIGHT-BLOCKING block and would
    // therefore call a section "empty" when it holds glass, torches or flowers.
    //
    // The single-column assumption is safe. Section positions are always
    // multiples of 16 on every axis (RenderGlobal::markRenderersForNewPosition
    // derives x/z as j1 * 16 - l1 * l with l a multiple of 16, and y as
    // section * 16), so a section lies inside exactly one chunk column and
    // covers a contiguous 16-block y range of it.
    static bool ps2SectionIsAllAir(World *world, int_t posX, int_t posY, int_t posZ)
    {
        if (world == nullptr || posY < 0 || posY + 16 > Chunk::WORLD_HEIGHT)
            return false;

        const int_t chunkX = JavaArithmetic::intShr(posX, 4);
        const int_t chunkZ = JavaArithmetic::intShr(posZ, 4);
        if (!world->chunkExists(chunkX, chunkZ))
            return false;

        Chunk *chunk = world->getChunkFromChunkCoords(chunkX, chunkZ);
        if (chunk == nullptr || chunk->isEmptyChunk())
            return false;

        // 1.2.5 stores blocks in 16x16x16 ExtendedBlockStorage sections. The
        // section reference count makes this exact O(1) test cheaper than the
        // old Beta contiguous-array sweep.
        return chunk->getAreLevelsEmpty(posY, posY + 15);
    }

    static constexpr unsigned int kMissingNeighbourWest = 1u;
    static constexpr unsigned int kMissingNeighbourEast = 2u;
    static constexpr unsigned int kMissingNeighbourNorth = 4u;
    static constexpr unsigned int kMissingNeighbourSouth = 8u;

    static unsigned int ps2MissingNeighbourMask(World *world, int_t posX, int_t posZ)
    {
        if (world == nullptr)
            return 0u;

        const int_t chunkX = JavaArithmetic::intShr(posX, 4);
        const int_t chunkZ = JavaArithmetic::intShr(posZ, 4);
        unsigned int mask = 0u;
        if (!world->chunkExists(chunkX - 1, chunkZ)) mask |= kMissingNeighbourWest;
        if (!world->chunkExists(chunkX + 1, chunkZ)) mask |= kMissingNeighbourEast;
        if (!world->chunkExists(chunkX, chunkZ - 1)) mask |= kMissingNeighbourNorth;
        if (!world->chunkExists(chunkX, chunkZ + 1)) mask |= kMissingNeighbourSouth;
        return mask;
    }

    static inline bool ps2IsWaterBlockId(int_t id)
    {
        return (Block::waterStill != nullptr && id == Block::waterStill->blockID) ||
               (Block::waterMoving != nullptr && id == Block::waterMoving->blockID);
    }

    static bool ps2IsFullyEnclosedWaterCell(const Ps2MeshSectionCache *sectionCache,
                                            int_t local, int_t lx, int_t ly, int_t lz)
    {
        if (sectionCache == nullptr || !sectionCache->valid ||
            lx <= 0 || lx >= 15 || ly <= 0 || ly >= 15 || lz <= 0 || lz >= 15)
            return false;

        const auto &ids = sectionCache->blockIds;
        return ps2IsWaterBlockId(ids[static_cast<std::size_t>(local - 1)]) &&
               ps2IsWaterBlockId(ids[static_cast<std::size_t>(local + 1)]) &&
               ps2IsWaterBlockId(ids[static_cast<std::size_t>(local - 16)]) &&
               ps2IsWaterBlockId(ids[static_cast<std::size_t>(local + 16)]) &&
               ps2IsWaterBlockId(ids[static_cast<std::size_t>(local - 256)]) &&
               ps2IsWaterBlockId(ids[static_cast<std::size_t>(local + 256)]);
    }

    static bool ps2OpaqueNeighbour(ChunkCache &cache, int_t x, int_t y, int_t z)
    {
        if (y >= 0 && y < Chunk::WORLD_HEIGHT && !cache.hasResidentChunkAtBlock(x, z))
            return true;

        const int_t id = cache.ChunkCache::getBlockId(x, y, z);
        if (id <= 0 || id >= Block::BLOCK_REGISTRY_SIZE)
            return false;

        Block *block = Block::blocksList[id];
        if (block == nullptr)
            return false;
        if (Block::staticOpaqueCubeLookupSafe[id])
            return Block::opaqueCubeLookup[id];
        return block->isOpaqueCube();
    }

    static std::uint8_t ps2ExposedCubeFaceMask(ChunkCache &cache, int_t x, int_t y, int_t z)
    {
        std::uint8_t opaqueMask = 0;
        if (ps2OpaqueNeighbour(cache, x, y - 1, z)) opaqueMask |= 1u << Ps2CubeFaceMask::Down;
        if (ps2OpaqueNeighbour(cache, x, y + 1, z)) opaqueMask |= 1u << Ps2CubeFaceMask::Up;
        if (ps2OpaqueNeighbour(cache, x, y, z - 1)) opaqueMask |= 1u << Ps2CubeFaceMask::North;
        if (ps2OpaqueNeighbour(cache, x, y, z + 1)) opaqueMask |= 1u << Ps2CubeFaceMask::South;
        if (ps2OpaqueNeighbour(cache, x - 1, y, z)) opaqueMask |= 1u << Ps2CubeFaceMask::West;
        if (ps2OpaqueNeighbour(cache, x + 1, y, z)) opaqueMask |= 1u << Ps2CubeFaceMask::East;
        return Ps2CubeFaceMask::exposedFromOpaque(opaqueMask);
    }

}

std::uint8_t WorldRenderer::ps2VisibleFacesFrom(int_t face) const
{
	if (face < 0 || face >= Ps2SectionVisibility::kFaceCount)
		return Ps2SectionVisibility::kAllFaces;
	return ps2PublishedVisibility[face];
}

void WorldRenderer::updateRenderer()
{
    if (!needsUpdate)
        return;

    ps2BuildRendererStep(PLATFORM_CHUNK_BUILD_BLOCKS_PER_STEP);
}

void WorldRenderer::setTerrainViewerPosition(double x, double y, double z)
{
    s_ps2ViewX = x;
    s_ps2ViewY = y;
    s_ps2ViewZ = z;
    renderTerrainCaptureFrame(s_ps2RendererFrame);
}

bool WorldRenderer::lastTerrainBuildStepDidWork() const
{
	return ps2StepDidWork;
}

bool WorldRenderer::terrainSourcesReady() const
{
	if (worldObj == nullptr)
		return false;

	const int_t x0 = posX;
	const int_t z0 = posZ;
	const int_t x1 = posX + sizeWidth;
	const int_t z1 = posZ + sizeDepth;
	const int_t ccx0 = JavaArithmetic::intShr(x0 - 1, 4);
	const int_t ccx1 = JavaArithmetic::intShr(x1 + 1, 4);
	const int_t ccz0 = JavaArithmetic::intShr(z0 - 1, 4);
	const int_t ccz1 = JavaArithmetic::intShr(z1 + 1, 4);

	for (int_t ccx = ccx0; ccx <= ccx1; ++ccx)
	{
		for (int_t ccz = ccz0; ccz <= ccz1; ++ccz)
		{
			if (!worldObj->chunkExists(ccx, ccz) && worldObj->isChunkInLoadRadius(ccx, ccz))
				return false;
		}
	}
	return true;
}

bool WorldRenderer::needsRebuildForPublishedChunk(int_t chunkX, int_t chunkZ) const
{
	if (ps2MissingNeighbourMask == 0u || worldObj == nullptr || !isInitialized)
		return false;

	const int_t rendererChunkX = JavaArithmetic::intShr(posX, 4);
	const int_t rendererChunkZ = JavaArithmetic::intShr(posZ, 4);
	if (chunkX == rendererChunkX - 1 && chunkZ == rendererChunkZ)
		return (ps2MissingNeighbourMask & kMissingNeighbourWest) != 0u;
	if (chunkX == rendererChunkX + 1 && chunkZ == rendererChunkZ)
		return (ps2MissingNeighbourMask & kMissingNeighbourEast) != 0u;
	if (chunkZ == rendererChunkZ - 1 && chunkX == rendererChunkX)
		return (ps2MissingNeighbourMask & kMissingNeighbourNorth) != 0u;
	if (chunkZ == rendererChunkZ + 1 && chunkX == rendererChunkX)
		return (ps2MissingNeighbourMask & kMissingNeighbourSouth) != 0u;
	return false;
}

std::vector<int_t> *WorldRenderer::ps2BuildBuffers() const
{
	return renderTerrainStagingBuffers(ps2BuildStagingSlot);
}

void WorldRenderer::addTerrainMeshRam(RenderTerrainMeshRam &out) const
{
	// Staging is deliberately absent here: those buffers belong to the pool,
	// not to this renderer, and RenderGlobal adds the pool total once so the
	// reported figure still covers the same storage it did before leasing.
	out.liveOpaque += ps2RawBuffer[0].capacity() * sizeof(int_t);
	out.liveTranslucent += ps2RawBuffer[1].capacity() * sizeof(int_t);

	RenderTerrainCacheRamBreakdown cacheRam;
	renderTerrainCacheRamBreakdown(ps2TerrainCache, cacheRam);
	out.packedMesh += cacheRam.packedMeshBytes;
	out.faceGroups += cacheRam.faceGroupBytes;
}

size_t WorldRenderer::sharedOpaquePublishScratchRamBytes()
{
	return s_ps2OpaquePublishScratch.capacity() * sizeof(int_t) +
		renderTerrainCacheRamBytes(s_ps2PublishCache);
}

size_t WorldRenderer::terrainMeshRamBytes() const
{
	RenderTerrainMeshRam ram;
	addTerrainMeshRam(ram);
	return ram.total();
}

size_t WorldRenderer::trimPs2UnusedMeshCapacity()
{
	if (isInitialized || ps2BuildActive)
		return 0;

	const size_t before = terrainMeshRamBytes();
	std::vector<int_t>().swap(ps2RawBuffer[0]);
	std::vector<int_t>().swap(ps2RawBuffer[1]);
	renderTerrainCacheRelease(ps2TerrainCache);

	for (int_t pass = 0; pass < 2; ++pass)
		std::vector<TessellatorTextureMesh>().swap(extraTextureMeshes[pass]);

	const size_t after = terrainMeshRamBytes();
	return before > after ? before - after : 0;
}

size_t WorldRenderer::releasePs2PublishedMeshForBudget()
{
	if (!isInitialized || ps2BuildActive)
		return 0;

	const size_t before = terrainMeshRamBytes();
	removeTileEntityRenderersFromGlobalList();

	std::vector<int_t>().swap(ps2RawBuffer[0]);
	std::vector<int_t>().swap(ps2RawBuffer[1]);
	renderTerrainCacheRelease(ps2TerrainCache);
	for (int_t pass = 0; pass < 2; ++pass)
	{
		std::vector<TessellatorTextureMesh>().swap(extraTextureMeshes[pass]);
		ps2VertexCount[pass] = 0;
		_skipRenderPass[pass] = true;
	}

	ps2MissingNeighbourMask = 0u;
	for (int_t face = 0; face < 6; ++face)
		ps2PublishedVisibility[face] = 0x3f;
	ps2CpuVisible = true;
	isInitialized = false;
	needsUpdate = true;

	const size_t after = terrainMeshRamBytes();
	return before > after ? before - after : 0;
}

WorldRenderer *WorldRenderer::ps2PublishOwnerRenderer()
{
	return s_ps2PublishOwner;
}

void WorldRenderer::ps2ResetBuildState()
{
#if PLATFORM_PS2 && MC_LOG_LEVEL >= 2
	if (ps2BuildActive)
		++ps2BuildRestarts;
#endif
	if (s_ps2PublishOwner == this)
	{
		renderTerrainCacheCancelOpaqueBuild(s_ps2PublishCache);
		s_ps2PublishOwner = nullptr;
		s_ps2PublishHandedOver = false;
	}
	ps2BuildActive = false;
	ps2BuildSourceAvailability = 0u;
	ps2BuildSourceAvailabilityValid = false;
	ps2BuildPass = 0;
	ps2BuildCursor = 0;
	ps2BuildGreedyFace = 0;
	ps2BuildGreedySlice = 0;
	ps2BuildHasPass1 = false;
	ps2BuildDirtyDuringBuild = false;
	ps2BuildOpaqueBits.fill(0);
	ps2BuildOpaqueCount = 0;
	ps2BuildTileEntityRenderers.clear();
	// Abandoning the build returns the staging pair. This is the single place
	// the lease is dropped, which is why every abandon path routes through here
	// -- a slot that is not returned is one fewer build the whole grid can ever
	// start again.
	renderTerrainStagingRelease(ps2BuildStagingSlot);
	ps2BuildStagingSlot = RENDER_TERRAIN_STAGING_INVALID_SLOT;
	for (int_t p = 0; p < 2; ++p)
	{
		ps2BuildVertexCount[p] = 0;
		ps2BuildDrawMode[p] = 7;
		ps2BuildHasTexture[p] = false;
		ps2BuildHasColor[p] = false;
		ps2BuildHasNormals[p] = false;
		ps2BuildDrew[p] = false;
		ps2BuildExtraTextureMeshes[p].clear();
	}
}

bool WorldRenderer::ps2BeginBuildState()
{
	// Releases any slot this renderer already held before taking one, so the
	// restart paths inside a step cannot fail for want of a buffer they were
	// themselves holding a moment earlier.
	ps2ResetBuildState();
	ps2BuildStagingSlot = renderTerrainStagingAcquire();
	std::vector<int_t> *staging = ps2BuildBuffers();
	if (staging == nullptr)
		return false;

	Chunk::isLit = false;
	ps2BuildActive = true;
	ps2BuildPass = 0;
	ps2BuildCursor = 0;
	// Rebuilds are mostly relights/edits of the same section, so the new mesh
	// is about the size of the live one. Pre-reserving that skips the
	// geometric realloc chain on every rebuild; with a build step running
	// every frame, that steady odd-size alloc/free churn fragmented the 32MB
	// heap (the FRAME log showed the arena growing until sbrk free hit 0KB —
	// the idle OOM). The completion swap's shrink_to_fit becomes a no-op when
	// the predicted size matches.
	for (int_t p = 0; p < 2; ++p)
		staging[p].reserve(ps2RawBuffer[p].size());
	return true;
}

bool WorldRenderer::isTerrainBuildInProgress() const
{
	return ps2BuildActive;
}

void WorldRenderer::abandonTerrainBuild()
{
	if (!ps2BuildActive)
		return;
#if MC_LOG_LEVEL > 2
	platformProfileMeshReset(PlatformMeshResetReason::DirtyRestart);
#endif
	ps2ResetBuildState();
}

bool WorldRenderer::ps2BuildRendererStep(int_t blockBudget)
{
	// Cleared before anything can return, and only set once real meshing
	// happens, so every early exit below (the detached-world case and the
	// generation-gate bail-outs) reports "no work" and leaves the caller's
	// per-frame meshing budget intact.
	ps2StepDidWork = false;

	if (worldObj == nullptr)
	{
		ps2ResetBuildState();
		needsUpdate = false;
		return true;
	}

	// (long long)(PlatformCompat::getMonotonicMicros() * 1000ULL) rather than System::nanoTime(): this runs twice per build
	// step -- once here and once at whichever exit is taken -- on every renderer
	// update of every frame, and nanoTime() is five calls deep into newlib on
	// this console. See Ps2Clock.h.
	long long ps2BuildStartNs = (long long)(PlatformCompat::getMonotonicMicros() * 1000ULL);
	int_t x0 = posX,             y0 = posY,              z0 = posZ;
	int_t x1 = posX + sizeWidth, y1 = posY + sizeHeight, z1 = posZ + sizeDepth;

	// Generation gate: the mesh ChunkCache samples a 3x3 chunk area (section plus
	// a 1-block margin). Check source residency BEFORE leasing a staging slot.
	// Renderers waiting on generation used to pin all six staging pairs even
	// though they had not emitted a vertex, starving renderers whose chunks were
	// already ready. A partial build is discarded only when its source set changes.
#if MC_LOG_LEVEL > 2
	const std::uint32_t ps2SourceGateStart = platformProfileRenderPhaseBegin();
#endif
	{
		int_t ccx0 = JavaArithmetic::intShr(x0 - 1, 4), ccx1 = JavaArithmetic::intShr(x1 + 1, 4);
		int_t ccz0 = JavaArithmetic::intShr(z0 - 1, 4), ccz1 = JavaArithmetic::intShr(z1 + 1, 4);
		unsigned int sourceAvailability = 0u;
		unsigned int sourceBit = 1u;
		for (int_t ccx = ccx0; ccx <= ccx1; ccx++)
		{
			for (int_t ccz = ccz0; ccz <= ccz1; ccz++, sourceBit <<= 1)
			{
				if (worldObj->chunkExists(ccx, ccz))
					sourceAvailability |= sourceBit;
			}
		}

		if (ps2BuildActive && ps2BuildSourceAvailabilityValid &&
			ps2BuildSourceAvailability != sourceAvailability)
		{
			// A source column changed while this section was partially staged.
			// Release the slot now; if the full source set is ready below, this
			// same call can immediately start a clean mesh.
#if MC_LOG_LEVEL > 2
			platformProfileMeshReset(PlatformMeshResetReason::SourceChanged);
#endif
			ps2ResetBuildState();
		}

		sourceBit = 1u;
		for (int_t ccx = ccx0; ccx <= ccx1; ccx++)
		{
			for (int_t ccz = ccz0; ccz <= ccz1; ccz++, sourceBit <<= 1)
			{
				if ((sourceAvailability & sourceBit) != 0u)
					continue;

				// With the gameplay generation throttle this may generate the chunk
				// now or leave it pending until a later world tick.
				worldObj->getChunkFromChunkCoords(ccx, ccz);
				if (worldObj->chunkExists(ccx, ccz))
				{
					// The source set changed after the snapshot. Any active partial
					// build must be discarded, but do not reacquire a staging slot on
					// an early-return path.
					if (ps2BuildActive)
					{
#if MC_LOG_LEVEL > 2
						platformProfileMeshReset(PlatformMeshResetReason::SourceGenerated);
#endif
						ps2ResetBuildState();
					}
#if MC_LOG_LEVEL > 2
					platformProfileMeshStage(ps2SourceGateStart, PlatformMeshStage::SourceGate);
#endif
					platformProfileChunkBuild((long long)(PlatformCompat::getMonotonicMicros() * 1000ULL) - ps2BuildStartNs, 0);
					return false;
				}

#if PLATFORM_MESH_WAIT_FOR_PENDING_SOURCES
				// A missing source inside the active cache radius will arrive later.
				// Keep no staging lease while waiting. Outside the radius, ChunkCache
				// may sample air at the cache edge exactly as before.
				//
				// See PLATFORM_MESH_WAIT_FOR_PENDING_SOURCES: once the streaming cache
				// is wider than the render window this branch stalls every visible edge
				// section on the outer ring, so the console profile meshes against the
				// cache edge instead and rebuilds when the column is published.
				if (worldObj->isChunkInLoadRadius(ccx, ccz))
				{
					if (ps2BuildActive)
					{
#if MC_LOG_LEVEL > 2
						platformProfileMeshReset(PlatformMeshResetReason::PendingSource);
#endif
						ps2ResetBuildState();
					}
#if MC_LOG_LEVEL > 2
					platformProfileMeshStage(ps2SourceGateStart, PlatformMeshStage::SourceGate);
#endif
					platformProfileChunkBuild((long long)(PlatformCompat::getMonotonicMicros() * 1000ULL) - ps2BuildStartNs, 0);
					return false;
				}
#endif
			}
		}

		if (!ps2BuildActive && !ps2BeginBuildState())
		{
			// Only source-ready renderers compete for staging slots now.
#if MC_LOG_LEVEL > 2
			platformProfileMeshStage(ps2SourceGateStart, PlatformMeshStage::SourceGate);
#endif
			return false;
		}

		ps2BuildSourceAvailability = sourceAvailability;
		ps2BuildSourceAvailabilityValid = true;
	}
#if MC_LOG_LEVEL > 2
	platformProfileMeshStage(ps2SourceGateStart, PlatformMeshStage::SourceGate);
#endif

	int_t totalBlocks = sizeWidth * sizeHeight * sizeDepth;
	if (blockBudget <= 0)
		blockBudget = totalBlocks;

	int_t processed = 0;
	int_t stepVerticesBuilt = 0;
	while (ps2BuildPass < 2)
	{
#if PLATFORM_SKIP_TRANSPARENT_WORLD_PASS
		if (ps2BuildPass != 0)
		{
			ps2BuildPass++;
			ps2BuildCursor = 0;
			continue;
		}
#endif
		// Nothing in this section renders in pass 1, so do not sweep all 4096
		// blocks again to emit nothing. The pass-0 loop below has already visited
		// every block by the time ps2BuildPass reaches 1, so ps2BuildHasPass1 is
		// final here. Measured: "terrain pass1 withGeom=0" on every section in
		// the FRAME log, i.e. this second sweep was pure cost for most of the
		// world -- roughly half of the whole meshing budget.
		if (ps2BuildPass == 1 && !ps2BuildHasPass1)
		{
			ps2BuildPass++;
			ps2BuildCursor = 0;
			continue;
		}

		// Empty section: skip the pass the same way the pass-1 check above does,
		// before a ChunkCache, a RenderBlocks and a Tessellator are set up for a
		// sweep that cannot emit anything. Tested once per pass (cursor at 0)
		// rather than once per step. ps2BuildDrew[] stays false, so the
		// completion code below marks both render passes skipped, exactly as a
		// full sweep over air would have.
		if (ps2BuildCursor == 0 && ps2SectionIsAllAir(worldObj, posX, posY, posZ))
		{
			ps2BuildPass++;
			ps2BuildCursor = 0;
			continue;
		}

		ps2StepDidWork = true;
#if defined(PS2_RENDER_STATS)
		const int_t ps2ProfileMeshPass = ps2BuildPass;
		const long long ps2ProfileMeshPassStartNs = (long long)(PlatformCompat::getMonotonicMicros() * 1000ULL);
#endif
		int_t margin = 1;
#if MC_LOG_LEVEL >= 2
		const std::uint32_t ps2CacheSetupStart = platformProfileRenderPhaseBegin();
#endif
		ChunkCache chunkcache(worldObj, x0 - margin, y0 - margin, z0 - margin,
		                                x1 + margin, y1 + margin, z1 + margin);
		Ps2MeshSectionCache *ps2BuildSectionCache = ps2_mesh_staging_section_cache(ps2BuildStagingSlot);
#if MC_LOG_LEVEL >= 2
		platformProfileMeshWork(ps2CacheSetupStart, PlatformMeshWork::CacheSetup);
#endif

		bool stepDrew = false;
#if PLATFORM_ENABLE_GREEDY_MESH
		const bool allowOptiFineGreedyMesh = !Config::isConnectedTextures() && !Config::isNaturalTextures();
		// Greedy pass: a bounded run of independent planes from one face
		// direction per build step, not all six directions at once.
		//
		// This used to run the whole section -- 6 directions x 16 slices x 256
		// cells = 24576 makeFaceKey probes -- on the single step where
		// ps2BuildCursor was 0, completely outside the blockBudget that governs
		// the per-block loop below. Measured 2026-07-28 that showed up as
		// "chunk build max=191.3ms" with the "build" render phase reaching 27ms
		// per frame while terrain streamed, which is the visible stutter (the
		// per-frame average was already fine at ~4ms).
		//
		// Slicing a face further caps the tail without changing its output: a
		// rectangle lies on exactly one plane, so planes can never merge with one
		// another. The per-block loop still skips greedy cubes so nothing is drawn
		// twice.
		if (allowOptiFineGreedyMesh && ps2BuildPass == 0 && ps2BuildGreedyFace < RENDER_TERRAIN_GREEDY_FACE_COUNT)
		{
#if MC_LOG_LEVEL >= 2
			const std::uint32_t ps2GreedyWorkStart = platformProfileRenderPhaseBegin();
#endif
			if (ps2BuildSectionCache != nullptr &&
				(!ps2BuildSectionCache->valid ||
				 ps2BuildSectionCache->originX != x0 ||
				 ps2BuildSectionCache->originY != y0 ||
				 ps2BuildSectionCache->originZ != z0))
			{
				ps2_prepare_greedy_section_cache(chunkcache, x0, y0, z0, *ps2BuildSectionCache);
			}

			const int_t slicesPerStep = PS2_GREEDY_SLICES_PER_STEP < 1
				? 1
				: (PS2_GREEDY_SLICES_PER_STEP > 16 ? 16 : PS2_GREEDY_SLICES_PER_STEP);
			const int_t greedySliceLimit = std::min(16, ps2BuildGreedySlice + slicesPerStep);

#if MC_LOG_LEVEL > 2
			const std::uint32_t ps2GreedyStageStart = platformProfileRenderPhaseBegin();
#endif
			// Build one independent plane at a time. Cheap planes can still consume
			// the configured slices-per-step throughput, while an expensive plane
			// yields once this updateRenderer() reaches the same elapsed-time bound
			// used by the normal block scan. Greedy rectangles never span planes, so
			// splitting the old multi-plane call does not change mesh output.
			std::vector<int_t> &greedyRaw = ps2BuildBuffers()[0];
			const Ps2GreedyRawTarget greedyTarget = { &greedyRaw, posX, posY, posZ };
			int_t faceVerts = 0;
			while (ps2BuildGreedySlice < greedySliceLimit)
			{
				const int_t sliceBegin = ps2BuildGreedySlice;
				const int_t sliceEnd = sliceBegin + 1;

				int_t greedyX0 = x0;
				int_t greedyY0 = y0;
				int_t greedyZ0 = z0;
				int_t greedyX1 = x1;
				int_t greedyY1 = y1;
				int_t greedyZ1 = z1;
				if (ps2BuildGreedyFace <= 1)
				{
					greedyY0 = y0 + sliceBegin;
					greedyY1 = y0 + sliceEnd;
				}
				else if (ps2BuildGreedyFace <= 3)
				{
					greedyZ0 = z0 + sliceBegin;
					greedyZ1 = z0 + sliceEnd;
				}
				else
				{
					greedyX0 = x0 + sliceBegin;
					greedyX1 = x0 + sliceEnd;
				}

				const int_t sliceVerts = ps2BuildSectionCache != nullptr && ps2BuildSectionCache->valid
					? ps2_greedy_mesh_face_raw(chunkcache, ps2BuildGreedyFace,
					                                 greedyX0, greedyY0, greedyZ0,
					                                 greedyX1, greedyY1, greedyZ1,
					                                 greedyTarget, *ps2BuildSectionCache)
					: ps2_greedy_mesh_face_raw(chunkcache, ps2BuildGreedyFace,
					                                 greedyX0, greedyY0, greedyZ0,
					                                 greedyX1, greedyY1, greedyZ1,
					                                 greedyTarget);
				faceVerts += sliceVerts;
				ps2BuildGreedySlice = sliceEnd;

				if (ps2BuildGreedySlice >= 16)
				{
					ps2BuildGreedySlice = 0;
					ps2BuildGreedyFace++;
					break;
				}

				if (PLATFORM_CHUNK_BUILD_STEP_US > 0)
				{
					const long long elapsedNs =
						(long long)(PlatformCompat::getMonotonicMicros() * 1000ULL) - ps2BuildStartNs;
					if (elapsedNs >= (long long)PLATFORM_CHUNK_BUILD_STEP_US * 1000LL)
						break;
				}
			}

			stepDrew = faceVerts > 0;
#if MC_LOG_LEVEL >= 2
			platformProfileMeshWork(ps2GreedyWorkStart, PlatformMeshWork::Greedy);
#endif
#if MC_LOG_LEVEL > 2
			platformProfileMeshStage(ps2GreedyStageStart, PlatformMeshStage::Greedy);
#endif

			if (faceVerts > 0)
			{
				ps2BuildVertexCount[0] += faceVerts;
				ps2BuildDrawMode[0] = renderPrimitiveValue(RenderPrimitive::Quads);
				ps2BuildHasTexture[0] = true;
				ps2BuildHasColor[0] = true;
				ps2BuildDrew[0] = true;
			}

			const long long ps2GreedyElapsedNs = (long long)(PlatformCompat::getMonotonicMicros() * 1000ULL) - ps2BuildStartNs;
			platformProfileChunkBuild(ps2GreedyElapsedNs, faceVerts);
#if defined(PS2_RENDER_STATS)
			platformProfileChunkMeshPass(0,
				(long long)(PlatformCompat::getMonotonicMicros() * 1000ULL) - ps2ProfileMeshPassStartNs,
				faceVerts);
#endif
			return false;
		}
#endif
#if MC_LOG_LEVEL >= 2
		const std::uint32_t ps2ScanSetupStart = platformProfileRenderPhaseBegin();
#endif
		RenderBlocks ps2Renderblocks(&chunkcache);
		Tessellator *ps2Tessellator = &Tessellator::instance;
		const ExtendedBlockStorage *ps2BuildSection = chunkcache.getResidentBlockStorageAt(x0, y0, z0);
		const std::vector<byte_t> *ps2BuildBlockLsb = ps2BuildSection != nullptr &&
			ps2BuildSection->getBlockMSBArray() == nullptr
			? &ps2BuildSection->func_48692_g()
			: nullptr;
		ps2Tessellator->startDrawingQuads();
		ps2Tessellator->setTranslationD(-(double)posX, -(double)posY, -(double)posZ);
#if MC_LOG_LEVEL >= 2
		platformProfileMeshWork(ps2ScanSetupStart, PlatformMeshWork::ScanSetup);
		const std::uint32_t ps2BlockScanStart = platformProfileRenderPhaseBegin();
#endif
		while (ps2BuildCursor < totalBlocks && processed < blockBudget)
		{
			// Block count is only a rough cost estimate: air is nearly free while a
			// visible block may emit several faces. Stop dense sections by elapsed
			// time, leaving ps2BuildCursor on the next unprocessed block. Checking
			// in small groups keeps the timer overhead out of the per-block hot path.
			if (processed > 0 &&
				(processed % PLATFORM_CHUNK_BUILD_TIME_CHECK_BLOCKS) == 0 &&
				PLATFORM_CHUNK_BUILD_STEP_US > 0)
			{
				const long long elapsedNs = (long long)(PlatformCompat::getMonotonicMicros() * 1000ULL) - ps2BuildStartNs;
				if (elapsedNs >= (long long)PLATFORM_CHUNK_BUILD_STEP_US * 1000LL)
					break;
			}

			int_t local = ps2BuildCursor++;
			processed++;
			int_t lx = local & 15;
			int_t lz = (local >> 4) & 15;
			int_t ly = (local >> 8) & 15;
			int_t x = x0 + lx;
			int_t y = y0 + ly;
			int_t z = z0 + lz;

			int_t id = 0;
			if (ps2BuildSectionCache != nullptr && ps2BuildSectionCache->valid)
				id = ps2BuildSectionCache->blockIds[static_cast<std::size_t>(local)];
			else if (ps2BuildBlockLsb != nullptr && static_cast<std::size_t>(local) < ps2BuildBlockLsb->size())
				id = (*ps2BuildBlockLsb)[static_cast<std::size_t>(local)] & 0xff;
			else if (ps2BuildSection != nullptr)
				id = ps2BuildSection->getExtBlockID(lx, ly, lz);
			else
				id = chunkcache.getBlockId(x, y, z);
			if (id <= 0)
				continue;

			Block *block = Block::blocksList[id];
			if (block == nullptr)
				continue;

			const Ps2BlockRenderInfo &renderInfo = ps2GetBlockRenderInfo(id);
			if (ps2BuildPass == 0 && renderInfo.simpleOpaqueCube)
			{
				const std::size_t opaqueWord = static_cast<std::size_t>(local >> 6);
				ps2BuildOpaqueBits[opaqueWord] |= std::uint64_t{1} << (local & 63);
				++ps2BuildOpaqueCount;
			}

			// Render pass is static per registered block in 1.2.5. Cache it once by
			// block ID instead of paying a virtual call for every non-air cell in
			// every incremental section sweep.
			const int_t blockPass = renderInfo.renderPass;
			if (ps2BuildPass == 0 && blockPass != 0)
				ps2BuildHasPass1 = true;

#if PLATFORM_ENABLE_GREEDY_MESH
			// Already emitted by the greedy pass for the opaque pass.
			if (allowOptiFineGreedyMesh && ps2BuildPass == 0 && renderTerrainIsGreedyCube(block))
				continue;
#endif

			if (ps2BuildPass == 0 && Block::isBlockContainer[id])
			{
				TileEntity *te = chunkcache.getBlockTileEntity(x, y, z);
				if (te != nullptr && TileEntityRenderer::instance.hasSpecialRenderer(te) &&
					std::find(ps2BuildTileEntityRenderers.begin(), ps2BuildTileEntityRenderers.end(), te) == ps2BuildTileEntityRenderers.end())
					ps2BuildTileEntityRenderers.push_back(te);
			}

			if (blockPass != ps2BuildPass)
				continue;

			// A fluid block completely surrounded by water cannot emit a face:
			// BlockFluid::shouldSideBeRendered() rejects neighbours with the same
			// material. Deep ocean sections contain thousands of these cells, and
			// routing every one through RenderBlocks repeats six neighbour/material
			// queries plus the fluid setup only to return false. The section cache
			// gives us the exact same answer with six direct 16-bit loads. Keep
			// section-edge cells on the generic path so cross-section/chunk borders,
			// shorelines, flowing water and partially loaded neighbours preserve the
			// existing behaviour.
			if (ps2BuildPass == 1 && ps2IsWaterBlockId(id) &&
				ps2IsFullyEnclosedWaterCell(ps2BuildSectionCache, local, lx, ly, lz))
			{
				continue;
			}

			std::uint8_t exposedFaceMask = Ps2CubeFaceMask::kAllFaces;
#if PLATFORM_SKIP_ENCLOSED_OPAQUE_CUBES || PLATFORM_FAST_SIMPLE_CUBE_RENDER
			if (ps2BuildPass == 0 && renderInfo.simpleOpaqueCube)
			{
				exposedFaceMask = ps2ExposedCubeFaceMask(chunkcache, x, y, z);
#if PLATFORM_SKIP_ENCLOSED_OPAQUE_CUBES
				if (exposedFaceMask == 0)
					continue;
#endif
			}
#endif

#if PLATFORM_FAST_SIMPLE_CUBE_RENDER
			if (ps2BuildPass == 0 && renderInfo.simpleOpaqueCube)
				stepDrew |= ps2Renderblocks.renderSimpleOpaqueCubePs2(block, x, y, z, exposedFaceMask);
			else
#endif
#ifdef PS2_MERGE_WATER_TOPS
			if (ps2BuildPass == 1 && block == Block::waterStill &&
				ps2RenderFlatStillWaterTop(ps2BuildSectionCache, chunkcache, ps2Tessellator,
					block, local, x, y, z))
				stepDrew = true;
			else
#endif
				stepDrew |= ps2Renderblocks.renderBlockByRenderType(block, x, y, z);
		}
#if MC_LOG_LEVEL >= 2
		platformProfileMeshWork(ps2BlockScanStart, PlatformMeshWork::BlockScan);
		const std::uint32_t ps2CaptureStart = platformProfileRenderPhaseBegin();
#endif
#if MC_LOG_LEVEL > 2
		platformProfileMeshStage(ps2BlockScanStart, PlatformMeshStage::BlockScan);
#endif

		// Static scratch: one build step runs per frame for the whole game, and a
		// fresh vector here malloc'd/free'd up to ~100KB every step — steady
		// allocator churn that fragments the small 32MB heap. Reuse one buffer.
		ps2Tessellator->captureTextureGroups(ps2BuildExtraTextureMeshes[ps2BuildPass], true);
		ps2BuildDrew[ps2BuildPass] = ps2BuildDrew[ps2BuildPass] || stepDrew;

		static RenderCapturedMesh stepMesh;
		stepMesh.clear();
		bool captured = ps2Tessellator->capture(stepMesh);
		int_t stepVerts = stepMesh.vertexCount;
		const int_t stepMode = renderPrimitiveValue(stepMesh.primitive);
		const bool stepTex = stepMesh.hasTexture, stepCol = stepMesh.hasColor, stepNorm = stepMesh.hasNormals;
		auto& stepRaw = stepMesh.raw;
		ps2Tessellator->setTranslationD(0.0, 0.0, 0.0);

		if (stepDrew && captured && stepVerts > 0 && !stepRaw.empty())
		{
			// The PS2 terrain cache has one fixed ABI: six int_t per vertex
			// (xyz, uv, rgba). Packed lightmap coordinates are folded into rgba
			// by RenderAPI_GS_PS2 before capture. Reject any backend regression
			// here rather than publishing a mesh with a mismatched stride into
			// Ps2TerrainMesh/VU1, where the wrong count can corrupt GIF/DMA data.
			const size_t slots = kPs2CapturedSlots;
			const bool compactTerrainLayout =
				stepMesh.stride == (int_t)(slots * sizeof(int_t)) &&
				!stepMesh.hasBrightness && !stepMesh.hasNormals;
			if (compactTerrainLayout && (stepRaw.size() % slots) == 0u &&
				(size_t)stepVerts <= stepRaw.size() / slots)
			{
#ifdef PS2_MERGE_WATER_TOPS
                if (ps2BuildPass == 1 && stepTex && stepCol &&
                    stepMode == 7 && Block::waterStill != nullptr)
                {
                    const Ps2WaterMergeStats water = ps2MergeWaterTops(stepRaw,
                        Block::waterStill->getBlockTextureFromSide(1),
                        [&](int lx, int ly, int lz) {
                            return chunkcache.getBlockId(posX+lx, posY+ly, posZ+lz) ==
                                Block::waterStill->blockID &&
                                chunkcache.getBlockMetadata(posX+lx, posY+ly, posZ+lz) == 0;
                        });
                    // Per-step merge statistics are intentionally trace-only. Level-2
                    // profiling runs while terrain streams, and each PS2 log line flushes
                    // stdout; keeping this at DEBUG made the diagnostic itself consume
                    // mesh-budget time and slowed the backlog it was measuring.
                    MC_LOG_TRACE("render", "[PS2] water merge: inputQuads=%u eligible=%u"
                        " rejectShapeUvColor=%u rejectMaterial=%u runBoundaries=%u"
                        " pairs=%u squares=%u removed=%u outputQuads=%u\n",
                        water.input, water.eligible, water.rejectedShape, water.rejectedMaterial,
                        water.boundaries, water.pairs, water.squares, water.removed,
                        water.input-water.removed);
                }
#endif
				stepVerts = (int_t)(stepRaw.size() / slots);
				// Non-null: the build is active, so the lease is held.
				std::vector<int_t> &dst = ps2BuildBuffers()[ps2BuildPass];
				reserveRawAppendCapacity(dst, stepRaw.size());
				dst.insert(dst.end(), stepRaw.begin(), stepRaw.end());
				ps2BuildVertexCount[ps2BuildPass] += stepVerts;
				ps2BuildDrawMode[ps2BuildPass] = stepMode;
				ps2BuildHasTexture[ps2BuildPass] = ps2BuildHasTexture[ps2BuildPass] || stepTex;
				ps2BuildHasColor[ps2BuildPass] = ps2BuildHasColor[ps2BuildPass] || stepCol;
				ps2BuildHasNormals[ps2BuildPass] = ps2BuildHasNormals[ps2BuildPass] || stepNorm;
				ps2BuildDrew[ps2BuildPass] = true;
				stepVerticesBuilt += stepVerts;
			}
		}
#if MC_LOG_LEVEL >= 2
		platformProfileMeshWork(ps2CaptureStart, PlatformMeshWork::Capture);
#endif
#if MC_LOG_LEVEL > 2
		platformProfileMeshStage(ps2CaptureStart, PlatformMeshStage::Capture);
#endif


		if (ps2BuildCursor < totalBlocks)
		{
			const long long ps2StepElapsedNs = (long long)(PlatformCompat::getMonotonicMicros() * 1000ULL) - ps2BuildStartNs;
			platformProfileChunkBuild(ps2StepElapsedNs, stepVerticesBuilt);
#if defined(PS2_RENDER_STATS)
			platformProfileChunkMeshPass(ps2ProfileMeshPass,
				(long long)(PlatformCompat::getMonotonicMicros() * 1000ULL) - ps2ProfileMeshPassStartNs,
				stepVerticesBuilt);
#endif
			return false;
		}

#if defined(PS2_RENDER_STATS)
		platformProfileChunkMeshPass(ps2ProfileMeshPass,
			(long long)(PlatformCompat::getMonotonicMicros() * 1000ULL) - ps2ProfileMeshPassStartNs,
			stepVerticesBuilt);
#endif
		ps2BuildPass++;
		ps2BuildCursor = 0;
		if (processed >= blockBudget && ps2BuildPass < 2)
		{
			platformProfileChunkBuild((long long)(PlatformCompat::getMonotonicMicros() * 1000ULL) - ps2BuildStartNs, stepVerticesBuilt);
			return false;
		}
	}

	if (processed > 0)
	{
		// Sorting, packing and publishing are atomic with respect to rendering.
		// Give that handoff its own update instead of adding it to a block sweep;
		// the live mesh remains untouched until the next call completes it.
		platformProfileChunkBuild((long long)(PlatformCompat::getMonotonicMicros() * 1000ULL) - ps2BuildStartNs, stepVerticesBuilt);
		return false;
	}

#if MC_LOG_LEVEL > 2
	const std::uint32_t ps2PublishStart = platformProfileRenderPhaseBegin();
#endif
	// Non-null: reaching completion means the build ran, so the lease is held.
	// The lease remains held while an async VU0 pack is pending because its raw
	// source must stay byte-stable until the publish cache is complete.
	std::vector<int_t> *staging = ps2BuildBuffers();
	if (staging == nullptr)
	{
		ps2ResetBuildState();
		return false;
	}

	bool packedOpaque = false;
	bool packedOpaqueRequired = false;
	bool handedOver = false;

#if defined(PS2_ENABLE_VU1_TERRAIN) && PS2_DIRECT_VU1_TERRAIN
	packedOpaqueRequired = ps2BuildDrew[0] && ps2BuildVertexCount[0] > 0 && !staging[0].empty();
	if (packedOpaqueRequired)
	{
		// Face-sort scratch and the staging cache are single-owner resources. A
		// second completed renderer waits at publish instead of falling back to CPU
		// or modifying the live cache while the first VU0 job is still in flight.
		if (s_ps2PublishOwner != nullptr && s_ps2PublishOwner != this)
		{
			platformProfileChunkBuild((long long)(PlatformCompat::getMonotonicMicros() * 1000ULL) - ps2BuildStartNs, 0);
			return false;
		}

		bool publishDidWork = false;
		RenderTerrainCacheBuildStatus buildStatus;
		if (s_ps2PublishOwner == nullptr)
		{
			s_ps2PublishOwner = this;
			s_ps2PublishHandedOver = false;
			renderTerrainCacheReset(s_ps2PublishCache);

#if PLATFORM_MESH_FACE_SORT
			if (ps2BuildDrawMode[0] == 7 && !Tessellator::convertQuadsToTriangles)
			{
				const size_t slots = kPs2CapturedSlots;
				const size_t raw = staging[0].size();
				const size_t verts = (raw % slots) == 0u ? raw / slots : 0u;
				const size_t quads = (verts % 4u) == 0u ? verts / 4u : 0u;
				if (quads > 0u && quads <= (size_t)PLATFORM_MESH_SORT_MAX_QUADS)
				{
					if (raw > s_ps2OpaquePublishScratch.capacity())
						s_ps2OpaquePublishScratch.reserve(raw);
					s_ps2OpaquePublishScratch.resize(raw);
#if MC_LOG_LEVEL >= 2
					const std::uint32_t ps2FaceSortStart = platformProfileRenderPhaseBegin();
#endif
					s_ps2PublishHandedOver = renderTerrainCacheSortFaces(s_ps2PublishCache,
						staging[0].data(), s_ps2OpaquePublishScratch.data(), (int_t)quads);
#if MC_LOG_LEVEL >= 2
					platformProfileMeshWork(ps2FaceSortStart, PlatformMeshWork::FaceSort);
#endif
				}
			}
#endif

			const int_t *opaqueRaw = s_ps2PublishHandedOver
				? s_ps2OpaquePublishScratch.data()
				: staging[0].data();
			const size_t opaqueRawInts = s_ps2PublishHandedOver
				? s_ps2OpaquePublishScratch.size()
				: staging[0].size();
#if MC_LOG_LEVEL >= 2
			const std::uint32_t ps2PackStart = platformProfileRenderPhaseBegin();
#endif
			buildStatus = renderTerrainCacheBeginOpaqueBuild(s_ps2PublishCache,
				opaqueRaw, opaqueRawInts, ps2BuildVertexCount[0], ps2BuildDrawMode[0],
				ps2BuildHasTexture[0], ps2BuildHasColor[0], ps2BuildHasNormals[0],
				publishDidWork);
#if MC_LOG_LEVEL >= 2
			platformProfileMeshWork(ps2PackStart, PlatformMeshWork::Pack);
#endif
		}
		else
		{
#if MC_LOG_LEVEL >= 2
			const std::uint32_t ps2PackStart = platformProfileRenderPhaseBegin();
#endif
			buildStatus = renderTerrainCacheContinueOpaqueBuild(s_ps2PublishCache,
				publishDidWork);
#if MC_LOG_LEVEL >= 2
			platformProfileMeshWork(ps2PackStart, PlatformMeshWork::Pack);
#endif
		}

		ps2StepDidWork = publishDidWork;
		if (buildStatus == RenderTerrainCacheBuildStatus::Pending)
		{
#if MC_LOG_LEVEL > 2
			platformProfileMeshStage(ps2PublishStart, PlatformMeshStage::Publish);
#endif
			platformProfileChunkBuild((long long)(PlatformCompat::getMonotonicMicros() * 1000ULL) - ps2BuildStartNs, 0);
			return false;
		}

		handedOver = s_ps2PublishHandedOver;
		packedOpaque = buildStatus == RenderTerrainCacheBuildStatus::Complete &&
			renderTerrainCacheOpaqueValid(s_ps2PublishCache);
		if (!packedOpaque)
			logPackedFallbackStats();
	}
#endif

#if !(defined(PS2_ENABLE_VU1_TERRAIN) && PS2_DIRECT_VU1_TERRAIN) && PLATFORM_MESH_FACE_SORT
	// Preserve the original sorted raw fallback when direct VU1 terrain is
	// disabled. The async publish cache above is only needed by the packed path.
	if (ps2BuildDrew[0] && ps2BuildDrawMode[0] == 7 &&
		!Tessellator::convertQuadsToTriangles && !staging[0].empty())
	{
		const size_t slots = kPs2CapturedSlots;
		const size_t raw = staging[0].size();
		const size_t verts = (raw % slots) == 0u ? raw / slots : 0u;
		const size_t quads = (verts % 4u) == 0u ? verts / 4u : 0u;
		if (quads > 0u && quads <= (size_t)PLATFORM_MESH_SORT_MAX_QUADS)
		{
			if (raw > s_ps2OpaquePublishScratch.capacity())
				s_ps2OpaquePublishScratch.reserve(raw);
			s_ps2OpaquePublishScratch.resize(raw);
#if MC_LOG_LEVEL >= 2
			const std::uint32_t ps2FaceSortStart = platformProfileRenderPhaseBegin();
#endif
			handedOver = renderTerrainCacheSortFaces(ps2TerrainCache,
				staging[0].data(), s_ps2OpaquePublishScratch.data(), (int_t)quads);
#if MC_LOG_LEVEL >= 2
			platformProfileMeshWork(ps2FaceSortStart, PlatformMeshWork::FaceSort);
#endif
		}
	}
#endif

#if MC_LOG_LEVEL >= 2
	const std::uint32_t ps2CommitStart = platformProfileRenderPhaseBegin();
#endif
	// Linear scans instead of hash sets: a section's tile-entity list is a
	// handful of entries at most, so the set allocation was pure overhead paid
	// on every completed rebuild. The visible list changes only after the async
	// packed mesh is ready, keeping tile entities and terrain atomic together.
	const std::vector<TileEntity *> oldTileEntityRenderers = std::move(tileEntityRenderers);
	tileEntityRenderers = ps2BuildTileEntityRenderers;
	for (TileEntity *te : tileEntityRenderers)
	{
		if (std::find(oldTileEntityRenderers.begin(), oldTileEntityRenderers.end(), te) == oldTileEntityRenderers.end())
			pushUniqueTileEntityRef(tileEntities, te);
	}
	for (TileEntity *te : oldTileEntityRenderers)
	{
		if (std::find(tileEntityRenderers.begin(), tileEntityRenderers.end(), te) == tileEntityRenderers.end())
			eraseAllTileEntityRefs(tileEntities, te);
	}

	for (int_t p = 0; p < 2; ++p)
	{
		extraTextureMeshes[p].swap(ps2BuildExtraTextureMeshes[p]);
		ps2BuildExtraTextureMeshes[p].clear();

		const bool opaqueAsync = p == 0 && packedOpaqueRequired;
		const bool passHandedOver = p == 0 && handedOver;

		if (p == 0)
		{
			if (opaqueAsync && (packedOpaque || passHandedOver))
				renderTerrainCacheSwap(ps2TerrainCache, s_ps2PublishCache);
			else
				renderTerrainCacheReset(ps2TerrainCache);
		}

		if (!passHandedOver)
		{
			if (p == 0 && opaqueAsync && packedOpaque)
			{
				// The packed cache owns the new opaque representation. Keep the new
				// staging allocation in the pool for the next build and drop any old
				// per-renderer raw fallback allocation.
				staging[0].clear();
				std::vector<int_t>().swap(ps2RawBuffer[0]);
			}
			else
			{
				ps2RawBuffer[p].swap(staging[p]);
				staging[p].clear();
			}
		}

		ps2VertexCount[p] = ps2BuildVertexCount[p];
		ps2DrawMode[p] = ps2BuildDrawMode[p];
		ps2HasTexture[p] = ps2BuildHasTexture[p];
		ps2HasColor[p] = ps2BuildHasColor[p];
		ps2HasNormals[p] = ps2BuildHasNormals[p];
		const bool hasPublishedRaw = !passHandedOver && !ps2RawBuffer[p].empty();
		const bool hasPrimaryMesh = ps2VertexCount[p] > 0 &&
			(p == 0 ? (packedOpaque || passHandedOver || hasPublishedRaw) : hasPublishedRaw);
		const bool hasExtraMesh = !extraTextureMeshes[p].empty();
		_skipRenderPass[p] = !(ps2BuildDrew[p] && (hasPrimaryMesh || hasExtraMesh));

		if (p == 0 && opaqueAsync)
		{
			if (packedOpaque)
			{
				staging[0].clear();
				if (passHandedOver)
					clearOrReleaseOversizedRaw(s_ps2OpaquePublishScratch);
			}
			else if (passHandedOver)
			{
				// Preserve the sorted AoS fallback together with the sorted face groups
				// that were swapped from the publish cache above.
				ps2RawBuffer[0].swap(s_ps2OpaquePublishScratch);
				staging[0].clear();
				clearOrReleaseOversizedRaw(s_ps2OpaquePublishScratch);
			}
		}
		else if (p == 0 && passHandedOver)
		{
			// Non-VU1 fallback keeps the original one-time face reorder, but does
			// not retain face-group metadata because that path draws the raw AoS.
			ps2RawBuffer[0].swap(s_ps2OpaquePublishScratch);
			staging[0].clear();
			clearOrReleaseOversizedRaw(s_ps2OpaquePublishScratch);
		}
	}

	// Reaching the completion block is itself work -- cache swap, tile-entity diff
	// and metadata publication all happen here even when the VU0 work completed on
	// an earlier scheduler pass.
	ps2StepDidWork = true;

	const bool dirtyDuringBuild = ps2BuildDirtyDuringBuild;
	isChunkLit = Chunk::isLit;
	ps2MissingNeighbourMask = ::ps2MissingNeighbourMask(worldObj, posX, posZ);
#if PLATFORM_CPU_SECTION_OCCLUSION
	Ps2SectionVisibility sectionVisibility;
	sectionVisibility.build(ps2BuildOpaqueBits, ps2BuildOpaqueCount);
	for (int_t face = 0; face < Ps2SectionVisibility::kFaceCount; ++face)
		ps2PublishedVisibility[face] = sectionVisibility.visibleFacesFrom(face);
#else
	for (int_t face = 0; face < 6; ++face)
		ps2PublishedVisibility[face] = 0x3f;
#endif
	isInitialized = true;
	needsUpdate = dirtyDuringBuild;
	int ps2TotalVertices = ps2VertexCount[0] + ps2VertexCount[1];
#if MC_LOG_LEVEL >= 2
	platformProfileMeshWork(ps2CommitStart, PlatformMeshWork::Commit);
#endif
	platformProfileChunkBuild((long long)(PlatformCompat::getMonotonicMicros() * 1000ULL) - ps2BuildStartNs, ps2TotalVertices);
	chunksUpdated++;
#if MC_LOG_LEVEL > 2
	platformProfileMeshStage(ps2PublishStart, PlatformMeshStage::Publish);
	platformProfileMeshReset(PlatformMeshResetReason::Completion);
#endif
	if (s_ps2PublishOwner == this)
	{
		s_ps2PublishOwner = nullptr;
		s_ps2PublishHandedOver = false;
	}
	ps2ResetBuildState();
	return true;
}

void WorldRenderer::renderExtraTerrainMeshes(int_t pass)
{
	if (pass < 0 || pass > 1 || _skipRenderPass[pass] || extraTextureMeshes[pass].empty())
		return;

	renderPushMatrix();
	// PS2's native terrain path receives the section transform directly. Captured
	// CTM groups use the regular matrix path, so reproduce the same translation
	// and the original WorldRenderer's tiny overlap scale here.
	renderTranslate((float)((double)posX - s_ps2ViewX),
	                (float)((double)posY - s_ps2ViewY),
	                (float)((double)posZ - s_ps2ViewZ));
	const float sectionHalf = 8.0f;
	const float sectionScale = 1.000001f;
	renderTranslate(-sectionHalf, -sectionHalf, -sectionHalf);
	renderScale(sectionScale, sectionScale, sectionScale);
	renderTranslate(sectionHalf, sectionHalf, sectionHalf);
	for (const TessellatorTextureMesh &group : extraTextureMeshes[pass])
	{
		renderBindTexture(group.textureId);
		(void)renderDrawCaptured(group.mesh);
	}
	renderPopMatrix();

	// Terrain state assumes /terrain.png is still bound after each section.
	renderBindTexture(ConnectedTextures::getTerrainTextureId());
}

void WorldRenderer::renderPassCached(int_t pass)
{
	if (worldObj == nullptr || pass < 0 || pass > 1 || _skipRenderPass[pass])
		return;
	const bool packedOpaque = pass == 0 && renderTerrainCacheOpaqueValid(ps2TerrainCache);
	const bool hasPrimaryMesh = ps2VertexCount[pass] > 0 &&
		(packedOpaque || !ps2RawBuffer[pass].empty());
	const bool hasExtraMesh = !extraTextureMeshes[pass].empty();
	if (!hasPrimaryMesh && !hasExtraMesh)
		return;

	const size_t slotsPerVertex = kPs2CapturedSlots;
	if (hasPrimaryMesh && !packedOpaque)
	{
		if ((ps2RawBuffer[pass].size() % slotsPerVertex) != 0u)
			return;
		if ((size_t)ps2VertexCount[pass] > ps2RawBuffer[pass].size() / slotsPerVertex)
			return;
	}
	else if (hasPrimaryMesh && renderTerrainCacheOpaqueVertexCount(ps2TerrainCache) < ps2VertexCount[pass])
	{
		return;
	}

	const float chunkTx = (float)((double)posX - s_ps2ViewX);
	const float chunkTy = (float)((double)posY - s_ps2ViewY);
	const float chunkTz = (float)((double)posZ - s_ps2ViewZ);

	RenderTerrainFallbackDraw fallback;
	fallback.user = nullptr;
	fallback.draw = nullptr;

	RenderTerrainSectionView section;
	section.raw = packedOpaque ? nullptr : ps2RawBuffer[pass].data();
	section.rawIntCount = packedOpaque ? 0u : ps2RawBuffer[pass].size();
	section.vertexCount = ps2VertexCount[pass];
	section.drawMode = ps2DrawMode[pass];
	section.hasTexture = ps2HasTexture[pass];
	section.hasColor = ps2HasColor[pass];
	section.hasNormals = ps2HasNormals[pass];
	section.faceGroups = pass == 0 ? renderTerrainCacheFaceGroups(ps2TerrainCache) : nullptr;
	section.opaqueMesh = pass == 0 && renderTerrainCacheOpaqueValid(ps2TerrainCache)
		? renderTerrainCacheOpaqueMesh(ps2TerrainCache)
		: nullptr;
	section.translateX = chunkTx;
	section.translateY = chunkTy;
	section.translateZ = chunkTz;
	section.eyeLocalX = (float)(s_ps2ViewX - (double)posX);
	section.eyeLocalY = (float)(s_ps2ViewY - (double)posY);
	section.eyeLocalZ = (float)(s_ps2ViewZ - (double)posZ);
	section.fullyInside = isFullyInFrustum;
	section.nativeEnabled = true;

	if (hasPrimaryMesh)
		renderTerrainDrawSection(s_ps2RendererFrame, section, fallback);
	renderExtraTerrainMeshes(pass);

}

void WorldRenderer::renderPassImmediate(int_t pass)
{
	renderPassCached(pass);
}



#endif // PS2_PLATFORM
