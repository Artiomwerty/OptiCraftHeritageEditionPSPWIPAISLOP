#ifdef PSP_PLATFORM
#include "lwjgl/GLContext.h"

namespace lwjgl
{
namespace GLContext
{
namespace
{
int g_samples = 0;
detail::GLCapabilities &caps()
{
	static detail::GLCapabilities c;
	return c;
}
} // namespace

void setRequestedSamples(int samples) { g_samples = samples; }
int getRequestedSamples() { return g_samples; }

// The GE has no extension string; an empty capability set keeps the desktop-only paths
// (occlusion queries, VBOs, shaders) off.
void instantiate() {}
const detail::GLCapabilities &getCapabilities() { return caps(); }
} // namespace GLContext
} // namespace lwjgl
#endif
