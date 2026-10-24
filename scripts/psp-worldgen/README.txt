OptiCraft PSP world generator
=============================

Builds a complete world on your PC with the vanilla Minecraft 1.2.5 terrain generator
(3D noise mountains and overhangs, caves, ravines, lakes, dungeons, villages, strongholds,
mineshafts, trees, ores, animals). The PSP then only loads it, so chunks appear much faster
and nothing has to be generated on the PSP. The world has a fixed size: at the edge it ends.

1. Double-click GeneratePSPWorld.bat
2. Enter a folder name, a seed (or leave empty for random) and the size in blocks
   (3000 = 3000 x 3000 blocks; 5000 = 5000 x 5000).
3. Wait until it says "Done".
4. Copy the folder  out\<name>  to  PSP\SAVEDATA\OPTICRAFT\saves\<name>
   - PSP Go internal storage: connect via USB, the PSP shows as a drive.
5. Start OptiCraft on the PSP; the world is in the world list.

Command line:  OptiCraft.exe --pregen <output folder> <name> <seed|random> <size in blocks>
