#include "ps2/render/Ps2TerrainMesh.h"

#ifdef PS2_PLATFORM

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <new>
#include <utility>

#include <kernel.h>

#include "ps2/render/Ps2CaptureLayout.h"
#include "ps2/render/Ps2ClipGuard.h"
#include "ps2/render/Ps2NativeDraw.h"
#include "ps2/render/Ps2Vu0MeshFinalize.h"

namespace
{
    static const std::size_t kStreamAlignment = 64u;
    static const float kPositionScale = 1024.0f;
    static const float kUvScale = 4096.0f;
    static const int kStripOrder[4] = { 1, 2, 0, 3 };

    static inline std::uintptr_t alignUp(std::uintptr_t value, std::size_t alignment)
    {
        return (value + alignment - 1u) & ~(std::uintptr_t)(alignment - 1u);
    }

    static inline float slotFloat(const int_t* raw, std::size_t index)
    {
        float value;
        std::memcpy(&value, raw + index, sizeof(value));
        return value;
    }

    static inline short packSigned16(float value)
    {
        if (value < -32768.0f)
            value = -32768.0f;
        else if (value > 32767.0f)
            value = 32767.0f;
        return (short)value;
    }

    static inline unsigned char gsColor(unsigned char value)
    {
        return (unsigned char)(((unsigned int)value * 128u + 127u) / 255u);
    }

    static inline int atlasTile(float uv)
    {
        int tile = ((int)(uv * 256.0f)) >> 4;
        if (tile < 0)
            return 0;
        if (tile > 15)
            return 15;
        return tile;
    }

    static void packBatchCpu(const int_t* raw, std::size_t slotsPerVertex,
                             int_t firstVertex, int_t vertexCount,
                             short* positionOut, short* uvOut)
    {
        const int_t endVertex = firstVertex + vertexCount;
        for (int_t vertex = firstVertex; vertex < endVertex; ++vertex)
        {
            const int_t sourceVertex = (vertex & ~3) + kStripOrder[vertex & 3];
            const std::size_t base = (std::size_t)sourceVertex * slotsPerVertex;
            const float x = slotFloat(raw, base + 0u);
            const float y = slotFloat(raw, base + 1u);
            const float z = slotFloat(raw, base + 2u);
            const float u = slotFloat(raw, base + 3u);
            const float v = slotFloat(raw, base + 4u);

            positionOut[(std::size_t)vertex * 4u + 0u] = packSigned16((x - 8.0f) * kPositionScale);
            positionOut[(std::size_t)vertex * 4u + 1u] = packSigned16((y - 8.0f) * kPositionScale);
            positionOut[(std::size_t)vertex * 4u + 2u] = packSigned16((z - 8.0f) * kPositionScale);
            positionOut[(std::size_t)vertex * 4u + 3u] = 4096;
            uvOut[(std::size_t)vertex * 2u + 0u] = packSigned16(u * kUvScale);
            uvOut[(std::size_t)vertex * 2u + 1u] = packSigned16(v * kUvScale);
        }
    }

    static void packBatchVu0(const volatile int_t* transformed,
                             std::size_t slotsPerVertex, int_t firstVertex,
                             int_t vertexCount, short* positionOut, short* uvOut)
    {
        const int_t endVertex = firstVertex + vertexCount;
        for (int_t vertex = firstVertex; vertex < endVertex; ++vertex)
        {
            const int_t sourceVertex = (vertex & ~3) + kStripOrder[vertex & 3];
            const int_t localSourceVertex = sourceVertex - firstVertex;
            const std::size_t base = (std::size_t)localSourceVertex * slotsPerVertex;

            positionOut[(std::size_t)vertex * 4u + 0u] = (short)transformed[base + 0u];
            positionOut[(std::size_t)vertex * 4u + 1u] = (short)transformed[base + 1u];
            positionOut[(std::size_t)vertex * 4u + 2u] = (short)transformed[base + 2u];
            positionOut[(std::size_t)vertex * 4u + 3u] = 4096;
            uvOut[(std::size_t)vertex * 2u + 0u] = (short)transformed[base + 3u];
            uvOut[(std::size_t)vertex * 2u + 1u] = (short)transformed[base + 4u];
        }
    }

    static void processBatchColorsAndRuns(const int_t* raw,
                                          std::size_t slotsPerVertex,
                                          int_t firstVertex, int_t vertexCount,
                                          bool hasColor, unsigned char* colorOut,
                                          std::vector<Ps2TerrainTileRun>& runs,
                                          int& currentTileX, int& currentTileY)
    {
        const int_t endVertex = firstVertex + vertexCount;
        for (int_t vertex = firstVertex; vertex < endVertex; ++vertex)
        {
            const int_t sourceVertex = (vertex & ~3) + kStripOrder[vertex & 3];
            const std::size_t base = (std::size_t)sourceVertex * slotsPerVertex;

            if (hasColor)
            {
                const unsigned char* packed = reinterpret_cast<const unsigned char*>(raw + base + 5u);
                colorOut[(std::size_t)vertex * 4u + 0u] = gsColor(packed[0]);
                colorOut[(std::size_t)vertex * 4u + 1u] = gsColor(packed[1]);
                colorOut[(std::size_t)vertex * 4u + 2u] = gsColor(packed[2]);
                colorOut[(std::size_t)vertex * 4u + 3u] = (unsigned char)(packed[3] >> 1);
            }
            else
            {
                colorOut[(std::size_t)vertex * 4u + 0u] = 128;
                colorOut[(std::size_t)vertex * 4u + 1u] = 128;
                colorOut[(std::size_t)vertex * 4u + 2u] = 128;
                colorOut[(std::size_t)vertex * 4u + 3u] = 127;
            }

            if ((vertex & 3) == 0)
            {
                float minU = slotFloat(raw, base + 3u);
                float minV = slotFloat(raw, base + 4u);
                for (int corner = 0; corner < 4; ++corner)
                {
                    const std::size_t cornerBase =
                        (std::size_t)(vertex + corner) * slotsPerVertex;
                    const float cornerU = slotFloat(raw, cornerBase + 3u);
                    const float cornerV = slotFloat(raw, cornerBase + 4u);
                    if (cornerU < minU) minU = cornerU;
                    if (cornerV < minV) minV = cornerV;
                }

                const int tileX = atlasTile(minU);
                const int tileY = atlasTile(minV);
                if (tileX != currentTileX || tileY != currentTileY)
                {
                    Ps2TerrainTileRun run;
                    run.firstVertex = vertex;
                    run.vertexCount = 4;
                    run.tileX = (unsigned char)tileX;
                    run.tileY = (unsigned char)tileY;
                    runs.push_back(run);
                    currentTileX = tileX;
                    currentTileY = tileY;
                }
                else
                {
                    runs.back().vertexCount += 4;
                }
            }
        }
    }

    Ps2TerrainMeshBuildStats s_buildStats;
}

Ps2TerrainMeshBuildStats ps2TerrainMeshBuildStats()
{
    return s_buildStats;
}

Ps2TerrainMesh::Ps2TerrainMesh()
    : m_positionOffset(0), m_texCoordOffset(0), m_colorOffset(0),
      m_vertexCount(0), m_valid(false), m_buildRaw(nullptr),
      m_buildVertexCount(0), m_buildFirstVertex(0), m_buildBatchVertices(0),
      m_buildBatchCapacity(0), m_buildCurrentTileX(-1), m_buildCurrentTileY(-1),
      m_buildHasColor(false), m_buildActive(false), m_buildVu0InFlight(false),
      m_buildBatchColorsProcessed(false)
{
}

const short* Ps2TerrainMesh::positions() const
{
    return m_valid ? reinterpret_cast<const short*>(m_storage.data() + m_positionOffset) : nullptr;
}

const short* Ps2TerrainMesh::texCoords() const
{
    return m_valid ? reinterpret_cast<const short*>(m_storage.data() + m_texCoordOffset) : nullptr;
}

const unsigned char* Ps2TerrainMesh::colors() const
{
    return m_valid ? m_storage.data() + m_colorOffset : nullptr;
}

void Ps2TerrainMesh::resetBuildState()
{
    m_buildRaw = nullptr;
    m_buildVertexCount = 0;
    m_buildFirstVertex = 0;
    m_buildBatchVertices = 0;
    m_buildBatchCapacity = 0;
    m_buildCurrentTileX = -1;
    m_buildCurrentTileY = -1;
    m_buildHasColor = false;
    m_buildActive = false;
    m_buildVu0InFlight = false;
    m_buildBatchColorsProcessed = false;
}

Ps2TerrainMeshBuildStepResult Ps2TerrainMesh::finishBuild()
{
    const std::size_t positionBytes = (std::size_t)m_buildVertexCount * 4u * sizeof(short);
    const std::size_t texCoordBytes = (std::size_t)m_buildVertexCount * 2u * sizeof(short);
    const std::size_t colorBytes = (std::size_t)m_buildVertexCount * 4u;

    short* positionOut = reinterpret_cast<short*>(m_storage.data() + m_positionOffset);
    short* uvOut = reinterpret_cast<short*>(m_storage.data() + m_texCoordOffset);
    unsigned char* colorOut = m_storage.data() + m_colorOffset;
    SyncDCache(positionOut, reinterpret_cast<unsigned char*>(positionOut) + positionBytes);
    SyncDCache(uvOut, reinterpret_cast<unsigned char*>(uvOut) + texCoordBytes);
    SyncDCache(colorOut, colorOut + colorBytes);

    m_vertexCount = m_buildVertexCount;
    m_valid = !m_runs.empty();
    if (!m_valid)
    {
        ++s_buildStats.emptyRuns;
        resetBuildState();
        return Ps2TerrainMeshBuildStepResult::Failed;
    }

    ++s_buildStats.successful;
    resetBuildState();
    return Ps2TerrainMeshBuildStepResult::Complete;
}

Ps2TerrainMeshBuildStepResult Ps2TerrainMesh::packRemainingCpu(bool& didWork)
{
    short* positionOut = reinterpret_cast<short*>(m_storage.data() + m_positionOffset);
    short* uvOut = reinterpret_cast<short*>(m_storage.data() + m_texCoordOffset);
    unsigned char* colorOut = m_storage.data() + m_colorOffset;
    const std::size_t slotsPerVertex = Ps2CaptureLayout::Slots;

    while (m_buildFirstVertex < m_buildVertexCount)
    {
        const int_t remaining = m_buildVertexCount - m_buildFirstVertex;
        const int_t batchVertices = std::min(m_buildBatchCapacity, remaining);
        if (!m_buildBatchColorsProcessed)
        {
            processBatchColorsAndRuns(m_buildRaw, slotsPerVertex, m_buildFirstVertex,
                                      batchVertices, m_buildHasColor, colorOut, m_runs,
                                      m_buildCurrentTileX, m_buildCurrentTileY);
        }
        packBatchCpu(m_buildRaw, slotsPerVertex, m_buildFirstVertex, batchVertices,
                     positionOut, uvOut);
        didWork = true;
        m_buildFirstVertex += batchVertices;
        m_buildBatchVertices = 0;
        m_buildBatchColorsProcessed = false;
    }

    return finishBuild();
}

Ps2TerrainMeshBuildStepResult Ps2TerrainMesh::startNextBatch(bool& didWork)
{
    if (m_buildFirstVertex >= m_buildVertexCount)
        return finishBuild();

    const int_t remaining = m_buildVertexCount - m_buildFirstVertex;
    m_buildBatchVertices = std::min(m_buildBatchCapacity, remaining);
    m_buildBatchColorsProcessed = false;

    const std::size_t slotsPerVertex = Ps2CaptureLayout::Slots;
    const int_t* batchRaw = m_buildRaw + (std::size_t)m_buildFirstVertex * slotsPerVertex;
    if (!ps2_vu0_mesh_finalize_begin(batchRaw, m_buildBatchVertices))
        return packRemainingCpu(didWork);

    m_buildVu0InFlight = true;
    unsigned char* colorOut = m_storage.data() + m_colorOffset;
    processBatchColorsAndRuns(m_buildRaw, slotsPerVertex, m_buildFirstVertex,
                              m_buildBatchVertices, m_buildHasColor, colorOut, m_runs,
                              m_buildCurrentTileX, m_buildCurrentTileY);
    m_buildBatchColorsProcessed = true;
    didWork = true;
    return Ps2TerrainMeshBuildStepResult::Pending;
}

Ps2TerrainMeshBuildStepResult Ps2TerrainMesh::beginBuild(const int_t* raw,
                                                         std::size_t rawIntCount,
                                                         int_t vertexCount,
                                                         int_t drawMode,
                                                         bool hasTexture,
                                                         bool hasColor,
                                                         bool hasNormals,
                                                         bool& didWork)
{
    didWork = false;
    cancelBuild();
    // Retain initialized bytes: stream packers overwrite every live vertex.
    // m_valid and the offsets, not vector size, control published geometry.
    m_runs.clear();
    m_positionOffset = 0;
    m_texCoordOffset = 0;
    m_colorOffset = 0;
    m_vertexCount = 0;
    m_valid = false;

    const std::size_t slotsPerVertex = Ps2CaptureLayout::Slots;
    if (raw == nullptr || vertexCount <= 0 || drawMode != PS2_NATIVE_PRIM_QUADS ||
        (vertexCount & 3) != 0 || !hasTexture || hasNormals ||
        rawIntCount < (std::size_t)vertexCount * slotsPerVertex)
    {
        ++s_buildStats.invalidInput;
        return Ps2TerrainMeshBuildStepResult::Failed;
    }

    try
    {
        const std::size_t positionBytes = (std::size_t)vertexCount * 4u * sizeof(short);
        const std::size_t texCoordBytes = (std::size_t)vertexCount * 2u * sizeof(short);
        const std::size_t colorBytes = (std::size_t)vertexCount * 4u;
        const std::size_t totalBytes = positionBytes + texCoordBytes + colorBytes + kStreamAlignment * 4u;
        if (totalBytes > m_storage.capacity())
        {
            std::vector<unsigned char>().swap(m_storage);
            m_storage.reserve(totalBytes);
        }
        // Grow only the initialized high-water mark; do not zero old storage
        // again when rebuilding a mesh that fits. Capacity policy is unchanged.
        if (totalBytes > m_storage.size())
            m_storage.resize(totalBytes);

        const std::uintptr_t storageBase = reinterpret_cast<std::uintptr_t>(m_storage.data());
        std::uintptr_t cursor = alignUp(storageBase, kStreamAlignment);
        m_positionOffset = (std::size_t)(cursor - storageBase);
        cursor = alignUp(cursor + positionBytes, kStreamAlignment);
        m_texCoordOffset = (std::size_t)(cursor - storageBase);
        cursor = alignUp(cursor + texCoordBytes, kStreamAlignment);
        m_colorOffset = (std::size_t)(cursor - storageBase);

        const std::size_t requiredRuns = (std::size_t)vertexCount / 4u;
        if (requiredRuns > m_runs.capacity())
        {
            std::vector<Ps2TerrainTileRun>().swap(m_runs);
            m_runs.reserve(requiredRuns);
        }

        m_buildRaw = raw;
        m_buildVertexCount = vertexCount;
        m_buildFirstVertex = 0;
        m_buildBatchVertices = 0;
        const int_t vu0BatchCapacity = ps2_vu0_mesh_finalize_batch_capacity();
        m_buildBatchCapacity = vu0BatchCapacity > 0 ? vu0BatchCapacity : vertexCount;
        m_buildCurrentTileX = -1;
        m_buildCurrentTileY = -1;
        m_buildHasColor = hasColor;
        m_buildActive = true;
        m_buildVu0InFlight = false;
        m_buildBatchColorsProcessed = false;
        return startNextBatch(didWork);
    }
    catch (const std::bad_alloc&)
    {
        ++s_buildStats.allocationFailed;
        release();
        return Ps2TerrainMeshBuildStepResult::Failed;
    }
}

Ps2TerrainMeshBuildStepResult Ps2TerrainMesh::continueBuild(bool& didWork)
{
    didWork = false;
    if (!m_buildActive)
        return Ps2TerrainMeshBuildStepResult::Failed;

    if (!m_buildVu0InFlight)
        return startNextBatch(didWork);

    const Ps2Vu0MeshFinalizePollResult poll = ps2_vu0_mesh_finalize_poll();
    if (poll == Ps2Vu0MeshFinalizePollResult::Pending)
        return Ps2TerrainMeshBuildStepResult::Pending;

    short* positionOut = reinterpret_cast<short*>(m_storage.data() + m_positionOffset);
    short* uvOut = reinterpret_cast<short*>(m_storage.data() + m_texCoordOffset);
    const std::size_t slotsPerVertex = Ps2CaptureLayout::Slots;

    if (poll == Ps2Vu0MeshFinalizePollResult::Complete ||
        poll == Ps2Vu0MeshFinalizePollResult::Idle)
    {
        const volatile int_t* transformed = ps2_vu0_mesh_finalize_result();
        if (transformed != nullptr)
        {
            packBatchVu0(transformed, slotsPerVertex, m_buildFirstVertex,
                         m_buildBatchVertices, positionOut, uvOut);
        }
        else
        {
            packBatchCpu(m_buildRaw, slotsPerVertex, m_buildFirstVertex,
                         m_buildBatchVertices, positionOut, uvOut);
        }
    }
    else
    {
        packBatchCpu(m_buildRaw, slotsPerVertex, m_buildFirstVertex,
                     m_buildBatchVertices, positionOut, uvOut);
    }

    didWork = true;
    m_buildVu0InFlight = false;
    m_buildFirstVertex += m_buildBatchVertices;
    m_buildBatchVertices = 0;
    m_buildBatchColorsProcessed = false;

    if (poll == Ps2Vu0MeshFinalizePollResult::Failed)
        return packRemainingCpu(didWork);
    return startNextBatch(didWork);
}

bool Ps2TerrainMesh::build(const int_t* raw, std::size_t rawIntCount, int_t vertexCount,
                           int_t drawMode, bool hasTexture, bool hasColor, bool hasNormals)
{
    bool didWork = false;
    Ps2TerrainMeshBuildStepResult result = beginBuild(raw, rawIntCount, vertexCount,
                                                      drawMode, hasTexture, hasColor,
                                                      hasNormals, didWork);
    while (result == Ps2TerrainMeshBuildStepResult::Pending)
    {
        if (m_buildVu0InFlight)
            (void)ps2_vu0_mesh_finalize_wait();
        result = continueBuild(didWork);
    }
    return result == Ps2TerrainMeshBuildStepResult::Complete;
}

void Ps2TerrainMesh::cancelBuild()
{
    if (m_buildVu0InFlight)
        (void)ps2_vu0_mesh_finalize_drain();
    resetBuildState();
}

void Ps2TerrainMesh::swap(Ps2TerrainMesh& other)
{
    if (this == &other)
        return;
    cancelBuild();
    other.cancelBuild();
    m_storage.swap(other.m_storage);
    m_runs.swap(other.m_runs);
    std::swap(m_positionOffset, other.m_positionOffset);
    std::swap(m_texCoordOffset, other.m_texCoordOffset);
    std::swap(m_colorOffset, other.m_colorOffset);
    std::swap(m_vertexCount, other.m_vertexCount);
    std::swap(m_valid, other.m_valid);
}

void Ps2TerrainMesh::clearKeepCapacity()
{
    cancelBuild();
    // Retain initialized bytes: stream packers overwrite every live vertex.
    // m_valid and the offsets, not vector size, control published geometry.
    m_runs.clear();
    m_positionOffset = 0;
    m_texCoordOffset = 0;
    m_colorOffset = 0;
    m_vertexCount = 0;
    m_valid = false;
}

void Ps2TerrainMesh::release()
{
    cancelBuild();
    std::vector<unsigned char>().swap(m_storage);
    std::vector<Ps2TerrainTileRun>().swap(m_runs);
    m_positionOffset = 0;
    m_texCoordOffset = 0;
    m_colorOffset = 0;
    m_vertexCount = 0;
    m_valid = false;
}

#endif // PS2_PLATFORM
