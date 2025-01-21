#include <gtkmm.h>
#include <cairomm/context.h>
#include <cmath>
#include <vector>

class Star {
public:
    double x, y;        // Position
    double size;        // Star size
    double brightness;  // Star brightness

    Star(double x_pos, double y_pos, double sz, double br) 
        : x(x_pos), y(y_pos), size(sz), brightness(br) {}

    void draw(const Cairo::RefPtr<Cairo::Context>& cr) {
        cr->set_source_rgba(1.0, 1.0, 1.0, brightness);  // White with varying alpha
        cr->arc(x, y, size, 0, 2 * M_PI);
        cr->fill();
    }
};

class Planet {
public:
    double orbit_radius;  // Distance from sun
    double angle;         // Current angle in orbit
    double speed;         // Angular speed
    double size;          // Planet size
    Gdk::RGBA color;     // Planet color
    bool is_saturn;

    Planet(double r, double s, double sz, const Gdk::RGBA& c, bool saturn = false) 
        : orbit_radius(r), angle(0), speed(s), size(sz), color(c), is_saturn(saturn) {}


    void draw(const Cairo::RefPtr<Cairo::Context>& cr, double center_x, double center_y) {
        double x = center_x + (orbit_radius * cos(angle));
        double y = center_y - (orbit_radius * sin(angle));  // Want to go counter-clockwise
        
        // Draw orbit path
        cr->set_source_rgba(0.2, 0.2, 0.2, 0.5);
        cr->arc(center_x, center_y, orbit_radius, 0, 2 * M_PI);
        cr->stroke();
        
        // Draw planet
        cr->set_source_rgba(color.get_red(), color.get_green(), color.get_blue(), color.get_alpha());
        cr->arc(x, y, size, 0, 2 * M_PI);
        cr->fill();

        if(is_saturn){
            // Draw Saturn's ring
            cr->set_source_rgba(0.9, 0.8, 0.5, 0.5);  // Golden color
            cr->set_line_width(12);
            cr->arc(x, y, size + 13, 0, 2 * M_PI);
            cr->stroke();
            cr->set_line_width(2);
        }
    }
    

    void update() {
        angle += speed;
        if (angle > 2 * M_PI) {
            angle -= 2 * M_PI;
        }
    }
};



class Asteroid {
public:
    double orbit_radius;  // Distance from sun
    double angle;         // Current angle in orbit
    double speed;         // Angular speed
    double size;         // Asteroid size

    Asteroid(double r, double a, double s, double sz) 
        : orbit_radius(r), angle(a), speed(s), size(sz) {}

    void draw(const Cairo::RefPtr<Cairo::Context>& cr, double center_x, double center_y) {
        double x = center_x + (orbit_radius * cos(angle));
        double y = center_y - (orbit_radius * sin(angle));
        
        // Draw asteroid
        cr->set_source_rgba(0.6, 0.6, 0.6, 0.8);  // Grey color
        cr->arc(x, y, size, 0, 2 * M_PI);
        cr->fill();
    }

    void update() {
        angle += speed;
        if (angle > 2 * M_PI) {
            angle -= 2 * M_PI;
        }
    }
};



class SolarSystem : public Gtk::DrawingArea {
private:
    std::vector<Star> stars;
    std::vector<Planet> planets;
    std::vector<Asteroid> asteroids;

    void setup_stars(int width, int height) {
        // Clear any existing stars
        stars.clear();
        
        const int NUM_STARS = 250;  // Number of stars
        
        // Create random stars
        for (int i = 0; i < NUM_STARS; i++) {
            double x = static_cast<double>(rand()) / RAND_MAX * width;   // Random x position
            double y = static_cast<double>(rand()) / RAND_MAX * height;  // Random y position
            double size = 0.5 + (static_cast<double>(rand()) / RAND_MAX);  // Random size between 0.5 and 1.5
            double brightness = 0.3 + (static_cast<double>(rand()) / RAND_MAX * 0.7);  // Random brightness between 0.3 and 1.0
            
            stars.emplace_back(x, y, size, brightness);
        }
    }

    void setup_planets() {
        // Mercury
        Gdk::RGBA mercury_color;
        mercury_color.set_rgba(0.7, 0.7, 0.7);
        planets.emplace_back(50, 0.09, 5, mercury_color);

        // Venus
        Gdk::RGBA venus_color;
        venus_color.set_rgba(0.9, 0.7, 0.5);
        planets.emplace_back(85, 0.075, 8, venus_color);

        // Earth
        Gdk::RGBA earth_color;
        earth_color.set_rgba(0.2, 0.5, 1.0);
        planets.emplace_back(130, 0.065, 10, earth_color);

        // Mars
        Gdk::RGBA mars_color;
        mars_color.set_rgba(1.0, 0.3, 0.0);
        planets.emplace_back(160, 0.06, 7, mars_color);

        // Jupiter (orange/beige)
        Gdk::RGBA jupiter_color;
        jupiter_color.set_rgba(0.8, 0.6, 0.4);
        planets.emplace_back(280, 0.04, 20, jupiter_color);

        // Saturn (golden)
        Gdk::RGBA saturn_color;
        saturn_color.set_rgba(0.9, 0.8, 0.5);
        planets.emplace_back(345, 0.036, 17, saturn_color, true);

        // Uranus (light blue)
        Gdk::RGBA uranus_color;
        uranus_color.set_rgba(0.5, 0.8, 0.9);
        planets.emplace_back(415, 0.03, 14, uranus_color);

        // Neptune (deep blue)
        Gdk::RGBA neptune_color;
        neptune_color.set_rgba(0.2, 0.3, 0.9);
        planets.emplace_back(468, 0.022, 14, neptune_color);
    }


    void setup_asteroids() {
        // Create asteroid belt between Mars and Jupiter (around radius 220)
        const int NUM_ASTEROIDS = 190;  // Number of asteroids to create
        const double BASE_RADIUS = 215;  // Base orbit radius
        const double RADIUS_VARIATION = 55;  // How wide the asteroid belt is
        
        srand(time(nullptr));  // Initialize random seed
        
        for (int i = 0; i < NUM_ASTEROIDS; i++) {
            // Random orbit radius within the belt
            double radius = BASE_RADIUS + (static_cast<double>(rand()) / RAND_MAX * RADIUS_VARIATION - RADIUS_VARIATION/2);

            // Random starting angle
            double angle = static_cast<double>(rand()) / RAND_MAX * (2 * M_PI);
            
            // Random speed variation
            double speed = 0.03 + (static_cast<double>(rand()) / RAND_MAX * 0.02 - 0.01);
            
            // Random size variation
            double size = 1 + (static_cast<double>(rand()) / RAND_MAX * 2);
            
            asteroids.emplace_back(radius, angle, speed, size);
        }
    }


    void on_draw(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height) {
        // Clear background
        cr->set_source_rgb(0, 0, 0);  // Space background
        cr->paint();

        // Calculate center
        double center_x = width / 2.0;
        double center_y = height / 2.0;

        // Recreate stars if window size changed
        static int last_width = 0;
        static int last_height = 0;
        if (width != last_width || height != last_height) {
            setup_stars(width, height);
            last_width = width;
            last_height = height;
        }

        // Draw stars
        for (auto& s : stars) {
            s.draw(cr);
        }

        // Draw sun
        cr->set_source_rgb(1.0, 0.8, 0.0);  // Yellow sun
        cr->arc(center_x, center_y, 20, 0, 2 * M_PI);
        cr->fill();

        // Draw planets
        for (auto& p : planets) {
            p.draw(cr, center_x, center_y);
        }

        // Draw asteroids
        for (auto& a : asteroids) {
            a.draw(cr, center_x, center_y);
        }
    }


    bool trigger_draw() {
        // Update planet positions
        for (auto& p : planets) {
            p.update();
        }

        for (auto& a : asteroids) {
            a.update();
        }
        
        // Request redraw (calls on_draw)
        queue_draw();
        
        return true;  // Keep timer running
    }


public:
    SolarSystem() {
        // Sets up drawing function to be called whenever the widget needs to be redrawn (e.g. queue_draw() or when resized)
        // This is just the registration step that tells GTK which function to call when it needs to draw,
        // but it never directly calls the function on_draw() itself.
        set_draw_func(sigc::mem_fun(*this, &SolarSystem::on_draw));

        // Get initial size
        int width = get_width();
        int height = get_height();
        if (width == 0) width = 800;  // Default size if not set
        if (height == 0) height = 800;

        // Initialize random seed for star positions
        srand(time(nullptr));
        
        // Initialize everything (only called once (before on_draw))
        setup_stars(width, height);
        setup_planets();
        setup_asteroids();
        

        
        // Set up timer for animation
        Glib::signal_timeout().connect(
            sigc::mem_fun(*this, &SolarSystem::trigger_draw),
            50  // Update every 50 milliseconds
        );
    }
};



class MainWindow : public Gtk::Window {
private:
    SolarSystem solar_system;

public:
    MainWindow() {
        set_title("Solar System Simulation");
        set_default_size(800, 800);
        
        // set_expand() is actually a method from Gtk::Widget, which DrawingArea (and therefore my SolarSystem class) inherits from. 
        // It's not a method of either SolarSystem or Gtk::Window specifically.
        // In GTK4, set_expand() tells a widget whether it should try to use any additional space that its parent container can provide. 
        // When set to true, the widget will expand to fill any extra space.
        solar_system.set_expand(true);
        set_child(solar_system);
    }
};



int main(int argc, char** argv) {
    auto app = Gtk::Application::create("org.gtkmm.solar.system");
    return app->make_window_and_run<MainWindow>(argc, argv);
}


// g++ -o A2 A2.cpp `pkg-config --cflags --libs gtkmm-4.0`