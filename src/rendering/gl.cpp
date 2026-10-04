#include "rendering/gl.hpp"

#include <SDL3/SDL.h>

#include <cstdio>

void (AB_GLAPI* glActiveTexture)(GLenum) = nullptr;
void (AB_GLAPI* glAttachShader)(GLuint, GLuint) = nullptr;
void (AB_GLAPI* glBindBuffer)(GLenum, GLuint) = nullptr;
void (AB_GLAPI* glBindTexture)(GLenum, GLuint) = nullptr;
void (AB_GLAPI* glBindVertexArray)(GLuint) = nullptr;
void (AB_GLAPI* glBlendFunc)(GLenum, GLenum) = nullptr;
void (AB_GLAPI* glBufferData)(GLenum, GLsizeiptr, const void*, GLenum) = nullptr;
void (AB_GLAPI* glClear)(GLbitfield) = nullptr;
void (AB_GLAPI* glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
void (AB_GLAPI* glCompileShader)(GLuint) = nullptr;
GLuint (AB_GLAPI* glCreateProgram)() = nullptr;
GLuint (AB_GLAPI* glCreateShader)(GLenum) = nullptr;
void (AB_GLAPI* glDeleteBuffers)(GLsizei, const GLuint*) = nullptr;
void (AB_GLAPI* glDeleteProgram)(GLuint) = nullptr;
void (AB_GLAPI* glDeleteShader)(GLuint) = nullptr;
void (AB_GLAPI* glDeleteTextures)(GLsizei, const GLuint*) = nullptr;
void (AB_GLAPI* glDeleteVertexArrays)(GLsizei, const GLuint*) = nullptr;
void (AB_GLAPI* glDrawArrays)(GLenum, GLint, GLsizei) = nullptr;
void (AB_GLAPI* glEnable)(GLenum) = nullptr;
void (AB_GLAPI* glEnableVertexAttribArray)(GLuint) = nullptr;
void (AB_GLAPI* glGenBuffers)(GLsizei, GLuint*) = nullptr;
void (AB_GLAPI* glGenTextures)(GLsizei, GLuint*) = nullptr;
void (AB_GLAPI* glGenVertexArrays)(GLsizei, GLuint*) = nullptr;
void (AB_GLAPI* glGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
void (AB_GLAPI* glGetShaderiv)(GLuint, GLenum, GLint*) = nullptr;
const GLubyte* (AB_GLAPI* glGetString)(GLenum) = nullptr;
GLint (AB_GLAPI* glGetUniformLocation)(GLuint, const GLchar*) = nullptr;
void (AB_GLAPI* glLinkProgram)(GLuint) = nullptr;
void (AB_GLAPI* glPixelStorei)(GLenum, GLint) = nullptr;
void (AB_GLAPI* glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*) = nullptr;
void (AB_GLAPI* glShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*) = nullptr;
void (AB_GLAPI* glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) = nullptr;
void (AB_GLAPI* glTexParameteri)(GLenum, GLenum, GLint) = nullptr;
void (AB_GLAPI* glUniform1i)(GLint, GLint) = nullptr;
void (AB_GLAPI* glUniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat*) = nullptr;
void (AB_GLAPI* glUseProgram)(GLuint) = nullptr;
void (AB_GLAPI* glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) = nullptr;
void (AB_GLAPI* glViewport)(GLint, GLint, GLsizei, GLsizei) = nullptr;

namespace ab {

namespace {
template <typename F>
bool load(F& fn, const char* name) {
    fn = reinterpret_cast<F>(SDL_GL_GetProcAddress(name));
    if (fn == nullptr) std::fprintf(stderr, "ERROR OpenGL function missing: %s\n", name);
    return fn != nullptr;
}
}  // namespace

bool loadOpenGL() {
    bool ok = true;
    ok = load(glActiveTexture, "glActiveTexture") && ok;
    ok = load(glAttachShader, "glAttachShader") && ok;
    ok = load(glBindBuffer, "glBindBuffer") && ok;
    ok = load(glBindTexture, "glBindTexture") && ok;
    ok = load(glBindVertexArray, "glBindVertexArray") && ok;
    ok = load(glBlendFunc, "glBlendFunc") && ok;
    ok = load(glBufferData, "glBufferData") && ok;
    ok = load(glClear, "glClear") && ok;
    ok = load(glClearColor, "glClearColor") && ok;
    ok = load(glCompileShader, "glCompileShader") && ok;
    ok = load(glCreateProgram, "glCreateProgram") && ok;
    ok = load(glCreateShader, "glCreateShader") && ok;
    ok = load(glDeleteBuffers, "glDeleteBuffers") && ok;
    ok = load(glDeleteProgram, "glDeleteProgram") && ok;
    ok = load(glDeleteShader, "glDeleteShader") && ok;
    ok = load(glDeleteTextures, "glDeleteTextures") && ok;
    ok = load(glDeleteVertexArrays, "glDeleteVertexArrays") && ok;
    ok = load(glDrawArrays, "glDrawArrays") && ok;
    ok = load(glEnable, "glEnable") && ok;
    ok = load(glEnableVertexAttribArray, "glEnableVertexAttribArray") && ok;
    ok = load(glGenBuffers, "glGenBuffers") && ok;
    ok = load(glGenTextures, "glGenTextures") && ok;
    ok = load(glGenVertexArrays, "glGenVertexArrays") && ok;
    ok = load(glGetShaderInfoLog, "glGetShaderInfoLog") && ok;
    ok = load(glGetShaderiv, "glGetShaderiv") && ok;
    ok = load(glGetString, "glGetString") && ok;
    ok = load(glGetUniformLocation, "glGetUniformLocation") && ok;
    ok = load(glLinkProgram, "glLinkProgram") && ok;
    ok = load(glPixelStorei, "glPixelStorei") && ok;
    ok = load(glReadPixels, "glReadPixels") && ok;
    ok = load(glShaderSource, "glShaderSource") && ok;
    ok = load(glTexImage2D, "glTexImage2D") && ok;
    ok = load(glTexParameteri, "glTexParameteri") && ok;
    ok = load(glUniform1i, "glUniform1i") && ok;
    ok = load(glUniformMatrix4fv, "glUniformMatrix4fv") && ok;
    ok = load(glUseProgram, "glUseProgram") && ok;
    ok = load(glVertexAttribPointer, "glVertexAttribPointer") && ok;
    ok = load(glViewport, "glViewport") && ok;
    return ok;
}

}  // namespace ab
