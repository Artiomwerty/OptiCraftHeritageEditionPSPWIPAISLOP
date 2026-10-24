#pragma once

// "OptiCraft --pregen <saves folder> <world name> <seed|random> <size in blocks>": builds a whole
// fixed-size world with vanilla 1.2.5 terrain for the PSP port. Returns the process exit code.
int runWorldPregen(int argc, char **argv);
