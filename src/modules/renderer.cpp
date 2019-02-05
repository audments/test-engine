#include "renderer.h"

#include <iostream>

#define STB_IMAGE_IMPLEMENTATION
#include <glad/glad.h>
#include <stb_image.h>
#include <wren.hpp>

#include "../assets.h"
#include "../engine.h"
#include "../scripting.h"

#define MAX_VERTICES 1024 * 32

static auto vertex_shader_source   = R"glsl(
    #version 300 es
    precision mediump float;

    in vec2 v_position;
    in vec2 v_uv;
    in vec4 v_color;

    out vec2 f_uv;
    out vec4 f_color;

    uniform vec2 u_screen_size;

    void main() {
        f_uv = v_uv;
        f_color = v_color;

        vec2 half_size = u_screen_size / 2.0;
        gl_Position = vec4((v_position - half_size) / half_size, 0.0, 1.0);
    }
    )glsl";
static auto fragment_shader_source = R"glsl(
    #version 300 es
    precision mediump float;

    in vec2 f_uv;
    in vec4 f_color;

    out vec4 out_color;

    uniform sampler2D u_texture;

    void main() {
        out_color = texture(u_texture, f_uv) * f_color;
    }
)glsl";

namespace Tea {
    std::shared_ptr<Texture> Texture::load(Asset& asset) {
        int w, h, channels_in_file;

        stbi_set_flip_vertically_on_load(true);
        uint8_t* c_data =
            stbi_load_from_memory(&asset.get_data().front(), asset.get_data().size(), &w, &h, &channels_in_file, 4);
        if (c_data == nullptr) {
            std::cerr << "Error loading image." << std::endl;
        }

        std::vector<uint8_t> data(c_data, c_data + (w * h * 4));

        stbi_image_free(c_data);
        return Texture::create(data, w, h);
    }

    std::shared_ptr<Texture> Texture::create(std::vector<uint8_t>& data, uint32_t width, uint32_t height) {
        std::shared_ptr<Texture> tex(new Texture());

        glGenTextures(1, &tex->tex);
        glBindTexture(GL_TEXTURE_2D, tex->tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, &data.front());

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        tex->width  = width;
        tex->height = height;
        return tex;
    }

    Texture::~Texture() { glDeleteTextures(1, &this->tex); }

    uint32_t Texture::get_width() { return this->width; }
    uint32_t Texture::get_height() { return this->height; }
    GLuint   Texture::get_gl_texture() { return this->tex; }

    bool get_shader_compile_error(GLuint shader, std::string& error) {
        GLint status;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &status);

        if (status) return false;

        GLint length;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);

        error.resize(length);
        glGetShaderInfoLog(shader, length, nullptr, &error.front());
        return true;
    }

    Renderer::Renderer(Engine& engine): Module(engine) { this->init(); }

    Renderer::~Renderer() {
        glDeleteBuffers(1, &this->vbo);
        glDetachShader(this->program, this->vertex_shader);
        glDetachShader(this->program, this->fragment_shader);
        glDeleteProgram(this->program);
        glDeleteShader(this->vertex_shader);
        glDeleteShader(this->fragment_shader);
    }

    void Renderer::init() {
        std::cout << "Initializing render backend:" << std::endl;
        std::cout << " - GL " << glGetString(GL_VERSION) << std::endl;
        std::cout << " - Vendor: " << glGetString(GL_VENDOR) << std::endl;
        std::cout << " - Renderer: " << glGetString(GL_RENDERER) << std::endl;

        glGenBuffers(1, &this->vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);

        this->vertex_shader   = glCreateShader(GL_VERTEX_SHADER);
        this->fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);

        glShaderSource(this->vertex_shader, 1, &vertex_shader_source, nullptr);
        glShaderSource(this->fragment_shader, 1, &fragment_shader_source, nullptr);

        glCompileShader(this->vertex_shader);
        glCompileShader(this->fragment_shader);

        std::string error;
        if (get_shader_compile_error(this->vertex_shader, error)) {
            std::cerr << "Error compiling vertex shader: " << error << std::endl;
            exit(1);
        }

        if (get_shader_compile_error(this->fragment_shader, error)) {
            std::cerr << "Error compiling fragment shader: " << error << std::endl;
            exit(1);
        }

        this->program = glCreateProgram();
        glAttachShader(this->program, this->vertex_shader);
        glAttachShader(this->program, this->fragment_shader);
        glLinkProgram(this->program);
        glUseProgram(this->program);

        GLint posAttrib   = glGetAttribLocation(this->program, "v_position");
        GLint uvAttrib    = glGetAttribLocation(this->program, "v_uv");
        GLint colorAttrib = glGetAttribLocation(this->program, "v_color");

        glVertexAttribPointer(
            posAttrib, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, x)));

        glVertexAttribPointer(
            uvAttrib, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, u)));

        glVertexAttribPointer(colorAttrib,
                              4,
                              GL_UNSIGNED_BYTE,
                              GL_TRUE,
                              sizeof(Vertex),
                              reinterpret_cast<const void*>(offsetof(Vertex, rgba)));

        glEnableVertexAttribArray(posAttrib);
        glEnableVertexAttribArray(uvAttrib);
        glEnableVertexAttribArray(colorAttrib);

        this->screen_size_uniform = glGetUniformLocation(this->program, "u_screen_size");

        std::vector<uint8_t> pixel_data = {255, 255, 255, 255};
        this->pixel_texture             = Texture::create(pixel_data, 1, 1);
        this->current_texture           = this->pixel_texture;
    }

    void Renderer::bind(Tea::Scripting& s) {
        s.bind("static tea/graphics::Graphics::rect(_,_,_,_,_)", [](Tea::Scripting& s) {
            uint32_t color = static_cast<uint32_t>(s.slot(5).as_num());
            s.get_engine().get_module<Renderer>()->rect(
                s.slot(1).as_num(),
                s.slot(2).as_num(),
                s.slot(3).as_num(),
                s.slot(4).as_num(),
                static_cast<uint8_t>(color >> 24),
                static_cast<uint8_t>((color >> 16) & 0xff),
                static_cast<uint8_t>((color >> 8) & 0xff),
                static_cast<uint8_t>(color & 0xff)
            );
        });

        s.bind("static tea/graphics::Graphics::setTexture(_)", [](Tea::Scripting& s) {
            s.get_engine().get_module<Renderer>()->set_texture(s.slot(1).get_native_type<std::shared_ptr<Texture>>());
        });

        s.bind("static tea/graphics::Texture::load(_)", [](Tea::Scripting& s) {
            auto filename = s.slot(1).as_str();

            auto file = s.get_engine().get_assets().find_asset(filename);
            if (file == nullptr) {
                s.error("Could not find file.");
                return;
            }

            s.slot(0).set_native_type(Texture::load(*file), 0);
        });

        s.bind("tea/graphics::Texture::width", [](Tea::Scripting& s) {
            auto w = s.slot(0).get_native_type<std::shared_ptr<Texture>>()->get_width();
            s.slot(0).set_num(w);
        });

        s.bind("tea/graphics::Texture::height", [](Tea::Scripting& s) {
            auto w = s.slot(0).get_native_type<std::shared_ptr<Texture>>()->get_height();
            s.slot(0).set_num(w);
        });
    }

    void Renderer::begin() {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    void Renderer::push_vertex(Vertex vtx) {
        if (this->vertices.size() >= MAX_VERTICES) flush();
        this->vertices.push_back(vtx);
    }

    void Renderer::flush() {
        auto& platform = this->engine.get_platform();
        glUniform2f(this->screen_size_uniform, platform.get_window_width(), platform.get_window_height());

        glBindTexture(GL_TEXTURE_2D, this->current_texture->get_gl_texture());

        glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * vertices.size(), &vertices.front(), GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, vertices.size());

        vertices.resize(0);
    }

    void Renderer::set_texture(std::shared_ptr<Texture>& tex) {
        if (this->current_texture->get_gl_texture() != tex->get_gl_texture() && !this->vertices.empty()) flush();
        this->current_texture = tex;
    }

    void Renderer::rect(float x, float y, float w, float h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        this->push_vertex({x, y, 0, 0, {r, g, b, a}});
        this->push_vertex({x + w, y, 1, 0, {r, g, b, a}});
        this->push_vertex({x, y + h, 0, 1, {r, g, b, a}});
        this->push_vertex({x, y + h, 0, 1, {r, g, b, a}});
        this->push_vertex({x + w, y, 1, 0, {r, g, b, a}});
        this->push_vertex({x + w, y + h, 1, 1, {r, g, b, a}});
    }

    void Renderer::pre_update() { this->begin(); }

    void Renderer::post_update() { this->flush(); }
}