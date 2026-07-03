#include "renderer.hpp"

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "server.hpp"
#include "view.hpp"
#include "wlr.hpp"

namespace fenriz::renderer {

    namespace {

        GLuint g_program = 0;
        GLint u_size = -1, u_radius = -1, u_bg = -1;
        GLint a_pos = -1, a_local = -1;

        const char* VERT = "attribute vec2 a_pos;\n"
                           "attribute vec2 a_local;\n"
                           "varying vec2 v_local;\n"
                           "void main() {\n"
                           "  v_local = a_local;\n"
                           "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
                           "}\n";

        // Draw the background color only OUTSIDE the rounded rectangle (i.e. in the
        // clipped-off corners); discard elsewhere so the window/border stays untouched.
        const char* FRAG = "precision mediump float;\n"
                           "varying vec2 v_local;\n"
                           "uniform vec2 u_size;\n"
                           "uniform float u_radius;\n"
                           "uniform vec4 u_bg;\n"
                           "void main() {\n"
                           "  vec2 p = v_local * u_size;\n"
                           "  vec2 b = u_size * 0.5;\n"
                           "  vec2 q = abs(p - b) - (b - vec2(u_radius));\n"
                           "  float d = length(max(q, vec2(0.0))) - u_radius;\n"
                           "  float a = smoothstep(-0.7, 0.7, d);\n"
                           "  if (a <= 0.0) discard;\n"
                           "  gl_FragColor = vec4(u_bg.rgb, u_bg.a * a);\n"
                           "}\n";

        GLuint compile(GLenum type, const char* src) {
            GLuint sh = glCreateShader(type);
            glShaderSource(sh, 1, &src, nullptr);
            glCompileShader(sh);
            GLint ok = 0;
            glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
            if (!ok) {
                char log[512];
                glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
                wlr_log(WLR_ERROR, "fenriz: shader compile failed: %s", log);
            }
            return sh;
        }

        // Compile+link the rounding program once, on the current GL context.
        bool ensure_program() {
            if (g_program)
                return true;
            GLuint vs = compile(GL_VERTEX_SHADER, VERT);
            GLuint fs = compile(GL_FRAGMENT_SHADER, FRAG);
            GLuint prog = glCreateProgram();
            glAttachShader(prog, vs);
            glAttachShader(prog, fs);
            glLinkProgram(prog);
            glDeleteShader(vs);
            glDeleteShader(fs);
            GLint ok = 0;
            glGetProgramiv(prog, GL_LINK_STATUS, &ok);
            if (!ok) {
                char log[512];
                glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
                wlr_log(WLR_ERROR, "fenriz: program link failed: %s", log);
                return false;
            }
            g_program = prog;
            u_size = glGetUniformLocation(prog, "u_size");
            u_radius = glGetUniformLocation(prog, "u_radius");
            u_bg = glGetUniformLocation(prog, "u_bg");
            a_pos = glGetAttribLocation(prog, "a_pos");
            a_local = glGetAttribLocation(prog, "a_local");
            return true;
        }

    } // namespace

    void round_corners(Server& server, wlr_buffer* buffer, int W, int H, const float bg[4]) {
        if (!buffer || server.config.rounding <= 0)
            return;
        if (!wlr_renderer_is_gles2(server.renderer))
            return;

        wlr_egl* egl = wlr_gles2_renderer_get_egl(server.renderer);
        EGLDisplay dpy = wlr_egl_get_display(egl);
        EGLContext ctx = wlr_egl_get_context(egl);
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);

        GLuint fbo = wlr_gles2_renderer_get_buffer_fbo(server.renderer, buffer);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, W, H);

        if (ensure_program()) {
            glUseProgram(g_program);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glUniform4fv(u_bg, 1, bg);
            glUniform1f(u_radius, (float)server.config.rounding);

            for (View* view : server.views) {
                if (!view->mapped)
                    continue;
                const View::Box& b = view->box;
                // Map the box (output-local pixels) to NDC. Local coords run 0..1 across
                // the quad; the SDF is corner-symmetric so vertical orientation is moot.
                float x0 = 2.0f * b.x / W - 1.0f;
                float x1 = 2.0f * (b.x + b.width) / W - 1.0f;
                float y0 = 1.0f - 2.0f * b.y / H;
                float y1 = 1.0f - 2.0f * (b.y + b.height) / H;
                const GLfloat pos[] = { x0, y0, x1, y0, x1, y1, x0, y0, x1, y1, x0, y1 };
                const GLfloat loc[] = { 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1 };

                glUniform2f(u_size, (float)b.width, (float)b.height);
                glVertexAttribPointer(a_pos, 2, GL_FLOAT, GL_FALSE, 0, pos);
                glEnableVertexAttribArray(a_pos);
                glVertexAttribPointer(a_local, 2, GL_FLOAT, GL_FALSE, 0, loc);
                glEnableVertexAttribArray(a_local);
                glDrawArrays(GL_TRIANGLES, 0, 6);
            }
        }

        glFlush();
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }

} // namespace fenriz::renderer
