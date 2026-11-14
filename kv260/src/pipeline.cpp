#include "pipeline.hpp"

#include <sstream>
#include <stdexcept>

namespace faceshield {

std::string capture_pipeline(const StreamConfig& c) {
  std::ostringstream s;
  if (c.source == "mipi") {
    // Same source the AMD Smart Camera app uses for the AR1335 IAS module:
    // mediasrcbin configures the sensor, AP1302 ISP and MIPI receiver.
    s << "mediasrcbin media-device=" << c.media_device
      << " v4l2src0::io-mode=mmap v4l2src0::stride-align=256";
  } else {
    s << c.source << " ! videoconvert ! videoscale";
  }
  s << " ! video/x-raw,format=NV12,width=" << c.width
    << ",height=" << c.height << ",framerate=" << c.fps << "/1"
    // Keep only the newest frame: a slow frame never builds up a backlog.
    << " ! appsink name=sink max-buffers=1 drop=true sync=false";
  return s.str();
}

std::string output_pipeline(const StreamConfig& c, int width, int height) {
  std::ostringstream s;
  s << "appsrc name=src is-live=true format=time do-timestamp=true"
    << " caps=video/x-raw,format=NV12,width=" << width << ",height=" << height
    << ",framerate=" << c.fps << "/1"
    << " ! queue leaky=downstream max-size-buffers=2 ! ";
  if (c.encoder == "omx") {
    // Zynq UltraScale+ VCU: hardware H.264, keeps the ARM cores free.
    // periodicity-idr: without it the VCU sends one IDR at the start and
    // plain I-frames after, so h264parse never repeats SPS/PPS and a viewer
    // that connects later can never start decoding ("non-existing PPS").
    s << "omxh264enc gop-mode=basic gop-length=" << c.fps
      << " periodicity-idr=" << c.fps
      << " b-frames=0 control-rate=constant target-bitrate=" << c.bitrate_kbps
      << " ! video/x-h264,profile=high";
  } else if (c.encoder == "x264") {
    s << "videoconvert ! x264enc tune=zerolatency speed-preset=ultrafast"
      << " bitrate=" << c.bitrate_kbps << " key-int-max=" << c.fps;
  } else {
    // videoconvert passes NV12 through untouched if the encoder accepts it.
    s << "videoconvert ! " << c.encoder;
  }
  // Repeat SPS/PPS so a viewer can join at any keyframe.
  s << " ! h264parse config-interval=-1 ! ";
  if (c.transport == "tcp") {
    s << "mpegtsmux ! tcpserversink host=0.0.0.0 port=" << c.port
      << " sync=false sync-method=latest-keyframe";
  } else if (c.transport == "udp") {
    if (c.host.empty()) throw std::invalid_argument("--host is required for udp");
    s << "rtph264pay config-interval=-1 pt=96 ! udpsink host=" << c.host
      << " port=" << c.port << " sync=false";
  } else {
    throw std::invalid_argument("transport must be tcp or udp");
  }
  return s.str();
}

}  // namespace faceshield
