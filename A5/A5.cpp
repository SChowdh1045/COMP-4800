#include <gtkmm.h>
#include <cairomm/context.h>
#include <iomanip>
#include <iostream>

class ImageEditor : public Gtk::Window {
protected:
    enum class Tool {
        GetColor,
        Paint
    };

private:
    // Widgets
    Gtk::Box m_vbox{Gtk::Orientation::VERTICAL};
    Gtk::Box m_toolbar{Gtk::Orientation::HORIZONTAL};
    Gtk::DrawingArea m_image_area;
    Gtk::Button m_getcolor_btn;
    Gtk::Button m_paint_btn;
    Gtk::Button m_redo_btn;
    Gtk::Button m_undo_btn;
    Gtk::Button m_save_btn;
    Gtk::Button m_load_btn;
    Gtk::Label m_color_label;

    // State
    Glib::RefPtr<Gdk::Pixbuf> m_pixbuf;
    std::optional<Gdk::RGBA> m_current_color;
    Glib::RefPtr<Gtk::GestureClick> m_click_controller;
    Glib::RefPtr<Gtk::EventControllerMotion> m_motion_controller;
    Glib::RefPtr<Gtk::GestureClick> m_button_controller;
    std::unique_ptr<Gtk::FileChooserDialog> m_active_dialog;
    Tool m_current_tool;
    bool m_is_drawing;
    bool m_is_dragging;  // To track if we're in the middle of a drag operation

    // Undo/Redo stacks
    std::vector<Glib::RefPtr<Gdk::Pixbuf>> m_undo_stack;
    std::vector<Glib::RefPtr<Gdk::Pixbuf>> m_redo_stack;
    static const size_t MAX_UNDO_STEPS = 20;  // Maximum number of undo steps

public:
    ImageEditor() {
        set_title("Image Editor");
        set_default_size(800, 600);

        // Main vertical box
        m_vbox.set_margin(10);
        set_child(m_vbox);

        // Button toolbar
        m_toolbar.set_margin(5);
        m_toolbar.set_orientation(Gtk::Orientation::HORIZONTAL);
        
        // Add buttons
        m_getcolor_btn.set_label("getcolor");
        m_getcolor_btn.signal_clicked().connect(
            sigc::mem_fun(*this, &ImageEditor::on_getcolor_clicked));
        m_toolbar.append(m_getcolor_btn);

        m_paint_btn.set_label("paint");
        m_paint_btn.signal_clicked().connect(
            sigc::mem_fun(*this, &ImageEditor::on_paint_clicked));
        m_toolbar.append(m_paint_btn);

        m_redo_btn.set_label("redo");
        m_redo_btn.signal_clicked().connect(
            sigc::mem_fun(*this, &ImageEditor::on_redo_clicked));
        m_toolbar.append(m_redo_btn);

        m_undo_btn.set_label("undo");
        m_undo_btn.signal_clicked().connect(
            sigc::mem_fun(*this, &ImageEditor::on_undo_clicked));
        m_toolbar.append(m_undo_btn);

        m_save_btn.set_label("save");
        m_save_btn.signal_clicked().connect(
            sigc::mem_fun(*this, &ImageEditor::on_save_clicked));
        m_toolbar.append(m_save_btn);

        m_load_btn.set_label("load");
        m_load_btn.signal_clicked().connect(
            sigc::mem_fun(*this, &ImageEditor::on_load_image_clicked));
        m_toolbar.append(m_load_btn);

        // Color text display
        m_color_label.set_text("color");
        m_color_label.set_margin_start(5);
        m_toolbar.append(m_color_label);

        m_vbox.append(m_toolbar);

        // Image area
        m_image_area.set_expand(true);  // Let image area fill available space
        m_image_area.set_draw_func(sigc::mem_fun(*this, &ImageEditor::on_draw));
        m_vbox.append(m_image_area);

        // Setup mouse click handling
        m_click_controller = Gtk::GestureClick::create();
        m_click_controller->signal_pressed().connect(
            sigc::mem_fun(*this, &ImageEditor::on_image_clicked));
        m_image_area.add_controller(m_click_controller);

        // Setup mouse motion handling
        m_motion_controller = Gtk::EventControllerMotion::create();
        m_motion_controller->signal_motion().connect(
            sigc::mem_fun(*this, &ImageEditor::on_mouse_motion));
        m_image_area.add_controller(m_motion_controller);

        // Setup mouse button handling
        m_button_controller = Gtk::GestureClick::create();
        m_button_controller->signal_pressed().connect(
            sigc::mem_fun(*this, &ImageEditor::on_button_pressed));
        m_button_controller->signal_released().connect(
            sigc::mem_fun(*this, &ImageEditor::on_button_released));
        m_image_area.add_controller(m_button_controller);

        // Set up initial state
        m_current_tool = Tool::GetColor;
        m_is_drawing = false;
        m_is_dragging = false;
    }

protected:
    void on_draw(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height) {
        if (m_pixbuf) {
            // Scale image to fit while maintaining aspect ratio
            double scale = std::min(
                (double)width / m_pixbuf->get_width(),
                (double)height / m_pixbuf->get_height()
            );
            
            cr->scale(scale, scale);
            Gdk::Cairo::set_source_pixbuf(cr, m_pixbuf, 0, 0);
            cr->paint();
        }
    }

    void on_getcolor_clicked() {
        m_current_tool = Tool::GetColor;
        std::cout << "Switched to getcolor tool" << std::endl;
    }

    void on_paint_clicked() {
        m_current_tool = Tool::Paint;
        std::cout << "Switched to paint tool" << std::endl;
    }

    void save_state() {
        if (!m_pixbuf) return;

        // Create a copy of the current state
        auto state = m_pixbuf->copy();
        
        // Add to undo stack
        m_undo_stack.push_back(state);
        
        // Limit undo stack size
        if (m_undo_stack.size() > MAX_UNDO_STEPS) {
            m_undo_stack.erase(m_undo_stack.begin());
        }
        
        // Clear redo stack when new action is performed
        m_redo_stack.clear();
    }

    void on_undo_clicked() {
        std::cout << "Undo clicked" << std::endl;
        if (m_undo_stack.empty()) return;

        // Save current state to redo stack
        m_redo_stack.push_back(m_pixbuf->copy());
        
        // Restore previous state
        m_pixbuf = m_undo_stack.back();
        m_undo_stack.pop_back();
        
        // Update display
        m_image_area.queue_draw();
    }

    void on_redo_clicked() {
        std::cout << "Redo clicked" << std::endl;
        if (m_redo_stack.empty()) return;

        // Save current state to undo stack
        m_undo_stack.push_back(m_pixbuf->copy());
        
        // Restore next state
        m_pixbuf = m_redo_stack.back();
        m_redo_stack.pop_back();
        
        // Update display
        m_image_area.queue_draw();
    }

    void on_save_clicked() {
        std::cout << "Save clicked" << std::endl;
        if (!m_pixbuf) {
            std::cout << "No image to save" << std::endl;
            return;
        }

        // Create and set up the dialog
        m_active_dialog = std::make_unique<Gtk::FileChooserDialog>(
            "Save Image",
            Gtk::FileChooser::Action::SAVE);

        m_active_dialog->set_transient_for(*this);
        m_active_dialog->set_modal(true);
        
        // Add response buttons
        m_active_dialog->add_button("_Cancel", Gtk::ResponseType::CANCEL);
        m_active_dialog->add_button("_Save", Gtk::ResponseType::OK);

        // Set current name suggestion
        m_active_dialog->set_current_name("untitled.png");

        // Add file filter for PNG files
        auto filter = Gtk::FileFilter::create();
        filter->set_name("PNG files");
        filter->add_pattern("*.png");
        m_active_dialog->add_filter(filter);

        // Connect response signal
        m_active_dialog->signal_response().connect(
            sigc::mem_fun(*this, &ImageEditor::on_save_dialog_response));

        // Show the dialog
        m_active_dialog->show();
        std::cout << "Save dialog shown" << std::endl;
    }

    void on_save_dialog_response(int response_id) {
        std::cout << "Save dialog response: " << response_id << std::endl;
        
        if (response_id == Gtk::ResponseType::OK) {
            try {
                auto file = m_active_dialog->get_file();
                if (file) {
                    std::string path = file->get_path();
                    // Ensure the file ends with .png
                    if (path.substr(path.length() - 4) != ".png") {
                        path += ".png";
                    }
                    std::cout << "Saving file to: " << path << std::endl;
                    m_pixbuf->save(path, "png");
                    std::cout << "File saved successfully" << std::endl;
                }
            } catch (const Glib::Error& ex) {
                std::cerr << "Error saving image: " << ex.what() << std::endl;
            }
        }
        
        m_active_dialog->hide();
        std::cout << "Save dialog hidden" << std::endl;
    }

    void on_load_image_clicked() {
        std::cout << "Load button clicked" << std::endl;
        
        // Create and set up the dialog
        m_active_dialog = std::make_unique<Gtk::FileChooserDialog>(
            "Please choose an image",
            Gtk::FileChooser::Action::OPEN);

        m_active_dialog->set_transient_for(*this);
        m_active_dialog->set_modal(true);
        
        // Add response buttons
        m_active_dialog->add_button("_Cancel", Gtk::ResponseType::CANCEL);
        m_active_dialog->add_button("_Open", Gtk::ResponseType::OK);

        // Add file filter for PNG files
        auto filter = Gtk::FileFilter::create();
        filter->set_name("PNG files");
        filter->add_pattern("*.png");
        m_active_dialog->add_filter(filter);

        // Connect response signal
        m_active_dialog->signal_response().connect(
            sigc::mem_fun(*this, &ImageEditor::on_file_dialog_response));

        // Show the dialog
        m_active_dialog->show();
        std::cout << "Dialog shown" << std::endl;
    }

    void on_file_dialog_response(int response_id) {
        std::cout << "Dialog response: " << response_id << std::endl;
        
        if (response_id == Gtk::ResponseType::OK) {
            try {
                auto file = m_active_dialog->get_file();
                if (file) {
                    std::cout << "Loading file: " << file->get_path() << std::endl;
                    m_pixbuf = Gdk::Pixbuf::create_from_file(file->get_path());
                    m_image_area.queue_draw();
                }
            } catch (const Glib::Error& ex) {
                std::cerr << "Error loading image: " << ex.what() << std::endl;
            }
        }
        
        m_active_dialog->hide();
        std::cout << "Dialog hidden" << std::endl;
    }

    void on_image_clicked(int n_press, double x, double y) {
        if (!m_pixbuf) return;

        // Convert coordinates based on scaling
        double scale = std::min(
            (double)m_image_area.get_width() / m_pixbuf->get_width(),
            (double)m_image_area.get_height() / m_pixbuf->get_height()
        );
        
        int img_x = static_cast<int>(x / scale);
        int img_y = static_cast<int>(y / scale);
        
        // Only proceed if coordinates are within the image bounds
        if (img_x >= 0 && img_x < m_pixbuf->get_width() && 
            img_y >= 0 && img_y < m_pixbuf->get_height()) {
            
            if (m_current_tool == Tool::GetColor) {
                get_pixel_color(img_x, img_y);
            }
            else if (m_current_tool == Tool::Paint) {
                std::cout << "Paint at " << img_x << "," << img_y << std::endl;
                paint_at(x, y);
            }
        }
    }

    void get_pixel_color(int x, int y) {
        if (x >= 0 && x < m_pixbuf->get_width() && 
            y >= 0 && y < m_pixbuf->get_height()) {
            
            guchar r, g, b;
            auto pixels = m_pixbuf->get_pixels();
            int channels = m_pixbuf->get_n_channels();
            int rowstride = m_pixbuf->get_rowstride();
            
            int index = y * rowstride + x * channels;
            r = pixels[index];
            g = pixels[index + 1];
            b = pixels[index + 2];
            
            // Store current color for painting
            m_current_color = Gdk::RGBA();
            m_current_color->set_rgba(r/255.0, g/255.0, b/255.0, 1.0);
            
            // Update color text
            std::stringstream ss;
            ss << "color: #" << std::hex << std::setfill('0') 
               << std::setw(2) << (int)r
               << std::setw(2) << (int)g 
               << std::setw(2) << (int)b;
            m_color_label.set_text(ss.str());
        }
    }

    void set_pixel_color(int x, int y, const Gdk::RGBA& color) {
        if (!m_pixbuf || !m_current_color.has_value()) return;
        if (x < 0 || x >= m_pixbuf->get_width() || 
            y < 0 || y >= m_pixbuf->get_height()) return;

        auto pixels = m_pixbuf->get_pixels();
        int channels = m_pixbuf->get_n_channels();
        int rowstride = m_pixbuf->get_rowstride();
        
        int index = y * rowstride + x * channels;
        pixels[index] = static_cast<guchar>(color.get_red() * 255);
        pixels[index + 1] = static_cast<guchar>(color.get_green() * 255);
        pixels[index + 2] = static_cast<guchar>(color.get_blue() * 255);
        
        // Request redraw of the image area
        m_image_area.queue_draw();
    }

    void on_button_pressed(int n_press, double x, double y) {
        if (m_current_tool == Tool::Paint) {
            m_is_drawing = true;
            m_is_dragging = false;  // Start of new drag operation
            save_state();  // Save state before starting to paint
            paint_at(x, y);
        }
    }

    void on_button_released(int n_press, double x, double y) {
        m_is_drawing = false;
    }

    void on_mouse_motion(double x, double y) {
        if (m_is_drawing && m_current_tool == Tool::Paint) {
            if (!m_is_dragging) {
                m_is_dragging = true;  // Mark the start of dragging
            }
            paint_at(x, y);
        }
    }

    void paint_at(double x, double y) {
        if (!m_pixbuf || !m_current_color.has_value()) return;

        // Get scale factor based on image size and display area
        double scale = std::min(
            (double)m_image_area.get_width() / m_pixbuf->get_width(),
            (double)m_image_area.get_height() / m_pixbuf->get_height()
        );
        
        // Convert screen coordinates to image coordinates
        int img_x = static_cast<int>(x / scale);
        int img_y = static_cast<int>(y / scale);

        // Base radius in screen pixels (constant visual size)
        const int SCREEN_RADIUS = 5;  // Adjust this value for desired brush size
        
        // Convert screen radius to image radius
        int radius = static_cast<int>(SCREEN_RADIUS / scale);
        
        // Ensure minimum radius of 1 pixel
        radius = std::max(1, radius);

        // Paint a circle around the point
        for (int dy = -radius; dy <= radius; dy++) {
            for (int dx = -radius; dx <= radius; dx++) {
                if (dx*dx + dy*dy <= radius*radius) {  // Circle shape
                    set_pixel_color(img_x + dx, img_y + dy, *m_current_color);
                }
            }
        }
    }
};

int main(int argc, char* argv[]) {
    auto app = Gtk::Application::create("org.gtkmm.image.editor");
    return app->make_window_and_run<ImageEditor>(argc, argv);
}

// g++ -o A5 A5.cpp `pkg-config --cflags --libs gtkmm-4.0`