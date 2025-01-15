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
        queue_draw(); // Request a redraw with the new data (calls on_draw())
    }

protected:
    // custom method
    void on_draw(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height) {
        // Clear background to white
        cr->set_source_rgb(1.0, 1.0, 1.0);
        cr->paint();

        // If no data, return
        if (cluster_data.points.empty() && cluster_data.centroids.empty()) return;

        
        // Initializing min_ and max_
        double min_x = std::numeric_limits<double>::max();  // Initialize min_ to largest possible value to get ready for looping
        double max_x = std::numeric_limits<double>::lowest();  // Initialize max_ to smallest possible value to get ready for looping
        double min_y = std::numeric_limits<double>::max();  // Same concept for min_
        double max_y = std::numeric_limits<double>::lowest();  // Same concept for max_


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

        // Find the maximum extent in any direction
        double max_extent_x = std::max(std::abs(min_x), std::abs(max_x));
        double max_extent_y = std::max(std::abs(min_y), std::abs(max_y));

        // Need to use these for scaling to ensure the graph is centered
        max_x = max_extent_x;
        min_x = -max_extent_x;
        max_y = max_extent_y;
        min_y = -max_extent_y;
        

        // Add padding
        double padding = 50;

        // Calculate available space
        double available_width = width - (2 * padding);
        double available_height = height - (2 * padding);

        // For a 4-quadrant system, each quadrant gets half the available space
        double quadrant_width = available_width / 2;
        double quadrant_height = available_height / 2;

        // Calculate scaling factors (i.e. how many pixels are between each data unit in the x and y direction)
        // Note: max_x and min_x are now equal in magnitude but opposite in sign
        // Same for max_y and min_y
        double scale_x = quadrant_width / max_x;   // or quadrant_height / max_extent_x
        double scale_y = quadrant_height / max_y;   // or quadrant_height / max_extent_y



       // DRAWING GRID LINES AND LABELS

        // Calculate reasonable grid spacing
        auto calculate_grid_interval = [](double max_val) {
            if (max_val <= 5) return 1.0;      // -5 to 5: step by 1 (at most 5 grid lines per quadrant - not including 0 (same for all cases))
            if (max_val <= 20) return 5.0;     // -20 to 20: step by 5 (at most 4 grid lines per quadrant)
            if (max_val <= 50) return 10.0;    // -50 to 50: step by 10 (at most 5 grid lines per quadrant)
            return ceil(max_val / 10);         // Larger ranges: step by max/10 rounded up (at most 10 grid lines per quadrant)
        };

        double x_interval = calculate_grid_interval(max_x);
        double y_interval = calculate_grid_interval(max_y);

        // Center point (origin) in screen coordinates
        double center_x = quadrant_width + padding;  // quadrant_width = available_width / 2
        double center_y = quadrant_height + padding;  // quadrant_height = available_height / 2

        cr->set_source_rgba(0.3, 0.3, 0.3, 0.5);  // Gray color
        cr->set_line_width(0.5);

        // Vertical grid lines and x labels
        for (double x = 0; x <= max_x; x += x_interval) {
            // Positive x grid lines
            double screen_x = center_x + (x * scale_x);
            cr->move_to(screen_x, padding);  // from top edge (on quadrants 1 and 4)
            cr->line_to(screen_x, height - padding);  // to bottom edge (on quadrants 1 and 4)
            cr->stroke();

            // Label +ive x-axis
            draw_XLabels(cr, screen_x, center_y, x);

            // Negative x grid lines (except for 0)
            if (x != 0) {
                screen_x = center_x - (x * scale_x);
                cr->move_to(screen_x, padding);  // from top edge (on quadrants 2 and 3)
                cr->line_to(screen_x, height - padding);  // to bottom edge (on quadrants 2 and 3)
                cr->stroke();
                
                // Label -ive x-axis
                draw_XLabels(cr, screen_x, center_y, -x);
            }
        }

        // Horizontal grid lines and y labels
        for (double y = 0; y <= max_y; y += y_interval) {
            // Positive y grid lines
            double screen_y = center_y - (y * scale_y);
            cr->move_to(padding, screen_y);  // from left edge (on quadrants 2 and 1)
            cr->line_to(width - padding, screen_y);  // to right edge (on quadrants 2 and 1)
            cr->stroke();
            
            // Negative y grid lines (except for 0) (and positive y labels)
            if (y != 0) {
                // Label +ive y-axis
                draw_YLabels(cr, center_x, screen_y, y); // I don't want to draw a 0 label on the y-axis

                screen_y = center_y + (y * scale_y);
                cr->move_to(padding, screen_y);  // from left edge (on quadrants 3 and 4)
                cr->line_to(width - padding, screen_y);  // to right edge (on quadrants 3 and 4)
                cr->stroke();

                // Label -ive y-axis
                draw_YLabels(cr, center_x, screen_y, -y);
            }   
        }



        // DRAWING AXES LINES
        cr->set_source_rgb(0.0, 0.0, 0.0);  // Black color
        cr->set_line_width(2.0);

        // X-axis
        cr->move_to(padding, center_y);  // from left edge
        cr->line_to(width - padding, center_y);  // to right edge
        cr->stroke();

        // Y-axis
        cr->move_to(center_x, padding);  // from top edge
        cr->line_to(center_x, height - padding);  // to bottom edge
        cr->stroke();



        // DRAWING DATA POINTS (BLUE CIRCLES)
        cr->set_source_rgb(0.0, 0.0, 1.0);  // Blue color
        for (const auto& p : cluster_data.points) {
            double screen_x = center_x + (p.x * scale_x);
            double screen_y = center_y - (p.y * scale_y);  // Invert y-axis
            
            // screen_x: x-coordinate of circle's center
            // screen_y: y-coordinate of circle's center
            // 5: radius of the circle in pixels
            // 0: starting angle in radians (0 = right side of circle)
            // 2 * M_PI: ending angle in radians (2π = full circle)
            cr->arc(screen_x, screen_y, 4, 0, 2 * M_PI);
            cr->fill();
        }

        // DRAWING CENTROIDS (RED SQUARES)
        cr->set_source_rgb(1.0, 0.0, 0.0);  // Red color
        for (const auto& p : cluster_data.centroids) {
            double screen_x = center_x + (p.x * scale_x);
            double screen_y = center_y - (p.y * scale_y);
            
            // 'rectangle' starts at top-left corner, so we need to adjust
            cr->rectangle(screen_x - 4, screen_y - 4, 8, 8);  // 8x8 square
            cr->fill();
        }
    }


    // helper methods for drawing labels
    void draw_XLabels(const Cairo::RefPtr<Cairo::Context>& cr, double screen_x, double center_y, double x){
        cr->set_font_size(12);
        cr->set_source_rgb(0.0, 0.0, 0.0);  // Black color
        cr->move_to(screen_x + 2, center_y + 12);  // Adjusted by trial and error for better visibility
        cr->show_text(std::to_string((int)x));
        
        cr->set_source_rgba(0.3, 0.3, 0.3, 0.5);  // Reset color for grid lines
    }
    void draw_YLabels(const Cairo::RefPtr<Cairo::Context>& cr, double center_x, double screen_y, double y){
        cr->set_font_size(12);
        cr->set_source_rgb(0.0, 0.0, 0.0);  // Black color
        cr->move_to(center_x + 3, screen_y - 3);  // Adjusted by trial and error for better visibility
        cr->show_text(std::to_string((int)y));

        cr->set_source_rgba(0.3, 0.3, 0.3, 0.5);  // Reset color for grid lines
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
        
        if (data.load_from_file("negatives.txt")) {
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