#include <gui/icons.h>
#include <stdint.h>
#include <config.h>

#define STB_IMAGE_IMPLEMENTATION
#include <imgui/stb_image.h>
#include <filesystem>
#include <utils/flog.h>

namespace icons {
    ImTextureID LOGO;
    ImTextureID PLAY;
    ImTextureID STOP;
    ImTextureID MENU;
    ImTextureID MUTED;
    ImTextureID UNMUTED;
    ImTextureID NORMAL_TUNING;
    ImTextureID CENTER_TUNING;

    GLuint fallbackTexture() {
        const uint8_t pixel[4] = { 255, 255, 255, 255 };
        GLuint texId = 0;
        glGenTextures(1, &texId);
        glBindTexture(GL_TEXTURE_2D, texId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        return texId;
    }

    GLuint loadTexture(std::string path) {
        int w = 0;
        int h = 0;
        int n = 0;
        stbi_uc* data = stbi_load(path.c_str(), &w, &h, &n, 4);
        if (!data || w <= 0 || h <= 0) {
            const char* reason = stbi_failure_reason();
            flog::error("Failed to load icon texture '{0}': {1}", path, reason ? reason : "unknown error");
            if (data) { stbi_image_free(data); }
            return fallbackTexture();
        }

        GLuint texId = 0;
        glGenTextures(1, &texId);
        if (texId == 0) {
            flog::error("Failed to allocate icon texture '{0}'", path);
            stbi_image_free(data);
            return fallbackTexture();
        }

        glBindTexture(GL_TEXTURE_2D, texId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        GLenum err = glGetError();
        stbi_image_free(data);

        if (err != GL_NO_ERROR) {
            flog::error("OpenGL error {0} while loading icon texture '{1}' ({2}x{3}, source channels={4})", (int)err, path, w, h, n);
            glDeleteTextures(1, &texId);
            return fallbackTexture();
        }

        return texId;
    }

    bool load(std::string resDir) {
        if (!std::filesystem::is_directory(resDir)) {
            flog::error("Invalid resource directory: {0}", resDir);
            return false;
        }

        LOGO = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/sdrpp.png");
        PLAY = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/play.png");
        STOP = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/stop.png");
        MENU = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/menu.png");
        MUTED = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/muted.png");
        UNMUTED = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/unmuted.png");
        NORMAL_TUNING = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/normal_tuning.png");
        CENTER_TUNING = (ImTextureID)(uintptr_t)loadTexture(resDir + "/icons/center_tuning.png");

        return true;
    }
}
