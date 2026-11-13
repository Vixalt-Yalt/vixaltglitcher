#ifndef VIX_GL_LOADER_H
#define VIX_GL_LOADER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef void GLvoid;
typedef signed char GLbyte;
typedef short GLshort;
typedef int GLint;
typedef unsigned char GLubyte;
typedef unsigned short GLushort;
typedef unsigned int GLuint;
typedef int GLsizei;
typedef float GLfloat;
typedef double GLdouble;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;

#define GL_FALSE                          0
#define GL_TRUE                           1
#define GL_TRIANGLES                      0x0004
#define GL_SRC_ALPHA                      0x0302
#define GL_ONE_MINUS_SRC_ALPHA            0x0303
#define GL_FRONT_AND_BACK                 0x0408
#define GL_DEPTH_TEST                     0x0B71
#define GL_STENCIL_TEST                   0x0B90
#define GL_CULL_FACE                      0x0B44
#define GL_BLEND                          0x0BE2
#define GL_SCISSOR_TEST                   0x0C11
#define GL_UNPACK_ALIGNMENT               0x0CF5
#define GL_PACK_ALIGNMENT                 0x0D05
#define GL_TEXTURE_2D                     0x0DE1
#define GL_UNSIGNED_BYTE                  0x1401
#define GL_FLOAT                          0x1406
#define GL_RED                            0x1903
#define GL_ALPHA                          0x1906
#define GL_RGB                            0x1907
#define GL_RGBA                           0x1908
#define GL_LUMINANCE                      0x1909
#define GL_VENDOR                         0x1F00
#define GL_RENDERER                       0x1F01
#define GL_VERSION                        0x1F02
#define GL_LINEAR                         0x2601
#define GL_TEXTURE_MAG_FILTER             0x2800
#define GL_TEXTURE_MIN_FILTER             0x2801
#define GL_TEXTURE_WRAP_S                 0x2802
#define GL_TEXTURE_WRAP_T                 0x2803
#define GL_CLAMP_TO_EDGE                  0x812F
#define GL_R8                             0x8229
#define GL_RGBA8                          0x8058
#define GL_TEXTURE0                       0x84C0
#define GL_ARRAY_BUFFER                   0x8892
#define GL_DYNAMIC_DRAW                   0x88E8
#define GL_FRAGMENT_SHADER                0x8B30
#define GL_VERTEX_SHADER                  0x8B31
#define GL_COMPILE_STATUS                 0x8B81
#define GL_LINK_STATUS                    0x8B82
#define GL_FRAMEBUFFER                    0x8D40
#define GL_COLOR_BUFFER_BIT               0x00004000

typedef struct VixGL {
    bool loaded;
    bool is_core_profile;

    const GLubyte *(*GetString)(GLenum name);
    void (*Viewport)(GLint x, GLint y, GLsizei width, GLsizei height);
    void (*Scissor)(GLint x, GLint y, GLsizei width, GLsizei height);
    void (*ClearColor)(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
    void (*Clear)(GLbitfield mask);
    void (*Enable)(GLenum cap);
    void (*Disable)(GLenum cap);
    void (*BlendFunc)(GLenum sfactor, GLenum dfactor);
    void (*PixelStorei)(GLenum pname, GLint param);

    void (*GenTextures)(GLsizei n, GLuint *textures);
    void (*DeleteTextures)(GLsizei n, const GLuint *textures);
    void (*BindTexture)(GLenum target, GLuint texture);
    void (*ActiveTexture)(GLenum texture);
    void (*TexParameteri)(GLenum target, GLenum pname, GLint param);
    void (*TexImage2D)(GLenum target, GLint level, GLint internalFormat,
                       GLsizei width, GLsizei height, GLint border,
                       GLenum format, GLenum type, const void *pixels);
    void (*TexSubImage2D)(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                          GLsizei width, GLsizei height, GLenum format,
                          GLenum type, const void *pixels);

    GLuint (*CreateShader)(GLenum shaderType);
    void (*ShaderSource)(GLuint shader, GLsizei count, const GLchar **string, const GLint *length);
    void (*CompileShader)(GLuint shader);
    void (*GetShaderiv)(GLuint shader, GLenum pname, GLint *params);
    void (*GetShaderInfoLog)(GLuint shader, GLsizei maxLength, GLsizei *length, GLchar *infoLog);
    void (*DeleteShader)(GLuint shader);

    GLuint (*CreateProgram)(void);
    void (*AttachShader)(GLuint program, GLuint shader);
    void (*BindAttribLocation)(GLuint program, GLuint index, const GLchar *name);
    void (*LinkProgram)(GLuint program);
    void (*GetProgramiv)(GLuint program, GLenum pname, GLint *params);
    void (*GetProgramInfoLog)(GLuint program, GLsizei maxLength, GLsizei *length, GLchar *infoLog);
    void (*UseProgram)(GLuint program);
    void (*DeleteProgram)(GLuint program);

    GLint (*GetUniformLocation)(GLuint program, const GLchar *name);
    void (*Uniform1i)(GLint location, GLint v0);
    void (*Uniform1f)(GLint location, GLfloat v0);
    void (*Uniform2f)(GLint location, GLfloat v0, GLfloat v1);
    void (*Uniform4f)(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);

    void (*GenBuffers)(GLsizei n, GLuint *buffers);
    void (*DeleteBuffers)(GLsizei n, const GLuint *buffers);
    void (*BindBuffer)(GLenum target, GLuint buffer);
    void (*BufferData)(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
    void (*BufferSubData)(GLenum target, GLintptr offset, GLsizeiptr size, const void *data);

    void (*GenVertexArrays)(GLsizei n, GLuint *arrays);
    void (*DeleteVertexArrays)(GLsizei n, const GLuint *arrays);
    void (*BindVertexArray)(GLuint array);

    void (*EnableVertexAttribArray)(GLuint index);
    void (*DisableVertexAttribArray)(GLuint index);
    void (*VertexAttribPointer)(GLuint index, GLint size, GLenum type,
                                GLboolean normalized, GLsizei stride, const void *pointer);
    void (*DrawArrays)(GLenum mode, GLint first, GLsizei count);
    void (*BindFramebuffer)(GLenum target, GLuint framebuffer);
} VixGL;

extern VixGL gl;

bool vix_gl_load(void *(*get_proc)(const char *name), bool is_core_profile);

#endif /* VIX_GL_LOADER_H */
