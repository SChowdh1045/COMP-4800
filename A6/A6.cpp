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
    AVCodecContext* dec_ctx = nullptr;
    SwsContext* sws_ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    int video_stream_idx = -1;
    int width = 0;
    int height = 0;

    bool open_codec_context(const char* filename) {
        // Opening input file
        if (avformat_open_input(&fmt_ctx, filename, nullptr, nullptr) < 0) {
            std::cerr << "Could not open source file" << std::endl;
            return false;
        }

        // Retrieving stream information
        if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
            std::cerr << "Could not find stream information" << std::endl;
            cleanup();
            return false;
        }

        // Dump input information
        av_dump_format(fmt_ctx, 0, filename, 0);

        // Finding the first video stream
        for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
            if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                video_stream_idx = i;
                break;
            }
        }

        if (video_stream_idx == -1) {
            std::cerr << "Could not find video stream" << std::endl;
            cleanup();
            return false;
        }

        // Get stream and codec parameters
        AVStream* video_stream = fmt_ctx->streams[video_stream_idx];
        AVCodecParameters* codecParams = video_stream->codecpar;
        width = codecParams->width;
        height = codecParams->height;

        // Finding decoder
        const AVCodec* decoder = avcodec_find_decoder(codecParams->codec_id);
        if (!decoder) {
            std::cerr << "Could not find decoder" << std::endl;
            cleanup();
            return false;
        }

        // Allocating decoder context
        dec_ctx = avcodec_alloc_context3(decoder);
        if (!dec_ctx) {
            std::cerr << "Could not allocate decoder context" << std::endl;
            cleanup();
            return false;
        }

        // Copy parameters to context
        if (avcodec_parameters_to_context(dec_ctx, codecParams) < 0) {
            std::cerr << "Could not copy codec params to decoder context" << std::endl;
            cleanup();
            return false;
        }

        // Initializing decoder
        if (avcodec_open2(dec_ctx, decoder, nullptr) < 0) {
            std::cerr << "Could not open codec" << std::endl;
            cleanup();
            return false;
        }

        // Allocating frame and packet
        frame = av_frame_alloc();
        pkt = av_packet_alloc();
        if (!frame || !pkt) {
            std::cerr << "Could not allocate frame or packet" << std::endl;
            cleanup();
            return false;
        }

        return true;
    }

    bool decode_frame(int target_frame, std::unique_ptr<uint8_t[]>& rgb_data) {
        int current_frame = 0;

        while (av_read_frame(fmt_ctx, pkt) >= 0) {
            if (pkt->stream_index == video_stream_idx) {
                int ret = avcodec_send_packet(dec_ctx, pkt);
                if (ret < 0) {
                    av_packet_unref(pkt);
                    return false;
                }

                while (ret >= 0) {
                    ret = avcodec_receive_frame(dec_ctx, frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                        break;
                    } else if (ret < 0) {
                        return false;
                    }

                    if (current_frame == target_frame) {
                        // Convert frame to RGB24
                        int rgb_buffer_size = av_image_get_buffer_size(
                            AV_PIX_FMT_RGB24, width, height, 1);
                        rgb_data = std::make_unique<uint8_t[]>(rgb_buffer_size);

                        sws_ctx = sws_getContext(
                            width, height, (AVPixelFormat)frame->format,
                            width, height, AV_PIX_FMT_RGB24,
                            SWS_BILINEAR, nullptr, nullptr, nullptr);

                        if (!sws_ctx) {
                            return false;
                        }

                        uint8_t* rgb_ptrs[4] = { rgb_data.get(), nullptr, nullptr, nullptr };
                        int rgb_linesizes[4] = { width * 3, 0, 0, 0 };
                        sws_scale(sws_ctx, frame->data, frame->linesize, 0,
                                height, rgb_ptrs, rgb_linesizes);

                        sws_freeContext(sws_ctx);
                        sws_ctx = nullptr;
                        av_packet_unref(pkt);
                        return true;
                    }
                    current_frame++;
                }
            }
            av_packet_unref(pkt);
        }
        return false;
    }

public:
    ~FrameExtractor() {
        cleanup();
    }

    FrameData extract_frames(const std::string& filename, int target_frame,
                            float r_coeff, float g_coeff, float b_coeff) {
        FrameData result;
        result.width = 0;
        result.height = 0;

        if (!open_codec_context(filename.c_str())) {
            return result;
        }

        result.width = width;
        result.height = height;

        std::unique_ptr<uint8_t[]> rgb_data;
        if (!decode_frame(target_frame, rgb_data)) {
            cleanup();
            return result;
        }

        // Store color data
        result.color_data.assign(rgb_data.get(), rgb_data.get() + (width * height * 3));

        // Creating grayscale data
        result.gray_data.resize(width * height * 3);
        for (int i = 0; i < width * height; i++) {
            uint8_t gray = static_cast<uint8_t>(
                r_coeff * rgb_data[i * 3] +
                g_coeff * rgb_data[i * 3 + 1] +
                b_coeff * rgb_data[i * 3 + 2]
            );
            result.gray_data[i * 3] = gray;
            result.gray_data[i * 3 + 1] = gray;
            result.gray_data[i * 3 + 2] = gray;
        }

        // Saving PPM/PGM files
        save_ppm(result.color_data.data(), "frame_color.ppm");
        save_pgm(result.color_data.data(), "frame_gray.pgm", r_coeff, g_coeff, b_coeff);

        cleanup();
        return result;
    }

private:
    void cleanup() {
        if (sws_ctx) sws_freeContext(sws_ctx);
        if (frame) av_frame_free(&frame);
        if (pkt) av_packet_free(&pkt);
        if (dec_ctx) avcodec_free_context(&dec_ctx);
        if (fmt_ctx) avformat_close_input(&fmt_ctx);
        
        sws_ctx = nullptr;
        frame = nullptr;
        pkt = nullptr;
        dec_ctx = nullptr;
        fmt_ctx = nullptr;
    }
    

    bool save_ppm(const uint8_t* rgb_data, const char* filename) {
        std::ofstream outfile(filename, std::ios::binary);
        if (!outfile) return false;
        outfile << "P6\n" << width << " " << height << "\n255\n";
        outfile.write(reinterpret_cast<const char*>(rgb_data), width * height * 3);
        return outfile.good();
    }

    bool save_pgm(const uint8_t* rgb_data, const char* filename,
                    float r_coeff, float g_coeff, float b_coeff) {
        std::ofstream outfile(filename, std::ios::binary);
        if (!outfile) return false;
        outfile << "P5\n" << width << " " << height << "\n255\n";
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
};


class FrameViewer : public Gtk::Window {
private:
    Gtk::Box m_box{Gtk::Orientation::HORIZONTAL};
    Gtk::Box m_color_box{Gtk::Orientation::VERTICAL};
    Gtk::Box m_gray_box{Gtk::Orientation::VERTICAL};
    Gtk::DrawingArea m_color_area;
    Gtk::DrawingArea m_gray_area;
    Gtk::Label m_color_label{"Color Frame"};
    Gtk::Label m_gray_label{"Grayscale Frame"};
    
    Glib::RefPtr<Gdk::Pixbuf> m_color_pixbuf;
    Glib::RefPtr<Gdk::Pixbuf> m_gray_pixbuf;

public:
    FrameViewer(const FrameData& frames) {
        std::cout << "Creating FrameViewer..." << std::endl;
        
        set_title("Frame Viewer");
        set_default_size(1600, 900);

        m_box.set_margin(10);
        m_box.set_spacing(20);
        set_child(m_box);

        // Setting up drawing areas
        m_color_area.set_content_width(750);
        m_color_area.set_content_height(750);
        m_gray_area.set_content_width(750);
        m_gray_area.set_content_height(750);

        // Setting up color image
        m_color_box.append(m_color_label);
        if (!frames.color_data.empty()) {
            try {
                m_color_pixbuf = Gdk::Pixbuf::create_from_data(
                    frames.color_data.data(),
                    Gdk::Colorspace::RGB,
                    false,
                    8,
                    frames.width,
                    frames.height,
                    frames.width * 3
                );
                
                m_color_area.set_draw_func(sigc::mem_fun(*this, &FrameViewer::on_draw_color));
            }
            catch (const Glib::Error& ex) {
                std::cerr << "Error creating color Pixbuf: " << ex.what() << std::endl;
            }
        }
        m_color_box.append(m_color_area);

        // Setting up grayscale image
        m_gray_box.append(m_gray_label);
        if (!frames.gray_data.empty()) {
            try {
                m_gray_pixbuf = Gdk::Pixbuf::create_from_data(
                    frames.gray_data.data(),
                    Gdk::Colorspace::RGB,
                    false,
                    8,
                    frames.width,
                    frames.height,
                    frames.width * 3
                );
                
                m_gray_area.set_draw_func(sigc::mem_fun(*this, &FrameViewer::on_draw_gray));
            }
            catch (const Glib::Error& ex) {
                std::cerr << "Error creating grayscale Pixbuf: " << ex.what() << std::endl;
            }
        }
        m_gray_box.append(m_gray_area);

        m_box.append(m_color_box);
        m_box.append(m_gray_box);
    }

protected:
    void on_draw_color(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height) {
        if (m_color_pixbuf) {
            // Scale image to fit while maintaining aspect ratio
            double scale = std::min(
                (double)width / m_color_pixbuf->get_width(),
                (double)height / m_color_pixbuf->get_height()
            );
            
            cr->scale(scale, scale);
            Gdk::Cairo::set_source_pixbuf(cr, m_color_pixbuf, 0, 0);
            cr->paint();
        }
    }

    void on_draw_gray(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height) {
        if (m_gray_pixbuf) {
            // Scale image to fit while maintaining aspect ratio
            double scale = std::min(
                (double)width / m_gray_pixbuf->get_width(),
                (double)height / m_gray_pixbuf->get_height()
            );
            
            cr->scale(scale, scale);
            Gdk::Cairo::set_source_pixbuf(cr, m_gray_pixbuf, 0, 0);
            cr->paint();
        }
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

    // Parsing arguments
    const char* filename = argv[1];
    int target_frame = std::stoi(argv[2]);
    float r_coeff = std::stof(argv[3]);
    float g_coeff = std::stof(argv[4]);
    float b_coeff = std::stof(argv[5]);

    std::cout << "Arguments parsed successfully" << std::endl;

    // To extract frames into memory
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
    // This is to fix the bug where it says: GLib-GIO-CRITICAL **: 01:33:48.773: This application can not open files.
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