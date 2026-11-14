// FaceShield on the KV260: IAS camera -> DPU face detection -> blur on the
// board -> hardware H.264 -> network. Only censored frames are encoded.

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include <opencv2/imgproc.hpp>

#include "censor.hpp"
#include "detector.hpp"
#include "pipeline.hpp"

using namespace faceshield;
using Clock = std::chrono::steady_clock;

namespace {

const char* kSmartcamModel =
    "/opt/xilinx/kv260-smartcam/share/vitis_ai_library/models/"
    "densebox_640_360/densebox_640_360.xmodel";

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

struct Options : StreamConfig {
  std::string detector = "vitis";
  std::string model;
  float threshold = 0.7f;
  float margin = 0.25f;
  int hold = 5;
  int blocks = 8;
  int max_failures = 30;
  long max_frames = 0;
  bool overlay = false;
};

void usage() {
  std::cout <<
R"(Usage: faceshield_kv260 [options]

Capture:
  --source S         mipi (IAS camera, default) or a GStreamer source,
                     e.g. "v4l2src device=/dev/video2" or "videotestsrc is-live=true"
  --media-device D   media device for mipi (default /dev/media0)
  --width W --height H --fps F   (default 1920 1080 30)
Detection and censoring:
  --detector D       vitis (DPU, default) or full-frame (blur everything; bring-up)
  --model M          xmodel path or model name (default: Smart Camera densebox_640_360)
  --threshold T      face score threshold, lower = more faces caught (default 0.7)
  --margin M         extra blur around each face, fraction of size (default 0.25)
  --hold N           keep blurring a face N frames after it is lost (default 5)
  --blocks N         blur strength: face reduced to NxN samples (default 8)
  --max-failures N   exit after N detector errors in a row (default 30)
Streaming:
  --transport T      tcp (MPEG-TS server, default) or udp (RTP to --host)
  --host H           laptop IP for udp
  --port P           default 5000
  --encoder E        omx (VCU hardware, default), x264, or a GStreamer encoder
  --bitrate K        kbit/s (default 8000)
Other:
  --overlay          draw fps and face count on the stream
  --max-frames N     stop after N frames (testing)
)";
}

Options parse(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "-h" || a == "--help") { usage(); std::exit(0); }
    if (a == "--overlay") { o.overlay = true; continue; }
    if (i + 1 >= argc) throw std::invalid_argument("missing value for " + a);
    const std::string v = argv[++i];
    if (a == "--source") o.source = v;
    else if (a == "--media-device") o.media_device = v;
    else if (a == "--width") o.width = std::stoi(v);
    else if (a == "--height") o.height = std::stoi(v);
    else if (a == "--fps") o.fps = std::stoi(v);
    else if (a == "--detector") o.detector = v;
    else if (a == "--model") o.model = v;
    else if (a == "--threshold") o.threshold = std::stof(v);
    else if (a == "--margin") o.margin = std::stof(v);
    else if (a == "--hold") o.hold = std::stoi(v);
    else if (a == "--blocks") o.blocks = std::stoi(v);
    else if (a == "--max-failures") o.max_failures = std::stoi(v);
    else if (a == "--max-frames") o.max_frames = std::stol(v);
    else if (a == "--transport") o.transport = v;
    else if (a == "--host") o.host = v;
    else if (a == "--port") o.port = std::stoi(v);
    else if (a == "--encoder") o.encoder = v;
    else if (a == "--bitrate") o.bitrate_kbps = std::stoi(v);
    else throw std::invalid_argument("unknown option " + a);
  }
  if (o.width <= 0 || o.height <= 0 || o.width % 2 || o.height % 2)
    throw std::invalid_argument("width and height must be positive and even");
  if (o.fps <= 0) throw std::invalid_argument("fps must be positive");
  if (!(o.margin >= 0) || o.blocks < 1 || o.hold < 0 || o.max_failures < 1)
    throw std::invalid_argument("invalid margin, blocks, hold or max-failures");
  if (o.model.empty())
    o.model = std::ifstream(kSmartcamModel).good() ? kSmartcamModel
                                                    : "densebox_640_360";
  return o;
}

struct PipelineDeleter {
  void operator()(GstElement* p) const {
    gst_element_set_state(p, GST_STATE_NULL);
    gst_object_unref(p);
  }
};
using PipelinePtr = std::unique_ptr<GstElement, PipelineDeleter>;

PipelinePtr launch(const std::string& description, const char* what) {
  std::cerr << what << ": " << description << "\n";
  GError* error = nullptr;
  GstElement* p = gst_parse_launch(description.c_str(), &error);
  if (error) {
    const std::string msg = error->message;
    g_error_free(error);
    if (p) gst_object_unref(p);
    throw std::runtime_error(std::string(what) + " pipeline: " + msg);
  }
  return PipelinePtr(p);
}

GstElement* child(GstElement* pipeline, const char* name) {
  GstElement* e = gst_bin_get_by_name(GST_BIN(pipeline), name);
  if (!e) throw std::runtime_error(std::string("no element named ") + name);
  return e;  // caller unrefs
}

void play(GstElement* pipeline, const char* what) {
  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE)
    throw std::runtime_error(std::string("could not start ") + what);
}

// Throws on a pipeline error; returns true on end-of-stream.
bool poll_bus(GstElement* pipeline, const char* what) {
  GstBus* bus = gst_element_get_bus(pipeline);
  bool eos = false;
  while (GstMessage* m = gst_bus_pop_filtered(
             bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR |
                                              GST_MESSAGE_EOS))) {
    if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR) {
      GError* err = nullptr;
      gchar* dbg = nullptr;
      gst_message_parse_error(m, &err, &dbg);
      std::string msg = std::string(what) + ": " + err->message;
      if (dbg) msg += std::string(" (") + dbg + ")";
      g_error_free(err);
      g_free(dbg);
      gst_message_unref(m);
      gst_object_unref(bus);
      throw std::runtime_error(msg);
    }
    eos = true;
    gst_message_unref(m);
  }
  gst_object_unref(bus);
  return eos;
}

// Copies a (possibly stride-padded) NV12 frame into a tightly packed buffer.
GstBuffer* copy_nv12(GstSample* sample, int& width, int& height) {
  GstVideoInfo info;
  if (!gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) ||
      GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_NV12)
    throw std::runtime_error("camera did not deliver NV12 frames");
  GstVideoFrame in;
  if (!gst_video_frame_map(&in, &info, gst_sample_get_buffer(sample),
                           GST_MAP_READ))
    throw std::runtime_error("could not map camera frame");
  width = GST_VIDEO_FRAME_WIDTH(&in);
  height = GST_VIDEO_FRAME_HEIGHT(&in);
  const size_t luma = static_cast<size_t>(width) * height;
  GstBuffer* out = gst_buffer_new_allocate(nullptr, luma * 3 / 2, nullptr);
  GstMapInfo om;
  gst_buffer_map(out, &om, GST_MAP_WRITE);
  for (int plane = 0; plane < 2; ++plane) {
    const auto* src =
        static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&in, plane));
    const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&in, plane);
    uint8_t* dst = om.data + (plane ? luma : 0);
    const int rows = plane ? height / 2 : height;
    for (int r = 0; r < rows; ++r)
      std::memcpy(dst + static_cast<size_t>(r) * width,
                  src + static_cast<size_t>(r) * stride, width);
  }
  gst_buffer_unmap(out, &om);
  gst_video_frame_unmap(&in);
  return out;
}

double ms_since(Clock::time_point t) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

int run(const Options& o) {
  std::unique_ptr<FaceDetector> detector =
      o.detector == "vitis"        ? make_vitis_detector(o.model, o.threshold)
      : o.detector == "full-frame" ? make_full_frame_detector()
      : throw std::invalid_argument("detector must be vitis or full-frame");
  const cv::Size det_size = detector->input_size();
  std::cerr << "detector: " << o.detector << " input " << det_size.width << "x"
            << det_size.height << "\n";

  PipelinePtr capture = launch(capture_pipeline(o), "capture");
  GstElement* sink = child(capture.get(), "sink");
  play(capture.get(), "camera capture");

  PipelinePtr output;
  GstElement* src = nullptr;
  int out_w = 0, out_h = 0;
  BoxHold hold(o.hold, 0.3f);
  int failures = 0;
  long frames = 0, window_frames = 0, window_faces = 0;
  double window_det_ms = 0, window_total_ms = 0;
  auto window_start = Clock::now();
  auto last_frame = Clock::now();
  double fps = 0;
  int exit_code = 0;

  while (!g_stop && (o.max_frames <= 0 || frames < o.max_frames)) {
    GstSample* sample =
        gst_app_sink_try_pull_sample(GST_APP_SINK(sink), GST_SECOND);
    if (!sample) {
      if (poll_bus(capture.get(), "camera") ||
          gst_app_sink_is_eos(GST_APP_SINK(sink))) {
        std::cerr << "camera stream ended\n";
        break;
      }
      if (ms_since(last_frame) > 10000)
        throw std::runtime_error("no camera frames for 10 s; run check_board.sh");
      continue;
    }
    const auto t0 = Clock::now();
    last_frame = t0;
    int w = 0, h = 0;
    GstBuffer* frame = nullptr;
    try {
      frame = copy_nv12(sample, w, h);
    } catch (...) {
      gst_sample_unref(sample);
      throw;
    }
    gst_sample_unref(sample);  // release the camera buffer early

    if (!output) {
      if (w % 2 || h % 2) throw std::runtime_error("odd frame size");
      out_w = w;
      out_h = h;
      std::cerr << "camera delivers " << w << "x" << h << "\n";
      output = launch(output_pipeline(o, w, h), "output");
      src = child(output.get(), "src");
      play(output.get(), "stream output");
    } else if (w != out_w || h != out_h) {
      gst_buffer_unref(frame);
      throw std::runtime_error("camera resolution changed mid-stream");
    }

    GstMapInfo map;
    gst_buffer_map(frame, &map, GST_MAP_READWRITE);
    Nv12View view{map.data, w, map.data + static_cast<size_t>(w) * h, w, w, h};
    double det_ms = 0;
    size_t faces = 0;
    try {
      const auto td = Clock::now();
      const cv::Mat small =
          small_bgr_from_nv12(view, det_size.width, det_size.height);
      const std::vector<Box> boxes =
          hold.update(detector->detect(small, cv::Size(w, h)));
      det_ms = ms_since(td);
      faces = boxes.size();
      censor_nv12(view, boxes, o.margin, o.blocks);
      failures = 0;
    } catch (const std::exception& e) {
      // Fail closed: never send a frame we could not censor.
      fill_black(view);
      hold.clear();
      std::cerr << "detection failed, sending black frame: " << e.what()
                << "\n";
      if (++failures >= o.max_failures) {
        std::cerr << "too many detection failures, stopping\n";
        g_stop = true;
        exit_code = 2;
      }
    }
    if (o.overlay) {
      char text[96];
      std::snprintf(text, sizeof text, "FaceShield KV260  %.1f fps  %zu face%s",
                    fps, faces, faces == 1 ? "" : "s");
      cv::Mat y = view.y_plane();
      cv::putText(y, text, {20, 50}, cv::FONT_HERSHEY_SIMPLEX, h / 900.0,
                  cv::Scalar(235), 2, cv::LINE_AA);
    }
    gst_buffer_unmap(frame, &map);
    // The only place a frame leaves this program, after censoring above.
    if (gst_app_src_push_buffer(GST_APP_SRC(src), frame) != GST_FLOW_OK)
      throw std::runtime_error("stream output stopped accepting frames");
    poll_bus(output.get(), "stream output");

    ++frames;
    ++window_frames;
    window_faces += static_cast<long>(faces);
    window_det_ms += det_ms;
    window_total_ms += ms_since(t0);
    const double elapsed = ms_since(window_start);
    if (elapsed >= 5000) {
      fps = window_frames * 1000.0 / elapsed;
      std::fprintf(stderr,
                   "%.1f fps | detect %.1f ms | per-frame %.1f ms | %.1f "
                   "faces/frame\n",
                   fps, window_det_ms / window_frames,
                   window_total_ms / window_frames,
                   static_cast<double>(window_faces) / window_frames);
      window_frames = window_faces = 0;
      window_det_ms = window_total_ms = 0;
      window_start = Clock::now();
    }
  }

  if (src) {
    gst_app_src_end_of_stream(GST_APP_SRC(src));
    GstBus* bus = gst_element_get_bus(output.get());
    if (GstMessage* m = gst_bus_timed_pop_filtered(
            bus, 2 * GST_SECOND,
            static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR)))
      gst_message_unref(m);
    gst_object_unref(bus);
    gst_object_unref(src);
  }
  gst_object_unref(sink);
  std::cerr << "stopped after " << frames << " frames\n";
  return exit_code;
}

}  // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  try {
    return run(parse(argc, argv));
  } catch (const std::exception& e) {
    std::cerr << "faceshield_kv260: " << e.what() << "\n";
    return 1;
  }
}
