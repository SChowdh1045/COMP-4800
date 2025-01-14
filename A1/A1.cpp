#include <gtkmm.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>

// Structure to represent a 2D point
struct Point {
    double x, y;
    Point(double x_, double y_) : x(x_), y(y_) {}
};


// Class to read and store clustering data
class ClusterData {
public:
    std::vector<Point> points;     // Data points
    std::vector<Point> centroids;  // Centroids
    
    bool load_from_file(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            std::cerr << "Error opening file: " << filename << std::endl;
            return false;
        }

        // Read number of data points
        int n_points;
        if (!(file >> n_points)) {
            std::cerr << "Error reading number of points" << std::endl;
            return false;
        }
        std::cout << "Expected number of points: " << n_points << std::endl;

        // Read data points coordinates
        points.clear();
        for (int i = 0; i < n_points; i++) {
            double x, y;
            if (!(file >> x >> y)) {
                std::cerr << "Error reading point " << i << std::endl;
                return false;
            }
            std::cout << "Read point: (" << x << ", " << y << ")" << std::endl;
            points.emplace_back(x, y);
        }


        // Read number of centroids
        int n_centroids;
        if (!(file >> n_centroids)) {
            std::cerr << "Error reading number of centroids" << std::endl;
            return false;
        }
        std::cout << "Expected number of centroids: " << n_centroids << std::endl;

        // Read centroid coordinates
        centroids.clear();
        for (int i = 0; i < n_centroids; i++) {
            double x, y;
            if (!(file >> x >> y)) {
                std::cerr << "Error reading centroid " << i << std::endl;
                return false;
            }
            std::cout << "Read centroid: (" << x << ", " << y << ")" << std::endl;
            centroids.emplace_back(x, y);
        }

        return true;
    }
};



class DrawingArea_ : public Gtk::DrawingArea {
private:
    ClusterData cluster_data;

public:
    DrawingArea_() {
        // Set a larger default size for better visualization
        set_content_width(800);
        set_content_height(600);
        
        // Set up drawing signal
        set_draw_func(sigc::mem_fun(*this, &DrawingArea_::on_draw));
    }

    void set_data(const ClusterData& data) {
        cluster_data = data;
        queue_draw(); // Request a redraw with the new data
    }

protected:
    void on_draw(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height) {
        // Clear background to white
        cr->set_source_rgb(1.0, 1.0, 1.0);
        cr->paint();

        // If no data, return
        if (cluster_data.points.empty() && cluster_data.centroids.empty()) return;

        // Add this as a class member in DrawingArea_
        bool start_from_origin = true;  // Toggle between (0,0) and (min_x,min_y)

        // Then modify the range calculation in on_draw:
        double min_x = start_from_origin ? 0 : std::numeric_limits<double>::max();
        double min_y = start_from_origin ? 0 : std::numeric_limits<double>::max();
        double max_x = std::numeric_limits<double>::lowest();
        double max_y = std::numeric_limits<double>::lowest();


        for (const auto& p : cluster_data.points) {
            min_x = std::min(min_x, p.x);  // Guaranteed to pick p.x on 1st iteration
            max_x = std::max(max_x, p.x);  // Same
            
            min_y = std::min(min_y, p.y);  // Guaranteed to pick p.y on 1st iteration
            max_y = std::max(max_y, p.y);  // Same
        }
        for (const auto& p : cluster_data.centroids) {
            min_x = std::min(min_x, p.x);
            max_x = std::max(max_x, p.x);
            
            min_y = std::min(min_y, p.y);
            max_y = std::max(max_y, p.y);
        }

        // Add padding
        double padding = 50;
        double scale_x = (width - 2 * padding) / (max_x - min_x);
        double scale_y = (height - 2 * padding) / (max_y - min_y);


        // Draw grid lines
        cr->set_source_rgba(0.8, 0.8, 0.8, 0.5);  // Light gray, semi-transparent
        cr->set_line_width(0.5);

        // Vertical grid lines
        for (int x = (int)min_x; x <= (int)max_x; x++) {
            double screen_x = (x - min_x) * scale_x + padding;
            cr->move_to(screen_x, padding);
            cr->line_to(screen_x, height - padding);
            cr->stroke();
        }

        // Horizontal grid lines
        for (int y = (int)min_y; y <= (int)max_y; y++) {
            double screen_y = height - ((y - min_y) * scale_y + padding);
            cr->move_to(padding, screen_y);
            cr->line_to(width - padding, screen_y);
            cr->stroke();
        }


        // Draw axes
        cr->set_source_rgb(0.0, 0.0, 0.0);  // Black color
        cr->set_line_width(2.0);

        // X-axis
        cr->move_to(padding, height - padding);
        cr->line_to(width - padding, height - padding);
        cr->stroke();

        // Y-axis
        cr->move_to(padding, height - padding);
        cr->line_to(padding, padding);
        cr->stroke();


        // Add axis labels
        cr->set_font_size(12);

        // X-axis labels
        for (int x = (int)min_x; x <= (int)max_x; x++) {
            double screen_x = (x - min_x) * scale_x + padding;
            cr->move_to(screen_x - 5, height - padding + 20);  // Adjusted by trial and error for better visibility
            cr->show_text(std::to_string(x));
        }

        // Y-axis labels
        for (int y = (int)min_y; y <= (int)max_y; y++) {
            double screen_y = height - ((y - min_y) * scale_y + padding);
            cr->move_to(padding - 30, screen_y + 5);  // Adjusted by trial and error for better visibility
            cr->show_text(std::to_string(y));
        }


        // Draw data points (blue circles)
        cr->set_source_rgb(0.0, 0.0, 1.0);  // Blue color
        for (const auto& p : cluster_data.points) {
            double screen_x = (p.x - min_x) * scale_x + padding;
            double screen_y = height - ((p.y - min_y) * scale_y + padding);
            
            cr->arc(screen_x, screen_y, 5, 0, 2 * M_PI);
            cr->fill();
        }

        // Draw centroids (red squares)
        cr->set_source_rgb(1.0, 0.0, 0.0);  // Red color
        for (const auto& p : cluster_data.centroids) {
            double screen_x = (p.x - min_x) * scale_x + padding;
            double screen_y = height - ((p.y - min_y) * scale_y + padding);
            
            // rectangle starts at top-left corner, so we need to adjust
            cr->rectangle(screen_x - 5, screen_y - 5, 10, 10);
            cr->fill();
        }
    }
};



class MainWindow : public Gtk::Window {
private:
    DrawingArea_ drawingArea;

public:
    MainWindow() {
        set_title("Clustering Visualization");
        set_default_size(800, 600);

        // Create a vertical box to hold the button and drawing area
        auto box = Gtk::Box(Gtk::Orientation::VERTICAL);
        set_child(box);

        // Add load button
        auto button = Gtk::Button("Load Data");
        button.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_button_clicked));
        box.append(button);

        // Add drawing area
        box.append(drawingArea);
    }

protected:
    void on_button_clicked() {
        std::cout << "Button clicked" << std::endl;
        
        ClusterData data; // Instantiated ClusterData object
        
        if (data.load_from_file("dataPoints2.txt")) {
            std::cout << "File loaded successfully" << std::endl;
            drawingArea.set_data(data);
        } else {
            std::cout << "File load failed" << std::endl;
        }
    }
};



int main(int argc, char* argv[]) {
    std::cout << "Starting application..." << std::endl;
    
    auto app = Gtk::Application::create("org.gtkmm.clustering");
    std::cout << "Application created..." << std::endl;
    
    return app->make_window_and_run<MainWindow>(argc, argv);
}


// g++ -o A1 A1.cpp `pkg-config --cflags --libs gtkmm-4.0`