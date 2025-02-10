I'm on Windows so I used MSYS2 environment with MinGW-w64

Compile the C++ file with this command: g++ -o A5 A5.cpp `pkg-config --cflags --libs gtkmm-4.0`
Then run it: ./A5

HOW THE PROGRAM WORKS:
- Load an image from your computer by pressing the 'load' button
- Select a color from the image by left-clicking anywhere on the image
- Switch to paint mode by pressing the 'paint' button
- You can left-click + drag your mouse around the image to paint
- Select a new color by pressing 'getcolor' button ; then start painting again by pressing 'paint' button
- You can undo and redo your actions
- You can save this new image to you computer by pressing the 'save' button
- Or load a new image and start all over again