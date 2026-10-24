// PC world generator for the PSP port ("OptiCraft.exe --pregen ...").
//
// Generates a whole square world with the desktop build's vanilla 1.2.5 terrain (3D density noise,
// caves, ravines, lakes, dungeons, structures, decoration, worldgen animals), saves it as a normal
// Anvil save and marks it as a fixed-size world (WorldInfo worldSizeType 3 + LimitedChunkRadius).
// The PSP then only loads chunks from the Memory Stick and never generates terrain itself; past the
// edge of the generated square there is nothing, like the 256x256 / 864x864 worlds.
//
// Usage: OptiCraft --pregen <saves folder> <world folder name> <seed|random> <size in blocks>
// The world is built row by row with a small window of resident chunks, so memory stays flat.
#include "pc/world/WorldPregen.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "net/minecraft/src/AchievementList.h"
#include "net/minecraft/src/AnvilSaveConverter.h"
#include "net/minecraft/src/AxisAlignedBB.h"
#include "net/minecraft/src/BiomeGenBase.h"
#include "net/minecraft/src/Block.h"
#include "net/minecraft/src/Chunk.h"
#include "net/minecraft/src/ChunkBlockMap.h"
#include "net/minecraft/src/ChunkProvider.h"
#include "net/minecraft/src/ISaveHandler.h"
#include "net/minecraft/src/Item.h"
#include "net/minecraft/src/Material.h"
#include "net/minecraft/src/StatList.h"
#include "net/minecraft/src/ThreadedFileIOBase.h"
#include "net/minecraft/src/Vec3D.h"
#include "net/minecraft/src/World.h"
#include "net/minecraft/src/WorldInfo.h"
#include "net/minecraft/src/WorldSettings.h"
#include "net/minecraft/src/WorldType.h"

namespace
{
void usage()
{
	std::printf(
		"OptiCraft PSP world generator (vanilla 1.2.5 terrain)\n"
		"\n"
		"  OptiCraft --pregen <saves folder> <world name> <seed|random> <size in blocks>\n"
		"\n"
		"Example: OptiCraft --pregen out MyWorld random 3000\n"
		"Then copy out\\MyWorld to PSP\\SAVEDATA\\OPTICRAFT\\saves\\ on the PSP.\n");
}

long long parseSeed(const char *text)
{
	if (std::strcmp(text, "random") == 0)
		return static_cast<long long>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
	char *end = nullptr;
	const long long value = std::strtoll(text, &end, 10);
	if (end != nullptr && *end == '\0')
		return value;
	// Text seeds hash like Java's String.hashCode(), as in the create-world screen.
	int h = 0;
	for (const char *p = text; *p; ++p)
		h = 31 * h + static_cast<unsigned char>(*p);
	return h;
}

void initializeGameData()
{
	Material::initialize();
	Block::initialize();
	ChunkBlockMap::initialize();
	Item::initItems();
	WorldType::initialize();
	BiomeGenBase::initialize();
	AchievementList::initialize();
	StatList::initStats();
}
} // namespace

int runWorldPregen(int argc, char **argv)
{
	if (argc < 6)
	{
		usage();
		return 2;
	}
	const std::string savesDir = argv[2];
	const std::string worldName = argv[3];
	const long long seed = parseSeed(argv[4]);
	const int sizeBlocks = std::atoi(argv[5]);
	if (worldName.empty() || sizeBlocks < 64 || sizeBlocks > 20000)
	{
		std::printf("Size must be between 64 and 20000 blocks.\n");
		return 2;
	}
	// Chunks -radius .. radius-1 on both axes (rounded up to whole chunks).
	const int radius = std::max(2, (sizeBlocks + 31) / 32);
	const long long total = 4LL * radius * radius;

	std::error_code ec;
	std::filesystem::create_directories(savesDir, ec);
	if (std::filesystem::exists(std::filesystem::path(savesDir) / worldName / "level.dat"))
	{
		std::printf("%s already exists in %s; choose another name or delete it first.\n", worldName.c_str(), savesDir.c_str());
		return 3;
	}

	std::printf("Generating '%s': seed %lld, %d x %d blocks (%lld chunks)\n", worldName.c_str(), seed,
	            radius * 32, radius * 32, total);
	std::fflush(stdout);

	initializeGameData();

	AnvilSaveConverter saveFormat(savesDir);
	ISaveHandler *saveHandler = saveFormat.getSaveLoader(worldName, false);
	// Generated as an ordinary infinite world: limited worlds turn their rim into ocean, and the
	// biome layout must be the generator's own. The fixed size is written into level.dat at the end.
	WorldSettings settings(seed, 0, true, false, WorldType::DEFAULT, 0);
	World *world = new World(saveHandler, worldName, &settings);
	world->enablePendingTickChunkIndex();
	ChunkProvider *provider = dynamic_cast<ChunkProvider *>(world->getIChunkProvider());
	if (provider == nullptr)
	{
		std::printf("Unexpected chunk provider; cannot continue.\n");
		return 4;
	}
	provider->setChunkLoadRadius(radius + 4);

	// The spawn search ran for an infinite world and may sit outside the square. Search again with the
	// final size (inside the square only), then generate the rest as an infinite world again.
	{
		WorldInfo *info = world->getWorldInfo();
		info->setWorldSizeType(3);
		info->setLimitedChunkRadius(radius);
		world->regenerateSpawnPoint();
		info->setWorldSizeType(0);
		std::printf("Spawn at %d %d %d\n", info->getSpawnX(), info->getSpawnY(), info->getSpawnZ());
	}

	const auto t0 = std::chrono::steady_clock::now();
	long long done = 0;
	// One extra ring is generated so that every chunk inside the square gets decorated (population
	// needs its +X/+Z neighbours); the ring itself is saved too but lies outside the playable area.
	for (int z = -radius - 1; z <= radius; ++z)
	{
		for (int x = -radius - 1; x <= radius; ++x)
		{
			provider->setCurrentChunkOver(x, z);
			world->getChunkFromChunkCoords(x, z);
			if (x >= -radius && x < radius && z >= -radius && z < radius)
				++done;
		}
		// Finish queued lighting for what is resident, then save and drop rows that can no longer
		// be decorated by a neighbour (population reaches one chunk back in -X/-Z).
		for (int guard = 0; guard < 20000 && world->updatingLighting(); ++guard) {}
		for (Chunk *chunk : provider->getLoadedChunksSnapshot())
			if (chunk != nullptr)
				chunk->updateSkylight();
		provider->unloadChunksOutside(-radius - 2, z - 2, radius + 1, radius + 1, true);
		world->dropPendingBlockUpdatesOutside((-radius - 2) * 16, (z - 2) * 16, (radius + 2) * 16 - 1, (radius + 2) * 16 - 1);
		world->processUnloadedEntities();
		ThreadedFileIOBase::threadedIOInstance.waitForFinish();
		AxisAlignedBB::clearBoundingBoxPool();
		Vec3D::initialize();

		const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		const double rate = secs > 0.0 ? static_cast<double>(done) / secs : 0.0;
		const double left = rate > 0.0 ? static_cast<double>(total - done) / rate : 0.0;
		if (std::getenv("PREGEN_STATS"))
			std::printf("\n[row %d] chunks %d entities %d tileEntities %d\n", z, provider->getLoadedChunkCount(),
			            (int)world->loadedEntityList.size(), (int)world->loadedTileEntityList.size());
		std::printf("\r  %5.1f%%  %lld / %lld chunks  %.0f chunks/s  about %d:%02d left   ",
		            100.0 * static_cast<double>(done) / static_cast<double>(total), done, total, rate,
		            static_cast<int>(left) / 60, static_cast<int>(left) % 60);
		std::fflush(stdout);
	}
	std::printf("\n");

	for (int guard = 0; guard < 100000 && world->updatingLighting(); ++guard) {}
	provider->unloadChunksOutside(1, 1, 0, 0, true); // everything: an empty box
	world->processUnloadedEntities();

	WorldInfo *info = world->getWorldInfo();
	info->setWorldSizeType(3);
	info->setLimitedChunkRadius(radius);
	world->saveWorld(true, nullptr);
	ThreadedFileIOBase::threadedIOInstance.waitForFinish();

	const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	std::printf("Done in %d:%02d. Spawn at %d %d %d.\n", static_cast<int>(secs) / 60, static_cast<int>(secs) % 60,
	            info->getSpawnX(), info->getSpawnY(), info->getSpawnZ());
	std::printf("Copy the folder %s to PSP/SAVEDATA/OPTICRAFT/saves/ on the PSP.\n",
	            (std::filesystem::path(savesDir) / worldName).string().c_str());
	std::fflush(stdout);
	// The process exits here; tearing the world down would only spend time freeing memory.
	std::_Exit(0);
}
