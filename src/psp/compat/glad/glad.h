#pragma once
// OpenGL stand-in for the PSP build. A few shared files (the End portal effect, the legacy
// colour-grade pass) talk to OpenGL directly on desktop. The PSP renderer is not OpenGL, so
// those calls are inert here: same names, no effect. Real drawing goes through RenderAPI.h.
typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
typedef double GLdouble;
typedef unsigned char GLubyte;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef char GLchar;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_LIGHTING 0x0B50
#define GL_BLEND 0x0BE2
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_ONE 1
#define GL_S 0x2000
#define GL_T 0x2001
#define GL_R 0x2002
#define GL_Q 0x2003
#define GL_TEXTURE_GEN_MODE 0x2500
#define GL_OBJECT_PLANE 0x2501
#define GL_EYE_PLANE 0x2502
#define GL_OBJECT_LINEAR 0x2401
#define GL_EYE_LINEAR 0x2400
#define GL_TEXTURE_GEN_S 0x0C60
#define GL_TEXTURE_GEN_T 0x0C61
#define GL_TEXTURE_GEN_R 0x0C62
#define GL_TEXTURE_GEN_Q 0x0C63
#define GL_TEXTURE 0x1702
#define GL_MODELVIEW 0x1700

inline void glDisable(GLenum) {}
inline void glEnable(GLenum) {}
inline void glBlendFunc(GLenum, GLenum) {}
inline void glPushMatrix() {}
inline void glPopMatrix() {}
inline void glLoadIdentity() {}
inline void glMatrixMode(GLenum) {}
inline void glTranslatef(float, float, float) {}
inline void glScalef(float, float, float) {}
inline void glRotatef(float, float, float, float) {}
inline void glTexGeni(GLenum, GLenum, GLint) {}
inline void glTexGenfv(GLenum, GLenum, const float *) {}
