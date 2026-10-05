// Minimal OpenGL 3.3 binding: only the functions and constants this program
// uses, resolved at run time through SDL. No system GL headers or loader
// library are needed, which keeps the build identical on Linux and Windows.
#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#define AB_GLAPI __stdcall
#else
#define AB_GLAPI
#endif

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLbitfield = unsigned int;
using GLboolean = unsigned char;
using GLfloat = float;
using GLchar = char;
using GLubyte = unsigned char;
using GLsizeiptr = std::ptrdiff_t;

inline constexpr GLenum GL_FALSE = 0;
inline constexpr GLenum GL_TRIANGLES = 0x0004;
inline constexpr GLenum GL_SRC_ALPHA = 0x0302;
inline constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
inline constexpr GLenum GL_BLEND = 0x0BE2;
inline constexpr GLenum GL_UNPACK_ALIGNMENT = 0x0CF5;
inline constexpr GLenum GL_PACK_ALIGNMENT = 0x0D05;
inline constexpr GLenum GL_TEXTURE_2D = 0x0DE1;
inline constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
inline constexpr GLenum GL_FLOAT = 0x1406;
inline constexpr GLenum GL_RGB = 0x1907;
inline constexpr GLenum GL_RGBA = 0x1908;
inline constexpr GLenum GL_VERSION = 0x1F02;
inline constexpr GLenum GL_NEAREST = 0x2600;
inline constexpr GLenum GL_LINEAR = 0x2601;
inline constexpr GLenum GL_TEXTURE_MAG_FILTER = 0x2800;
inline constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801;
inline constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802;
inline constexpr GLenum GL_TEXTURE_WRAP_T = 0x2803;
inline constexpr GLenum GL_COLOR_BUFFER_BIT = 0x4000;
inline constexpr GLenum GL_RGBA8 = 0x8058;
inline constexpr GLenum GL_CLAMP_TO_EDGE = 0x812F;
inline constexpr GLenum GL_TEXTURE0 = 0x84C0;
inline constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
inline constexpr GLenum GL_STREAM_DRAW = 0x88E0;
inline constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
inline constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
inline constexpr GLenum GL_COMPILE_STATUS = 0x8B81;

extern void (AB_GLAPI* glActiveTexture)(GLenum);
extern void (AB_GLAPI* glAttachShader)(GLuint, GLuint);
extern void (AB_GLAPI* glBindBuffer)(GLenum, GLuint);
extern void (AB_GLAPI* glBindTexture)(GLenum, GLuint);
extern void (AB_GLAPI* glBindVertexArray)(GLuint);
extern void (AB_GLAPI* glBlendFunc)(GLenum, GLenum);
extern void (AB_GLAPI* glBufferData)(GLenum, GLsizeiptr, const void*, GLenum);
extern void (AB_GLAPI* glClear)(GLbitfield);
extern void (AB_GLAPI* glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
extern void (AB_GLAPI* glCompileShader)(GLuint);
extern GLuint (AB_GLAPI* glCreateProgram)();
extern GLuint (AB_GLAPI* glCreateShader)(GLenum);
extern void (AB_GLAPI* glDeleteBuffers)(GLsizei, const GLuint*);
extern void (AB_GLAPI* glDeleteProgram)(GLuint);
extern void (AB_GLAPI* glDeleteShader)(GLuint);
extern void (AB_GLAPI* glDeleteTextures)(GLsizei, const GLuint*);
extern void (AB_GLAPI* glDeleteVertexArrays)(GLsizei, const GLuint*);
extern void (AB_GLAPI* glDrawArrays)(GLenum, GLint, GLsizei);
extern void (AB_GLAPI* glEnable)(GLenum);
extern void (AB_GLAPI* glEnableVertexAttribArray)(GLuint);
extern void (AB_GLAPI* glGenBuffers)(GLsizei, GLuint*);
extern void (AB_GLAPI* glGenTextures)(GLsizei, GLuint*);
extern void (AB_GLAPI* glGenVertexArrays)(GLsizei, GLuint*);
extern void (AB_GLAPI* glGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
extern void (AB_GLAPI* glGetShaderiv)(GLuint, GLenum, GLint*);
extern const GLubyte* (AB_GLAPI* glGetString)(GLenum);
extern GLint (AB_GLAPI* glGetUniformLocation)(GLuint, const GLchar*);
extern void (AB_GLAPI* glLinkProgram)(GLuint);
extern void (AB_GLAPI* glPixelStorei)(GLenum, GLint);
extern void (AB_GLAPI* glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
extern void (AB_GLAPI* glShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
extern void (AB_GLAPI* glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
extern void (AB_GLAPI* glTexParameteri)(GLenum, GLenum, GLint);
extern void (AB_GLAPI* glUniform1i)(GLint, GLint);
extern void (AB_GLAPI* glUniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat*);
extern void (AB_GLAPI* glUseProgram)(GLuint);
extern void (AB_GLAPI* glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
extern void (AB_GLAPI* glViewport)(GLint, GLint, GLsizei, GLsizei);

namespace ab {
// Resolves every function above. Call once after the GL context exists.
// Returns false (and names the missing function on stderr) if any is unavailable.
bool loadOpenGL();
}  // namespace ab
