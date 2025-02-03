#include <gtkmm.h>
#include <cairomm/context.h>
#include <iomanip>
#include <iostream>

class PixelViewer : public Gtk::Window {
private:
    Gtk::Box m_vbox{Gtk::Orientation::VERTICAL};
    Gtk::DrawingArea m_image_area;
    Gtk::DrawingArea m_color_display;
    Gtk::Entry m_x_entry;
    Gtk::Entry m_y_entry;
    Gtk::Label m_color_info;
    Gtk::Box m_coord_box;
    Gtk::Button m_get_color_btn;
    Gtk::Button m_load_btn;
    
    Glib::RefPtr<Gdk::Pixbuf> m_pixbuf;
    std::optional<Gdk::RGBA> m_current_color;
    Glib::RefPtr<Gtk::GestureClick> m_click_controller;
    std::unique_ptr<Gtk::FileChooserDialog> m_active_dialog;
public:
    PixelViewer() {
        set_title("Pixel Viewer");
        set_default_size(800, 600);

        // Main vertical box
        m_vbox.set_margin(10);
        set_child(m_vbox);

        // Image area
        m_image_area.set_content_width(400);
        m_image_area.set_content_height(400);
        m_image_area.set_draw_func(sigc::mem_fun(*this, &PixelViewer::on_draw));
        m_vbox.append(m_image_area);

        // Coordinate input area
        m_coord_box.set_orientation(Gtk::Orientation::HORIZONTAL);
        m_coord_box.set_margin(5);
        m_coord_box.append(*Gtk::make_managed<Gtk::Label>("X:"));
        m_coord_box.append(m_x_entry);
        m_coord_box.append(*Gtk::make_managed<Gtk::Label>("Y:"));
        m_coord_box.append(m_y_entry);
        m_get_color_btn.set_label("Get Color");
        m_get_color_btn.signal_clicked().connect(
            sigc::mem_fun(*this, &PixelViewer::on_get_color_clicked));
        m_coord_box.append(m_get_color_btn);
        m_vbox.append(m_coord_box);

        // Color display area
        m_color_display.set_content_width(100);
        m_color_display.set_content_height(100);
        m_color_display.set_draw_func(sigc::mem_fun(*this, &PixelViewer::on_draw_color));
        m_vbox.append(m_color_display);

        // Color info label
        m_color_info.set_margin(5);
        m_vbox.append(m_color_info);

        // Load image button
        m_load_btn.set_label("Load Image");
        m_load_btn.signal_clicked().connect(
            sigc::mem_fun(*this, &PixelViewer::on_load_image_clicked));
        m_vbox.append(m_load_btn);

        // Setup mouse click handling
        m_click_controller = Gtk::GestureClick::create();
        m_click_controller->signal_pressed().connect(
            sigc::mem_fun(*this, &PixelViewer::on_image_clicked));
        m_image_area.add_controller(m_click_controller);
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

    void on_draw_color(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height) {
        if (m_current_color.has_value()) {
            cr->set_source_rgb(
                m_current_color->get_red(),
                m_current_color->get_green(),
                m_current_color->get_blue()
            );
            cr->rectangle(0, 0, width, height);
            cr->fill();
        }
    }

    void on_load_image_clicked() {
        // Create and set up the dialog
        m_active_dialog = std::make_unique<Gtk::FileChooserDialog>(
            "Please choose a PNG image",
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
            sigc::mem_fun(*this, &PixelViewer::on_file_dialog_response));

        // Show the dialog
        m_active_dialog->show();
    }

    void on_file_dialog_response(int response_id) {
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
    }

    void on_get_color_clicked() {
        if (!m_pixbuf) return;

        try {
            int x = std::stoi(m_x_entry.get_text());
            int y = std::stoi(m_y_entry.get_text());
            get_pixel_color(x, y);
        } catch (const std::exception& ex) {
            std::cerr << "Error parsing coordinates: " << ex.what() << std::endl;
        }
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
            
            get_pixel_color(img_x, img_y);
            
            // Update entry fields
            m_x_entry.set_text(std::to_string(img_x));
            m_y_entry.set_text(std::to_string(img_y));
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
            
            m_current_color = Gdk::RGBA();
            m_current_color->set_rgba(r/255.0, g/255.0, b/255.0, 1.0);
            
            std::stringstream ss;
            ss << "RGB: (" << (int)r << "," << (int)g << "," << (int)b << ")";
            ss << " Hex: #" << std::hex << std::setfill('0') 
               << std::setw(2) << (int)r
               << std::setw(2) << (int)g 
               << std::setw(2) << (int)b;
            m_color_info.set_text(ss.str());
            
            m_color_display.queue_draw();
        }
    }


};


int main(int argc, char* argv[]) {
    auto app = Gtk::Application::create("org.gtkmm.pixel.viewer");
    return app->make_window_and_run<PixelViewer>(argc, argv);
}

// g++ -o A4 A4.cpp `pkg-config --cflags --libs gtkmm-4.0`