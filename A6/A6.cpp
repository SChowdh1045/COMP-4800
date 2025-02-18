#include <gtkmm.h>
#include <cairomm/context.h>
#include <cairomm/surface.h>
#include <iostream>
#include <filesystem>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

bool save_frame_as_png(const uint8_t* rgb_data, int width, int height, 
                      const char* filename, bool grayscale = false,
                      float r_coeff = 0.0f, float g_coeff = 0.0f, float b_coeff = 0.0f) {
    auto surface = Cairo::ImageSurface::create(Cairo::Surface::Format::RGB24, width, height);
    auto cr = Cairo::Context::create(surface);
    
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int src_pos = (y * width + x) * 3;
            if (grayscale) {
                uint8_t gray = static_cast<uint8_t>(
                    r_coeff * rgb_data[src_pos] +
                    g_coeff * rgb_data[src_pos + 1] +
                    b_coeff * rgb_data[src_pos + 2]
                );
                cr->set_source_rgb(gray/255.0, gray/255.0, gray/255.0);
            } else {
                cr->set_source_rgb(
                    rgb_data[src_pos]/255.0,
                    rgb_data[src_pos + 1]/255.0,
                    rgb_data[src_pos + 2]/255.0
                );
            }
            cr->rectangle(x, y, 1, 1);
            cr->fill();
        }
    }
    
    try {
        surface->write_to_png(filename);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error saving PNG: " << e.what() << std::endl;
        return false;
    }
}

class FrameViewer : public Gtk::Window {
private:
    Gtk::Box m_box{Gtk::Orientation::HORIZONTAL};
    Gtk::Box m_color_box{Gtk::Orientation::VERTICAL};
    Gtk::Box m_gray_box{Gtk::Orientation::VERTICAL};
    Gtk::Image m_color_image;
    Gtk::Image m_gray_image;
    Gtk::Label m_color_label{"Color Frame"};
    Gtk::Label m_gray_label{"Grayscale Frame"};
    
    Glib::RefPtr<Gdk::Pixbuf> m_color_pixbuf;
    Glib::RefPtr<Gdk::Pixbuf> m_gray_pixbuf;

protected:
    void on_draw_color(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height) {
        if (m_color_pixbuf) {
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
            double scale = std::min(
                (double)width / m_gray_pixbuf->get_width(),
                (double)height / m_gray_pixbuf->get_height()
            );
            cr->scale(scale, scale);
            Gdk::Cairo::set_source_pixbuf(cr, m_gray_pixbuf, 0, 0);
            cr->paint();
        }
    }

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
            std::cout << "Attempting to load frame_color.png..." << std::endl;
            auto color_pixbuf = Gdk::Pixbuf::create_from_file("frame_color.png");
            if (color_pixbuf) {
                std::cout << "Successfully loaded color image" << std::endl;
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
            std::cout << "Attempting to load frame_gray.png..." << std::endl;
            auto gray_pixbuf = Gdk::Pixbuf::create_from_file("frame_gray.png");
            if (gray_pixbuf) {
                std::cout << "Successfully loaded grayscale image" << std::endl;
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

// class FrameViewer : public Gtk::Window {
// private:
//     Gtk::Box m_box{Gtk::Orientation::HORIZONTAL};
//     Gtk::Box m_color_box{Gtk::Orientation::VERTICAL};
//     Gtk::Box m_gray_box{Gtk::Orientation::VERTICAL};
//     Gtk::Image m_color_image;
//     Gtk::Image m_gray_image;
//     Gtk::Label m_color_label{"Color Frame"};
//     Gtk::Label m_gray_label{"Grayscale Frame"};

// public:
//     FrameViewer() {
//         std::cout << "Creating Frame Viewer window..." << std::endl;
        
//         set_title("Frame Viewer");
//         set_default_size(800, 600);

//         m_box.set_margin(10);
//         m_box.set_spacing(20);
//         set_child(m_box);

//         // Color frame section
//         std::cout << "Setting up color frame..." << std::endl;
//         m_color_box.append(m_color_label);
//         try {
//             std::cout << "Attempting to load frame_color.png..." << std::endl;
//             auto color_pixbuf = Gdk::Pixbuf::create_from_file("frame_color.png");
//             if (color_pixbuf) {
//                 std::cout << "Successfully loaded color image" << std::endl;
//                 int max_height = 500;
//                 if (color_pixbuf->get_height() > max_height) {
//                     double scale = (double)max_height / color_pixbuf->get_height();
//                     auto scaled = color_pixbuf->scale_simple(
//                         color_pixbuf->get_width() * scale,
//                         max_height,
//                         Gdk::InterpType::BILINEAR
//                     );
//                     m_color_image.set(scaled);
//                 } else {
//                     m_color_image.set(color_pixbuf);
//                 }
//             }
//         }
//         catch (const Glib::Error& ex) {
//             std::cerr << "Error loading color image: " << ex.what() << std::endl;
//         }
//         m_color_box.append(m_color_image);

//         // Grayscale frame section
//         std::cout << "Setting up grayscale frame..." << std::endl;
//         m_gray_box.append(m_gray_label);
//         try {
//             std::cout << "Attempting to load frame_gray.png..." << std::endl;
//             auto gray_pixbuf = Gdk::Pixbuf::create_from_file("frame_gray.png");
//             if (gray_pixbuf) {
//                 std::cout << "Successfully loaded grayscale image" << std::endl;
//                 int max_height = 500;
//                 if (gray_pixbuf->get_height() > max_height) {
//                     double scale = (double)max_height / gray_pixbuf->get_height();
//                     auto scaled = gray_pixbuf->scale_simple(
//                         gray_pixbuf->get_width() * scale,
//                         max_height,
//                         Gdk::InterpType::BILINEAR
//                     );
//                     m_gray_image.set(scaled);
//                 } else {
//                     m_gray_image.set(gray_pixbuf);
//                 }
//             }
//         }
//         catch (const Glib::Error& ex) {
//             std::cerr << "Error loading grayscale image: " << ex.what() << std::endl;
//         }
//         m_gray_box.append(m_gray_image);

//         m_box.append(m_color_box);
//         m_box.append(m_gray_box);
        
//         std::cout << "Frame Viewer window setup complete" << std::endl;
//     }
// };

int main(int argc, char* argv[]) {
    // Initialize Gtk before anything else
    gtk_init();
    std::cout << "GTK initialized" << std::endl;
    if (argc != 6) {
        std::cerr << "Usage: " << argv[0] << " <video_file> <frame_number> <r_coeff> <g_coeff> <b_coeff>" << std::endl;
        return 1;
    }

    // Process arguments and extract frame
    const char* filename = argv[1];
    int target_frame = std::stoi(argv[2]);
    float r_coeff = std::stof(argv[3]);
    float g_coeff = std::stof(argv[4]);
    float b_coeff = std::stof(argv[5]);

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

                    // Save frames
                    std::cout << "Saving color frame..." << std::endl;
                    save_frame_as_png(rgb_buffer, frame->width, frame->height, 
                                    "frame_color.png");
                    
                    std::cout << "Saving grayscale frame..." << std::endl;
                    save_frame_as_png(rgb_buffer, frame->width, frame->height,
                                    "frame_gray.png", true,
                                    r_coeff, g_coeff, b_coeff);

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

    std::cout << "Successfully saved frames as PNG" << std::endl;
    std::cout << "Starting GTK application..." << std::endl;

    auto app = Gtk::Application::create("org.gtkmm.pixel.viewer");
    return app->make_window_and_run<FrameViewer>(argc, argv);
}

// Compile with:
// g++ -o A6 A6.cpp `pkg-config --cflags --libs gtkmm-4.0` -lavformat -lavcodec -lavutil -lswscale

// Run with:
// ./A6 <video_file> <frame_number> <r_coeff> <g_coeff> <b_coeff>
// Example: ./A6 1.mp4 10 0.299 0.587 0.114