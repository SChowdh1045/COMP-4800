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

        // Read data point coordinates
        points.clear();
        for (int i = 0; i < n_points; i++) {
            double x, y;
            if (!(file >> x >> y)) {
                std::cerr << "Error reading point " << i << std::endl;
                return false;
            }
            // std::cout << "Read point: (" << x << ", " << y << ")" << std::endl;
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
            // std::cout << "Read centroid: (" << x << ", " << y << ")" << std::endl;
            centroids.emplace_back(x, y);
        }

        return true;
    }


    // K-MEANS CLUSTERING
    std::vector<int> point_clusters;  // Store cluster assignment for each point
    
    // Assign points to nearest centroid
    void assign_clusters() {
        point_clusters.resize(points.size());
        for (size_t i = 0; i < points.size(); i++) {
            double min_dist = std::numeric_limits<double>::max();
            int closest_centroid = 0;
            
            for (size_t j = 0; j < centroids.size(); j++) {
                double dist = std::pow(points[i].x - centroids[j].x, 2) + 
                            std::pow(points[i].y - centroids[j].y, 2);
                if (dist < min_dist) {
                    min_dist = dist;
                    closest_centroid = j;
                }
            }
            point_clusters[i] = closest_centroid;
        }
    }

    // Calculate new centroid positions
    std::vector<Point> calculate_new_centroids() {
        std::vector<Point> new_centroids = centroids;  // Start with current positions
        std::vector<int> counts(centroids.size(), 0);
        
        // Zero out the centroids that will be recalculated
        for (size_t i = 0; i < new_centroids.size(); i++) {
            new_centroids[i].x = 0;
            new_centroids[i].y = 0;
        }
        
        // Sum up points
        for (size_t i = 0; i < points.size(); i++) {
            int cluster = point_clusters[i];
            new_centroids[cluster].x += points[i].x;
            new_centroids[cluster].y += points[i].y;
            counts[cluster]++;
        }
        
        // Calculate means only for non-empty clusters
        for (size_t i = 0; i < new_centroids.size(); i++) {
            if (counts[i] > 0) {
                new_centroids[i].x /= counts[i];
                new_centroids[i].y /= counts[i];
            } else {
                // Keep the previous position for empty clusters
                new_centroids[i] = centroids[i];
            }
        }
        
        return new_centroids;
    }
};



class DrawingArea_ : public Gtk::DrawingArea {
private:
    ClusterData cluster_data;

    // For K-means animation
    int current_iteration;
    double animation_progress;
    bool is_running;
    std::vector<Point> old_centroids;
    sigc::connection timer_connection;
    Gtk::Scale* speed_slider;
    Gtk::Label* iteration_label;

    // Checking for convergence
    std::vector<int> previous_clusters;
    bool has_converged;

    // To fix the scaling issue at the beginning
    double stored_scale_x;
    double stored_scale_y;

    double stored_center_x;
    double stored_center_y;

    double stored_max_x;
    double stored_max_y;

    double stored_padding;

    double stored_quadrant_width;
    double stored_quadrant_height;


public:
    DrawingArea_() {
        // Set a larger default size for better visualization
        set_content_width(800);
        set_content_height(600);

        signal_resize().connect(sigc::mem_fun(*this, &DrawingArea_::on_size_allocate));
        
        // Set up drawing signal
        set_draw_func(sigc::mem_fun(*this, &DrawingArea_::on_draw));

        current_iteration = 0;
        is_running = false;
        animation_progress = 0;
    }

    void on_size_allocate(int width, int height) {
        // Recalculate scales when window size changes
        if (!cluster_data.points.empty()) {
            calculate_scales(width, height);
            queue_draw();
        }
    }

    void set_data(const ClusterData& data) {
        cluster_data = data;
        cluster_data.point_clusters.resize(data.points.size(), 0);
        
        // Calculate scales once and store them
        calculate_scales(get_width(), get_height());

        // When I press reset, I want the centroids to go back to their original positions (see the last for-loop in on_draw)
        old_centroids.clear();

        // Reset iteration count
        current_iteration = 0;
        iteration_label->set_markup("<span font='20' weight='bold'>Iteration: 0</span>");
        
        queue_draw();
    }

    void set_controls(Gtk::Scale* slider, Gtk::Label* label) {
        speed_slider = slider;
        iteration_label = label;
    }

    void start_animation() {
        if (!is_running) {
            is_running = true;
            cluster_data.assign_clusters();
            
            iteration_label->set_markup("<span font='20' weight='bold'>Iteration: 1</span>");
            
            on_timer();  // Starts animation immediately
            // schedule_next_frame() is called at the end of on_timer()
        }
    }

    // 'const' makes a promise not to modify any member variables of the class
    bool is_animating() const {
        return is_running;
    }

    void stopRunning(){
        is_running = false;
    }

    bool hasConverged(const std::vector<Point>& old_pos, const std::vector<Point>& new_pos) {
        double epsilon = 0.0001;
        for (size_t i = 0; i < old_pos.size(); i++) {
            double dx = old_pos[i].x - new_pos[i].x;
            double dy = old_pos[i].y - new_pos[i].y;
            double distance = std::sqrt(dx*dx + dy*dy);
            if (distance > epsilon) {
                return false;
            }
        }
        return true;
    }

    void calculate_scales(int width, int height) {
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

        // Store max values for grid lines
        stored_max_x = max_extent_x;
        stored_max_y = max_extent_y;

        // Store padding and calculate available space
        stored_padding = 35;
        double available_width = width - (2 * stored_padding);
        double available_height = height - (2 * stored_padding);

        // Store quadrant dimensions
        stored_quadrant_width = available_width / 2;
        stored_quadrant_height = available_height / 2;

        // Store scaling factors
        stored_scale_x = stored_quadrant_width / max_extent_x;
        stored_scale_y = stored_quadrant_height / max_extent_y;
        
        // Store center coordinates
        stored_center_x = stored_quadrant_width + stored_padding;
        stored_center_y = stored_quadrant_height + stored_padding;
    }

    std::vector<std::array<double, 3>> generate_colors(int num_clusters) {
        std::vector<std::array<double, 3>> colors;
        
        for (int i = 0; i < num_clusters; i++) {
            // Distribute hue evenly around the color wheel (0 to 360 degrees)
            double hue = (360.0 * i) / num_clusters;
            double saturation = 1.0;  // Full saturation
            double value = 1.0;       // Full brightness

            // Convert HSV to RGB
            double c = value * saturation;
            double x = c * (1 - std::abs(std::fmod(hue / 60.0, 2) - 1));
            double m = value - c;

            double r, g, b;
            if (hue >= 0 && hue < 60) {
                r = c; g = x; b = 0;
            } else if (hue >= 60 && hue < 120) {
                r = x; g = c; b = 0;
            } else if (hue >= 120 && hue < 180) {
                r = 0; g = c; b = x;
            } else if (hue >= 180 && hue < 240) {
                r = 0; g = x; b = c;
            } else if (hue >= 240 && hue < 300) {
                r = x; g = 0; b = c;
            } else {
                r = c; g = 0; b = x;
            }

            colors.push_back({r + m, g + m, b + m});
        }
        
        return colors;
    }


protected:
    // custom method
    void on_draw(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height) {
        // Clear background to white
        cr->set_source_rgb(1.0, 1.0, 1.0);
        cr->paint();

        // If no data, return
        if (cluster_data.points.empty() && cluster_data.centroids.empty()) return;

        
        // Use stored values:
        double scale_x = stored_scale_x;
        double scale_y = stored_scale_y;

        double center_x = stored_center_x;
        double center_y = stored_center_y;

        double max_x = stored_max_x;
        double max_y = stored_max_y;

        double padding = stored_padding;

        double quadrant_width = stored_quadrant_width;
        double quadrant_height = stored_quadrant_height;


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


        // K-MEANS //

        // Generate colors based on number of centroids
        const auto colors = generate_colors(cluster_data.centroids.size());

        // Draw points
        for (size_t i = 0; i < cluster_data.points.size(); i++) {
            int cluster = cluster_data.point_clusters[i];
            auto& color = colors[cluster];
            cr->set_source_rgb(color[0], color[1], color[2]);
            
            double screen_x = center_x + (cluster_data.points[i].x * scale_x);
            double screen_y = center_y - (cluster_data.points[i].y * scale_y);
            cr->arc(screen_x, screen_y, 4, 0, 2 * M_PI);
            cr->fill();
        }

        // Draw centroids with animation
        for (size_t i = 0; i < cluster_data.centroids.size(); i++) {
            auto& color = colors[i % colors.size()];
            cr->set_source_rgb(color[0], color[1], color[2]);
            
            double start_x = old_centroids.empty() ? 
                cluster_data.centroids[i].x : old_centroids[i].x;
            double start_y = old_centroids.empty() ? 
                cluster_data.centroids[i].y : old_centroids[i].y;
            
            double end_x = cluster_data.centroids[i].x;
            double end_y = cluster_data.centroids[i].y;
            
            double current_x = start_x + (end_x - start_x) * animation_progress;
            double current_y = start_y + (end_y - start_y) * animation_progress;
            
            double screen_x = center_x + (current_x * scale_x);
            double screen_y = center_y - (current_y * scale_y);

            // Draw black circle outline
            cr->set_source_rgb(0.0, 0.0, 0.0);  // Black
            cr->arc(screen_x, screen_y, 14, 0, 2 * M_PI);  // Slightly larger radius
            cr->stroke();

            // Draw colored square
            cr->set_source_rgb(color[0], color[1], color[2]);
            cr->rectangle(screen_x - 4, screen_y - 4, 8, 8);
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


    // Timer callback for K-MEANS animation
    void schedule_next_frame() {
        if (!is_running) return;
        
        try {
            int fps = static_cast<int>(speed_slider->get_value());
            if (fps <= 0) {
                std::cerr << "FPS must be positive" << std::endl;
                is_running = false;
                return;
            }
            int frame_duration = 1000 / fps;  // Convert seconds to milliseconds (1000 ms = 1 s)
            timer_connection = Glib::signal_timeout().connect(
                sigc::mem_fun(*this, &DrawingArea_::on_timer),
                frame_duration);
        } catch (...) {
            std::cerr << "Invalid speed value" << std::endl;
            is_running = false;
        }
    }

    bool on_timer() {
        if (!is_running) return false;
        
        animation_progress += 0.1;
        
        if (animation_progress >= 1.0) {
            animation_progress = 0;
            
            if (old_centroids.empty()) {
                // First: Assign points phase
                cluster_data.assign_clusters();
                
                // Save current positions AFTER assigning clusters
                old_centroids = cluster_data.centroids;
                
                // Calculate but don't apply new positions yet
                std::vector<Point> new_positions = cluster_data.calculate_new_centroids();

                // Check for convergence
                if (hasConverged(cluster_data.centroids, new_positions)) {
                    is_running = false;
                    iteration_label->set_markup("<span font='20' weight='bold'>Converged at iteration: " + std::to_string(current_iteration) + "</span>");
                    return false;
                }

                cluster_data.centroids = new_positions;

                current_iteration++;
                iteration_label->set_markup("<span font='20' weight='bold'>Iteration: " + std::to_string(current_iteration) + "</span>");
            } else {
                // Second: Finish centroid movement phase
                old_centroids.clear();
            }
        }
        
        queue_draw();
        schedule_next_frame();  // Schedule next timer 
        return false; // Return false to stop the previous timer (from the previous call to schedule_next_frame)
    }
};



class MainWindow : public Gtk::Window {
private:
    DrawingArea_ drawingArea;
    ClusterData data;

    Gtk::Box vbox;
    Gtk::Button start_button;
    Gtk::Button reset_button;
    Gtk::Scale speed_slider;
    Gtk::Label iteration_label;
    Gtk::Label speed_label;  // Add a label to explain the entry

public:
    MainWindow() : speed_slider(Gtk::Orientation::HORIZONTAL) {
        set_title("K-Means Clustering Animation");
        set_default_size(800, 600);

        vbox.set_orientation(Gtk::Orientation::VERTICAL);
        vbox.set_margin(10);  // Add margin around everything
        set_child(vbox);

        // Top controls box
        auto controls = Gtk::Box(Gtk::Orientation::HORIZONTAL);
        controls.set_margin(5);  // Margin around control box
        // controls.set_spacing(20);  // Space between controls

        // Left side - Start button
        start_button.set_label("Start Animation");
        start_button.signal_clicked().connect(
            sigc::mem_fun(*this, &MainWindow::on_start_clicked));
        start_button.set_margin(5);
        controls.append(start_button);

        // Middle - Speed control
        auto speed_box = Gtk::Box(Gtk::Orientation::HORIZONTAL);
        speed_box.set_margin_start(50);
        speed_box.set_hexpand(true);
        speed_box.set_halign(Gtk::Align::CENTER);  // Center in the expanded space
        
        speed_label.set_text("Animation Speed (FPS):");
        speed_box.append(speed_label);

        speed_slider.set_range(1, 60);
        speed_slider.set_value(10);
        speed_slider.set_size_request(200, -1);  // Make slider wider
        speed_slider.set_draw_value(true);
        speed_slider.set_digits(0);
        speed_box.append(speed_slider);

        controls.append(speed_box);

        // Right side - Reset button with margin-left:auto to push it right
        reset_button.set_label("Reset");
        reset_button.signal_clicked().connect(
            sigc::mem_fun(*this, &MainWindow::load_data));
        reset_button.set_margin(5);
        reset_button.set_hexpand(true);  // Allow horizontal expansion
        reset_button.set_halign(Gtk::Align::END);  // Align to the right
        controls.append(reset_button);

        vbox.append(controls);

        // Main drawing area
        vbox.append(drawingArea);

        // Bottom - Iteration label
        // iteration_label.set_text("Iteration: 0");
        iteration_label.set_margin(10);
        iteration_label.set_margin_start(20);
        iteration_label.set_markup("<span font='20' weight='bold'>Iteration: 0</span>");
        vbox.append(iteration_label);

        drawingArea.set_controls(&speed_slider, &iteration_label);

        load_data();
    }

protected:
    void load_data() {
        drawingArea.stopRunning();

        if (data.load_from_file("main.txt")) {
            drawingArea.set_data(data);
            start_button.set_sensitive(true);
        }
    }

    void on_start_clicked() {
        drawingArea.start_animation();
    }
};



int main(int argc, char* argv[]) {
    std::cout << "Starting application..." << std::endl;
    
    auto app = Gtk::Application::create("org.gtkmm.clustering");
    std::cout << "Application created..." << std::endl;
    
    return app->make_window_and_run<MainWindow>(argc, argv);
}


// g++ -o A3 A3.cpp `pkg-config --cflags --libs gtkmm-4.0`