// FaceShield on the KV260: IAS camera -> DPU face detection -> faces filled
// (fail-closed) -> hardware H.264 -> network. Only scrubbed frames are encoded.
//
// --scrub cpu (default): the ARM cores fill face boxes, and black out frames
//   with no current face, before the frame is handed to the encoder.
// --scrub pl: the PL scrubber IP (pl/scrubber/) does the filling in hardware
//   on the clean capture path; this program only detects on the raw path and
//   commits boxes to the IP. Needs the kv260-faceshield firmware.

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <algorithm>
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
#include <vector>

#include <opencv2/imgproc.hpp>

#include "censor.hpp"
#include "detector.hpp"
#include "pipeline.hpp"
#include "pl_scrubber.hpp"
#include "pl_scrubber_regs.hpp"

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
  int max_age_ms = 250;
  std::string scrub = "cpu";
  std::string raw_source;            // pl: GStreamer source for raw frames
  std::string uio = "/dev/uio4";     // pl: scrubber registers
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
  --detector D       vitis (DPU, default) or full-frame (black out everything; bring-up)
  --model M          xmodel path or model name (default: Smart Camera densebox_640_360)
  --threshold T      face score threshold, lower = more faces caught (default 0.7)
  --margin M         extra fill around each face, fraction of size (default 0.25)
  --max-age MS       keep a face filled MS ms after the detector last saw it;
                     with no current face the whole frame is black (default 250)
  --max-failures N   exit after N detector errors in a row (default 30)
Scrubbing:
  --scrub S          cpu (default) or pl (hardware scrubber, kv260-faceshield
                     firmware; --source is then the clean, scrubbed stream)
  --raw-source S     pl only: GStreamer source for the raw detector stream,
                     e.g. "v4l2src device=/dev/video1 io-mode=mmap"
  --uio D            pl only: scrubber register device (default /dev/uio4)
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
    else if (a == "--max-age") o.max_age_ms = std::stoi(v);
    else if (a == "--scrub") o.scrub = v;
    else if (a == "--raw-source") o.raw_source = v;
    else if (a == "--uio") o.uio = v;
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
  if (!(o.margin >= 0) || o.max_age_ms <= 0 || o.max_failures < 1)
    throw std::invalid_argument("invalid margin, max-age or max-failures");
  if (o.scrub != "cpu" && o.scrub != "pl")
    throw std::invalid_argument("--scrub must be cpu or pl");
  if (o.scrub == "pl" && o.raw_source.empty())
    throw std::invalid_argument("--scrub pl needs --raw-source");
  if (o.scrub == "pl" && !pl_regs::kGenerated)
    throw std::invalid_argument(
        "--scrub pl: src/pl_scrubber_regs.hpp holds predicted offsets; "
        "regenerate it from the HLS export (see pl/README.md) and rebuild");
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

// Waits for the next sample. Returns nullptr on end-of-stream or Ctrl+C;
// throws on a pipeline error or if no frame arrives for 10 s.
GstSample* pull_sample(GstElement* pipeline, GstElement* sink,
                       const char* what) {
  const auto start = Clock::now();
  while (!g_stop) {
    if (GstSample* sample =
            gst_app_sink_try_pull_sample(GST_APP_SINK(sink), GST_SECOND))
      return sample;
    if (poll_bus(pipeline, what) || gst_app_sink_is_eos(GST_APP_SINK(sink))) {
      std::cerr << what << " stream ended\n";
      return nullptr;
    }
    if (ms_since(start) > 10000)
      throw std::runtime_error(std::string("no ") + what +
                               " frames for 10 s; run check_board.sh");
  }
  return nullptr;
}

// Read-only, stride-aware view of a mapped NV12 sample (no copy).
struct MappedNv12 {
  GstVideoFrame frame;
  Nv12View view;
  explicit MappedNv12(GstSample* sample) {
    GstVideoInfo info;
    if (!gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) ||
        GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_NV12)
      throw std::runtime_error("raw stream did not deliver NV12 frames");
    if (!gst_video_frame_map(&frame, &info, gst_sample_get_buffer(sample),
                             GST_MAP_READ))
      throw std::runtime_error("could not map raw frame");
    view = {static_cast<uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0)),
            GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0),
            static_cast<uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 1)),
            GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1),
            GST_VIDEO_FRAME_WIDTH(&frame), GST_VIDEO_FRAME_HEIGHT(&frame)};
  }
  ~MappedNv12() { gst_video_frame_unmap(&frame); }
  MappedNv12(const MappedNv12&) = delete;
  MappedNv12& operator=(const MappedNv12&) = delete;
};

// Runs the detector and returns the boxes that are current for this frame.
std::vector<Box> detect_current(FaceDetector& detector, BoxHold& hold,
                                const Nv12View& view, Clock::time_point at) {
  const cv::Size det_size = detector.input_size();
  const cv::Mat small =
      small_bgr_from_nv12(view, det_size.width, det_size.height);
  return hold.update(detector.detect(small, cv::Size(view.width, view.height)),
                     at);
}

int run(const Options& o) {
  std::unique_ptr<FaceDetector> detector =
      o.detector == "vitis"        ? make_vitis_detector(o.model, o.threshold)
      : o.detector == "full-frame" ? make_full_frame_detector()
      : throw std::invalid_argument("detector must be vitis or full-frame");
  const cv::Size det_size = detector->input_size();
  std::cerr << "detector: " << o.detector << " input " << det_size.width << "x"
            << det_size.height << ", scrub: " << o.scrub << "\n";
  const bool pl = o.scrub == "pl";

  // In pl mode, opening the scrubber first means a missing IP fails before
  // anything is captured.
  std::unique_ptr<PlScrubber> pl_scrubber;
  if (pl) {
    const int max_age_frames =
        std::max(1, static_cast<int>(o.max_age_ms * o.fps / 1000));
    pl_scrubber = std::make_unique<PlScrubber>(open_uio_registers(o.uio),
                                               o.width, o.height,
                                               max_age_frames);
  }

  // cpu: the camera stream, scrubbed here. pl: the clean stream, already
  // scrubbed by hardware before it reached memory.
  PipelinePtr capture = launch(capture_pipeline(o), "capture");
  GstElement* sink = child(capture.get(), "sink");
  play(capture.get(), "camera capture");

  PipelinePtr raw_capture;
  GstElement* raw_sink = nullptr;
  if (pl) {
    StreamConfig raw = o;
    raw.source = o.raw_source;
    raw_capture = launch(capture_pipeline(raw), "raw capture");
    raw_sink = child(raw_capture.get(), "sink");
    play(raw_capture.get(), "raw capture");
  }

  PipelinePtr output;
  GstElement* src = nullptr;
  int out_w = 0, out_h = 0;
  BoxHold hold(std::chrono::milliseconds(o.max_age_ms), 0.3f);
  int failures = 0;
  long frames = 0, window_frames = 0, window_faces = 0, window_blanked = 0;
  long window_skipped = 0;
  double window_det_ms = 0, window_total_ms = 0;
  auto window_start = Clock::now();
  double fps = 0;
  int exit_code = 0;
  uint32_t pl_frames_seen = 0;

  // A detector error is handled the same way in both modes: fail closed.
  auto on_detect_error = [&](const std::exception& e) {
    hold.clear();
    std::cerr << "detection failed, frame blanked: " << e.what() << "\n";
    if (++failures >= o.max_failures) {
      std::cerr << "too many detection failures, stopping\n";
      g_stop = true;
      exit_code = 2;
    }
  };

  while (!g_stop && (o.max_frames <= 0 || frames < o.max_frames)) {
    double det_ms = 0;
    size_t faces = 0;
    bool blanked = false;

    if (pl) {
      // Detect on the raw stream; only box coordinates reach the hardware.
      GstSample* raw_sample =
          pull_sample(raw_capture.get(), raw_sink, "raw camera");
      if (!raw_sample) break;
      const auto captured_at = Clock::now();
      try {
        const auto td = Clock::now();
        std::vector<cv::Rect> regions;
        {
          MappedNv12 raw(raw_sample);
          if (raw.view.width != o.width || raw.view.height != o.height)
            throw std::runtime_error(
                "raw stream size differs from --width/--height");
          for (const Box& b :
               detect_current(*detector, hold, raw.view, captured_at)) {
            cv::Rect r;
            if (expand_and_clip(b, o.margin, raw.view.width, raw.view.height,
                                r))
              regions.push_back(r);
          }
        }
        det_ms = ms_since(td);
        faces = regions.size();
        blanked = regions.empty();
        if (!pl_scrubber->commit(regions)) ++window_skipped;
        failures = 0;
      } catch (const std::exception& e) {
        gst_sample_unref(raw_sample);
        // Tell the hardware "no face" so it blanks; if that also fails, the
        // hardware blanks on its own once the last commit is max-age old.
        try { pl_scrubber->commit({}); } catch (...) {}
        blanked = true;
        on_detect_error(e);
        raw_sample = nullptr;
      }
      if (raw_sample) gst_sample_unref(raw_sample);
    }

    GstSample* sample = pull_sample(capture.get(), sink, "camera");
    if (!sample) break;
    const auto t0 = Clock::now();
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
    if (!pl) {
      try {
        const auto td = Clock::now();
        const std::vector<Box> boxes =
            detect_current(*detector, hold, view, t0);
        det_ms = ms_since(td);
        faces = boxes.size();
        // Fail closed: fills every face, or blacks out the whole frame if
        // no face is current.
        blanked = !scrub_nv12(view, boxes, o.margin);
        failures = 0;
      } catch (const std::exception& e) {
        fill_black(view);  // never send a frame we could not scrub
        blanked = true;
        on_detect_error(e);
      }
    }
    if (o.overlay) {
      char text[112];
      std::snprintf(text, sizeof text,
                    "FaceShield KV260 [%s]  %.1f fps  %zu face%s%s",
                    o.scrub.c_str(), fps, faces, faces == 1 ? "" : "s",
                    blanked ? "  (blanked)" : "");
      cv::Mat y = view.y_plane();
      cv::putText(y, text, {20, 50}, cv::FONT_HERSHEY_SIMPLEX, h / 900.0,
                  cv::Scalar(235), 2, cv::LINE_AA);
    }
    gst_buffer_unmap(frame, &map);
    // The only place a frame leaves this program: scrubbed above (cpu) or by
    // the PL scrubber before it was captured (pl).
    if (gst_app_src_push_buffer(GST_APP_SRC(src), frame) != GST_FLOW_OK)
      throw std::runtime_error("stream output stopped accepting frames");
    poll_bus(output.get(), "stream output");

    ++frames;
    ++window_frames;
    window_faces += static_cast<long>(faces);
    window_blanked += blanked ? 1 : 0;
    window_det_ms += det_ms;
    window_total_ms += ms_since(t0);
    const double elapsed = ms_since(window_start);
    if (elapsed >= 5000) {
      fps = window_frames * 1000.0 / elapsed;
      std::fprintf(stderr,
                   "%.1f fps | detect %.1f ms | per-frame %.1f ms | %.1f "
                   "faces/frame | blanked %ld/%ld",
                   fps, window_det_ms / window_frames,
                   window_total_ms / window_frames,
                   static_cast<double>(window_faces) / window_frames,
                   window_blanked, window_frames);
      if (pl) {
        // The clean stream must be passing through the scrubber: if its
        // frame counter stopped while frames arrive, refuse to continue.
        const PlStatus st = pl_scrubber->status();
        std::fprintf(stderr, " | PL frames %u blanked %u skipped commits %ld",
                     st.frames, st.blanked_frames, window_skipped);
        if (st.frames == pl_frames_seen)
          throw std::runtime_error(
              "PL scrubber saw no frames while the clean stream delivered "
              "some: clean stream is not passing through the scrubber");
        pl_frames_seen = st.frames;
      }
      std::fputc('\n', stderr);
      window_frames = window_faces = window_blanked = window_skipped = 0;
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
  if (raw_sink) gst_object_unref(raw_sink);
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
