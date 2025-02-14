#include "libslic3r/libslic3r.h"

#include "3DBed.hpp"

#include "libslic3r/Polygon.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Geometry.hpp"

#include "GUI_App.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "GLCanvas3D.hpp"
#include "3DScene.hpp"

#include <GL/glew.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>

static const float GROUND_Z = -0.02f;

namespace Slic3r {
namespace GUI {

bool GeometryBuffer::set_from_triangles(const Polygons& triangles, float z, bool generate_tex_coords)
{
    m_vertices.clear();
    unsigned int v_size = 3 * (unsigned int)triangles.size();

    if (v_size == 0)
        return false;

    m_vertices = std::vector<Vertex>(v_size, Vertex());

    float min_x = unscale<float>(triangles[0].points[0](0));
    float min_y = unscale<float>(triangles[0].points[0](1));
    float max_x = min_x;
    float max_y = min_y;

    unsigned int v_count = 0;
    for (const Polygon& t : triangles) {
        for (unsigned int i = 0; i < 3; ++i) {
            Vertex& v = m_vertices[v_count];

            const Point& p = t.points[i];
            float x = unscale<float>(p(0));
            float y = unscale<float>(p(1));

            v.position[0] = x;
            v.position[1] = y;
            v.position[2] = z;

            if (generate_tex_coords) {
                v.tex_coords[0] = x;
                v.tex_coords[1] = y;

                min_x = std::min(min_x, x);
                max_x = std::max(max_x, x);
                min_y = std::min(min_y, y);
                max_y = std::max(max_y, y);
            }

            ++v_count;
        }
    }

    if (generate_tex_coords) {
        float size_x = max_x - min_x;
        float size_y = max_y - min_y;

        if ((size_x != 0.0f) && (size_y != 0.0f)) {
            float inv_size_x = 1.0f / size_x;
            float inv_size_y = -1.0f / size_y;
            for (Vertex& v : m_vertices) {
                v.tex_coords[0] = (v.tex_coords[0] - min_x) * inv_size_x;
                v.tex_coords[1] = (v.tex_coords[1] - min_y) * inv_size_y;
            }
        }
    }

    return true;
}

bool GeometryBuffer::set_from_lines(const Lines& lines, float z)
{
    m_vertices.clear();

    unsigned int v_size = 2 * (unsigned int)lines.size();
    if (v_size == 0)
        return false;

    m_vertices = std::vector<Vertex>(v_size, Vertex());

    unsigned int v_count = 0;
    for (const Line& l : lines) {
        Vertex& v1 = m_vertices[v_count];
        v1.position[0] = unscale<float>(l.a(0));
        v1.position[1] = unscale<float>(l.a(1));
        v1.position[2] = z;
        ++v_count;

        Vertex& v2 = m_vertices[v_count];
        v2.position[0] = unscale<float>(l.b(0));
        v2.position[1] = unscale<float>(l.b(1));
        v2.position[2] = z;
        ++v_count;
    }

    return true;
}

const float* GeometryBuffer::get_vertices_data() const
{
    return (m_vertices.size() > 0) ? (const float*)m_vertices.data() : nullptr;
}

const float Bed3D::Axes::DefaultStemRadius = 0.5f;
const float Bed3D::Axes::DefaultStemLength = 25.0f;
const float Bed3D::Axes::DefaultTipRadius = 2.5f * Bed3D::Axes::DefaultStemRadius;
const float Bed3D::Axes::DefaultTipLength = 5.0f;

void Bed3D::Axes::set_stem_length(float length)
{
    m_stem_length = length;
    m_arrow.reset();
}

void Bed3D::Axes::render() const
{
    auto render_axis = [this](const Transform3f& transform) {
        glsafe(::glPushMatrix());
        glsafe(::glMultMatrixf(transform.data()));
        m_arrow.render();
        glsafe(::glPopMatrix());
    };

    m_arrow.init_from(stilized_arrow(16, DefaultTipRadius, DefaultTipLength, DefaultStemRadius, m_stem_length));

    GLShaderProgram* shader = wxGetApp().get_shader("gouraud_light");
    if (shader == nullptr)
        return;

    glsafe(::glEnable(GL_DEPTH_TEST));

    shader->start_using();

    // x axis
    std::array<float, 4> color = { 0.75f, 0.0f, 0.0f, 1.0f };
    shader->set_uniform("uniform_color", color);
    render_axis(Geometry::assemble_transform(m_origin, { 0.0, 0.5 * M_PI, 0.0f }).cast<float>());

    // y axis
    color = { 0.0f, 0.75f, 0.0f, 1.0f };
    shader->set_uniform("uniform_color", color);
    render_axis(Geometry::assemble_transform(m_origin, { -0.5 * M_PI, 0.0, 0.0f }).cast<float>());

    // z axis
    color = { 0.0f, 0.0f, 0.75f, 1.0f };
    shader->set_uniform("uniform_color", color);
    render_axis(Geometry::assemble_transform(m_origin).cast<float>());

    shader->stop_using();

    glsafe(::glDisable(GL_DEPTH_TEST));
}

Bed3D::Bed3D()
    : m_type(Custom)
    , m_vbo_id(0)
    , m_scale_factor(1.0f)
{
    {
        //try to load splashscreen from ui file
        boost::property_tree::ptree tree_colors;
        boost::filesystem::path path_colors = boost::filesystem::path(resources_dir()) / "ui_layout" / "colors.ini";
        try {
            boost::nowide::ifstream ifs;
            ifs.imbue(boost::locale::generator()("en_US.UTF-8"));
            ifs.open(path_colors.string());
            boost::property_tree::read_ini(ifs, tree_colors);

            std::string color_code = tree_colors.get<std::string>("Gui_plater");
            if (color_code.length() > 5) {
                wxColour color;
                color.Set((color_code[0] == '#') ? color_code : ("#" + color_code));
                this->m_model_color[0] = color.Red() / 256.f;
                this->m_model_color[1] = color.Green() / 256.f;
                this->m_model_color[2] = color.Blue() / 256.f;
            }
            color_code = tree_colors.get<std::string>("Gui_plater_grid");
            if (color_code.length() > 5) {
                wxColour color;
                color.Set((color_code[0] == '#') ? color_code : ("#" + color_code));
                this->m_grid_color[0] = color.Red() / 256.f;
                this->m_grid_color[1] = color.Green() / 256.f;
                this->m_grid_color[2] = color.Blue() / 256.f;
            }
        }
        catch (const std::ifstream::failure& err) {
            trace(1, (std::string("The color file cannot be loaded. Reason: ") + err.what(), path_colors.string()).c_str());
        }
        catch (const std::runtime_error& err) {
            trace(1, (std::string("Failed loading the color file. Reason: ") + err.what(), path_colors.string()).c_str());
        }
    }

}

bool Bed3D::set_shape(const Pointfs& shape, const std::string& custom_texture, const std::string& custom_model, bool force_as_custom)
{
    auto check_texture = [](const std::string& texture) {
        return !texture.empty() && (boost::algorithm::iends_with(texture, ".png") || boost::algorithm::iends_with(texture, ".svg")) && boost::filesystem::exists(texture);
    };

    auto check_model = [](const std::string& model) {
        return !model.empty() && boost::algorithm::iends_with(model, ".stl") && boost::filesystem::exists(model);
    };

    EType type;
    std::string model;
    std::string texture;
    if (force_as_custom)
        type = Custom;
    else {
        auto [new_type, system_model, system_texture] = detect_type(shape);
        type = new_type;
        model = system_model;
        texture = system_texture;
    }

    std::string texture_filename = custom_texture.empty() ? texture : custom_texture;
    if (!check_texture(texture_filename))
        texture_filename.clear();

    std::string model_filename = custom_model.empty() ? model : custom_model;
    if (!check_model(model_filename))
        model_filename.clear();

    if (m_shape == shape && m_type == type && m_texture_filename == texture_filename && m_model_filename == model_filename)
        // No change, no need to update the UI.
        return false;

    m_shape = shape;
    m_texture_filename = texture_filename;
    m_model_filename = model_filename;
    m_type = type;

    calc_bounding_boxes();

    ExPolygon poly;
    for (const Vec2d& p : m_shape) {
        poly.contour.append(Point(scale_(p(0)), scale_(p(1))));
    }

    calc_triangles(poly);

    const BoundingBox& bed_bbox = poly.contour.bounding_box();
    calc_gridlines(poly, bed_bbox);

    m_polygon = offset_ex(poly.contour, (float)bed_bbox.radius() * 1.7f, jtRound, scale_(0.5))[0].contour;

    reset();
    m_texture.reset();
    m_model.reset();

    // Set the origin and size for rendering the coordinate system axes.
    m_axes.set_origin({ 0.0, 0.0, static_cast<double>(GROUND_Z) });
    m_axes.set_stem_length(0.1f * static_cast<float>(m_bounding_box.max_size()));

    // Let the calee to update the UI.
    return true;
}

bool Bed3D::contains(const Point& point) const
{
    return m_polygon.contains(point);
}

Point Bed3D::point_projection(const Point& point) const
{
    return m_polygon.point_projection(point);
}

void Bed3D::render(GLCanvas3D& canvas, bool bottom, float scale_factor,
                   bool show_axes, bool show_texture) const
{
    m_scale_factor = scale_factor;

    if (show_axes)
        render_axes();

    glsafe(::glEnable(GL_DEPTH_TEST));

    switch (m_type)
    {
    case System: { render_system(canvas, bottom, show_texture); break; }
    default:
    case Custom: { render_custom(canvas, bottom, show_texture); break; }
    }

    glsafe(::glDisable(GL_DEPTH_TEST));
}

void Bed3D::calc_bounding_boxes() const
{
    m_bounding_box = BoundingBoxf3();
    for (const Vec2d& p : m_shape) {
        m_bounding_box.merge(Vec3d(p(0), p(1), 0.0));
    }

    m_extended_bounding_box = m_bounding_box;

    // extend to contain axes
    m_extended_bounding_box.merge(m_axes.get_origin() + m_axes.get_total_length() * Vec3d::Ones());
    m_extended_bounding_box.merge(m_extended_bounding_box.min + Vec3d(-Axes::DefaultTipRadius, -Axes::DefaultTipRadius, m_extended_bounding_box.max(2)));

    // extend to contain model, if any
    BoundingBoxf3 model_bb = m_model.get_bounding_box();
    if (model_bb.defined) {
        model_bb.translate(m_model_offset);
        m_extended_bounding_box.merge(model_bb);
    }
}

void Bed3D::calc_triangles(const ExPolygon& poly)
{
    Polygons triangles;
    poly.triangulate_p2t(&triangles);

    if (!m_triangles.set_from_triangles(triangles, GROUND_Z, true))
        printf("Unable to create bed triangles\n");
}

void Bed3D::calc_gridlines(const ExPolygon& poly, const BoundingBox& bed_bbox)
{
    Polylines axes_lines;
    Polylines axes_lines_big;
    Polylines axes_lines_small;
    coord_t square = scale_(5);
    while (bed_bbox.radius() > square * 100) {
        square *= 10;
    }
    for (coord_t x = bed_bbox.min(0), idx= 0; x <= bed_bbox.max(0); x += square, idx++) {
        Polyline line;
        line.append(Point(x, bed_bbox.min(1)));
        line.append(Point(x, bed_bbox.max(1)));
        if (idx % 10 == 0)
            axes_lines_big.push_back(line);
        else if(idx%2==1)
            axes_lines_small.push_back(line);
        else
            axes_lines.push_back(line);
    }
    for (coord_t y = bed_bbox.min(1), idx = 0; y <= bed_bbox.max(1); y += square, idx++) {
        Polyline line;
        line.append(Point(bed_bbox.min(0), y));
        line.append(Point(bed_bbox.max(0), y));
        if (idx % 10 == 0)
            axes_lines_big.push_back(line);
        else if (idx % 2 == 1)
            axes_lines_small.push_back(line);
        else
            axes_lines.push_back(line);
    }

    // clip with a slightly grown expolygon because our lines lay on the contours and may get erroneously clipped
    Lines gridlines = to_lines(intersection_pl(axes_lines, offset(poly, (float)SCALED_EPSILON)));
    Lines gridlines_big = to_lines(intersection_pl(axes_lines_big, offset(poly, (float)SCALED_EPSILON)));
    Lines gridlines_small = to_lines(intersection_pl(axes_lines_small, offset(poly, (float)SCALED_EPSILON)));


    // append bed contours
    Lines contour_lines = to_lines(poly);
    std::copy(contour_lines.begin(), contour_lines.end(), std::back_inserter(gridlines));

    if (!m_gridlines.set_from_lines(gridlines, GROUND_Z))
        printf("Unable to create bed grid lines\n");
    if (!m_gridlines_big.set_from_lines(gridlines_big, GROUND_Z))
        printf("Unable to create bed grid lines\n");
    if (!m_gridlines_small.set_from_lines(gridlines_small, GROUND_Z))
        printf("Unable to create bed grid lines\n");
}


std::tuple<Bed3D::EType, std::string, std::string> Bed3D::detect_type(const Pointfs& shape) const
{
    auto bundle = wxGetApp().preset_bundle;
    if (bundle != nullptr) {
        const Preset* curr = &bundle->printers.get_selected_preset();
        while (curr != nullptr) {
            if (curr->config.has("bed_shape")) {
                if (shape == dynamic_cast<const ConfigOptionPoints*>(curr->config.option("bed_shape"))->values) {
                    std::string model_filename = PresetUtils::system_printer_bed_model(*curr);
                    std::string texture_filename = PresetUtils::system_printer_bed_texture(*curr);
                    if (!model_filename.empty() && !texture_filename.empty())
                        return { System, model_filename, texture_filename };
                }
            }

            curr = bundle->printers.get_preset_parent(*curr);
        }
    }

    return { Custom, "", "" };
}

void Bed3D::render_axes() const
{
    if (!m_shape.empty())
        m_axes.render();
}

void Bed3D::render_system(GLCanvas3D& canvas, bool bottom, bool show_texture) const
{
    if (!bottom)
        render_model();

    if (show_texture)
        render_texture(bottom, canvas);
}

void Bed3D::render_texture(bool bottom, GLCanvas3D& canvas) const
{
    if (m_texture_filename.empty()) {
        m_texture.reset();
        render_default(bottom);
        return;
    }

    if ((m_texture.get_id() == 0) || (m_texture.get_source() != m_texture_filename)) {
        m_texture.reset();

        if (boost::algorithm::iends_with(m_texture_filename, ".svg")) {
            // use higher resolution images if graphic card and opengl version allow
            GLint max_tex_size = OpenGLManager::get_gl_info().get_max_tex_size();
            if ((m_temp_texture.get_id() == 0) || (m_temp_texture.get_source() != m_texture_filename)) {
                // generate a temporary lower resolution texture to show while no main texture levels have been compressed
                if (!m_temp_texture.load_from_svg_file(m_texture_filename, false, false, false, max_tex_size / 8)) {
                    render_default(bottom);
                    return;
                }
                canvas.request_extra_frame();
            }

            // starts generating the main texture, compression will run asynchronously
            if (!m_texture.load_from_svg_file(m_texture_filename, true, true, true, max_tex_size)) {
                render_default(bottom);
                return;
            }
        } 
        else if (boost::algorithm::iends_with(m_texture_filename, ".png")) {
            // generate a temporary lower resolution texture to show while no main texture levels have been compressed
            if ((m_temp_texture.get_id() == 0) || (m_temp_texture.get_source() != m_texture_filename)) {
                if (!m_temp_texture.load_from_file(m_texture_filename, false, GLTexture::None, false)) {
                    render_default(bottom);
                    return;
                }
                canvas.request_extra_frame();
            }

            // starts generating the main texture, compression will run asynchronously
            if (!m_texture.load_from_file(m_texture_filename, true, GLTexture::MultiThreaded, true)) {
                render_default(bottom);
                return;
            }
        }
        else {
            render_default(bottom);
            return;
        }
    }
    else if (m_texture.unsent_compressed_data_available()) {
        // sends to gpu the already available compressed levels of the main texture
        m_texture.send_compressed_data_to_gpu();

        // the temporary texture is not needed anymore, reset it
        if (m_temp_texture.get_id() != 0)
            m_temp_texture.reset();

        canvas.request_extra_frame();

    }

    if (m_triangles.get_vertices_count() > 0) {
        GLShaderProgram* shader = wxGetApp().get_shader("printbed");
        if (shader != nullptr) {
            shader->start_using();
            shader->set_uniform("transparent_background", bottom);
            shader->set_uniform("svg_source", boost::algorithm::iends_with(m_texture.get_source(), ".svg"));

            if (m_vbo_id == 0) {
                glsafe(::glGenBuffers(1, &m_vbo_id));
                glsafe(::glBindBuffer(GL_ARRAY_BUFFER, m_vbo_id));
                glsafe(::glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)m_triangles.get_vertices_data_size(), (const GLvoid*)m_triangles.get_vertices_data(), GL_STATIC_DRAW));
                glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
            }

            glsafe(::glEnable(GL_DEPTH_TEST));
            glsafe(::glDepthMask(GL_FALSE));

            glsafe(::glEnable(GL_BLEND));
            glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

            if (bottom)
                glsafe(::glFrontFace(GL_CW));

            unsigned int stride = m_triangles.get_vertex_data_size();

            GLint position_id = shader->get_attrib_location("v_position");
            GLint tex_coords_id = shader->get_attrib_location("v_tex_coords");

            // show the temporary texture while no compressed data is available
            GLuint tex_id = (GLuint)m_temp_texture.get_id();
            if (tex_id == 0)
                tex_id = (GLuint)m_texture.get_id();

            glsafe(::glBindTexture(GL_TEXTURE_2D, tex_id));
            glsafe(::glBindBuffer(GL_ARRAY_BUFFER, m_vbo_id));

            if (position_id != -1) {
                glsafe(::glEnableVertexAttribArray(position_id));
                glsafe(::glVertexAttribPointer(position_id, 3, GL_FLOAT, GL_FALSE, stride, (GLvoid*)(intptr_t)m_triangles.get_position_offset()));
            }
            if (tex_coords_id != -1) {
                glsafe(::glEnableVertexAttribArray(tex_coords_id));
                glsafe(::glVertexAttribPointer(tex_coords_id, 2, GL_FLOAT, GL_FALSE, stride, (GLvoid*)(intptr_t)m_triangles.get_tex_coords_offset()));
            }

            glsafe(::glDrawArrays(GL_TRIANGLES, 0, (GLsizei)m_triangles.get_vertices_count()));

            if (tex_coords_id != -1)
                glsafe(::glDisableVertexAttribArray(tex_coords_id));

            if (position_id != -1)
                glsafe(::glDisableVertexAttribArray(position_id));

            glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
            glsafe(::glBindTexture(GL_TEXTURE_2D, 0));

            if (bottom)
                glsafe(::glFrontFace(GL_CCW));

            glsafe(::glDisable(GL_BLEND));
            glsafe(::glDepthMask(GL_TRUE));

            shader->stop_using();
        }
    }
}

void Bed3D::render_model() const
{
    if (m_model_filename.empty())
        return;

    if ((m_model.get_filename() != m_model_filename) && m_model.init_from_file(m_model_filename)) {
        // move the model so that its origin (0.0, 0.0, 0.0) goes into the bed shape center and a bit down to avoid z-fighting with the texture quad
        Vec3d shift = m_bounding_box.center();
        shift(2) = -0.03;
        m_model_offset = shift;

        // update extended bounding box
        calc_bounding_boxes();
    }

    if (!m_model.get_filename().empty()) {
        GLShaderProgram* shader = wxGetApp().get_shader("gouraud_light");
        if (shader != nullptr) {
            shader->start_using();
            shader->set_uniform("uniform_color", m_model_color);
            ::glPushMatrix();
            ::glTranslated(m_model_offset(0), m_model_offset(1), m_model_offset(2));
            m_model.render();
            ::glPopMatrix();
            shader->stop_using();
        }
    }
}

void Bed3D::render_custom(GLCanvas3D& canvas, bool bottom, bool show_texture) const
{
    if (m_texture_filename.empty() && m_model_filename.empty()) {
        render_default(bottom);
        return;
    }

    if (!bottom)
        render_model();

    if (show_texture)
        render_texture(bottom, canvas);
}

void Bed3D::render_default(bool bottom) const
{
    m_texture.reset();

    unsigned int triangles_vcount = m_triangles.get_vertices_count();
    if (triangles_vcount > 0) {
        bool has_model = !m_model.get_filename().empty();

        glsafe(::glEnable(GL_DEPTH_TEST));
        glsafe(::glEnable(GL_BLEND));
        glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

        glsafe(::glEnableClientState(GL_VERTEX_ARRAY));

        if (!has_model && !bottom) {
            // draw background
            glsafe(::glDepthMask(GL_FALSE));
            glsafe(::glColor4fv(m_model_color.data()));
            glsafe(::glNormal3d(0.0f, 0.0f, 1.0f));
            glsafe(::glVertexPointer(3, GL_FLOAT, m_triangles.get_vertex_data_size(), (GLvoid*)m_triangles.get_vertices_data()));
            glsafe(::glDrawArrays(GL_TRIANGLES, 0, (GLsizei)triangles_vcount));
            glsafe(::glDepthMask(GL_TRUE));
        }

        // draw grid
        if (has_model && !bottom)
            glsafe(::glColor4f(0.9f, 0.9f, 0.9f, 1.0f));
        else if (bottom)
            glsafe(::glColor4f(0.9f, 0.9f, 0.9f, 0.6f));
        else
            glsafe(::glColor4fv(m_grid_color.data()));
        glsafe(::glLineWidth(0.5f * m_scale_factor));
        glsafe(::glVertexPointer(3, GL_FLOAT, m_gridlines_small.get_vertex_data_size(), (GLvoid*)m_gridlines_small.get_vertices_data()));
        glsafe(::glDrawArrays(GL_LINES, 0, (GLsizei)m_gridlines_small.get_vertices_count()));
        glsafe(::glLineWidth(1.5f * m_scale_factor));
        glsafe(::glVertexPointer(3, GL_FLOAT, m_gridlines.get_vertex_data_size(), (GLvoid*)m_gridlines.get_vertices_data()));
        glsafe(::glDrawArrays(GL_LINES, 0, (GLsizei)m_gridlines.get_vertices_count()));
        glsafe(::glLineWidth(3.0f * m_scale_factor));
        glsafe(::glVertexPointer(3, GL_FLOAT, m_gridlines_big.get_vertex_data_size(), (GLvoid*)m_gridlines_big.get_vertices_data()));
        glsafe(::glDrawArrays(GL_LINES, 0, (GLsizei)m_gridlines_big.get_vertices_count()));

        glsafe(::glDisableClientState(GL_VERTEX_ARRAY));

        glsafe(::glDisable(GL_BLEND));
    }
}

void Bed3D::reset()
{
    if (m_vbo_id > 0) {
        glsafe(::glDeleteBuffers(1, &m_vbo_id));
        m_vbo_id = 0;
    }
}

} // GUI
} // Slic3r
