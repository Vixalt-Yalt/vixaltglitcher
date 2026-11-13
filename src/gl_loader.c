#include "gl_loader.h"
#include <string.h>

VixGL gl;

#define LOAD_REQ(field, sym) do { \
    *(void **)(&gl.field) = get_proc(sym); \
    if (!gl.field) return false; \
} while (0)

#define LOAD_OPT(field, sym) do { \
    *(void **)(&gl.field) = get_proc(sym); \
} while (0)

bool vix_gl_load(void *(*get_proc)(const char *name), bool is_core_profile) {
    memset(&gl, 0, sizeof(gl));
    if (!get_proc) return false;

    gl.is_core_profile = is_core_profile;

    LOAD_REQ(GetString, "glGetString");
    LOAD_REQ(Viewport, "glViewport");
    LOAD_REQ(Scissor, "glScissor");
    LOAD_REQ(ClearColor, "glClearColor");
    LOAD_REQ(Clear, "glClear");
    LOAD_REQ(Enable, "glEnable");
    LOAD_REQ(Disable, "glDisable");
    LOAD_REQ(BlendFunc, "glBlendFunc");
    LOAD_REQ(PixelStorei, "glPixelStorei");

    LOAD_REQ(GenTextures, "glGenTextures");
    LOAD_REQ(DeleteTextures, "glDeleteTextures");
    LOAD_REQ(BindTexture, "glBindTexture");
    LOAD_REQ(ActiveTexture, "glActiveTexture");
    LOAD_REQ(TexParameteri, "glTexParameteri");
    LOAD_REQ(TexImage2D, "glTexImage2D");
    LOAD_REQ(TexSubImage2D, "glTexSubImage2D");

    LOAD_REQ(CreateShader, "glCreateShader");
    LOAD_REQ(ShaderSource, "glShaderSource");
    LOAD_REQ(CompileShader, "glCompileShader");
    LOAD_REQ(GetShaderiv, "glGetShaderiv");
    LOAD_REQ(GetShaderInfoLog, "glGetShaderInfoLog");
    LOAD_REQ(DeleteShader, "glDeleteShader");

    LOAD_REQ(CreateProgram, "glCreateProgram");
    LOAD_REQ(AttachShader, "glAttachShader");
    LOAD_REQ(BindAttribLocation, "glBindAttribLocation");
    LOAD_REQ(LinkProgram, "glLinkProgram");
    LOAD_REQ(GetProgramiv, "glGetProgramiv");
    LOAD_REQ(GetProgramInfoLog, "glGetProgramInfoLog");
    LOAD_REQ(UseProgram, "glUseProgram");
    LOAD_REQ(DeleteProgram, "glDeleteProgram");

    LOAD_REQ(GetUniformLocation, "glGetUniformLocation");
    LOAD_REQ(Uniform1i, "glUniform1i");
    LOAD_REQ(Uniform1f, "glUniform1f");
    LOAD_REQ(Uniform2f, "glUniform2f");
    LOAD_REQ(Uniform4f, "glUniform4f");

    LOAD_REQ(GenBuffers, "glGenBuffers");
    LOAD_REQ(DeleteBuffers, "glDeleteBuffers");
    LOAD_REQ(BindBuffer, "glBindBuffer");
    LOAD_REQ(BufferData, "glBufferData");
    LOAD_REQ(BufferSubData, "glBufferSubData");

    LOAD_OPT(GenVertexArrays, "glGenVertexArrays");
    LOAD_OPT(DeleteVertexArrays, "glDeleteVertexArrays");
    LOAD_OPT(BindVertexArray, "glBindVertexArray");

    if (is_core_profile && (!gl.GenVertexArrays || !gl.BindVertexArray)) {
        return false;
    }

    LOAD_REQ(EnableVertexAttribArray, "glEnableVertexAttribArray");
    LOAD_REQ(DisableVertexAttribArray, "glDisableVertexAttribArray");
    LOAD_REQ(VertexAttribPointer, "glVertexAttribPointer");
    LOAD_REQ(DrawArrays, "glDrawArrays");
    LOAD_OPT(BindFramebuffer, "glBindFramebuffer");

    gl.loaded = true;
    return true;
}
