#include "EntityFX.h"
#include "java/Math.h"

#include <cmath>
#include <cstdlib>

#include "AxisAlignedBB.h"
#include "Block.h"
#include "Chunk.h"
#include "MathHelper.h"
#include "NBTTagCompound.h"
#include "Tessellator.h"
#include "World.h"
#include "platform/PlatformTuning.h"
#include "java/Arithmetic.h"

double EntityFX::interpPosX = 0.0;
double EntityFX::interpPosY = 0.0;
double EntityFX::interpPosZ = 0.0;

static inline double mathRandom()
{
	return Math::random();
}

float EntityFX::interpolateRenderCoordinate(double previous, double current, double camera, float partialTick)
{
#if PLATFORM_FLOAT_VERTEX_MATH
	const float relativePrevious = static_cast<float>(previous - camera);
	const float delta = static_cast<float>(current - previous);
	return relativePrevious + delta * partialTick;
#else
	return static_cast<float>(previous + (current - previous) * static_cast<double>(partialTick) - camera);
#endif
}

EntityFX::EntityFX(World *world, double d, double d1, double d2,
                   double d3, double d4, double d5)
	: Entity(world)
{
	entityInit();
	particleAge = 0;
	particleMaxAge = 0;
	setSize(0.2f, 0.2f);
	yOffset = height / 2.0f;
	setPosition(d, d1, d2);
	particleRed = particleGreen = particleBlue = 1.0f;
	motionX = d3 + (double)((float)(mathRandom() * 2.0 - 1.0) * 0.4f);
	motionY = d4 + (double)((float)(mathRandom() * 2.0 - 1.0) * 0.4f);
	motionZ = d5 + (double)((float)(mathRandom() * 2.0 - 1.0) * 0.4f);
	float f = (float)(mathRandom() + mathRandom() + 1.0) * 0.15f;
#if PLATFORM_FLOAT_VERTEX_MATH
	const float motionXF = static_cast<float>(motionX);
	const float motionYF = static_cast<float>(motionY);
	const float motionZF = static_cast<float>(motionZ);
	const float f1 = MathHelper::sqrt_float(motionXF * motionXF + motionYF * motionYF + motionZF * motionZF);
	const float scale = f / f1 * 0.4f;
	motionX = static_cast<double>(motionXF * scale);
	motionY = static_cast<double>(motionYF * scale + 0.1f);
	motionZ = static_cast<double>(motionZF * scale);
#else
	float f1 = MathHelper::sqrt_double(motionX * motionX + motionY * motionY + motionZ * motionZ);
	motionX = (motionX / (double)f1) * (double)f * 0.40000000596046448;
	motionY = (motionY / (double)f1) * (double)f * 0.40000000596046448 + 0.10000000149011612;
	motionZ = (motionZ / (double)f1) * (double)f * 0.40000000596046448;
#endif
	particleTextureJitterX = rand.nextFloat() * 3.0f;
	particleTextureJitterY = rand.nextFloat() * 3.0f;
	particleScale = (rand.nextFloat() * 0.5f + 0.5f) * 2.0f;
	particleMaxAge = (int_t)(4.0f / (rand.nextFloat() * 0.9f + 0.1f));
	particleAge = 0;
	particleTextureIndex = 0;
	particleGravity = 0.0f;
	cachedBrightness = -1;
	cachedBrightnessAge = -1;
}

EntityFX *EntityFX::multiplyVelocity(float f)
{
	motionX *= f;
	motionY = (motionY - 0.10000000149011612) * (double)f + 0.10000000149011612;
	motionZ *= f;
	return this;
}

EntityFX *EntityFX::multiplyParticleScaleBy(float f)
{
	setSize(0.2f * f, 0.2f * f);
	particleScale *= f;
	return this;
}

bool EntityFX::canTriggerWalking()
{
	return false;
}

void EntityFX::entityInit()
{
}

void EntityFX::onUpdate()
{
	prevPosX = posX;
	prevPosY = posY;
	prevPosZ = posZ;
	if (particleAge++ >= particleMaxAge)
	{
		setEntityDead();
	}
	motionY -= 0.04 * (double)particleGravity;
	moveEntity(motionX, motionY, motionZ);
	motionX *= 0.98000001907348633;
	motionY *= 0.98000001907348633;
	motionZ *= 0.98000001907348633;
	if (onGround)
	{
		motionX *= 0.69999998807907104;
		motionZ *= 0.69999998807907104;
	}
}

#if PLATFORM_FAST_PARTICLE_PHYSICS
namespace
{
// The fast particle path probes up to three nearby points for one movement.
// They almost always live in the same chunk, so keep that chunk lookup local to
// the movement instead of walking the provider once per axis. Block ID and
// collision-shape semantics remain the same as World::getBlockId() for normal
// in-world coordinates.
struct ParticleBlockLookup
{
	World *world = nullptr;
	Chunk *chunk = nullptr;
	int_t chunkX = 0;
	int_t chunkZ = 0;
	bool hasChunk = false;

	int_t blockId(int_t blockX, int_t blockY, int_t blockZ)
	{
		if (world == nullptr || blockX < -30000000 || blockZ < -30000000 ||
		    blockX >= 30000000 || blockZ >= 30000000 ||
		    blockY < 0 || blockY >= WorldHeight::HEIGHT)
		{
			return 0;
		}

		const int_t wantedChunkX = JavaArithmetic::intShr(blockX, 4);
		const int_t wantedChunkZ = JavaArithmetic::intShr(blockZ, 4);
		if (!hasChunk || wantedChunkX != chunkX || wantedChunkZ != chunkZ)
		{
			chunkX = wantedChunkX;
			chunkZ = wantedChunkZ;
			chunk = world->getChunkFromChunkCoords(chunkX, chunkZ);
			hasChunk = true;
		}

		return chunk != nullptr ? chunk->getBlockID(blockX & 0xf, blockY, blockZ & 0xf) : 0;
	}
};

// True when the point lies inside the collision box of the block at that
// position. Particles are 0.2 wide, so one point per axis stands in for the
// AABB sweep Entity::moveEntity performs.
bool particleBlockedAt(ParticleBlockLookup &lookup, double x, double y, double z)
{
	const int_t blockX = MathHelper::floor_double(x);
	const int_t blockY = MathHelper::floor_double(y);
	const int_t blockZ = MathHelper::floor_double(z);
	const int_t blockId = lookup.blockId(blockX, blockY, blockZ);
	if (blockId <= 0 || blockId >= Block::BLOCK_REGISTRY_SIZE)
		return false;
	Block *block = Block::blocksList[blockId];
	if (block == nullptr)
		return false;
	AxisAlignedBB *box = block->getCollisionBoundingBoxFromPool(lookup.world, blockX, blockY, blockZ);
	return box != nullptr &&
	       x >= box->minX && x < box->maxX &&
	       y >= box->minY && y < box->maxY &&
	       z >= box->minZ && z < box->maxZ;
}
}

void EntityFX::moveEntity(double d, double d1, double d2)
{
	if (noClip)
	{
		Entity::moveEntity(d, d1, d2);
		return;
	}

	// Same outputs Entity::moveEntity leaves for a particle -- position,
	// onGround, isCollided*, motion zeroed on the blocked axis -- without the
	// colliding-box gather, fall tracking or step sounds none of the FX use.
	ParticleBlockLookup blockLookup;
	blockLookup.world = worldObj;
	const double halfWidth = static_cast<double>(width) * 0.5;
	const double bottom = boundingBox->minY;
	const double top = boundingBox->maxY;
	const double midY = (bottom + top) * 0.5;

	bool blockedY = false;
	if (d1 != 0.0)
	{
		const double probeY = d1 < 0.0 ? bottom + d1 : top + d1;
		blockedY = particleBlockedAt(blockLookup, posX, probeY, posZ);
	}
	onGround = blockedY && d1 < 0.0;
	if (blockedY)
		d1 = 0.0;

	bool blockedX = false;
	if (d != 0.0)
	{
		const double probeX = posX + d + (d < 0.0 ? -halfWidth : halfWidth);
		blockedX = particleBlockedAt(blockLookup, probeX, midY + d1, posZ);
		if (blockedX)
			d = 0.0;
	}

	bool blockedZ = false;
	if (d2 != 0.0)
	{
		const double probeZ = posZ + d2 + (d2 < 0.0 ? -halfWidth : halfWidth);
		blockedZ = particleBlockedAt(blockLookup, posX + d, midY + d1, probeZ);
		if (blockedZ)
			d2 = 0.0;
	}

	boundingBox->offset(d, d1, d2);
	posX = (boundingBox->minX + boundingBox->maxX) / 2.0;
	posY = (boundingBox->minY + (double)yOffset) - (double)ySize;
	posZ = (boundingBox->minZ + boundingBox->maxZ) / 2.0;

	isCollidedHorizontally = blockedX || blockedZ;
	isCollidedVertically = blockedY;
	isCollided = isCollidedHorizontally || isCollidedVertically;
	if (blockedX)
		motionX = 0.0;
	if (blockedY)
		motionY = 0.0;
	if (blockedZ)
		motionZ = 0.0;
}
#endif

int_t EntityFX::getBrightnessForRender(float partialTick)
{
#if PLATFORM_PARTICLE_BRIGHTNESS_INTERVAL > 1
	// Keyed on particle age, which every FX subclass advances in its own
	// onUpdate, so the sample refreshes per tick bucket rather than per frame.
	const int_t bucket = particleAge / PLATFORM_PARTICLE_BRIGHTNESS_INTERVAL;
	if (cachedBrightness < 0 || bucket != cachedBrightnessAge)
	{
		cachedBrightness = Entity::getBrightnessForRender(partialTick);
		cachedBrightnessAge = bucket;
	}
	return cachedBrightness;
#else
	return Entity::getBrightnessForRender(partialTick);
#endif
}

void EntityFX::renderParticle(Tessellator *tessellator, float f, float f1, float f2, float f3, float f4, float f5)
{
	float f6  = (float)(particleTextureIndex % 16) / 16.0f;
	float f7  = f6 + 0.0624375f;
	float f8  = (float)(particleTextureIndex / 16) / 16.0f;
	float f9  = f8 + 0.0624375f;
	float f10 = 0.1f * particleScale;
	float f11 = interpolateRenderCoordinate(prevPosX, posX, interpPosX, f);
	float f12 = interpolateRenderCoordinate(prevPosY, posY, interpPosY, f);
	float f13 = interpolateRenderCoordinate(prevPosZ, posZ, interpPosZ, f);
	const float brightness = 1.0f;
	tessellator->setColorOpaque_F(particleRed * brightness, particleGreen * brightness, particleBlue * brightness);
	tessellator->addVertexWithUV(f11 - f1 * f10 - f4 * f10, f12 - f2 * f10, f13 - f3 * f10 - f5 * f10, f7, f9);
	tessellator->addVertexWithUV((f11 - f1 * f10) + f4 * f10, f12 + f2 * f10, (f13 - f3 * f10) + f5 * f10, f7, f8);
	tessellator->addVertexWithUV(f11 + f1 * f10 + f4 * f10, f12 + f2 * f10, f13 + f3 * f10 + f5 * f10, f6, f8);
	tessellator->addVertexWithUV((f11 + f1 * f10) - f4 * f10, f12 - f2 * f10, (f13 + f3 * f10) - f5 * f10, f6, f9);
}

void EntityFX::setParticleColor(float red, float green, float blue)
{
	particleRed = red;
	particleGreen = green;
	particleBlue = blue;
}

float EntityFX::getParticleRed() const
{
	return particleRed;
}

float EntityFX::getParticleGreen() const
{
	return particleGreen;
}

float EntityFX::getParticleBlue() const
{
	return particleBlue;
}

void EntityFX::setParticleTextureIndex(int_t index)
{
	particleTextureIndex = index;
}

int_t EntityFX::getParticleTextureIndex() const
{
	return particleTextureIndex;
}

bool EntityFX::canAttackWithItem()
{
	return false;
}

int_t EntityFX::getFXLayer()
{
	return 0;
}

void EntityFX::writeEntityToNBT(NBTTagCompound *nbttagcompound)
{
}

void EntityFX::readEntityFromNBT(NBTTagCompound *nbttagcompound)
{
}
