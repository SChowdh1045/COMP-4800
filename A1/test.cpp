#include <gtkmm.h>      // Include GTK for C++ library
#include <iostream>

// Define a window class that inherits from GTK's Window class
class MainWindow : public Gtk::Window {
public:
    // Constructor for our window
    MainWindow() {
        set_title("GTK4 Test");
        set_default_size(400, 300);

        // Create a button widget
        auto button = Gtk::Button("Click Me!");  // 'auto' lets compiler deduce type
                                                // Creates button with text "Click Me!"

        // Connect button click event to our handler function
        button.signal_clicked().connect(
            sigc::mem_fun(*this, &MainWindow::on_button_clicked)
        );
        // This line is complex, let's break it down:
        // - signal_clicked() gets the button's click signal
        // - connect() attaches a function to that signal
        // - sigc::mem_fun connects to a class member function
        // - *this points to current window instance
        // - &MainWindow::on_button_clicked is pointer to member function
        
        // Add the button to the window
        set_child(button);  // Makes button a child widget of the window
    }

protected:
    // Function that handles button clicks
    void on_button_clicked() {
        std::cout << "Button clicked!" << std::endl;
    }
};

// Main program entry point
int main(int argc, char* argv[]) {
    // Create a GTK application
    auto app = Gtk::Application::create("org.gtkmm.test");
    // "org.gtkmm.test" is the application ID
    
    // Create window and start the application
    return app->make_window_and_run<MainWindow>(argc, argv);
    // - Creates an instance of MainWindow
    // - Starts GTK main loop
    // - Returns exit status when window is closed
}