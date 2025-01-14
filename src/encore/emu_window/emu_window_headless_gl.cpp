// Copyright 2024 Encore Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/settings.h"
#include "emu_window_headless_gl.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_opengl/gl_state.h"

namespace Headless {

class HeadlessSharedContext : public Frontend::GraphicsContext {
public:
    explicit HeadlessSharedContext(const GLCallbackInterface& gl_interface_)
        : gl_interface(gl_interface_) {
        gl_context = gl_interface.RequestGLContext();
        ASSERT(gl_context);
    }

    ~HeadlessSharedContext() override {
        gl_interface.ReleaseGLContext(gl_context);
    }

    void MakeCurrent() override {
        gl_interface.ActivateGLContext(gl_context);
    }

private:
    const GLCallbackInterface gl_interface;
    void* gl_context;
};

} // namespace Headless

using namespace Headless;

EmuWindow_Headless_GL::EmuWindow_Headless_GL(Core::System& system,
                                             GLCallbackInterface& gl_interface_)
    : EmuWindow_Headless(system), gl_interface(gl_interface_) {
    context = std::make_unique<HeadlessSharedContext>(gl_interface);
    ReloadConfig();
    const auto& layout = GetFramebufferLayout();
    width = layout.width;
    height = layout.height;
    ASSERT(gladLoadGLLoader(static_cast<GLADloadproc>(gl_interface.GetGLProcAddress)));
    final_texture_fbo.Create();
    final_texture_pbo.Create();
    ResetGLTexture();
}

EmuWindow_Headless_GL::~EmuWindow_Headless_GL() {
    context->MakeCurrent();
    final_texture.Release();
    final_texture_fbo.Release();
    final_texture_pbo.Release();
}

void EmuWindow_Headless_GL::ResetGLTexture() {
    // release and reallocate the texture
    final_texture.Release();
    final_texture.Create();
    final_texture.Allocate(GL_TEXTURE_2D, 1, GL_RGBA8, width, height, 0);

    // bind the texture to our FBO
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, final_texture_fbo.handle);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           final_texture.handle, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
}

void EmuWindow_Headless_GL::Present() {
    // get old state, so we can restore it afterwards
    const auto& prev_state = OpenGL::OpenGLState::GetCurState();

    // check if video dimensions have changed
    // if they have, we need to recreate the texture
    const auto& layout = GetFramebufferLayout();
    if (width != layout.width || height != layout.height) {
        width = layout.width;
        height = layout.height;
        ResetGLTexture();
    }

    // disable the scissor test while we're presenting
    glDisable(GL_SCISSOR_TEST);

    // present to our FBO
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, final_texture_fbo.handle);
    system.GPU().Renderer().TryPresent(0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

    // start async readback of our FBO
    glBindFramebuffer(GL_READ_FRAMEBUFFER, final_texture_fbo.handle);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, final_texture_pbo.handle);
    glBufferData(GL_PIXEL_PACK_BUFFER, width * height * sizeof(u32), nullptr, GL_STREAM_READ);
    glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, static_cast<void*>(0));
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    // restore the old state
    if (prev_state.scissor.enabled) {
        glEnable(GL_SCISSOR_TEST);
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_state.draw.read_framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_state.draw.draw_framebuffer);
}

u32 EmuWindow_Headless_GL::GetGLTexture() const {
    return final_texture.handle;
}

std::pair<u32, u32> EmuWindow_Headless_GL::GetVideoBufferDimensions() const {
    return std::make_pair(width, height);
}

void EmuWindow_Headless_GL::ReadFrameBuffer(u32* dest_buffer) const {
    glBindBuffer(GL_PIXEL_PACK_BUFFER, final_texture_pbo.handle);
    const auto p = static_cast<const u32*>(glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY));
    if (p) {
        // FBOs render upside down, so flip vertically to counteract that
        dest_buffer += width * (height - 1);
        const int w = width;
        const int h = height;
        for (int i = 0; i < h; i++) {
            std::memcpy(&dest_buffer[-i * w], &p[i * w], width * sizeof(u32));
        }

        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}

void EmuWindow_Headless_GL::ReloadConfig() {
    // in case of a custom layout, which case we need to do some more work for the correct layout
    if (Settings::values.custom_layout.GetValue()) {
        auto layout = Layout::CustomFrameLayout(1, 1, Settings::values.swap_screen.GetValue());
        const auto left = std::min(layout.top_screen.left, layout.bottom_screen.left);
        const auto right = std::max(layout.top_screen.right, layout.bottom_screen.right);
        const auto bottom = std::min(layout.top_screen.bottom, layout.bottom_screen.bottom);
        const auto top = std::max(layout.top_screen.top, layout.bottom_screen.top);
        layout.width = right - left;
        layout.height = top - bottom;
        if (layout.is_rotated) {
            std::swap(layout.width, layout.height);
        }
        UpdateCurrentFramebufferLayout(std::max(layout.width, 1u), std::max(layout.height, 1u),
                                       false);
    } else {
        // will be set back to the minimum size
        UpdateCurrentFramebufferLayout(1, 1, false);
    }

    const auto& layout = GetFramebufferLayout();
    const auto scale_factor = Settings::values.resolution_factor.GetValue();
    UpdateCurrentFramebufferLayout(layout.width * scale_factor, layout.height * scale_factor,
                                   false);
}

std::unique_ptr<Frontend::GraphicsContext> EmuWindow_Headless_GL::CreateSharedContext() const {
    auto ret = std::make_unique<HeadlessSharedContext>(gl_interface);
    context->MakeCurrent();
    return ret;
}

void EmuWindow_Headless_GL::MakeCurrent() {
    context->MakeCurrent();
}
