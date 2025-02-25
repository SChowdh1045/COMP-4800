# Assignment 6: Video Frame Extractor - Debugging Journey

## Original Task
Create a program that:
1. Takes command line arguments for:
   - Video filename
   - Frame number to extract
   - RGB coefficients for grayscale conversion (r, g, b)
2. Uses FFmpeg to extract the specified frame
3. Saves the frame in two formats
4. Displays both versions in a GTK4 window

## Current Status
- FFmpeg part works correctly:
  - Successfully extracts frame from video
  - Converts to both color and grayscale
  - Saves as PNG files (2730x1440 resolution)
  - Files can be opened and viewed in Windows

- GTK part fails with error:
  ```
  GLib-GIO-CRITICAL **: HH:MM:SS.sss: This application can not open files.
  ```

## Approaches Tried

### 1. Basic GTK Test
- Created minimal GTK window test without FFmpeg
- Test worked perfectly
- Confirmed GTK environment is working

### 2. Image Format Changes
- Initially used PPM/PGM format
- Switched to PNG format (known to work from Assignment 4)
- Same error persisted

### 3. Image Loading Methods
- Tried direct file loading with `Pixbuf::create_from_file`
- Tried memory loading using `PixbufLoader`
- Added error checking and debug output
- Files load but window doesn't appear

### 4. GTK Initialization Patterns
- Tried different window creation patterns:
  - Direct window creation
  - Signal-based creation
  - Explicit window management
- Tried moving GTK initialization before FFmpeg
- Tried explicit GTK initialization with `gtk_init()`

### 5. Code Organization
- Separated FFmpeg and GTK code
- Moved file I/O operations to standard C++
- Added extensive error checking
- Added debug output throughout the process

## Key Observations
1. Basic GTK apps work in the environment
2. Error occurs after FFmpeg operations
3. Files are successfully created and readable
4. Error appears during GTK app initialization
5. Error specifically mentions file operations

## Current Theory
The issue might be related to:
1. GTK file handling system initialization
2. Interaction between FFmpeg and GTK libraries
3. File descriptor or resource handling conflicts

## Next Steps
Potential approaches to try:
1. Isolate GTK and FFmpeg operations more thoroughly
2. Try different GTK initialization patterns
3. Investigate GTK file system initialization
4. Consider using different window creation approach
5. Look into GTK application flags and settings


## Solution Found
The issue was resolved by modifying how GTK receives command line arguments:

1. Root Cause:
   - GTK's handling of command line arguments was causing the GLib-GIO error
   - The error occurred when passing video filename and coefficients to GTK initialization

2. Solution Implementation:
   - Created separate argc/argv for GTK initialization containing only program name
   - Processed command line arguments separately from GTK
   - Modified window creation pattern using signal_activate()

3. Key Changes:
```cpp
// Create modified argc/argv for GTK
char* new_argv[1];
new_argv[0] = argv[0];
int new_argc = 1;

// Initialize GTK with modified args
auto app = Gtk::Application::create("org.gtkmm.frame.viewer");
auto window = new FrameViewer(frames);
app->signal_activate().connect([app, window]() {
    app->add_window(*window);
    window->show();
});
return app->run(new_argc, new_argv);
```

4. Results:
   - Successfully displays GTK window
   - Shows both color and grayscale frames
   - No more GLib-GIO errors
   - All file operations working correctly

## Lessons Learned
1. GTK command line argument handling can conflict with file operations
2. Separating argument processing from GTK initialization can resolve GLib-GIO errors
3. The signal_activate() approach provides more reliable window creation
4. Debug output throughout the process helps identify exact error points