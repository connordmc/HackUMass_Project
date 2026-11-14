// Builds the GStreamer launch strings for capture and streaming.
#pragma once

#include <string>

namespace faceshield {

struct StreamConfig {
  // "mipi" = the IAS camera through the Smart Camera media pipeline.
  // Anything else is a GStreamer source fragment, e.g. "videotestsrc".
  std::string source = "mipi";
  std::string media_device = "/dev/media0";
  int width = 1920;
  int height = 1080;
  int fps = 30;

  // "omx" = the KV260's hardware VCU encoder, "x264" = software, anything
  // else is used verbatim as an encoder fragment that accepts NV12.
  std::string encoder = "omx";
  int bitrate_kbps = 8000;

  std::string transport = "tcp";  // "tcp" (MPEG-TS server) or "udp" (RTP)
  std::string host;               // laptop address, UDP only
  int port = 5000;
};

// Ends in "appsink name=sink"; delivers NV12 frames, newest only.
std::string capture_pipeline(const StreamConfig& config);

// Starts with "appsrc name=src"; takes censored NV12 frames of the given size.
std::string output_pipeline(const StreamConfig& config, int width, int height);

}  // namespace faceshield
