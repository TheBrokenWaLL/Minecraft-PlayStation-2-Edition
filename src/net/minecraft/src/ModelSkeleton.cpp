#include "ModelSkeleton.h"

#include "ModelRenderer.h"
#include "MathHelper.h"
#include "platform/PlatformConfig.h"
#include "platform/RenderAPI.h"

ModelSkeleton::ModelSkeleton()
{
	float f = 0.0f;
	delete bipedRightArm; // Java reassignment relied on GC
	bipedRightArm = new ModelRenderer(40, 16);
	bipedRightArm->addBox(-1.0f, -2.0f, -1.0f, 2, 12, 2, f);
	bipedRightArm->setRotationPoint(-5.0f, 2.0f, 0.0f);
	delete bipedLeftArm; // Java reassignment relied on GC
	bipedLeftArm = new ModelRenderer(40, 16);
	bipedLeftArm->mirror = true;
	bipedLeftArm->addBox(-1.0f, -2.0f, -1.0f, 2, 12, 2, f);
	bipedLeftArm->setRotationPoint(5.0f, 2.0f, 0.0f);
	delete bipedRightLeg; // Java reassignment relied on GC
	bipedRightLeg = new ModelRenderer(0, 16);
	bipedRightLeg->addBox(-1.0f, 0.0f, -1.0f, 2, 12, 2, f);
	bipedRightLeg->setRotationPoint(-2.0f, 12.0f, 0.0f);
	delete bipedLeftLeg; // Java reassignment relied on GC
	bipedLeftLeg = new ModelRenderer(0, 16);
	bipedLeftLeg->mirror = true;
	bipedLeftLeg->addBox(-1.0f, 0.0f, -1.0f, 2, 12, 2, f);
	bipedLeftLeg->setRotationPoint(2.0f, 12.0f, 0.0f);
	aimedBow = true;

#if PLATFORM_PS2
	// Skeleton head and headwear always share the same transform. Fold the
	// overlay cube into the head mesh so the PS2 submits both with one
	// persistent-mesh draw instead of two. Skeletons never toggle the
	// player-only head/headwear visibility split.
	bipedHead->setTextureOffset(32, 0)->addBox(-4.0f, -8.0f, -4.0f, 8, 8, 8, 0.5f);
	bipedHeadwear->showModel = false;
#endif
}

void ModelSkeleton::render(Entity* entity, float f, float f1, float f2, float f3, float f4, float f5)
{
	(void)entity;
#if PLATFORM_PS2
	// Skeleton parts are closed boxes with consistent winding (mirrored parts
	// explicitly flip their faces in ModelBox). RenderLiving disables culling
	// for legacy model compatibility, but that makes the PS2 transform/emit the
	// hidden half of every skeleton box. Cull those back faces here while
	// preserving RenderLiving's surrounding state contract.
	renderCullFace(RenderFace::Back);
	renderEnable(RenderCapability::CullFace);
	ModelBiped::render(f, f1, f2, f3, f4, f5);
	renderDisable(RenderCapability::CullFace);
#else
	ModelBiped::render(f, f1, f2, f3, f4, f5);
#endif
}

void ModelSkeleton::setRotationAngles(float f, float f1, float f2, float f3, float f4, float f5)
{
#if PLATFORM_PS2
	(void)f5;

	// ModelZombie::setRotationAngles() calls ModelBiped first, but for a
	// skeleton the zombie arm pose then overwrites nearly all biped arm work.
	// Reproduce the final values directly so the R5900 does not spend time on
	// trigonometry and assignments whose results are discarded immediately.
	bipedHead->rotateAngleY = f3 / 57.295776f;
	bipedHead->rotateAngleX = f4 / 57.295776f;
	bipedHeadwear->rotateAngleY = bipedHead->rotateAngleY;
	bipedHeadwear->rotateAngleX = bipedHead->rotateAngleX;

	if (isRiding)
	{
		bipedRightLeg->rotateAngleX = -1.2566371f;
		bipedLeftLeg->rotateAngleX = -1.2566371f;
		bipedRightLeg->rotateAngleY = 0.3141593f;
		bipedLeftLeg->rotateAngleY = -0.3141593f;
	}
	else
	{
		bipedRightLeg->rotateAngleX = MathHelper::cos(f * 0.6662f) * 1.4f * f1;
		bipedLeftLeg->rotateAngleX = MathHelper::cos(f * 0.6662f + 3.1415927f) * 1.4f * f1;
		bipedRightLeg->rotateAngleY = 0.0f;
		bipedLeftLeg->rotateAngleY = 0.0f;
	}

	// Normal living entities keep swing progress in [0, 1]. Fall back to the
	// inherited implementation for the old sentinel path so unusual callers
	// retain vanilla state semantics.
	if (onGround <= -9990.0f)
	{
		ModelZombie::setRotationAngles(f, f1, f2, f3, f4, f5);
		return;
	}

	if (onGround == 0.0f)
	{
		bipedBody->rotateAngleY = 0.0f;
		bipedRightArm->rotationPointZ = 0.0f;
		bipedRightArm->rotationPointX = -5.0f;
		bipedLeftArm->rotationPointZ = 0.0f;
		bipedLeftArm->rotationPointX = 5.0f;
	}
	else
	{
		bipedBody->rotateAngleY = MathHelper::sin(MathHelper::sqrt_float(onGround) * 3.1415927f * 2.0f) * 0.2f;
		const float bodySin = MathHelper::sin(bipedBody->rotateAngleY);
		const float bodyCos = MathHelper::cos(bipedBody->rotateAngleY);
		bipedRightArm->rotationPointZ = bodySin * 5.0f;
		bipedRightArm->rotationPointX = -bodyCos * 5.0f;
		bipedLeftArm->rotationPointZ = -bodySin * 5.0f;
		bipedLeftArm->rotationPointX = bodyCos * 5.0f;
	}

	if (isSneak)
	{
		bipedBody->rotateAngleX = 0.5f;
		bipedRightLeg->rotationPointZ = 4.0f;
		bipedLeftLeg->rotationPointZ = 4.0f;
		bipedRightLeg->rotationPointY = 9.0f;
		bipedLeftLeg->rotationPointY = 9.0f;
		bipedHead->rotationPointY = 1.0f;
	}
	else
	{
		bipedBody->rotateAngleX = 0.0f;
		bipedRightLeg->rotationPointZ = 0.0f;
		bipedLeftLeg->rotationPointZ = 0.0f;
		bipedRightLeg->rotationPointY = 12.0f;
		bipedLeftLeg->rotationPointY = 12.0f;
		bipedHead->rotationPointY = 0.0f;
	}

	float swingSin = 0.0f;
	float swingEaseSin = 0.0f;
	if (onGround != 0.0f)
	{
		swingSin = MathHelper::sin(onGround * 3.1415927f);
		const float inverseSwing = 1.0f - onGround;
		swingEaseSin = MathHelper::sin((1.0f - inverseSwing * inverseSwing) * 3.1415927f);
	}

	bipedRightArm->rotateAngleY = -0.1f + swingSin * 0.6f;
	bipedLeftArm->rotateAngleY = 0.1f - swingSin * 0.6f;
	const float armBaseX = -1.5707964f - swingSin * 1.2f + swingEaseSin * 0.4f;
	const float idleZ = MathHelper::cos(f2 * 0.09f) * 0.05f + 0.05f;
	const float idleX = MathHelper::sin(f2 * 0.067f) * 0.05f;
	bipedRightArm->rotateAngleZ = idleZ;
	bipedLeftArm->rotateAngleZ = -idleZ;
	bipedRightArm->rotateAngleX = armBaseX + idleX;
	bipedLeftArm->rotateAngleX = armBaseX - idleX;
#else
	ModelZombie::setRotationAngles(f, f1, f2, f3, f4, f5);
#endif
}
