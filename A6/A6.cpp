#include <gtkmm.h>
#include <iostream>
#include <memory>
#include <vector>
#include <fstream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

// Structure to hold frame data
struct FrameData {
    std::vector<guint8> color_data;
    std::vector<guint8> gray_data;
    int width;
    int height;
};


class FrameExtractor {
private:
    AVFormatContext* fmt_ctx = nullptr;
    AVCodecContext* decoder_ctx = nullptr;
    SwsContext* sws_ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    int video_stream_idx = -1;
    int width = 0;
    int height = 0;

public:
    ~FrameExtractor() {
        cleanup();
    }

    FrameData extract_frames(const std::string& filename, int target_frame,
                            float r_coeff, float g_coeff, float b_coeff) {
        FrameData result;
        result.width = 0;
        result.height = 0;

        if (!open_video(filename)) {
            return result;
        }

        result.width = width;
        result.height = height;

        bool success = false;
        auto rgb_data = extract_frame(target_frame, success);
        if (!success || !rgb_data) {
            cleanup();
            return result;
        }

        // Store color data
        result.color_data.assign(rgb_data.get(), rgb_data.get() + (width * height * 3));

        // Create grayscale data
        result.gray_data.resize(width * height * 3);  // Using RGB format for compatibility
        for (int i = 0; i < width * height; i++) {
            uint8_t gray = static_cast<uint8_t>(
                r_coeff * rgb_data[i * 3] +
                g_coeff * rgb_data[i * 3 + 1] +
                b_coeff * rgb_data[i * 3 + 2]
            );
            result.gray_data[i * 3] = gray;     // R
            result.gray_data[i * 3 + 1] = gray; // G
            result.gray_data[i * 3 + 2] = gray; // B
        }

        // Save PPM/PGM files as required by assignment
        save_ppm(result.color_data.data(), "frame_color.ppm");
        save_pgm(result.color_data.data(), "frame_gray.pgm", r_coeff, g_coeff, b_coeff);

        cleanup();
        return result;
    }

private:
    bool open_video(const std::string& filename) {
        cleanup();  // Ensure clean state

        if (avformat_open_input(&fmt_ctx, filename.c_str(), nullptr, nullptr) < 0) {
            std::cerr << "Could not open source file" << std::endl;
            return false;
        }

        if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
            std::cerr << "Could not find stream information" << std::endl;
            return false;
        }

        // Find video stream
        for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
            if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                video_stream_idx = i;
                break;
            }
        }

        if (video_stream_idx == -1) {
            std::cerr << "Could not find video stream" << std::endl;
            return false;
        }

        AVStream* video_stream = fmt_ctx->streams[video_stream_idx];
        AVCodecParameters* codecParams = video_stream->codecpar;
        width = codecParams->width;
        height = codecParams->height;

        const AVCodec* decoder = avcodec_find_decoder(codecParams->codec_id);
        if (!decoder) {
            std::cerr << "Could not find decoder" << std::endl;
            return false;
        }

        decoder_ctx = avcodec_alloc_context3(decoder);
        if (!decoder_ctx) {
            std::cerr << "Could not allocate decoder context" << std::endl;
            return false;
        }

        if (avcodec_parameters_to_context(decoder_ctx, codecParams) < 0) {
            std::cerr << "Could not copy codec params to decoder context" << std::endl;
            return false;
        }

        if (avcodec_open2(decoder_ctx, decoder, nullptr) < 0) {
            std::cerr << "Could not open codec" << std::endl;
            return false;
        }

        frame = av_frame_alloc();
        packet = av_packet_alloc();
        
        return true;
    }

    std::unique_ptr<uint8_t[]> extract_frame(int target_frame, bool& success) {
        success = false;
        int current_frame = 0;

        while (av_read_frame(fmt_ctx, packet) >= 0) {
            if (packet->stream_index == video_stream_idx) {
                if (avcodec_send_packet(decoder_ctx, packet) < 0) {
                    av_packet_unref(packet);
                    continue;
                }

                while (avcodec_receive_frame(decoder_ctx, frame) >= 0) {
                    if (current_frame == target_frame) {
                        // Convert frame to RGB24
                        int rgb_buffer_size = av_image_get_buffer_size(
                            AV_PIX_FMT_RGB24, width, height, 1);
                        auto rgb_data = std::make_unique<uint8_t[]>(rgb_buffer_size);

                        sws_ctx = sws_getContext(
                            width, height, (AVPixelFormat)frame->format,
                            width, height, AV_PIX_FMT_RGB24,
                            SWS_BILINEAR, nullptr, nullptr, nullptr);

                        if (!sws_ctx) {
                            av_packet_unref(packet);
                            return nullptr;
                        }

                        uint8_t* rgb_ptrs[4] = { rgb_data.get(), nullptr, nullptr, nullptr };
                        int rgb_linesizes[4] = { width * 3, 0, 0, 0 };
                        sws_scale(sws_ctx, frame->data, frame->linesize, 0,
                                height, rgb_ptrs, rgb_linesizes);

                        sws_freeContext(sws_ctx);
                        sws_ctx = nullptr;
                        av_packet_unref(packet);
                        success = true;
                        return rgb_data;
                    }
                    current_frame++;
                }
            }
            av_packet_unref(packet);
        }
        return nullptr;
    }

    bool save_ppm(const uint8_t* rgb_data, const char* filename) {
        std::ofstream outfile(filename, std::ios::binary);
        if (!outfile) {
            return false;
        }

        // Write PPM header
        outfile << "P6\n" << width << " " << height << "\n255\n";
        
        // Write RGB data
        outfile.write(reinterpret_cast<const char*>(rgb_data), width * height * 3);
        
        return outfile.good();
    }

    bool save_pgm(const uint8_t* rgb_data, const char* filename,
                    float r_coeff, float g_coeff, float b_coeff) {
        std::ofstream outfile(filename, std::ios::binary);
        if (!outfile) {
            return false;
        }

        // Write PGM header
        outfile << "P5\n" << width << " " << height << "\n255\n";
        
        // Convert RGB to grayscale and write
        for (int i = 0; i < width * height; i++) {
            int rgb_pos = i * 3;
            uint8_t gray = static_cast<uint8_t>(
                r_coeff * rgb_data[rgb_pos] +
                g_coeff * rgb_data[rgb_pos + 1] +
                b_coeff * rgb_data[rgb_pos + 2]
            );
            outfile.write(reinterpret_cast<const char*>(&gray), 1);
        }
        
        return outfile.good();
    }

    void cleanup() {
        if (sws_ctx) sws_freeContext(sws_ctx);
        if (frame) av_frame_free(&frame);
        if (packet) av_packet_free(&packet);
        if (decoder_ctx) avcodec_free_context(&decoder_ctx);
        if (fmt_ctx) avformat_close_input(&fmt_ctx);
        
        sws_ctx = nullptr;
        frame = nullptr;
        packet = nullptr;
        decoder_ctx = nullptr;
        fmt_ctx = nullptr;
    }
};


class FrameViewer : public Gtk::Window {
private:
    Gtk::Box m_box{Gtk::Orientation::HORIZONTAL};
    Gtk::Box m_color_box{Gtk::Orientation::VERTICAL};
    Gtk::Box m_gray_box{Gtk::Orientation::VERTICAL};
    Gtk::Image m_color_image;
    Gtk::Image m_gray_image;
    Gtk::Label m_color_label{"Color Frame"};
    Gtk::Label m_gray_label{"Grayscale Frame"};

public:
    FrameViewer(const FrameData& frames) {
        std::cout << "Creating FrameViewer..." << std::endl;
        
        set_title("Frame Viewer");
        set_default_size(800, 600);

        std::cout << "Window properties set" << std::endl;

        m_box.set_margin(10);
        m_box.set_spacing(20);
        set_child(m_box);

        std::cout << "Box layout set up" << std::endl;

        // Set up color image
        m_color_box.append(m_color_label);
        std::cout << "Setting up color image..." << std::endl;
        
        if (!frames.color_data.empty()) {
            std::cout << "Creating color Pixbuf from data (" 
                    << frames.width << "x" << frames.height << ")" << std::endl;
                    
            try {
                auto color_pixbuf = Gdk::Pixbuf::create_from_data(
                    frames.color_data.data(),
                    Gdk::Colorspace::RGB,
                    false,
                    8,
                    frames.width,
                    frames.height,
                    frames.width * 3
                );

                std::cout << "Color Pixbuf created successfully" << std::endl;

                if (color_pixbuf) {
                    int max_height = 500;
                    if (color_pixbuf->get_height() > max_height) {
                        double scale = (double)max_height / color_pixbuf->get_height();
                        auto scaled = color_pixbuf->scale_simple(
                            color_pixbuf->get_width() * scale,
                            max_height,
                            Gdk::InterpType::BILINEAR
                        );
                        m_color_image.set(scaled);
                    } else {
                        m_color_image.set(color_pixbuf);
                    }
                    std::cout << "Color image set successfully" << std::endl;
                }
            } catch (const Glib::Error& ex) {
                std::cerr << "Error creating color Pixbuf: " << ex.what() << std::endl;
            }
        }
        m_color_box.append(m_color_image);

        // Set up grayscale image with similar debug output
        std::cout << "Setting up grayscale image..." << std::endl;
        m_gray_box.append(m_gray_label);
        if (!frames.gray_data.empty()) {
            std::cout << "Creating grayscale Pixbuf..." << std::endl;
            try {
                auto gray_pixbuf = Gdk::Pixbuf::create_from_data(
                    frames.gray_data.data(),
                    Gdk::Colorspace::RGB,
                    false,
                    8,
                    frames.width,
                    frames.height,
                    frames.width * 3
                );

                std::cout << "Grayscale Pixbuf created successfully" << std::endl;

                if (gray_pixbuf) {
                    int max_height = 500;
                    if (gray_pixbuf->get_height() > max_height) {
                        double scale = (double)max_height / gray_pixbuf->get_height();
                        auto scaled = gray_pixbuf->scale_simple(
                            gray_pixbuf->get_width() * scale,
                            max_height,
                            Gdk::InterpType::BILINEAR
                        );
                        m_gray_image.set(scaled);
                    } else {
                        m_gray_image.set(gray_pixbuf);
                    }
                    std::cout << "Grayscale image set successfully" << std::endl;
                }
            } catch (const Glib::Error& ex) {
                std::cerr << "Error creating grayscale Pixbuf: " << ex.what() << std::endl;
            }
        }
        m_gray_box.append(m_gray_image);

        m_box.append(m_color_box);
        m_box.append(m_gray_box);

        std::cout << "FrameViewer setup complete" << std::endl;
    }
};


int main(int argc, char* argv[]) {
    std::cout << "GTK compile-time version: "
              << GTK_MAJOR_VERSION << "."
              << GTK_MINOR_VERSION << "."
              << GTK_MICRO_VERSION << std::endl;

    if (argc != 6) {
        std::cerr << "Usage: " << argv[0] << " <video_file> <frame_number> <r_coeff> <g_coeff> <b_coeff>" << std::endl;
        return 1;
    }

    // Parse arguments
    const char* filename = argv[1];
    int target_frame = std::stoi(argv[2]);
    float r_coeff = std::stof(argv[3]);
    float g_coeff = std::stof(argv[4]);
    float b_coeff = std::stof(argv[5]);

    std::cout << "Arguments parsed successfully" << std::endl;

    // Extract frames into memory
    FrameExtractor extractor;
    std::cout << "Starting frame extraction..." << std::endl;
    FrameData frames = extractor.extract_frames(filename, target_frame, r_coeff, g_coeff, b_coeff);
    
    if (frames.width == 0 || frames.height == 0) {
        std::cerr << "Failed to extract frames" << std::endl;
        return 1;
    }
    
    std::cout << "Frames extracted successfully. Size: " << frames.width << "x" << frames.height << std::endl;
    std::cout << "Color data size: " << frames.color_data.size() << std::endl;
    std::cout << "Gray data size: " << frames.gray_data.size() << std::endl;

    // Create new argc/argv for GTK with just the program name
    char* new_argv[1];
    new_argv[0] = argv[0];
    int new_argc = 1;

    std::cout << "Creating application..." << std::endl;
    auto app = Gtk::Application::create("org.gtkmm.frame.viewer");
    std::cout << "Application created" << std::endl;

    try {
        std::cout << "Setting up window creation..." << std::endl;
        
        auto window = new FrameViewer(frames);
        app->signal_activate().connect([app, window]() {
            std::cout << "Activation signal received..." << std::endl;
            app->add_window(*window);
            window->show();
            std::cout << "Window created and shown" << std::endl;
        });

        std::cout << "Running application..." << std::endl;
        return app->run(new_argc, new_argv);
    } catch (const Glib::Error& ex) {
        std::cerr << "Glib::Error caught: " << ex.what() << std::endl;
        return 1;
    } catch (const std::exception& ex) {
        std::cerr << "std::exception caught: " << ex.what() << std::endl;
        return 1;
    }
}


// Compile with:
// g++ -o A6 A6.cpp `pkg-config --cflags --libs gtkmm-4.0` -lavformat -lavcodec -lavutil -lswscale

// Run with:
// ./A6 <video_file> <frame_number> <r_coeff> <g_coeff> <b_coeff>
// Example: ./A6 1.mp4 10 0.299 0.587 0.114