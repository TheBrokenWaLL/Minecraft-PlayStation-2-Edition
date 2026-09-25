#pragma once

class Minecraft;
class ItemStack;
class EntityLiving;
class RenderBlocks;
class MapItemRenderer;
class Tessellator;

class ItemRenderer {
public:
    ItemRenderer(Minecraft* minecraft);
    ~ItemRenderer();

    void renderItem(EntityLiving* entityliving, ItemStack* itemstack);
    void renderItem(EntityLiving* entityliving, ItemStack* itemstack, int renderPass);
    void renderItemInFirstPerson(float f);
    void renderOverlays(float f);
    void updateEquippedItem();
    void resetEquippedProgress();
    void resetEquippedProgressAfterBlockPlace();
    void resetEquippedProgressAfterItemUse();

private:
    void emitItemIn2DGeometry(Tessellator* tessellator, float maxU, float minV, float minU, float maxV);
    void renderItemIn2D(Tessellator* tessellator, float maxU, float minV, float minU, float maxV);
#ifdef PS2_PLATFORM
    bool renderCachedBowIn2D(Tessellator* tessellator, int icon, float maxU, float minV, float minU, float maxV);
#endif
    void renderInsideOfBlock(float f, int i);
    void renderWarpedTextureOverlay(float f);
    void renderFireInFirstPerson(float f);

    Minecraft* mc;
    ItemStack* itemToRender;
    float equippedProgress;
    float prevEquippedProgress;
    RenderBlocks* renderBlocksInstance;
    MapItemRenderer* field_28131_f;
    int field_20099_f;
#ifdef PS2_PLATFORM
    int ps2BowMeshHandle;
    int ps2BowMeshIcon;
#endif
};
