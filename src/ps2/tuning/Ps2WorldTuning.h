#pragma once

#define PS2_SKIP_MOB_SPAWNING 0

// Simulation radii follow the RENDER window, not the streaming cache.
//
// Both of these used to read PS2_CHUNK_CACHE_RADIUS, which was fine only while
// that value equalled PS2_VISIBLE_CHUNK_RADIUS. It no longer does: the cache is
// deliberately one ring larger so terrain can be generated before it is visible.
// Left aliased, raising the cache radius from 2 to 3 would silently have taken
// the random-tick set from 25 to 49 chunks, and since World::func_48461_r scales
// PS2_RANDOM_BLOCK_TICKS_PER_CHUNK by ceil(totalChunks / visitCount), that
// doubles the issued block ticks per world tick (200 -> 400) -- inflating the
// exact phase this file elsewhere records at 162-182 ms. Spawning has the same
// shape: twice the eligible chunks to scan per pass.
//
// Chunks in the outer streaming ring are therefore generated and lit, but not
// simulated, until the player walks close enough to render them.
#define PS2_RANDOM_TICK_CHUNK_RADIUS PS2_VISIBLE_CHUNK_RADIUS
#define PS2_MOB_SPAWN_CHUNK_RADIUS PS2_VISIBLE_CHUNK_RADIUS
#define PS2_RANDOM_BLOCK_TICKS_PER_CHUNK 8

// Reuse the desktop Legacy selection caches on PS2. These preserve the same
// JavaHashSet iteration order and only skip rebuilding selections while the
// ordered player chunk coordinates are unchanged. The entity cache is also
// invalidated by the provider topology version, so streamed chunk changes are
// visible immediately.
#define PS2_CACHE_RANDOM_TICK_CHUNKS 1
#define PS2_CACHE_SPAWN_CHUNKS 1
#define PS2_CACHE_ENTITY_CHUNK_EXISTENCE 1

// With world particles on (PS2_SKIP_WORLD_PARTICLES 0) randomDisplayUpdates()
// probes blocks around the player every tick to let torches, lava and portals
// emit. Vanilla probes 1000 (6 RNG draws plus a block lookup each); it was the
// 347 ms slowTick=randomDisplay spike on 2026-09-16. The first PS2 pass used
// 250 probes, but ocean profiling later showed this phase still consuming about
// 1.5-1.6 ms per rendered frame once a 20 FPS stretch made nearly every frame
// carry a game tick. 128 probes preserve ambient torch/lava/portal particles
// while giving the 30 FPS recovery path roughly another 0.7 ms of tick headroom.
#define PS2_CACHE_RANDOM_DISPLAY_CHUNKS 1
#define PS2_REUSE_RANDOM_DISPLAY_RNG 1
#define PS2_RANDOM_DISPLAY_PROBES 128

// Resident chunks visited per world tick by updateBlocksAndPlayCaveSounds.
//
// That phase is the single worst spike in the port: the [PS2][FRAME] log
// reported "slowTick=randomBlocks" at 162.9 / 176.2 / 182.8 ms against a tick
// AVERAGE of 4-10ms, which is what turns a 30 fps stretch into a visible 4 fps
// stall. Radius 2 makes it sweep all 25 resident chunks every tick, and each
// chunk can hit findTopSolidBlock + getBiomeGenAt (the snow/ice branch) and 8
// updateTick calls, any of which can cascade through setBlockWithNotify.
//
// Round-robin instead: visit this many chunks per tick and carry a cursor across
// ticks, so the PER-CHUNK overhead (the cave-sound light probe, the snow
// branch's findTopSolidBlock + getBiomeGenAt, the chunk lookup) is divided by
// 25/N while every chunk is still reached.
//
// This is NOT a world-speed cut. updateBlocksAndPlayCaveSounds scales
// PS2_RANDOM_BLOCK_TICKS_PER_CHUNK by exactly the same 25/N factor, so the
// number of random block ticks issued per world tick is unchanged (25 chunks x
// 8 == 5 chunks x 40) and grass, crops, saplings, leaf decay and fluids run at
// the rate PS2_RANDOM_BLOCK_TICKS_PER_CHUNK alone decides. Only the fixed
// per-chunk cost is spread.
//
// If the profile line below shows ticks= is what spikes rather than snow=/sound=,
// this knob will not help and PS2_RANDOM_BLOCK_TICKS_PER_CHUNK is the one to
// lower -- that one does change world speed.
//
// 0 = visit every chunk every tick (previous console behaviour).
#define PS2_RANDOM_TICK_CHUNKS_PER_TICK 5

// Per-branch breakdown of that phase, printed every N world ticks:
//   sound = the ambient.cave probe (getFullBlockLightValue + getSavedLightValue)
//   snow  = the snow/ice branch (findTopSolidBlock + getBiomeGenAt + setBlock)
//   ticks = the PS2_RANDOM_BLOCK_TICKS_PER_CHUNK updateTick calls
// slowTick only ever names the whole phase, so it cannot say which of the three
// produced a 180ms tick. Each figure is the worst single tick in the window.
// 0 = compiled out.
#ifdef PS2_RENDER_STATS
#define PS2_RANDOM_TICK_PROFILE_INTERVAL 200
#else
#define PS2_RANDOM_TICK_PROFILE_INTERVAL 0
#endif

// Mob CPU budget. Mobs are the heaviest per-tick EE cost: each one runs an A*
// pathfinder (unbounded node expansion in vanilla) plus block/entity collision
// scans every tick, so a handful of mobs can drop the tick from ~5ms to whole
// frames. These knobs bound that cost:
//   - MAX_LIVE_MOBS:      hard cap on simultaneously alive spawned mobs.
//   - PATHFIND_BUDGET:    max A* path computations allowed per world tick
//                         (round-robin; mobs that miss their turn skip repathing
//                         this tick and retry next tick).
//   - PATHFIND_MAX_NODES: cap on A* node expansions per single path search; when
//                         hit, the best-so-far partial path is returned.
// 6: the eighth and seventh slot were rarely on screen at once inside the
// 32-block entity render radius, while every live mob still pays its
// pathfinder turn and collision scan whether it is visible or not.
// 4: with the spawn band narrowed to 16 below, the four slots are the ones
// the player actually meets on the surface.
#define PS2_MAX_LIVE_MOBS 4
// The cap above only gates natural spawning. Chunk population spawns its own
// animal groups (SpawnerAnimals::performWorldGenSpawning) of exactly four,
// and those add up while the player explores: measured 2026-09-16 with 18-36
// animals alive, the entity tick averaged 5 ms with 13 ms peaks and each
// visible animal drew in ~0.85 ms. These bound that source without turning
// it off, so new land still has animals on it.
//   - GROUP_MAX: members per worldgen group (vanilla 4). 0 keeps vanilla.
//   - LIVE_MAX:  no new worldgen group while at least this many creature
//                entities are alive (animals only, the monster and water
//                counts are separate). 0 keeps vanilla.
#define PS2_WORLDGEN_ANIMAL_GROUP_MAX 2
#define PS2_WORLDGEN_ANIMAL_LIVE_MAX  12

// Half-height, in blocks, of the vertical band around the players that mobs may
// spawn in. 0 restores vanilla's uniform draw over the whole 0..128 column.
//
// This is a companion to MAX_LIVE_MOBS rather than another CPU cut. Vanilla
// picks the spawn Y uniformly (SpawnerAnimals::getRandomSpawningPointInChunk),
// so roughly half of every attempt lands in rock or in caves the player has no
// way to reach. On a PC that only wastes a few block lookups, but here the cap
// is a hard eight: a mob that spawns thirty blocks underground holds one of
// those eight slots for its whole life, and the surface the player is actually
// looking at stays empty. Restricting the draw spends the cap where it is
// visible.
//
// 24 covers the surface plus shallow caves for a player standing at sea level.
// Lower it toward 12 for surface-only spawning; raise it to reach deeper caves.
// Spawn validity is untouched -- the block, material and light tests still
// reject every position exactly as before, so this only changes which Y values
// are offered, never which ones are accepted.
// 16: surface plus the first cave layer for a sea-level player; with four
// live slots a mob spawned deeper than that mostly holds a slot unseen.
#define PS2_MOB_SPAWN_Y_BAND 16

// Entity simulation profile. Physics, timers and the entity itself still tick
// every world tick; only creature AI is rate-limited outside the near-player
// radius. This keeps motion continuous while avoiding repeated target/path work
// for mobs at the edge of the 5x5 playable cache. Divisors are powers of two so
// the stagger is a cheap mask in EntityCreature.
// NEAR 16: half the entity render radius. A mob between 16 and 32 blocks
// is on screen but small; running its AI every other tick is not visible
// while the target/path work it saves is.
#define PS2_ENTITY_AI_NEAR_RADIUS_BLOCKS 16.0f
#define PS2_ENTITY_AI_FAR_RADIUS_BLOCKS  40.0f
#define PS2_ENTITY_AI_MID_TICK_DIVISOR   2
#define PS2_ENTITY_AI_FAR_TICK_DIVISOR   4

// Remote mobs in multiplayer are server-authoritative, but vanilla still runs
// their full local water/lava, movement and block-collision physics every tick
// after applying network interpolation. Dense villages can therefore spend most
// of the EE frame re-simulating entities whose position the server immediately
// corrects. Keep interpolation, base entity timers and animation updates at 20
// TPS, but refresh the expensive local living-physics path periodically. The
// phase is staggered by entity ID so a crowd does not refresh on one tick.
// Must stay a power of two; 1 restores the vanilla per-tick path.
#define PS2_MULTIPLAYER_REMOTE_LIVING_PHYSICS_TICK_DIVISOR 4

// Living entities beyond the visible terrain window are not worth submitting
// through the expensive animated-model path. Frustum-exempt entities keep their
// normal behavior, so bosses and other special renderers are unaffected.
// 24: the cache radius is 2 chunks again (Ps2CoreTuning.h), so 32 reached
// past the resident world; 24 covers what the terrain window can show.
// 20: a 1.8 m mob at 24 blocks is ~13 px tall on the 448-line frame, and the
// animated-model path costs ~1 ms per mob (1.9 ms for a sheep, two models)
// whatever its size on screen; measured 2026-09-16 at 6-7 ms of entity draw
// with 11 animals alive.
#define PS2_ENTITY_RENDER_RADIUS_BLOCKS 20.0f
// Sheep draw a second full model for the wool. Beyond this distance (squared,
// blocks) the fur pass is skipped: at 16 blocks the sheep is ~20 px tall and
// the wool colour is not readable, while the pass costs as much as the body.
// The shorn body model draws underneath either way. 0 disables the cut.
#define PS2_SHEEP_WOOL_LOD_DISTANCE_SQ 256.0f

// Entity-entity push resolution is only observable around the rendered player.
// Outside the 32-block visible radius, skip the chunk/AABB scan but continue all
// other movement, block collision and entity timers normally.
#define PS2_ENTITY_PUSH_COLLISION_RADIUS_BLOCKS 24.0f

// Reuse the Legacy PC collision/query fast paths on the EE. These preserve the
// same block collision semantics while replacing per-block virtual World/Chunk
// lookups with direct resident-section reads. Special collision shapes keep
// their block-specific virtual fallback.
#define PS2_FAST_BLOCK_COLLISIONS 1
#define PS2_EARLY_UNIT_CUBE_COLLISION_TEST 1
#define PS2_EARLY_COLLISION_EXIT 1
#define PS2_ENTITY_QUERY_CACHE 1

// The R5900 has no hardware double-precision arithmetic. Entity positions and
// AABBs remain double for Java/world-coordinate fidelity, but local AI deltas,
// distances and angle calculations can narrow to float because they operate over
// tens of blocks and are immediately consumed as float rotations/speeds.
#define PS2_FLOAT_ENTITY_AI_MATH 1
#define PS2_FLOAT_ENTITY_CORE_MATH 1

// Flowing-fluid top-face UV rotation is consumed as float by RenderBlocks. Keep
// the Vec3D storage/API unchanged, but avoid the software-emulated double atan2
// on the EE after the local flow vector has already been normalized.
#define PS2_FLOAT_FLUID_FLOW 1
// Drop search radius for spreading water/lava (BlockFlowing::calculateFlowCost).
// Vanilla looks 4 blocks out through a 3-way recursion: up to 324 leaf probes,
// each two or three chunk lookups, per spreading tick. 2 keeps the nearby
// steering (drops within three blocks still win) at 36 probes.
#ifndef PS2_FLUID_FLOW_SEARCH_DEPTH
#  define PS2_FLUID_FLOW_SEARCH_DEPTH 2
#endif

// Explosion rays only travel a few blocks from their origin, so keep their
// marching coordinates rebased around the integer origin block and run the hot
// normalization/step arithmetic in float. Entity/world positions remain double.
// Block-density sampling follows the same local-coordinate rule.
#define PS2_FLOAT_EXPLOSION_MATH 1

// Vec3D's scalar helpers (lengthVector, distanceTo, normalize) and the block ray
// cast in World::rayTraceBlocks. Both keep their double storage and their double
// signatures; only the arithmetic inside narrows.
//
// Vec3D holds world coordinates, so this is not the blanket "Vec3D is float"
// change: each helper is a length or a ratio, and both are scale-relative, so
// narrowing the operands costs relative precision rather than absolute
// precision. lengthVector and distanceTo already truncate to float on return
// (MathHelper::sqrt_double returns float), so for those two the narrowing only
// moves where the existing truncation happens.
//
// rayTraceBlocks is the case that pays best. It is a bounded march -- the picker
// gives it a five block reach and it caps at 200 steps -- so the ray is
// rebased on its own start point and every step runs in float. That also deletes
// the six std::isnan(double) probes per iteration: each is an __unorddf2 call on
// the EE, and the R5900 FPU cannot produce a NaN in the first place.
//
// Set to 0 to restore the exact double paths.
#define PS2_FLOAT_VECTOR_MATH 1

// Run the AABB sweep in Entity::moveEntity against float candidate boxes in a
// local frame anchored on an integer block corner next to the entity, instead
// of comparing double world coordinates six times over per box per axis.
//
// The entity position and its AxisAlignedBB stay double. Unit-cube blocks --
// nearly every candidate -- are emitted straight from their integer cell into
// the local frame (PlatformBlockCollisionSweeper), so they never touch a pooled
// double AxisAlignedBB or platformUnitCubeIntersects' six double compares;
// shaped blocks and entity boxes are rebased with six double subtractions each.
// Precision is relative to the entity, not the world origin, so it does not
// decay away from spawn.
//
// The float result is not what moves the entity. When a box shortens an axis,
// the delta is recomputed in double against that block face (origin + face is
// exact; the subtraction is Sterbenz-exact), so an entity lands ON the block
// coordinate exactly as vanilla does. Float only decides which box wins, which
// can differ from double when two faces sit within ~1e-7 of each other.
#define PS2_FLOAT_COLLISION_SWEEP 1

// Square the separation between two points in float after subtracting in double.
//
// Entity::getDistanceToEntity already does exactly this; the squared variants
// and getDistance were left on the full double path. They are the hotter ones:
// AI target selection, despawn checks and every proximity test go through
// getDistanceSqToEntity, and nearly all of them immediately compare the result
// against a constant threshold (4.0, 49.0, 144.0).
//
// The subtraction stays double, which is what keeps the result accurate far
// from the origin -- the difference of two world coordinates is exact there,
// and only that difference is narrowed. It is the same rebasing rule the
// renderer and the collision sweep follow.
//
// Why it is worth a flag at all: on the EE, __muldf3 is 171 instructions
// against 22 for __truncdfsf2 and 30 for a comparison, so the three double
// multiplies dominate the whole call. Measured on the EE toolchain, one
// distance test plus its threshold comparison drops from roughly 650 soft-float
// instructions to 135.
//
// What it gives up: about 1e-5 blocks of resolution on a 1000-block separation.
// A test sitting exactly on its threshold can therefore resolve one tick
// earlier or later. No saved world or seed depends on it -- this is live
// simulation only -- but it is not bit-identical to the double path.
#define PS2_FLOAT_ENTITY_DISTANCE 1

// 1 path per tick and 128 nodes: with four mobs the round-robin still
// repaths each one every four ticks, and 128 nodes reaches ~16 blocks of
// open ground, which is all the 16-block AI radius asks for.
#define PS2_PATHFIND_BUDGET_PER_TICK 1
#define PS2_PATHFIND_MAX_NODES 128
// Path following still advances the current node every tick. Only the expensive
// line-of-sight shortcut scan is staggered; keep this a power of two.
#define PS2_PATH_SHORTCUT_TICK_DIVISOR 4
// Run the full SpawnerAnimals pass every N world ticks instead of every tick.
// The pass costs ~8-10 ms of EE time (3 creature types x eligible chunks x
// biome lookups + cluster probes) plus a burst of allocator traffic. Spawn
// density is attempt-based, so mobs still appear at a similar rate.
// 8: with PS2_MAX_LIVE_MOBS at 6 the cap is what limits population, not the
// attempt rate, so the pass mostly finds it full; halving its frequency takes
// the average cost from ~2.5 ms to ~1.2 ms per tick.
#define PS2_MOB_SPAWN_INTERVAL_TICKS 8

// End profile. The End biome only spawns endermen, in groups of four, so one
// spawn pass fills the global cap with mobs that each run a teleport search,
// a block-carry scan and a player stare test per tick. 2 keeps the fight's
// side threat without competing with the dragon for the tick budget.
#define PS2_END_MAX_ENDERMEN 2
// Particles are never drawn here (PS2_SKIP_WORLD_PARTICLES); skip building the
// 128-sample teleport trail (nine RNG draws each) as well.
#define PS2_ENDERMAN_TELEPORT_PARTICLES 0
// The dragon runs three AABB entity scans per tick (both wings and the head)
// on top of its seven part updates and block sweep. Every other tick is enough:
// hurtResistantTime already blocks repeat damage inside that window and the
// wing push is a continuous motion, so alternating ticks only halves the scan.
#define PS2_DRAGON_COLLISION_TICK_DIVISOR 2
// The End is one island of ~+-100 blocks around the origin with nothing past
// it in this version, and the dragon circles waypoints within +-60 blocks.
// Streaming it through the 5x5 sliding window regenerated the dragon's 3x3
// footprint continuously (measured 2026-09-17: 25-37 synchronous 23 ms
// generates per 120 frames, ServerChunkCache 36-47 vs the player's 25) and
// re-decorated the central chunk, which respawned the ender crystals. Chunks
// within this radius of the origin are generated on the loading screen and
// stay resident for the whole End visit. 5 -> 11x11 = 121 columns; End
// columns have no skylight and only mid-height sections, ~3-3.5 MB total
// against the ~14.5 MB free on entry. -1 disables it.
#define PS2_END_RESIDENT_CHUNK_RADIUS 5

// Whether Chunk::generateSkylightMap seeds the cross-chunk gap-lighting scan for
// every one of its 256 columns.
//
// generateSkylightMap computes skylight VERTICALLY: full 15 down to each column's
// heightMap, then attenuated below it. It then flags all 256 columns so
// updateSkylight_do() can compare each against its four neighbours and schedule a
// flood fill wherever the heights disagree. That flood fill is what carries light
// SIDEWAYS -- into an overhang, or a few blocks into a cave mouth.
//
// The vanilla 3D density generator needs it, because it produces overhangs and
// floating islands whose underside is lit only from the side. The PS2 profile does
// not run that generator: PS2_USE_HEIGHTMAP_TERRAIN builds every column as solid
// stone from y=1 to h, then water to sea level, then open air. Such a world has no
// overhang anywhere, so the vertical pass is already the final answer for the
// surface, and the whole 256-column scan re-derives values it cannot change.
//
// Cost of switching it off, and it is a real one: cave openings. Caves are still
// carved (PS2_SKIP_CAVE_GENERATION is 0), and those DO create sideways-lit air. A
// cave mouth will read as an abrupt dark edge instead of a short gradient. Nothing
// else regresses -- notably, tree shade is unaffected, because blocks placed by
// populate() go through Chunk::setBlockIDWithMetadata, which flags its own column
// via propagateSkylightOcclusion(). That per-block path stays live either way; the
// only thing this removes is the blanket seed after a chunk is (re)lit.
//
// Set to 1 to restore the vanilla scan.
#ifndef PS2_SEED_GAP_LIGHTING_ON_RELIGHT
#  define PS2_SEED_GAP_LIGHTING_ON_RELIGHT 0
#endif

// Temporary PS2 visual profile: the vanilla lighting arrays can be incomplete
// while chunks are generated lazily, producing fully black terrain faces.
// Force terrain brightness until a cheaper/fixed skylight pipeline is added.
#define PS2_FORCE_FULLBRIGHT_TERRAIN 0

// World-generation performance profile. PS2 uses the lightweight heightmap and
// float hot paths by default, while retaining the vanilla cave source radius and
// decoration counts. Set PS2_FAST_WORLDGEN=1 to additionally reduce the cave
// source sweep and decoration work. Every fast path trades seed parity for speed,
// and individual knobs can still be overridden independently.
#ifndef PS2_FAST_WORLDGEN
#define PS2_FAST_WORLDGEN 1
#endif

// Cave carving is one of the biggest chunk-generation costs. Java 1.2.5
// scans a source radius of 8 (17x17 chunks) around every target chunk. Keep
// that value as the default so cave origins and cross-chunk branches remain
// deterministic with the desktop port. The old radius-3 PS2 profile remains
// available as an explicit build-time optimization, for example
// -DPS2_CAVE_SOURCE_RADIUS=3, when performance is preferred over seed parity.
//
// The cave geometry can still use float on PS2 because the R5900 has no
// hardware double-precision arithmetic. This can move a rounded cave edge by
// a block, but it no longer silently drops distant vanilla cave origins.
#define PS2_SKIP_CAVE_GENERATION 0
#ifndef PS2_CAVE_SOURCE_RADIUS
#  if PS2_FAST_WORLDGEN
#    define PS2_CAVE_SOURCE_RADIUS 3
#  else
#    define PS2_CAVE_SOURCE_RADIUS 8
#  endif
#endif
#define PS2_CAVE_RARITY 15
// Ravines share the cave sweep: (2*PS2_CAVE_SOURCE_RADIUS+1)^2 Random re-seeds
// per chunk (three 64-bit multiplies each on the EE) for a 1-in-50 roll, and a
// hit walks the same sin/cos node loop as a cave. Pocket Edition never had
// them. Off, MapGenRavine is still allocated but never swept; chunks already
// saved keep theirs.
#ifndef PS2_SKIP_RAVINE_GENERATION
#  define PS2_SKIP_RAVINE_GENERATION (PS2_FAST_WORLDGEN ? 1 : 0)
#endif
#ifndef PS2_FLOAT_CAVE_GENERATION
#  define PS2_FLOAT_CAVE_GENERATION 1
#endif

// Map features: mineshafts, villages and strongholds. These are the three most
// expensive items in the per-chunk generation budget and the least visible in a
// 5x5 chunk window.
//
// Each is a MapGenStructure, which pins its own sweep to the vanilla radius of 8
// (MapGenStructure::MapGenStructure) regardless of PS2_CAVE_SOURCE_RADIUS, so
// every generated chunk walks 17x17 = 289 source columns apiece -- 867
// generateChunk calls on top of the 49 caves pay. They also occupy three of the eight stages of
// ChunkProviderGenerate::advanceGenerationTask, and because a stage is atomic
// while the generation budget is only checked between stages, each one is a tick
// the streaming queue cannot spend on the chunk the player is walking toward.
// A fourth cost sits in decoration: PopulateStage::Structures runs
// generateStructuresInChunk for all three
// (ChunkProviderGeneratePopulateIncremental.cpp).
//
// Off, the generation state machine goes straight from Ravines to BuildChunk and
// the three generators are never allocated, so their persistent structure maps
// stop growing with exploration as well.
//
// This is a build profile, not a world property: WorldInfo keeps its own
// MapFeatures flag, so the same save still generates structures on a desktop
// build. Trade-off: it CHANGES generated terrain. villageGenerated is then always
// false, which re-enables the water/lava lake rolls a village used to suppress,
// and the skipped generateStructuresInChunk calls no longer consume from the
// populate RNG, so ore and lake placement shifts. That is exactly what vanilla
// MapFeatures=false does; already-saved chunks are unaffected.
#ifndef PS2_GENERATE_MAP_FEATURES
#  define PS2_GENERATE_MAP_FEATURES 1
#endif


// Lightweight 2D heightmap terrain generator (ChunkProviderGenerateLite.cpp).
// Replaces the vanilla 3D double-precision density field with a single all-float
// noise column per block. The EE has no hardware double FPU, so the vanilla
// 8/16-octave double Perlin sweeps are software-emulated and are the dominant
// cost of the multi-second stall when crossing a chunk border; the heightmap
// path is roughly an order of magnitude cheaper. Trade-off: no overhangs,
// floating islands or noise caves (caves are below the visible window anyway).
// Set to 0 to fall back to the vanilla 3D generator.
#ifndef PS2_USE_HEIGHTMAP_TERRAIN
#  define PS2_USE_HEIGHTMAP_TERRAIN 1
#endif
// Surface replacement for the PS2 heightmap path. The vanilla routine scans
// all 128 Y values and draws bedrock RNG at every level for every column. The
// heightmap already knows the solid top, so the fast path touches only the
// surface/filler band plus Y 0..4 bedrock. This changes the per-chunk surface
// RNG stream, which is acceptable under PS2_FAST_WORLDGEN.
#ifndef PS2_FAST_SURFACE_PASS
#  define PS2_FAST_SURFACE_PASS (PS2_FAST_WORLDGEN && PS2_USE_HEIGHTMAP_TERRAIN)
#endif
// The legacy block buffer is already scanned once while Chunk converts it into
// ExtendedBlockStorage. Cache the final opaque-column heights during that pass
// so the initial skylight build does not scan every column a second time.
#ifndef PS2_PRECOMPUTE_INITIAL_HEIGHTMAP
#  define PS2_PRECOMPUTE_INITIAL_HEIGHTMAP PS2_FAST_WORLDGEN
#endif
// Collapse the thousands of per-block renderer invalidations emitted while
// initial skylight is filled into at most one dirty range per section.
#ifndef PS2_BATCH_INITIAL_SKYLIGHT_RENDER_UPDATES
#  define PS2_BATCH_INITIAL_SKYLIGHT_RENDER_UPDATES 1
#endif
// Take the block and random-tick counts straight from the fill loop that turns
// the generator's legacy 32 KB block buffer into ExtendedBlockStorage sections,
// instead of calling recalculateBlockCounts() afterwards.
//
// That loop already visits every non-air block of the chunk, so the section
// counters can be accumulated as it goes. recalculateBlockCounts() then re-scans
// all 4096 cells of every allocated section to reach the same two numbers -- with
// the heightmap generator that is five to eight sections, i.e. 20k-32k redundant
// iterations per generated chunk.
//
// The two predicates are the same test: the fill loop checks
// Block::blocksList[id] != nullptr && Block::tickOnLoad[id], which is exactly
// what ExtendedBlockStorage::isRandomTickBlock() evaluates. Counts are therefore
// identical, not approximated.
//
// Only the generated-chunk constructor is affected. Chunks read back from the
// Memory Card go through the NBT loader, which keeps its own path.
#ifndef PS2_BULK_GENERATED_CHUNK_IMPORT
#  define PS2_BULK_GENERATED_CHUNK_IMPORT 1
#endif
// Y at/below which empty columns fill with water in the optional fast
// heightmap generator. Release 1.2.5 uses sea level 63.
#define PS2_HEIGHTMAP_SEA_LEVEL 63
// Average land height the noise oscillates around.
#define PS2_HEIGHTMAP_BASE_HEIGHT 64
// Peak +/- swing of the broad continental noise, in blocks. 28 is a modest
// lift over the old 24-block profile: more hills without turning every biome
// into mountains or materially increasing the generator's noise cost.
#define PS2_HEIGHTMAP_AMPLITUDE 28
// Surface water freezes to ice when the column's biome temperature is below this.
// The fast heightmap path uses this approximate freeze threshold; the parity path
// uses the normal Java 1.2.5 biome/freezing rules. Float biome noise can nudge
// temperate columns across the threshold, so 0.35 keeps cold biomes frozen without
// the spurious patches. Set to a negative value to disable freezing entirely.
#define PS2_HEIGHTMAP_FREEZE_TEMP 0.35f

// Chunk decoration (populate) budget. populate() runs synchronously for up to
// 4 chunks in a single worldTick when crossing a chunk border, and the liquid
// spring passes are the worst offenders: vanilla scatters 50 water + 20 lava
// sources per chunk, each placed with setBlockWithNotify -> neighbour block
// updates and fluid propagation. 70 fluid foci x4 chunks = thousands of chained
// updates in one tick -- the dominant worldTick stall (and a deep update chain
// that can smash the small EE stack). These cap the per-chunk counts on console.
// 0 disables a pass entirely. PC keeps vanilla counts.
#ifndef PS2_POPULATE_WATER_SPRINGS
#  define PS2_POPULATE_WATER_SPRINGS (PS2_FAST_WORLDGEN ? 4 : 50)
#endif
#ifndef PS2_POPULATE_LAVA_SPRINGS
#  define PS2_POPULATE_LAVA_SPRINGS (PS2_FAST_WORLDGEN ? 2 : 20)
#endif
// A dungeon attempt scans a ~7x6x7 box through World::getBlockId (~300 chunk
// lookups) to validate the site and nearly always rejects it. Kept at 2 so
// spawners and chest loot still exist; 0 skips the pass.
#ifndef PS2_POPULATE_DUNGEONS
#  define PS2_POPULATE_DUNGEONS (PS2_FAST_WORLDGEN ? 2 : 8)
#endif
// Water (1 in 4) and lava (1 in 8) lakes: WorldGenLakes fills a 16x16x8 blob
// grid from five double-precision spheres, then makes three passes over the
// 2048 cells with world reads and lit setBlock calls. 0 skips both rolls.
#ifndef PS2_POPULATE_LAKES
#  define PS2_POPULATE_LAKES (PS2_FAST_WORLDGEN ? 0 : 1)
#endif
// Animal groups spawned together with the chunk (SpawnerAnimals::
// performWorldGenSpawning). Kept on: in 1.2.5 this is where most passive
// animals come from, natural passive spawning is slow and grass-gated. 0 skips it.
#ifndef PS2_POPULATE_WORLDGEN_ANIMALS
#  define PS2_POPULATE_WORLDGEN_ANIMALS 1
#endif
// Scatter attempts per WorldGenFlowers / WorldGenTallGrass call. Every attempt
// is an isAirBlock + canBlockStay pair through the chunk map; vanilla makes 64
// and 128 of them for two flower and one grass call per chunk.
#ifndef PS2_FLOWER_PLACEMENT_ATTEMPTS
#  define PS2_FLOWER_PLACEMENT_ATTEMPTS (PS2_FAST_WORLDGEN ? 16 : 64)
#endif
#ifndef PS2_TALL_GRASS_PLACEMENT_ATTEMPTS
#  define PS2_TALL_GRASS_PLACEMENT_ATTEMPTS (PS2_FAST_WORLDGEN ? 32 : 128)
#endif
#ifndef PS2_POPULATE_TREE_BONUS
#  define PS2_POPULATE_TREE_BONUS (PS2_FAST_WORLDGEN ? 2 : 0)
#endif
// Cap on the tree attempts per decorated chunk. Vanilla asks for 10 in forest
// and taiga and 50 in jungle; Pocket Edition 0.6 forests landed at 6-8 (Beta's
// noise-driven k+5). Every attempt is a heightmap lookup plus a generator run
// and, once meshed, a canopy's worth of leaf faces on screen. 4 is below the
// Pocket Edition figure: with the PS2 render radius most of the visible
// sections are canopy in a forest, and leaf faces are what the frame pays
// for. -1 keeps vanilla.
#ifndef PS2_POPULATE_TREES_PER_CHUNK_MAX
#  define PS2_POPULATE_TREES_PER_CHUNK_MAX (PS2_FAST_WORLDGEN ? 2 : -1)
#endif
// Cap on the WorldGenTallGrass calls per decorated chunk (vanilla: jungle 25,
// plains 10, forest 2, taiga 1), each PS2_TALL_GRASS_PLACEMENT_ATTEMPTS
// scatter attempts. Pocket Edition 0.6 had no tall grass at all. Every blade
// that lands is a crossed pair of alpha-tested quads in the mesh, and it
// keeps the grass block under it from being skipped as an enclosed cube.
// -1 keeps vanilla.
#ifndef PS2_POPULATE_GRASS_PER_CHUNK_MAX
#  define PS2_POPULATE_GRASS_PER_CHUNK_MAX (PS2_FAST_WORLDGEN ? 2 : -1)
#endif
// Jungle only. WorldGenHugeTrees grows a 2x2 trunk 10-30 blocks tall with a
// canopy several times the size of a normal tree; a handful of them fill the
// PS2 render radius with leaf faces. 0 grows a normal jungle tree instead.
#ifndef PS2_POPULATE_JUNGLE_HUGE_TREES
#  define PS2_POPULATE_JUNGLE_HUGE_TREES (PS2_FAST_WORLDGEN ? 0 : 1)
#endif
// WorldGenVines passes per jungle chunk (vanilla 50). Vines are alpha-tested
// faces that also random-tick to spread.
#ifndef PS2_POPULATE_JUNGLE_VINES
#  define PS2_POPULATE_JUNGLE_VINES (PS2_FAST_WORLDGEN ? 8 : 50)
#endif
// These three passes replace stone with large 32-block filler pockets. They do
// not affect the actual ore economy, but vanilla runs 40 of them per populated
// chunk and every changed underground section is then fingerprinted, relit and
// remeshed. Keep a little variation while cutting that streaming-only work by
// 80% (8 attempts instead of 40). PC retains the vanilla counts below.
#ifndef PS2_POPULATE_CLAY_VEINS
#  define PS2_POPULATE_CLAY_VEINS (PS2_FAST_WORLDGEN ? 2 : 10)
#endif
#ifndef PS2_POPULATE_DIRT_VEINS
#  define PS2_POPULATE_DIRT_VEINS (PS2_FAST_WORLDGEN ? 4 : 20)
#endif
#ifndef PS2_POPULATE_GRAVEL_VEINS
#  define PS2_POPULATE_GRAVEL_VEINS (PS2_FAST_WORLDGEN ? 2 : 10)
#endif
// Surface snow pass scans 16x16 columns doing findTopSolidBlock + setBlockWithNotify.
// Cheap-ish but pure CPU with no gameplay value on console. 0 = skip it.
#ifndef PS2_POPULATE_SNOW_PASS
#  define PS2_POPULATE_SNOW_PASS (PS2_FAST_WORLDGEN ? 0 : 1)
#endif

// Light propagation is fed most heavily by the same newly populated chunks.
// A job count alone is not a frame-time bound because each MetadataChunkBlock
// covers a different-sized box. Check both: 128 jobs is the deterministic
// backstop, while 2.5 ms yields the queue between jobs on the normal render
// path. A single job remains atomic, so this cannot leave a half-updated box.
#define PS2_LIGHTING_UPDATES_PER_FRAME 128 // vanilla 500
// When the queue is at most QUEUE_MAX jobs at drain start it is interactive
// work (a torch, a dug block), not streaming, and the drain may run BURST jobs
// ignoring PS2_LIGHTING_BUDGET_US so the light settles in one frame. A torch is
// ~1000 cells, ~1-2 ms with the direct section reads in MetadataChunkBlock.
#define PS2_LIGHTING_INTERACTIVE_QUEUE_MAX 256
#define PS2_LIGHTING_INTERACTIVE_BURST     2048
#define PS2_LIGHTING_BUDGET_US         2500
// Vanilla only probes the five newest jobs for overlap. Chunk generation on PS2
// can enqueue tens of thousands of nearly-identical light boxes, so scan a
// wider tail before allocating another MetadataChunkBlock. The hard cap prevents pathological propagation storms from consuming the
// remaining heap; recent-neighbour merging catches the common duplicates first.
#define PS2_LIGHTING_MERGE_SCAN        96
// 6144 (from 12288): ~32 bytes per queued box, so the cap is ~200 KB instead
// of ~400 KB, and a shorter queue is also a shorter merge tail. Chunks are now
// generated inside a smaller cache radius, so fewer boxes arrive at once.
#define PS2_LIGHTING_QUEUE_HARD_CAP    6144
#define PS2_FAST_LIGHTING_CHUNK_ACCESS 1

// Coalesce the light updates emitted while a chunk is being decorated.
//
// Every block populate() places goes through Chunk::setBlockIDWithMetadata,
// which ends in two scheduleLightingUpdate calls -- one Sky, one Block -- for a
// single 1x1x1 box. A tree is ~60 blocks, so ~120 jobs; the lake and spring
// passes produce thousands. PS2_LIGHTING_MERGE_SCAN above already exists to
// re-merge that flood after the fact, which is treating the symptom.
//
// With this on, a light update issued while the population fast path owns the
// 2x2 chunk group is instead unioned into one box per (sky/block, chunk,
// vertical section) and the accumulated boxes are enqueued once, from
// endPopulationFastPath(). Each box stays clipped to a single 16x16x16 section,
// so it cannot exceed MetadataChunkBlock's cell cap, and the flush happens after
// the fast path is closed, so the enqueued jobs take the normal path.
//
// Light VALUES are unchanged: the flood-fill still runs over the same volume,
// just as a handful of jobs instead of thousands of overlapping ones. The
// mechanism is shared with the Wii profile (WiiWorldTuning.h), where it has been
// live; PS2 was simply never given the knob. Set to 0 to restore per-block jobs.
#define PS2_BATCH_POPULATION_LIGHTING  1

// Population already owns the 2x2 chunk group through World::beginPopulationFastPath().
// Use those retained Chunk pointers for generator reads/writes instead of resolving
// the same chunk through ChunkProvider for every placed tree/plant block. The Chunk
// setter still runs the normal block callbacks and lighting rules; this only removes
// redundant provider/hash lookups while the population fast path is active.
#define PS2_POPULATION_BLOCK_WRITER    1

// Deferred decoration. Vanilla populates up to 4 chunks *synchronously* inside the
// generation call (prepareChunk), on whatever frame first touches a new chunk --
// that is the worldTick spike when crossing a border. Worse, populate() places
// blocks with setBlockWithNotify, which re-enters provideChunk/prepareChunk and
// can recurse into more populate() calls (deep chain that can smash the small EE
// stack). On console we instead enqueue ready chunks and decorate this many per
// world tick, off the critical frame. This also breaks the re-entrancy entirely.
//
// Was DISABLED (0) at 32 MB: per-frame [PS2][FRAME] profiling proved the
// "chunkUnload" phase (= drainPendingPopulate -> populate) was the dominant tick
// cost (~400 ms EVERY tick) AND the RAM leak that drove a second OOM: vanilla
// populate() runs ~110 ore/dirt/gravel vein passes + lakes + dungeons + trees
// per chunk, and its +8-block decoration reached into neighbour chunks,
// force-generating them (chunk growth -> OOM).
//
// RE-ENABLED (1) trial: two things changed since. The Tessellator shrink freed
// ~7.5 MB of headroom, and the deferred queue now gates on canPopulateChunk()
// (+x/+z/diagonal neighbours must already exist), so decoration can no longer
// force-generate chunks — the mechanism behind the old leak. Watch slowTick/
// mallocUsed in the [PS2][FRAME] log; drop back to 0 (bare terrain) or switch
// to a trees-only populate if the tick regresses.
// Decorate vegetation and ores while the chunk is generated, straight into the
// generation buffer (ChunkProviderGenerateDecorateLocal.cpp). The chunk is
// published complete: no per-block setBlock, lighting job or remesh for trees
// and veins after it is visible. Populate keeps structures, lakes, dungeons,
// springs and animal groups. Trees on a chunk border are clipped at it and the
// decoration RNG stream separates from the populate one, so chunks generated
// with this on differ from vanilla; saved chunks are unaffected.
#ifndef PS2_CHUNK_LOCAL_DECORATION
#  define PS2_CHUNK_LOCAL_DECORATION (PS2_FAST_WORLDGEN ? 1 : 0)
#endif
#define PS2_POPULATE_CHUNKS_PER_TICK 1
// Incremental PS2 populate advances individual generator operations rather than
// whole chunks. Allow several cheap operations in one tick, but stop after the
// time budget once an atomic generator returns. Heavy generators such as lakes
// remain atomic and therefore can exceed the budget by themselves.
#define PS2_POPULATE_STEPS_PER_TICK 8
#define PS2_POPULATE_BUDGET_US      4000

// Deferred terrain GENERATION. The decompiled provider generates every requested
// chunk *synchronously*, so when the player crosses into ungenerated territory
// the physics/entity/render code force-generates the whole 5x5 cache area inside
// a single worldTick -- the multi-second stall, and the EE stack/heap pressure
// behind the wild-jump / "Syscall undefined" crashes. These throttle it:
//   - GENERATE_SYNC_RADIUS:     chunks within this Chebyshev radius of the player's
//                               chunk always generate immediately, so the player
//                               never stands on (or falls through) un-generated air.
//                               1 = the 3x3 the player stands and collides in.
//                               0 = ONLY the chunk the player is in is forced
//                               synchronous; the surrounding ring streams at
//                               GENERATE_CHUNKS_PER_TICK. This is the key knob for
//                               the ~1s border-cross freeze: at radius 1 the whole
//                               new leading row (3 chunks) generates in one tick; at
//                               radius 0 the chunk being entered already exists from
//                               the previous frame's 3x3, so a normal walk generates
//                               0 sync chunks and the freeze disappears. Trade-off:
//                               sprinting into un-generated land briefly shows a bare
//                               edge until it streams in (no fall -- collision still
//                               force-generates the chunk under the player).
//   - GENERATE_CHUNKS_PER_TICK: max *non-critical* chunks generated per world tick.
//                               The outer cache ring streams in this many per tick;
//                               over-budget requests get the blank (air) chunk this
//                               tick and are retried next tick. 0 = no throttle
//                               (vanilla synchronous behaviour).
#define PS2_GENERATE_SYNC_RADIUS 0
#define PS2_GENERATE_CHUNKS_PER_TICK 1
#define PS2_INCREMENTAL_CHUNK_GENERATION 1
#define PS2_GENERATION_STEPS_PER_TICK    16
// 4000 (from 8000) with PS2_STREAMING_FRAME_BUDGET_US below: the tick slice
// runs first in the frame, so whatever it takes comes off populate and
// lighting. Half the shared allowance leaves the other drains a turn on the
// frames where every queue is busy; on the frames where only generation is,
// the frame slice below hands it the rest.
#define PS2_GENERATION_BUDGET_US         4000
// Frame-side slice on top of the tick one (EntityRenderer calls
// ChunkProvider::serviceFrameGeneration before rendering). Pocket Edition ran
// its generator in the frame loop; at 50-60 frames against 20 ticks this
// roughly triples how often a chunk gets a step without raising any single
// slice above the tick budget. Off at 0.
#define PS2_GENERATION_STEPS_PER_FRAME   8
#define PS2_GENERATION_FRAME_BUDGET_US   3000

// Shared wall-clock allowance per rendered frame for the world-side streaming
// drains together: generation (tick and frame slices), deferred populate and
// the lighting queue. See platform/world/StreamingFrameBudget.h.
//
// The individual ceilings were tuned one at a time, and on a frame that runs
// a tick they stack: 8000 generation + 4000 populate + 2500 lighting + 3000
// frame generation = 17.5 ms of streaming before the renderer starts, on top
// of the 6 ms mesh budget. At 30 fps that is the frame. The next frame carries
// no tick and pays 3 ms, which is the alternating hitch the player feels while
// flying into new terrain.
//
// 8000 caps the world side at a quarter of a 30 fps frame. Together with
// PS2_CHUNK_BUILD_BUDGET_MS the worst case is ~14 ms of streaming, against
// ~23 ms before, plus whatever an atomic step overshoots. Each drain still
// runs at least one step per frame, so nothing starves; the queues just
// spread over more frames. 0 restores the independent ceilings.
#define PS2_STREAMING_FRAME_BUDGET_US    8000

// Source columns advanced per generation step by the cave and ravine sweeps.
//
// PS2_GENERATION_BUDGET_US can only stop the generator BETWEEN steps -- see the
// budget check at the end of ChunkProvider::drainPendingGeneration -- so a step
// that is internally atomic sets the floor on how long a generation tick runs,
// and no budget can bring it below that. Caves and ravines were exactly that:
// one step each, sweeping (2*PS2_CAVE_SOURCE_RADIUS+1)^2 = 49 source columns, so
// the budget bought nothing on the two heaviest of the remaining stages.
//
// Slicing them puts those two under the budget like every other stage. 8 columns
// is a seventh of the sweep at radius 3.
//
// Cost per column is not uniform: most fail their rarity roll and return almost
// immediately, while one that passes carves a whole tunnel. So this bounds how
// many carving columns can land in a single step, not a step's absolute cost.
// Lower it if the frame log still shows generation spikes at a small budget.
//
// This does not by itself generate chunks FASTER -- the same work is spread over
// more, cheaper steps. It is what makes PS2_GENERATION_BUDGET_US buy throughput
// instead of being overshot by the first stage that ignores it.
// 16 since the cave walk was tied to the source radius (MapGenBase): a node now
// walks 48 steps instead of 112, so a carving column costs well under half of
// what it did, and the 49-column sweep fits in four steps instead of seven.
#ifndef PS2_GENERATION_SOURCE_COLUMNS_PER_STEP
#  define PS2_GENERATION_SOURCE_COLUMNS_PER_STEP 16
#endif

// Structure source sweeps are much cheaper per source after MapGenStructure's
// negative-result cache removes overlapping checks. Keep a larger slice than
// caves/ravines so cached 17x17 sweeps do not take dozens of scheduler steps,
// while still giving the generation wall-clock budget interruption points.
#ifndef PS2_STRUCTURE_SOURCE_COLUMNS_PER_STEP
// One step for the whole 17x17 sweep: MapGenBase::generateRangeShared walks
// the three generators together and only seeds a column that none of them has
// resolved yet, so in steady state a full sweep is ~900 hash probes.
#  define PS2_STRUCTURE_SOURCE_COLUMNS_PER_STEP 289
#endif

// Replace the GenLayer biome chain with an all-float noise field.
//
// GenLayer::func_48425_a builds ~26 layers, and WorldChunkManager reads every
// biome id, temperature and rainfall through it. The heightmap generator asks for
// a 20x20 halo, which misses BiomeCache's aligned-16x16 fast path, so the whole
// chain runs uncached for every generated chunk. Each layer walks its area doing
// initChunkSeed()/nextInt() per cell -- 64-bit multiplies that compile to
// __muldi3 calls because the R5900 has no dmult, plus a 64-bit modulo.
//
// The replacement (WorldChunkManagerFast.cpp) is three 2-octave float fields --
// continent, temperature, humidity -- and a threshold table, the same shape
// Pocket Edition uses.
//
// This CHANGES the generated world, and more than PS2_FLOAT_BIOME_NOISE does:
// that one only rounds differently, this is a different biome field. Rivers and
// beaches disappear, because they came from dedicated layers rather than from a
// climate field. Terrain heights move with the biomes, since the Lite generator
// blends its column heights from biome minHeight/maxHeight. Saved chunks are not
// touched; only newly generated terrain differs.
//
// Set to 0 for the exact vanilla biome field.
#ifndef PS2_FAST_BIOME_SOURCE
#  define PS2_FAST_BIOME_SOURCE 1
#endif

// All-float biome noise. The biome temperature/humidity field
// a 2D simplex) is sampled ~3000 times per generated chunk, entirely in `double`
// -- software-emulated on the EE. Those values only feed threshold comparisons
// and a smoothstep, so float precision is ample. This runs that simplex in float.
// Trade-off: float rounding slightly shifts biome boundaries vs the double path,
// i.e. it CHANGES the generated world. Set to 0 to fall back to the exact double
// biome noise (useful to isolate any world-gen regression from the generation
// throttle above). Normal PC uses the double path; PC Legacy can opt in separately.
#ifndef PS2_FLOAT_BIOME_NOISE
#  define PS2_FLOAT_BIOME_NOISE 1
#endif

// All-float ore/dirt/gravel/clay vein placement (WorldGenMinable). Measured:
// the "chunkUnload" tick phase — which is really drainPendingPopulate ->
// populate(), not chunk unloading — showed 428-514 ms spikes in the
// [PS2][FRAME] log on the frames where a chunk decorated. populate() runs ~92
// veins per chunk (10 clay + 20 dirt + 10 gravel + 20 coal + 20 iron + 2 gold +
// 8 redstone + 1 diamond + 1 lapis), and WorldGenMinable::generate was written
// entirely in `double`: 33 interpolation steps, each sweeping a ~5x5x5 box with
// several doubles per cell. The EE has no hardware double FPU, so that is a few
// million software-emulated operations per chunk.
//
// The values only feed an ellipsoid test against 1.0 and a floor(), so float is
// ample. RNG calls are untouched (same calls, same order), so vein *placement*
// still follows the world seed; only a vein's rounded edge can shift by a block.
// Ore counts and depths are unchanged — nothing becomes unmineable.
//
// Trade-off: like PS2_FLOAT_BIOME_NOISE, this CHANGES generated vein shapes very
// slightly versus the double path. Set to 0 for bit-exact vanilla arithmetic
// (useful to isolate a world-gen regression); the 0 path is bit-identical to the
// original, the hoisting done alongside this is exact.
#ifndef PS2_FLOAT_ORE_VEINS
#  define PS2_FLOAT_ORE_VEINS 1
#endif

// Octave counts for ChunkProviderGenerate's main terrain-shape noise. Vanilla
// is 16 octaves for the high/low density fields (field_912_k, field_911_l) and
// 8 for the blend selector (field_910_m); measured, that is ~17,000 Perlin
// samples per chunk (a 5x17x5 grid x 40 combined octaves), already in float
// (PLATFORM_CONSOLE_LOW) and already reusing the corner interpolation across
// unchanged Y cells -- the "generate" frame-log figure this trims is close to
// the algorithmic floor of that approach, not a leftover double or a missed
// cache. Each dropped octave is a halving of that frequency's contribution, so
// the terrain gets very slightly smoother/less detailed at the highest
// frequencies; the low-frequency shape (the part a player actually reads as
// "terrain") is unchanged. This does NOT preserve vanilla terrain for a given
// seed -- unlike PS2_FLOAT_BIOME_NOISE/PS2_FLOAT_ORE_VEINS above, which only
// round differently, fewer octaves is fewer octaves. Set both back to 16/8 for
// bit-for-bit vanilla shape at the original CPU cost.
#ifndef PS2_TERRAIN_DENSITY_OCTAVES
#  define PS2_TERRAIN_DENSITY_OCTAVES 12
#endif
#ifndef PS2_TERRAIN_SELECT_OCTAVES
#  define PS2_TERRAIN_SELECT_OCTAVES 6
#endif

// Size of MathHelper's sine lookup table, as a power of two. Vanilla is 16, i.e.
// 65536 entries, and on this console that is the wrong shape twice over.
//
// It is 256 KB of a 32 MB budget, sitting permanently in .bss. And the R5900 has
// an 8 KB data cache, so a table 32x larger than the whole cache turns every
// MathHelper::sin/cos into a guaranteed miss at main-memory latency -- the index
// is an angle, so consecutive calls land nowhere near each other and there is no
// locality to recover. Entity movement, mob AI, particles and every rotated
// model call these constantly.
//
// 12 is 4096 entries = 16 KB: 16x smaller than the D-cache pressure it used to
// apply, and 240 KB returned to the heap.
//
// Trade-off, and it is a real one: the angular step goes from 2*pi/65536 to
// 2*pi/4096, so sin/cos gain a worst-case error of ~0.0015 (0.09 degrees). That
// is invisible in a rendered rotation but it does mean entity motion no longer
// matches the double-precision Java path exactly. Set to 16 to restore the
// vanilla table; the desktop build always uses 16.
//
// The alternative not taken: 256 entries plus linear interpolation would be 1 KB
// (permanently cache-resident) AND more accurate than vanilla, at the cost of a
// second load and a multiply-add inside sin(). Worth measuring if this knob
// proves to matter.
