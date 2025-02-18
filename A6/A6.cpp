#include <gtkmm.h>
#include <iostream>

// FFmpeg includes
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

bool save_frame_as_ppm(const uint8_t* rgb_data, int width, int height, const char* filename) {
    FILE* fp = fopen(filename, "wb");
    if (!fp) {
        std::cerr << "Could not open " << filename << std::endl;
        return false;
    }

    fprintf(fp, "P6\n%d %d\n255\n", width, height);
    fwrite(rgb_data, 1, width * height * 3, fp);
    fclose(fp);
    return true;
}

bool save_frame_as_pgm(const uint8_t* rgb_data, int width, int height, 
                      float r_coeff, float g_coeff, float b_coeff, 
                      const char* filename) {
    FILE* fp = fopen(filename, "wb");
    if (!fp) {
        std::cerr << "Could not open " << filename << std::endl;
        return false;
    }

    fprintf(fp, "P5\n%d %d\n255\n", width, height);
    
    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            int pos = (i * width + j) * 3;
            uint8_t gray = static_cast<uint8_t>(
                r_coeff * rgb_data[pos] +
                g_coeff * rgb_data[pos + 1] +
                b_coeff * rgb_data[pos + 2]
            );
            fwrite(&gray, 1, 1, fp);
        }
    }
    
    fclose(fp);
    return true;
}


// First: FFmpeg frame extraction function (completely separate from GTK)
bool extract_frame(const char* filename, int target_frame, 
                  float r_coeff, float g_coeff, float b_coeff) {
    std::cout << "Starting frame extraction..." << std::endl;
    
    std::cout << "Opening: " << filename << ", frame: " << target_frame << std::endl;

    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, filename, nullptr, nullptr) < 0) {
        std::cerr << "Could not open source file" << std::endl;
        return 1;
    }

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        std::cerr << "Could not find stream information" << std::endl;
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    int video_stream_idx = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = i;
            break;
        }
    }

    if (video_stream_idx == -1) {
        std::cerr << "Could not find video stream" << std::endl;
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    AVStream* video_stream = fmt_ctx->streams[video_stream_idx];
    AVCodecParameters* codecParams = video_stream->codecpar;

    const AVCodec* decoder = avcodec_find_decoder(codecParams->codec_id);
    if (!decoder) {
        std::cerr << "Could not find decoder" << std::endl;
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    AVCodecContext* decoder_ctx = avcodec_alloc_context3(decoder);
    if (!decoder_ctx) {
        std::cerr << "Could not allocate decoder context" << std::endl;
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    if (avcodec_parameters_to_context(decoder_ctx, codecParams) < 0) {
        std::cerr << "Could not copy codec params to decoder context" << std::endl;
        avcodec_free_context(&decoder_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    if (avcodec_open2(decoder_ctx, decoder, nullptr) < 0) {
        std::cerr << "Could not open codec" << std::endl;
        avcodec_free_context(&decoder_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    int current_frame = 0;
    bool frame_found = false;
    uint8_t* rgb_buffer = nullptr;

    std::cout << "Searching for frame " << target_frame << std::endl;

    while (av_read_frame(fmt_ctx, packet) >= 0 && !frame_found) {
        if (packet->stream_index == video_stream_idx) {
            if (avcodec_send_packet(decoder_ctx, packet) < 0) {
                std::cerr << "Error sending packet for decoding" << std::endl;
                break;
            }

            while (avcodec_receive_frame(decoder_ctx, frame) >= 0) {
                if (current_frame == target_frame) {
                    std::cout << "Found target frame" << std::endl;
                    
                    int rgb_buffer_size = av_image_get_buffer_size(
                        AV_PIX_FMT_RGB24, 
                        frame->width, 
                        frame->height, 
                        1
                    );
                    rgb_buffer = (uint8_t*)av_malloc(rgb_buffer_size);

                    SwsContext* sws_ctx = sws_getContext(
                        frame->width, frame->height, (AVPixelFormat)frame->format,
                        frame->width, frame->height, AV_PIX_FMT_RGB24,
                        SWS_BILINEAR, nullptr, nullptr, nullptr
                    );

                    if (!sws_ctx) {
                        std::cerr << "Could not initialize SwsContext" << std::endl;
                        break;
                    }

                    AVFrame* rgb_frame = av_frame_alloc();
                    av_image_fill_arrays(rgb_frame->data, rgb_frame->linesize, 
                                       rgb_buffer,
                                       AV_PIX_FMT_RGB24, frame->width, 
                                       frame->height, 1);

                    sws_scale(sws_ctx, frame->data, frame->linesize, 0, 
                             frame->height, rgb_frame->data, rgb_frame->linesize);

                    save_frame_as_ppm(rgb_buffer, frame->width, frame->height, 
                                    "frame.ppm");
                    save_frame_as_pgm(rgb_buffer, frame->width, frame->height,
                                    r_coeff, g_coeff, b_coeff, "frame.pgm");

                    av_frame_free(&rgb_frame);
                    sws_freeContext(sws_ctx);
                    frame_found = true;
                    break;
                }
                current_frame++;
            }
        }
        av_packet_unref(packet);
        if (frame_found) break;
    }

    if (rgb_buffer) av_free(rgb_buffer);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder_ctx);
    avformat_close_input(&fmt_ctx);

    if (!frame_found) {
        std::cerr << "Could not find specified frame" << std::endl;
        return 1;
    }

    std::cout << "Successfully saved frame as PPM and PGM" << std::endl;
    
    // Return true if files were saved successfully
    return true;
}

// Second: GTK window class (unchanged from our working version)
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
    FrameViewer() {
        std::cout << "Creating Frame Viewer window..." << std::endl;
        
        set_title("Frame Viewer");
        set_default_size(800, 600);

        m_box.set_margin(10);
        m_box.set_spacing(20);
        set_child(m_box);

        // Color frame section
        std::cout << "Setting up color frame..." << std::endl;
        m_color_box.append(m_color_label);
        try {
            std::cout << "Attempting to load frame.ppm..." << std::endl;
            auto color_pixbuf = Gdk::Pixbuf::create_from_file("frame.ppm");
            if (color_pixbuf) {
                std::cout << "Successfully loaded color image" << std::endl;
                // Scale to a reasonable size
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
            }
        }
        catch (const Glib::Error& ex) {
            std::cerr << "Error loading color image: " << ex.what() << std::endl;
        }
        m_color_box.append(m_color_image);

        // Grayscale frame section
        std::cout << "Setting up grayscale frame..." << std::endl;
        m_gray_box.append(m_gray_label);
        try {
            std::cout << "Attempting to load frame.pgm..." << std::endl;
            auto gray_pixbuf = Gdk::Pixbuf::create_from_file("frame.pgm");
            if (gray_pixbuf) {
                std::cout << "Successfully loaded grayscale image" << std::endl;
                // Scale to match color image
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
            }
        }
        catch (const Glib::Error& ex) {
            std::cerr << "Error loading grayscale image: " << ex.what() << std::endl;
        }
        m_gray_box.append(m_gray_image);

        m_box.append(m_color_box);
        m_box.append(m_gray_box);
        
        std::cout << "Frame Viewer window setup complete" << std::endl;
    }
};


int main(int argc, char* argv[]) {
    if (argc != 6) {
        std::cerr << "Usage: " << argv[0] << " <video_file> <frame_number> <r_coeff> <g_coeff> <b_coeff>" << std::endl;
        return 1;
    }

    // First phase: Extract frame using FFmpeg
    if (!extract_frame(argv[1], std::stoi(argv[2]), 
                      std::stof(argv[3]), std::stof(argv[4]), std::stof(argv[5]))) {
        std::cerr << "Frame extraction failed" << std::endl;
        return 1;
    }

    std::cout << "Frame extraction complete, starting GTK application..." << std::endl;

    // Second phase: GTK display (exactly as in our working version)
    auto app = Gtk::Application::create("org.gtkmm.pixel.viewer");
    return app->make_window_and_run<FrameViewer>(argc, argv);
}

// Compile with:
// g++ -o A6 A6.cpp `pkg-config --cflags --libs gtkmm-4.0` -lavformat -lavcodec -lavutil -lswscale

// Run with:
// ./A6 <video_file> <frame_number> <r_coeff> <g_coeff> <b_coeff>
// Example: ./A6 1.mp4 10 0.299 0.587 0.114

