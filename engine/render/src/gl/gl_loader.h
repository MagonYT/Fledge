#pragma once
// Tiny in-repo OpenGL 3.3 core function loader (no GLAD/GLEW dependency).
// Only the entry points Fledge uses are declared.
#include <cstddef>
#include <cstdint>

#if defined(_WIN32) && !defined(APIENTRY)
#define APIENTRY __stdcall
#elif !defined(APIENTRY)
#define APIENTRY
#endif

namespace fledge::gl {

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLboolean = unsigned char;
using GLfloat = float;
using GLbitfield = unsigned int;
using GLchar = char;
using GLubyte = unsigned char;
using GLsizeiptr = std::ptrdiff_t;
using GLintptr = std::ptrdiff_t;

constexpr GLenum GL_FALSE_ = 0;
constexpr GLenum GL_TRUE_ = 1;
constexpr GLenum GL_DEPTH_BUFFER_BIT = 0x00000100;
constexpr GLenum GL_COLOR_BUFFER_BIT = 0x00004000;
constexpr GLenum GL_LINES = 0x0001;
constexpr GLenum GL_TRIANGLES = 0x0004;
constexpr GLenum GL_DEPTH_TEST = 0x0B71;
constexpr GLenum GL_CULL_FACE = 0x0B44;
constexpr GLenum GL_BLEND = 0x0BE2;
constexpr GLenum GL_LEQUAL = 0x0203;
constexpr GLenum GL_BACK = 0x0405;
constexpr GLenum GL_SRC_ALPHA = 0x0302;
constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
constexpr GLenum GL_UNSIGNED_INT = 0x1405;
constexpr GLenum GL_FLOAT = 0x1406;
constexpr GLenum GL_RGBA = 0x1908;
constexpr GLenum GL_RGBA8 = 0x8058;
constexpr GLenum GL_SRGB8_ALPHA8 = 0x8C43;
constexpr GLenum GL_COMPRESSED_SRGB_S3TC_DXT1_EXT = 0x8C4C;
constexpr GLenum GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT = 0x8C4F;
constexpr GLenum GL_COMPRESSED_RGB_S3TC_DXT1_EXT = 0x83F0;
constexpr GLenum GL_COMPRESSED_RGBA_S3TC_DXT5_EXT = 0x83F3;
constexpr GLenum GL_NUM_EXTENSIONS = 0x821D;
constexpr GLenum GL_EXTENSIONS = 0x1F03;
constexpr GLenum GL_TEXTURE_MAX_LEVEL = 0x813D;
constexpr GLenum GL_TEXTURE_BASE_LEVEL = 0x813C;
constexpr GLenum GL_NEAREST = 0x2600;
constexpr GLenum GL_TEXTURE_2D_ARRAY = 0x8C1A;
constexpr GLenum GL_TEXTURE_CUBE_MAP = 0x8513;
constexpr GLenum GL_TEXTURE_CUBE_MAP_POSITIVE_X = 0x8515;
constexpr GLenum GL_TEXTURE_CUBE_MAP_SEAMLESS = 0x884F;
constexpr GLenum GL_TEXTURE_2D_MULTISAMPLE = 0x9100;
constexpr GLenum GL_TEXTURE_WRAP_R = 0x8072;
constexpr GLenum GL_TEXTURE_COMPARE_MODE = 0x884C;
constexpr GLenum GL_TEXTURE_COMPARE_FUNC = 0x884D;
constexpr GLenum GL_COMPARE_REF_TO_TEXTURE = 0x884E;
constexpr GLenum GL_RGBA16F = 0x881A;
constexpr GLenum GL_RG16F = 0x822F;
constexpr GLenum GL_R16F = 0x822D;
constexpr GLenum GL_RGBA32F = 0x8814;
constexpr GLenum GL_RG = 0x8227;
constexpr GLenum GL_RED = 0x1903;
constexpr GLenum GL_DEPTH_COMPONENT = 0x1902;
constexpr GLenum GL_DEPTH_COMPONENT32F = 0x8CAC;
constexpr GLenum GL_DEPTH_ATTACHMENT = 0x8D00;
constexpr GLenum GL_NONE = 0;
constexpr GLenum GL_READ_FRAMEBUFFER = 0x8CA8;
constexpr GLenum GL_DRAW_FRAMEBUFFER = 0x8CA9;
constexpr GLenum GL_UNIFORM_BUFFER = 0x8A11;
constexpr GLenum GL_INVALID_INDEX = 0xFFFFFFFFu;
constexpr GLenum GL_FRONT = 0x0404;
constexpr GLenum GL_FRONT_AND_BACK = 0x0408;
constexpr GLenum GL_LINE = 0x1B01;
constexpr GLenum GL_FILL = 0x1B02;
constexpr GLenum GL_LESS = 0x0201;
constexpr GLenum GL_ALWAYS = 0x0207;
constexpr GLenum GL_ONE = 1;
constexpr GLenum GL_MAX_SAMPLES = 0x8D57;
constexpr GLenum GL_HALF_FLOAT = 0x140B;
constexpr GLenum GL_VENDOR = 0x1F00;
constexpr GLenum GL_RENDERER = 0x1F01;
constexpr GLenum GL_VERSION = 0x1F02;
constexpr GLenum GL_TEXTURE_2D = 0x0DE1;
constexpr GLenum GL_TEXTURE_MAG_FILTER = 0x2800;
constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801;
constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802;
constexpr GLenum GL_TEXTURE_WRAP_T = 0x2803;
constexpr GLenum GL_LINEAR = 0x2601;
constexpr GLenum GL_LINEAR_MIPMAP_LINEAR = 0x2703;
constexpr GLenum GL_REPEAT = 0x2901;
constexpr GLenum GL_CLAMP_TO_EDGE = 0x812F;
constexpr GLenum GL_TEXTURE0 = 0x84C0;
constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
constexpr GLenum GL_ELEMENT_ARRAY_BUFFER = 0x8893;
constexpr GLenum GL_STATIC_DRAW = 0x88E4;
constexpr GLenum GL_DYNAMIC_DRAW = 0x88E8;
constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
constexpr GLenum GL_LINK_STATUS = 0x8B82;
constexpr GLenum GL_INFO_LOG_LENGTH = 0x8B84;
constexpr GLenum GL_FRAMEBUFFER = 0x8D40;
constexpr GLenum GL_RENDERBUFFER = 0x8D41;
constexpr GLenum GL_COLOR_ATTACHMENT0 = 0x8CE0;
constexpr GLenum GL_DEPTH_STENCIL_ATTACHMENT = 0x821A;
constexpr GLenum GL_DEPTH24_STENCIL8 = 0x88F0;
constexpr GLenum GL_FRAMEBUFFER_COMPLETE = 0x8CD5;
constexpr GLenum GL_PACK_ALIGNMENT = 0x0D05;
constexpr GLenum GL_UNPACK_ALIGNMENT = 0x0CF5;

#define FLEDGE_GL_FUNCS(X) \
    X(void, glClearColor, GLfloat, GLfloat, GLfloat, GLfloat) \
    X(void, glClear, GLbitfield) \
    X(void, glViewport, GLint, GLint, GLsizei, GLsizei) \
    X(void, glEnable, GLenum) \
    X(void, glDisable, GLenum) \
    X(void, glDepthFunc, GLenum) \
    X(void, glCullFace, GLenum) \
    X(void, glBlendFunc, GLenum, GLenum) \
    X(GLenum, glGetError) \
    X(const GLubyte*, glGetString, GLenum) \
    X(void, glPixelStorei, GLenum, GLint) \
    X(void, glReadPixels, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*) \
    X(void, glGenVertexArrays, GLsizei, GLuint*) \
    X(void, glBindVertexArray, GLuint) \
    X(void, glDeleteVertexArrays, GLsizei, const GLuint*) \
    X(void, glGenBuffers, GLsizei, GLuint*) \
    X(void, glBindBuffer, GLenum, GLuint) \
    X(void, glBufferData, GLenum, GLsizeiptr, const void*, GLenum) \
    X(void, glDeleteBuffers, GLsizei, const GLuint*) \
    X(void, glEnableVertexAttribArray, GLuint) \
    X(void, glVertexAttribPointer, GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) \
    X(GLuint, glCreateShader, GLenum) \
    X(void, glShaderSource, GLuint, GLsizei, const GLchar* const*, const GLint*) \
    X(void, glCompileShader, GLuint) \
    X(void, glGetShaderiv, GLuint, GLenum, GLint*) \
    X(void, glGetShaderInfoLog, GLuint, GLsizei, GLsizei*, GLchar*) \
    X(void, glDeleteShader, GLuint) \
    X(GLuint, glCreateProgram) \
    X(void, glAttachShader, GLuint, GLuint) \
    X(void, glLinkProgram, GLuint) \
    X(void, glGetProgramiv, GLuint, GLenum, GLint*) \
    X(void, glGetProgramInfoLog, GLuint, GLsizei, GLsizei*, GLchar*) \
    X(void, glDeleteProgram, GLuint) \
    X(void, glUseProgram, GLuint) \
    X(GLint, glGetUniformLocation, GLuint, const GLchar*) \
    X(void, glUniformMatrix4fv, GLint, GLsizei, GLboolean, const GLfloat*) \
    X(void, glUniform3fv, GLint, GLsizei, const GLfloat*) \
    X(void, glUniform1f, GLint, GLfloat) \
    X(void, glUniform1i, GLint, GLint) \
    X(void, glGenTextures, GLsizei, GLuint*) \
    X(void, glBindTexture, GLenum, GLuint) \
    X(void, glTexImage2D, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) \
    X(void, glTexSubImage2D, GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*) \
    X(void, glTexParameteri, GLenum, GLenum, GLint) \
    X(void, glDeleteTextures, GLsizei, const GLuint*) \
    X(void, glActiveTexture, GLenum) \
    X(void, glGenerateMipmap, GLenum) \
    X(void, glGenFramebuffers, GLsizei, GLuint*) \
    X(void, glBindFramebuffer, GLenum, GLuint) \
    X(void, glFramebufferTexture2D, GLenum, GLenum, GLenum, GLuint, GLint) \
    X(GLenum, glCheckFramebufferStatus, GLenum) \
    X(void, glDeleteFramebuffers, GLsizei, const GLuint*) \
    X(void, glGenRenderbuffers, GLsizei, GLuint*) \
    X(void, glBindRenderbuffer, GLenum, GLuint) \
    X(void, glRenderbufferStorage, GLenum, GLenum, GLsizei, GLsizei) \
    X(void, glFramebufferRenderbuffer, GLenum, GLenum, GLenum, GLuint) \
    X(void, glDeleteRenderbuffers, GLsizei, const GLuint*) \
    X(void, glDrawElements, GLenum, GLsizei, GLenum, const void*) \
    X(void, glDrawArrays, GLenum, GLint, GLsizei) \
    X(void, glCompressedTexImage2D, GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void*) \
    X(void, glGetIntegerv, GLenum, GLint*) \
    X(const GLubyte*, glGetStringi, GLenum, GLuint) \
    X(void, glDepthMask, GLboolean) \
    X(void, glTexImage3D, GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) \
    X(void, glFramebufferTextureLayer, GLenum, GLenum, GLuint, GLint, GLint) \
    X(void, glTexImage2DMultisample, GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLboolean) \
    X(void, glBlitFramebuffer, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum) \
    X(void, glDrawBuffers, GLsizei, const GLenum*) \
    X(void, glReadBuffer, GLenum) \
    X(GLuint, glGetUniformBlockIndex, GLuint, const GLchar*) \
    X(void, glUniformBlockBinding, GLuint, GLuint, GLuint) \
    X(void, glBindBufferBase, GLenum, GLuint, GLuint) \
    X(void, glBufferSubData, GLenum, GLintptr, GLsizeiptr, const void*) \
    X(void, glDrawElementsInstanced, GLenum, GLsizei, GLenum, const void*, GLsizei) \
    X(void, glPolygonMode, GLenum, GLenum) \
    X(void, glUniform4fv, GLint, GLsizei, const GLfloat*) \
    X(void, glVertexAttrib4f, GLuint, GLfloat, GLfloat, GLfloat, GLfloat) \
    X(void, glDisableVertexAttribArray, GLuint)

#define FLEDGE_GL_DECLARE(ret, name, ...) using PFN_##name = ret(APIENTRY*)(__VA_ARGS__); extern PFN_##name name;
FLEDGE_GL_FUNCS(FLEDGE_GL_DECLARE)
#undef FLEDGE_GL_DECLARE

// Returns the name of the first missing function, or nullptr on success.
const char* loadFunctions(void* (*getProc)(const char*));

} // namespace fledge::gl
