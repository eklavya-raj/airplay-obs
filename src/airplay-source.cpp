#include <obs-module.h>
#include <util/platform.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <atomic>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <arpa/inet.h>
#include <mutex>
#include <netinet/in.h>
#include <spawn.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <dlfcn.h>

extern char **environ;

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-airplay-source", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "AirPlay receiver source using UxPlay RTP and VideoToolbox decode";
}

namespace {

constexpr const char *kDefaultUxPlay = "uxplay";

enum class Codec {
	H264,
	H265,
};

struct ChildProcess {
	pid_t pid = -1;
	int output_fd = -1;
	bool running() const { return pid > 0; }
};

struct AirPlayReceiver {
	std::mutex lock;
	std::mutex video_queue_mutex;
	std::condition_variable video_queue_cv;
	std::thread decoder_thread;
	std::thread audio_decoder_thread;
	std::thread video_output_thread;
	std::thread uxplay_log_thread;
	std::atomic<bool> stopping{false};
	ChildProcess uxplay;

	std::string name = "OBS AirPlay";
	std::string uxplay_path = kDefaultUxPlay;
	std::string sdp_path;
	std::string audio_sdp_path;
	Codec codec = Codec::H264;
	int width = 1920;
	int height = 1080;
	int fps = 60;
	int rtp_port = 5000;
	int rtp_audio_port = 5002;
	uint32_t actual_width = 1920;
	uint32_t actual_height = 1080;
	bool auto_start = true;
	bool hardware_decode = true;
	uint64_t packet_count = 0;
	uint64_t decoded_count = 0;
	uint64_t output_count = 0;
	uint64_t audio_packet_count = 0;
	uint64_t audio_decoded_count = 0;
	uint64_t audio_output_count = 0;
	double active_fps = 0.0;
	uint64_t fps_start_time = 0;
	uint32_t fps_frame_count = 0;
	uint64_t last_output_time_ns = 0;
	std::vector<obs_source_t *> sources;
	std::vector<obs_source_t *> audio_sources;

	struct QueuedVideoFrame {
		std::vector<uint8_t> data_buf;
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t format = 0; // video_format
		uint32_t linesize[4] = {0};
		size_t plane_offsets[4] = {0};
		uint64_t timestamp = 0;
		bool full_range = false;
	};
	std::deque<QueuedVideoFrame> video_queue;
	std::mutex free_video_buffers_mutex;
	std::vector<std::vector<uint8_t>> free_video_buffers;
};

struct AirPlaySource {
	obs_source_t *source = nullptr;
};

static AirPlayReceiver g_receiver;
static std::thread g_http_thread;
static std::atomic<bool> g_http_stopping{false};
static int g_http_fd = -1;

struct DecoderResources {
	struct TimingState {
		AVRational time_base = {0, 1};
		int64_t origin_pts = AV_NOPTS_VALUE;
		uint64_t origin_ns = 0;
		uint64_t last_timestamp_ns = 0;
		uint64_t frame_interval_ns = 16666667;
		double smoothed_offset = 0.0;
		bool initialized = false;
	} timing;

	AVFormatContext *format = nullptr;
	AVCodecContext *codec = nullptr;
	AVBufferRef *hw_device = nullptr;
	AVPacket *packet = nullptr;
	AVFrame *frame = nullptr;
	AVFrame *sw_frame = nullptr;
	SwsContext *sws = nullptr;
	std::vector<uint8_t> bgra;

	~DecoderResources()
	{
		if (sws)
			sws_freeContext(sws);
		if (frame)
			av_frame_free(&frame);
		if (sw_frame)
			av_frame_free(&sw_frame);
		if (packet)
			av_packet_free(&packet);
		if (codec)
			avcodec_free_context(&codec);
		if (format)
			avformat_close_input(&format);
		if (hw_device)
			av_buffer_unref(&hw_device);
	}
};

static std::string av_err(int error)
{
	char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(error, buffer, sizeof(buffer));
	return buffer;
}

static std::string get_string(obs_data_t *settings, const char *key,
			      const char *fallback)
{
	const char *value = obs_data_get_string(settings, key);
	return value && *value ? value : fallback;
}

static Codec get_codec(obs_data_t *settings)
{
	const char *value = obs_data_get_string(settings, "codec");
	if (value && strcmp(value, "h265") == 0)
		return Codec::H265;
	return Codec::H264;
}

static const char *codec_id(Codec codec)
{
	return codec == Codec::H265 ? "h265" : "h264";
}

static AVCodecID av_codec_id(Codec codec)
{
	return codec == Codec::H265 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
}

static std::string make_sdp(int port, Codec codec)
{
	const char *rtp_codec = codec == Codec::H265 ? "H265" : "H264";
	std::ostringstream out;
	out << "v=0\n"
	    << "o=- 0 0 IN IP4 127.0.0.1\n"
	    << "s=OBS AirPlay Source\n"
	    << "c=IN IP4 127.0.0.1\n"
	    << "t=0 0\n"
	    << "m=video " << port << " RTP/AVP 96\n"
	    << "a=rtpmap:96 " << rtp_codec << "/90000\n";
	if (codec == Codec::H264)
		out << "a=fmtp:96 packetization-mode=1\n";
	return out.str();
}

static bool write_text_file(const std::string &path, const std::string &text)
{
	FILE *file = fopen(path.c_str(), "wb");
	if (!file)
		return false;
	const size_t written = fwrite(text.data(), 1, text.size(), file);
	const int close_status = fclose(file);
	return written == text.size() && close_status == 0;
}

static std::string make_temp_sdp_path(int port)
{
	std::ostringstream out;
	out << "/tmp/obs-airplay-source-" << getpid() << "-" << port << ".sdp";
	return out.str();
}

static void append_arg(std::vector<std::string> &args, const std::string &arg)
{
	args.emplace_back(arg);
}

static std::string join_args(const std::vector<std::string> &args)
{
	std::ostringstream out;
	for (const std::string &arg : args) {
		if (out.tellp() > 0)
			out << ' ';
		out << arg;
	}
	return out.str();
}

static bool spawn_child(const std::string &path,
			const std::vector<std::string> &args,
			ChildProcess &child)
{
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);

	int output_pipe[2] = {-1, -1};
	if (pipe(output_pipe) != 0) {
		blog(LOG_ERROR, "[obs-airplay] output pipe failed: %s",
		     strerror(errno));
		posix_spawn_file_actions_destroy(&actions);
		return false;
	}

	posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO);
	posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDERR_FILENO);
	posix_spawn_file_actions_addclose(&actions, output_pipe[0]);
	posix_spawn_file_actions_addclose(&actions, output_pipe[1]);

	std::vector<char *> argv;
	argv.reserve(args.size() + 2);
	argv.push_back(const_cast<char *>(path.c_str()));
	for (const std::string &arg : args)
		argv.push_back(const_cast<char *>(arg.c_str()));
	argv.push_back(nullptr);

	pid_t pid = -1;
	const bool search_path = path.find('/') == std::string::npos;
	const int status = search_path
				   ? posix_spawnp(&pid, path.c_str(), &actions,
						  nullptr, argv.data(), environ)
				   : posix_spawn(&pid, path.c_str(), &actions,
						 nullptr, argv.data(), environ);

	posix_spawn_file_actions_destroy(&actions);
	close(output_pipe[1]);

	if (status != 0) {
		close(output_pipe[0]);
		blog(LOG_ERROR, "[obs-airplay] failed to start %s: %s",
		     path.c_str(), strerror(status));
		return false;
	}

	child.pid = pid;
	child.output_fd = output_pipe[0];
	blog(LOG_INFO, "[obs-airplay] started %s %s", path.c_str(),
	     join_args(args).c_str());
	return true;
}

static void stop_child(ChildProcess &child)
{
	if (child.output_fd >= 0) {
		close(child.output_fd);
		child.output_fd = -1;
	}

	if (child.pid <= 0)
		return;

	kill(child.pid, SIGTERM);

	for (int i = 0; i < 20; ++i) {
		int status = 0;
		const pid_t result = waitpid(child.pid, &status, WNOHANG);
		if (result == child.pid) {
			child.pid = -1;
			return;
		}
		usleep(50000);
	}

	kill(child.pid, SIGKILL);
	waitpid(child.pid, nullptr, 0);
	child.pid = -1;
}

static void log_fd_loop(AirPlayReceiver *s, int fd, const char *prefix)
{
	std::string pending;
	char buffer[512];

	while (!s->stopping.load()) {
		const ssize_t n = read(fd, buffer, sizeof(buffer) - 1);
		if (n <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			break;
		}

		buffer[n] = '\0';
		pending.append(buffer);

		size_t newline = std::string::npos;
		while ((newline = pending.find('\n')) != std::string::npos) {
			std::string line = pending.substr(0, newline);
			pending.erase(0, newline + 1);
			if (!line.empty())
				blog(LOG_INFO, "[obs-airplay] %s: %s", prefix,
				     line.c_str());
		}
	}

	if (!pending.empty())
		blog(LOG_INFO, "[obs-airplay] %s: %s", prefix, pending.c_str());
}

static std::vector<std::string> build_uxplay_args(const AirPlayReceiver *s)
{
	std::vector<std::string> args;
	append_arg(args, "-n");
	append_arg(args, s->name);
	append_arg(args, "-nh");
	append_arg(args, "-s");
	append_arg(args, std::to_string(s->width) + "x" +
				 std::to_string(s->height) + "@" +
				 std::to_string(s->fps));
	append_arg(args, "-fps");
	append_arg(args, std::to_string(s->fps));
	
	// Disable local audio and video windows in UxPlay, routing only to the virtual ports
	// to maximize encoding/decoding efficiency and eliminate redundant rendering.
	append_arg(args, "-as");
	append_arg(args, "fakesink");
	append_arg(args, "-vs");
	append_arg(args, "fakesink");

	append_arg(args, "-artp");
	append_arg(args,
		   "pt=96 ! udpsink host=127.0.0.1 port=" +
			   std::to_string(s->rtp_audio_port) +
			   " sync=false async=false");
	append_arg(args, "-nohold");

	const bool h265 = s->codec == Codec::H265;
	if (h265)
		append_arg(args, "-h265");

	// GStreamer's rtphevcpay payloader does not support the 'config-interval' property.
	// We only include it in H.264's rtph264pay to prevent pipeline construction failure.
	std::string vrtp_pipeline;
	if (h265) {
		vrtp_pipeline = "pt=96 ! udpsink host=127.0.0.1 port=" +
				std::to_string(s->rtp_port) +
				" sync=false async=false";
	} else {
		vrtp_pipeline = "config-interval=1 pt=96 ! udpsink host=127.0.0.1 port=" +
				std::to_string(s->rtp_port) +
				" sync=false async=false";
	}

	append_arg(args, "-vrtp");
	append_arg(args, vrtp_pipeline);
	return args;
}

static int decode_interrupt(void *opaque)
{
	const AirPlayReceiver *s = static_cast<const AirPlayReceiver *>(opaque);
	return s->stopping.load() ? 1 : 0;
}

static enum AVPixelFormat choose_hw_format(AVCodecContext *,
					   const enum AVPixelFormat *formats)
{
	for (const enum AVPixelFormat *format = formats; *format != AV_PIX_FMT_NONE;
	     ++format) {
		if (*format == AV_PIX_FMT_VIDEOTOOLBOX)
			return *format;
	}
	return formats[0];
}

static int find_video_stream(AVFormatContext *format)
{
	int best = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr,
				      0);
	if (best >= 0)
		return best;

	for (unsigned i = 0; i < format->nb_streams; ++i) {
		if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
			return static_cast<int>(i);
	}

	return AVERROR_STREAM_NOT_FOUND;
}



static uint64_t get_video_timestamp_ns(AirPlayReceiver *s, DecoderResources &r, const AVFrame *decoded)
{
	(void)decoded;
	const uint64_t now_ns = os_gettime_ns();
	auto &timing = r.timing;
	
	const uint64_t target_interval_ns = 1000000000ULL / std::max(1, s->fps);
	uint64_t candidate = timing.last_timestamp_ns + target_interval_ns;
	
	if (!timing.initialized) {
		candidate = now_ns;
		timing.initialized = true;
	} else {
		const int64_t drift = static_cast<int64_t>(candidate) - static_cast<int64_t>(now_ns);
		if (std::abs(drift) > 100000000LL) { // 100ms
			candidate = now_ns;
		} else {
			// Proportional feedback to gently keep the timeline in sync with system clock without jitter
			candidate = candidate - (drift / 100);
		}
	}
	
	if (candidate > now_ns) {
		candidate = now_ns;
	}
	if (candidate <= timing.last_timestamp_ns) {
		candidate = timing.last_timestamp_ns + 1;
	}
	
	timing.last_timestamp_ns = candidate;
	return candidate;
}

static void output_video_frame_to_sources(AirPlayReceiver *s,
					 const AirPlayReceiver::QueuedVideoFrame &queued)
{
	// Frame rate capping logic based on smoothed frame timestamps
	if (s->fps < 60) {
		const uint64_t target_interval_ns = 1000000000ULL / std::max(1, s->fps);
		if (s->last_output_time_ns != 0) {
			const int64_t elapsed_ns = static_cast<int64_t>(queued.timestamp) - static_cast<int64_t>(s->last_output_time_ns);
			// Use a 70% threshold to decimate higher frame rates cleanly while fully tolerating network jitter
			if (elapsed_ns > 0 && elapsed_ns < static_cast<int64_t>(target_interval_ns * 7 / 10)) {
				return; // Drop this frame to respect the target FPS limit
			}
		}
		s->last_output_time_ns = queued.timestamp;
	} else {
		s->last_output_time_ns = 0; // Disable capping for 60fps to ensure zero frame drops
	}

	obs_source_frame frame = {};
	frame.width = queued.width;
	frame.height = queued.height;
	frame.timestamp = queued.timestamp;
	frame.format = static_cast<video_format>(queued.format);
	frame.full_range = queued.full_range;

	// Populate colorspace matrix and range for planar YUV formats to avoid a black screen
	if (frame.format == VIDEO_FORMAT_NV12 || frame.format == VIDEO_FORMAT_I420) {
		video_format_get_parameters_for_format(
			VIDEO_CS_709,
			VIDEO_RANGE_PARTIAL,
			frame.format,
			frame.color_matrix,
			frame.color_range_min,
			frame.color_range_max);
	}

	frame.data[0] = const_cast<uint8_t *>(queued.data_buf.data()) + queued.plane_offsets[0];
	frame.linesize[0] = queued.linesize[0];

	if (frame.format == VIDEO_FORMAT_NV12) {
		frame.data[1] = const_cast<uint8_t *>(queued.data_buf.data()) + queued.plane_offsets[1];
		frame.linesize[1] = queued.linesize[1];
	} else if (frame.format == VIDEO_FORMAT_I420) {
		frame.data[1] = const_cast<uint8_t *>(queued.data_buf.data()) + queued.plane_offsets[1];
		frame.linesize[1] = queued.linesize[1];
		frame.data[2] = const_cast<uint8_t *>(queued.data_buf.data()) + queued.plane_offsets[2];
		frame.linesize[2] = queued.linesize[2];
	} else if (frame.format == VIDEO_FORMAT_BGRA) {
		frame.data[0] = const_cast<uint8_t *>(queued.data_buf.data());
		frame.linesize[0] = queued.linesize[0];
	}

	obs_source_t *sources_buf[16];
	size_t num_sources = 0;
	{
		std::lock_guard<std::mutex> guard(s->lock);
		num_sources = std::min(s->sources.size(), size_t(16));
		for (size_t i = 0; i < num_sources; ++i)
			sources_buf[i] = s->sources[i];
	}

	for (size_t i = 0; i < num_sources; ++i)
		obs_source_output_video(sources_buf[i], &frame);

	s->output_count++;
	if (s->output_count == 1 || s->output_count % 120 == 0) {
		blog(LOG_INFO,
		     "[obs-airplay] output video frames=%llu size=%ux%u fps=%d",
		     static_cast<unsigned long long>(s->output_count), frame.width,
		     frame.height, s->fps);
	}
}

static void queue_video_frame(AirPlayReceiver *s,
			      AirPlayReceiver::QueuedVideoFrame &&frame)
{
	{
		std::lock_guard<std::mutex> guard(s->video_queue_mutex);
		// Cap queue size to bound latency (O(1) Space Complexity)
		if (s->video_queue.size() > 5) {
			s->video_queue.pop_front();
		}
		s->video_queue.push_back(std::move(frame));
	}
	s->video_queue_cv.notify_one();
}

static void video_output_loop(AirPlayReceiver *s)
{
	while (true) {
		AirPlayReceiver::QueuedVideoFrame queued;
		{
			std::unique_lock<std::mutex> lock(s->video_queue_mutex);
			s->video_queue_cv.wait(lock, [&] {
				return s->stopping.load() || !s->video_queue.empty();
			});
			if (s->stopping.load() && s->video_queue.empty())
				break;
			queued = std::move(s->video_queue.front());
			s->video_queue.pop_front();
		}

		if (queued.timestamp > os_gettime_ns())
			os_sleepto_ns(queued.timestamp);

		output_video_frame_to_sources(s, queued);
		
		// Recycle buffer (O(1) Time Complexity allocator)
		{
			std::lock_guard<std::mutex> guard(s->free_video_buffers_mutex);
			if (s->free_video_buffers.size() < 10) {
				s->free_video_buffers.push_back(std::move(queued.data_buf));
			}
		}
	}
}

static bool open_decoder(AirPlayReceiver *s, DecoderResources &r,
			 int &video_stream)
{
	r.format = avformat_alloc_context();
	if (!r.format) {
		blog(LOG_ERROR, "[obs-airplay] could not allocate AVFormatContext");
		return false;
	}

	r.format->interrupt_callback.callback = decode_interrupt;
	r.format->interrupt_callback.opaque = s;

	AVDictionary *options = nullptr;
	av_dict_set(&options, "protocol_whitelist", "file,udp,rtp", 0);
	av_dict_set(&options, "fflags", "nobuffer", 0);
	av_dict_set(&options, "flags", "low_delay", 0);
	av_dict_set(&options, "analyzeduration", "100000", 0);
	av_dict_set(&options, "probesize", "32768", 0);
	av_dict_set(&options, "reorder_queue_size", "0", 0);
	av_dict_set(&options, "buffer_size", "1048576", 0);
	av_dict_set(&options, "timeout", "100000", 0);
	av_dict_set(&options, "rw_timeout", "100000", 0);

	const AVInputFormat *input_format = av_find_input_format("sdp");
	int ret = avformat_open_input(&r.format, s->sdp_path.c_str(),
				      input_format, &options);
	av_dict_free(&options);
	if (ret < 0) {
		blog(LOG_ERROR, "[obs-airplay] avformat_open_input failed: %s",
		     av_err(ret).c_str());
		return false;
	}

	// Skip avformat_find_stream_info to avoid startup delay and timeouts when no stream is present.
	video_stream = find_video_stream(r.format);
	if (video_stream < 0) {
		blog(LOG_ERROR, "[obs-airplay] no video stream found in RTP SDP");
		return false;
	}

	AVStream *stream = r.format->streams[video_stream];
	r.timing.time_base = (stream->time_base.num > 0 && stream->time_base.den > 0)
				     ? stream->time_base
				     : AVRational{1, 90000};
	r.timing.frame_interval_ns =
		static_cast<uint64_t>(1000000000LL / std::max(1, s->fps));
	const AVCodec *decoder =
		avcodec_find_decoder(stream->codecpar->codec_id != AV_CODEC_ID_NONE
					     ? stream->codecpar->codec_id
					     : av_codec_id(s->codec));
	if (!decoder) {
		blog(LOG_ERROR, "[obs-airplay] no decoder available for %s",
		     codec_id(s->codec));
		return false;
	}

	r.codec = avcodec_alloc_context3(decoder);
	if (!r.codec) {
		blog(LOG_ERROR, "[obs-airplay] could not allocate AVCodecContext");
		return false;
	}

	ret = avcodec_parameters_to_context(r.codec, stream->codecpar);
	if (ret < 0) {
		blog(LOG_ERROR,
		     "[obs-airplay] avcodec_parameters_to_context failed: %s",
		     av_err(ret).c_str());
		return false;
	}

	// Manually initialize decoding context properties since we skipped stream probing
	if (r.codec->width == 0)
		r.codec->width = s->width;
	if (r.codec->height == 0)
		r.codec->height = s->height;
	r.codec->pix_fmt = AV_PIX_FMT_YUV420P;
	r.codec->time_base = {1, 90000};

	r.codec->flags |= AV_CODEC_FLAG_LOW_DELAY;
	r.codec->thread_count = 1;

	if (s->hardware_decode) {
		if (s->codec == Codec::H265) {
			blog(LOG_INFO,
			     "[obs-airplay] Using software decode for H.265/HEVC (VideoToolbox is disabled for HEVC to prevent black screen issues due to missing extradata)");
		} else {
			ret = av_hwdevice_ctx_create(&r.hw_device,
						     AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
						     nullptr, nullptr, 0);
			if (ret >= 0) {
				r.codec->hw_device_ctx = av_buffer_ref(r.hw_device);
				r.codec->get_format = choose_hw_format;
				blog(LOG_INFO,
				     "[obs-airplay] VideoToolbox hardware decode enabled");
			} else {
				blog(LOG_WARNING,
				     "[obs-airplay] VideoToolbox unavailable, using software decode: %s",
				     av_err(ret).c_str());
			}
		}
	}

	AVDictionary *opts = nullptr;
	av_dict_set(&opts, "realtime", "1", 0);
	av_dict_set(&opts, "low_delay", "1", 0);
	ret = avcodec_open2(r.codec, decoder, &opts);
	av_dict_free(&opts);
	if (ret < 0) {
		blog(LOG_ERROR, "[obs-airplay] avcodec_open2 failed: %s",
		     av_err(ret).c_str());
		return false;
	}

	r.packet = av_packet_alloc();
	r.frame = av_frame_alloc();
	r.sw_frame = av_frame_alloc();
	r.bgra.resize(static_cast<size_t>(s->width) *
		      static_cast<size_t>(s->height) * 4);
	if (!r.packet || !r.frame || !r.sw_frame || r.bgra.empty()) {
		blog(LOG_ERROR, "[obs-airplay] decoder allocation failed");
		return false;
	}

	blog(LOG_INFO, "[obs-airplay] internal decoder opened for %s",
	     codec_id(s->codec));
	return true;
}

static bool output_decoded_frame(AirPlayReceiver *s, DecoderResources &r,
				 AVFrame *decoded)
{
	AVFrame *src = decoded;

	if (decoded->format == AV_PIX_FMT_VIDEOTOOLBOX ||
	    decoded->hw_frames_ctx != nullptr) {
		av_frame_unref(r.sw_frame);
		const int ret = av_hwframe_transfer_data(r.sw_frame, decoded, 0);
		if (ret < 0) {
			blog(LOG_WARNING,
			     "[obs-airplay] hardware frame transfer failed: %s",
			     av_err(ret).c_str());
			return false;
		}
		src = r.sw_frame;
	}

	if (s->decoded_count == 1) {
		const char *format =
			av_get_pix_fmt_name(static_cast<AVPixelFormat>(src->format));
		blog(LOG_INFO,
		     "[obs-airplay] first decoded frame: %dx%d format=%s",
		     src->width, src->height, format ? format : "unknown");
	}

	AirPlayReceiver::QueuedVideoFrame frame;
	frame.width = static_cast<uint32_t>(src->width);
	frame.height = static_cast<uint32_t>(src->height);
	frame.timestamp = get_video_timestamp_ns(s, r, decoded);
	frame.full_range = (src->color_range == AVCOL_RANGE_JPEG);

	{
		std::lock_guard<std::mutex> guard(s->free_video_buffers_mutex);
		if (!s->free_video_buffers.empty()) {
			frame.data_buf = std::move(s->free_video_buffers.back());
			s->free_video_buffers.pop_back();
		}
	}

	if (src->format == AV_PIX_FMT_NV12) {
		frame.format = VIDEO_FORMAT_NV12;
		frame.linesize[0] = src->linesize[0];
		frame.linesize[1] = src->linesize[1];
		
		const size_t y_size = static_cast<size_t>(src->linesize[0]) * src->height;
		const size_t uv_size = static_cast<size_t>(src->linesize[1]) * (src->height / 2);
		frame.data_buf.resize(y_size + uv_size);
		
		memcpy(frame.data_buf.data(), src->data[0], y_size);
		memcpy(frame.data_buf.data() + y_size, src->data[1], uv_size);
		
		frame.plane_offsets[0] = 0;
		frame.plane_offsets[1] = y_size;
	} else if (src->format == AV_PIX_FMT_YUV420P) {
		frame.format = VIDEO_FORMAT_I420;
		frame.linesize[0] = src->linesize[0];
		frame.linesize[1] = src->linesize[1];
		frame.linesize[2] = src->linesize[2];
		
		const size_t y_size = static_cast<size_t>(src->linesize[0]) * src->height;
		const size_t u_size = static_cast<size_t>(src->linesize[1]) * (src->height / 2);
		const size_t v_size = static_cast<size_t>(src->linesize[2]) * (src->height / 2);
		frame.data_buf.resize(y_size + u_size + v_size);
		
		memcpy(frame.data_buf.data(), src->data[0], y_size);
		memcpy(frame.data_buf.data() + y_size, src->data[1], u_size);
		memcpy(frame.data_buf.data() + y_size + u_size, src->data[2], v_size);
		
		frame.plane_offsets[0] = 0;
		frame.plane_offsets[1] = y_size;
		frame.plane_offsets[2] = y_size + u_size;
	} else {
		// Fallback to BGRA via sws_scale
		r.sws = sws_getCachedContext(r.sws, src->width, src->height,
					     static_cast<AVPixelFormat>(src->format),
					     src->width, src->height, AV_PIX_FMT_BGRA,
					     SWS_FAST_BILINEAR, nullptr, nullptr,
					     nullptr);
		if (!r.sws) {
			blog(LOG_ERROR, "[obs-airplay] sws_getCachedContext failed");
			return false;
		}

		const size_t needed = static_cast<size_t>(src->width) * static_cast<size_t>(src->height) * 4;
		frame.data_buf.resize(needed);
		
		uint8_t *dst_data[4] = {frame.data_buf.data(), nullptr, nullptr, nullptr};
		int dst_linesize[4] = {src->width * 4, 0, 0, 0};
		const int scaled = sws_scale(r.sws, src->data, src->linesize, 0,
					     src->height, dst_data, dst_linesize);
		if (scaled <= 0)
			return false;
			
		frame.format = VIDEO_FORMAT_BGRA;
		frame.linesize[0] = src->width * 4;
		frame.plane_offsets[0] = 0;
	}

	{
		std::lock_guard<std::mutex> guard(s->lock);
		s->actual_width = frame.width;
		s->actual_height = frame.height;
	}
	queue_video_frame(s, std::move(frame));
	return true;
}

static void drain_decoder(AirPlayReceiver *s, DecoderResources &r)
{
	while (!s->stopping.load()) {
		av_frame_unref(r.frame);
		const int ret = avcodec_receive_frame(r.codec, r.frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			return;
		if (ret < 0) {
			blog(LOG_WARNING,
			     "[obs-airplay] avcodec_receive_frame failed: %s",
			     av_err(ret).c_str());
			return;
		}

		s->decoded_count++;
		output_decoded_frame(s, r, r.frame);
	}
}

static void decoder_loop(AirPlayReceiver *s)
{
	DecoderResources r;
	int video_stream = -1;

	if (!open_decoder(s, r, video_stream)) {
		blog(LOG_ERROR, "[obs-airplay] decoder failed to start");
		return;
	}

	while (!s->stopping.load()) {
		av_packet_unref(r.packet);
		const int ret = av_read_frame(r.format, r.packet);
		if (ret == AVERROR_EXIT || s->stopping.load())
			break;
		if (ret < 0) {
			if (ret != AVERROR(EAGAIN) && ret != AVERROR(ETIMEDOUT) && ret != -ETIMEDOUT)
				blog(LOG_WARNING, "[obs-airplay] av_read_frame: %s",
				     av_err(ret).c_str());
			continue;
		}

		if (r.packet->stream_index != video_stream)
			continue;

		s->packet_count++;
		if (s->packet_count == 1 || s->packet_count % 300 == 0) {
			blog(LOG_INFO,
			     "[obs-airplay] RTP packets=%llu latest_size=%d flags=0x%x",
			     static_cast<unsigned long long>(s->packet_count),
			     r.packet->size, r.packet->flags);
		}

		const int send_ret = avcodec_send_packet(r.codec, r.packet);
		if (send_ret < 0 && send_ret != AVERROR(EAGAIN)) {
			blog(LOG_WARNING,
			     "[obs-airplay] avcodec_send_packet failed: %s",
			     av_err(send_ret).c_str());
			continue;
		}

		drain_decoder(s, r);
	}

	blog(LOG_INFO, "[obs-airplay] decoder stopped");
}

static std::string make_audio_sdp(int port)
{
	std::ostringstream out;
	out << "v=0\n"
	    << "o=- 0 0 IN IP4 127.0.0.1\n"
	    << "s=OBS AirPlay Audio\n"
	    << "c=IN IP4 127.0.0.1\n"
	    << "t=0 0\n"
	    << "m=audio " << port << " RTP/AVP 96\n"
	    << "a=rtpmap:96 L16/44100/2\n";
	return out.str();
}

static std::string make_temp_audio_sdp_path(int port)
{
	std::ostringstream out;
	out << "/tmp/obs-airplay-audio-" << getpid() << "-" << port << ".sdp";
	return out.str();
}

struct AudioDecoderResources {
	struct TimingState {
		AVRational time_base = {0, 1};
		int64_t origin_pts = AV_NOPTS_VALUE;
		uint64_t origin_ns = 0;
		uint64_t last_timestamp_ns = 0;
		uint64_t next_timestamp_ns = 0;
		int sample_rate = 0;
		double smoothed_offset = 0.0;
		bool initialized = false;
	} timing;

	AVFormatContext *format = nullptr;
	AVCodecContext *codec = nullptr;
	AVPacket *packet = nullptr;
	AVFrame *frame = nullptr;

	~AudioDecoderResources()
	{
		if (frame)
			av_frame_free(&frame);
		if (packet)
			av_packet_free(&packet);
		if (codec)
			avcodec_free_context(&codec);
		if (format)
			avformat_close_input(&format);
	}
};

static bool open_audio_decoder(AirPlayReceiver *s, AudioDecoderResources &r, int &audio_stream)
{
	r.format = avformat_alloc_context();
	if (!r.format) {
		blog(LOG_ERROR, "[obs-airplay] could not allocate AVFormatContext for audio");
		return false;
	}

	r.format->interrupt_callback.callback = decode_interrupt;
	r.format->interrupt_callback.opaque = s;

	AVDictionary *options = nullptr;
	av_dict_set(&options, "protocol_whitelist", "file,udp,rtp", 0);
	av_dict_set(&options, "fflags", "nobuffer", 0);
	av_dict_set(&options, "flags", "low_delay", 0);
	av_dict_set(&options, "analyzeduration", "100000", 0);
	av_dict_set(&options, "probesize", "32768", 0);
	av_dict_set(&options, "reorder_queue_size", "0", 0);
	av_dict_set(&options, "buffer_size", "1048576", 0);
	av_dict_set(&options, "timeout", "100000", 0);
	av_dict_set(&options, "rw_timeout", "100000", 0);

	const AVInputFormat *input_format = av_find_input_format("sdp");
	int ret = avformat_open_input(&r.format, s->audio_sdp_path.c_str(),
				      input_format, &options);
	av_dict_free(&options);
	if (ret < 0) {
		blog(LOG_ERROR, "[obs-airplay] audio avformat_open_input failed: %s",
		     av_err(ret).c_str());
		return false;
	}

	// Skip avformat_find_stream_info for audio to start instantly without blocking
	audio_stream = -1;
	for (unsigned i = 0; i < r.format->nb_streams; ++i) {
		if (r.format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
			audio_stream = static_cast<int>(i);
			break;
		}
	}

	if (audio_stream < 0) {
		blog(LOG_ERROR, "[obs-airplay] no audio stream found in RTP SDP");
		return false;
	}

	AVStream *stream = r.format->streams[audio_stream];
	r.timing.time_base = (stream->time_base.num > 0 && stream->time_base.den > 0)
				     ? stream->time_base
				     : AVRational{1, 44100};

	const AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
	if (!decoder) {
		blog(LOG_ERROR, "[obs-airplay] no audio decoder available");
		return false;
	}

	r.codec = avcodec_alloc_context3(decoder);
	if (!r.codec) {
		blog(LOG_ERROR, "[obs-airplay] could not allocate AVCodecContext for audio");
		return false;
	}

	ret = avcodec_parameters_to_context(r.codec, stream->codecpar);
	if (ret < 0) {
		blog(LOG_ERROR, "[obs-airplay] audio avcodec_parameters_to_context failed: %s", av_err(ret).c_str());
		return false;
	}

	// Manually initialize decoding context properties since we skipped stream probing
	if (r.codec->sample_rate == 0)
		r.codec->sample_rate = 44100;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 24, 100)
	if (r.codec->ch_layout.nb_channels == 0)
		av_channel_layout_default(&r.codec->ch_layout, 2);
#else
	if (r.codec->channels == 0)
		r.codec->channels = 2;
#endif

	ret = avcodec_open2(r.codec, decoder, nullptr);
	if (ret < 0) {
		blog(LOG_ERROR, "[obs-airplay] audio avcodec_open2 failed: %s", av_err(ret).c_str());
		return false;
	}

	r.packet = av_packet_alloc();
	r.frame = av_frame_alloc();
	if (!r.packet || !r.frame) {
		blog(LOG_ERROR, "[obs-airplay] audio decoder allocation failed");
		return false;
	}

	blog(LOG_INFO, "[obs-airplay] internal audio decoder opened");
	return true;
}

static uint64_t get_audio_timestamp_ns(AudioDecoderResources &r, const AVFrame *decoded)
{
	const uint64_t now_ns = os_gettime_ns();
	auto &timing = r.timing;
	const int sample_rate = std::max(decoded->sample_rate, 1);
	const uint64_t duration_ns = static_cast<uint64_t>(decoded->nb_samples) * 1000000000ULL / sample_rate;

	if (decoded->pts != AV_NOPTS_VALUE && timing.time_base.num > 0) {
		uint64_t pts_ns = static_cast<uint64_t>(decoded->pts) * 1000000000ULL * timing.time_base.num / timing.time_base.den;
		
		if (!timing.initialized || timing.sample_rate != sample_rate) {
			timing.origin_pts = pts_ns;
			timing.origin_ns = now_ns;
			timing.sample_rate = sample_rate;
			timing.initialized = true;
			timing.last_timestamp_ns = now_ns;
			return now_ns;
		}
		
		uint64_t candidate = timing.origin_ns + (pts_ns - timing.origin_pts);
		const int64_t drift = static_cast<int64_t>(candidate) - static_cast<int64_t>(now_ns);
		
		if (std::abs(drift) > 100000000LL) { // 100ms drift or gap
			timing.origin_pts = pts_ns;
			timing.origin_ns = now_ns;
			candidate = now_ns;
		}
		
		// Prevent any backward jump that would cause OBS to stutter
		if (candidate < timing.last_timestamp_ns + duration_ns / 2) {
			candidate = timing.last_timestamp_ns + duration_ns;
			timing.origin_ns = candidate;
			timing.origin_pts = pts_ns;
		}
		
		timing.last_timestamp_ns = candidate;
		return candidate;
	}

	// Fallback if no PTS
	if (!timing.initialized || timing.sample_rate != sample_rate) {
		timing.sample_rate = sample_rate;
		timing.initialized = true;
		timing.last_timestamp_ns = now_ns;
		return now_ns;
	}

	uint64_t candidate = timing.last_timestamp_ns + duration_ns;
	const int64_t drift = static_cast<int64_t>(candidate) - static_cast<int64_t>(now_ns);
	
	if (drift < -50000000LL) { // 50ms gap
		candidate = now_ns;
		if (candidate < timing.last_timestamp_ns + duration_ns) {
			candidate = timing.last_timestamp_ns + duration_ns;
		}
	}
	
	timing.last_timestamp_ns = candidate;
	return candidate;
}

static bool output_decoded_audio(AirPlayReceiver *s, AudioDecoderResources &r,
				 AVFrame *decoded)
{
	obs_source_audio frame = {};
	
	if (decoded->format == AV_SAMPLE_FMT_S16) {
		frame.format = AUDIO_FORMAT_16BIT;
	} else if (decoded->format == AV_SAMPLE_FMT_S16P) {
		frame.format = AUDIO_FORMAT_16BIT_PLANAR;
	} else if (decoded->format == AV_SAMPLE_FMT_FLT) {
		frame.format = AUDIO_FORMAT_FLOAT;
	} else if (decoded->format == AV_SAMPLE_FMT_FLTP) {
		frame.format = AUDIO_FORMAT_FLOAT_PLANAR;
	} else {
		frame.format = AUDIO_FORMAT_16BIT;
	}

	frame.samples_per_sec = static_cast<uint32_t>(decoded->sample_rate);
	frame.frames = static_cast<uint32_t>(decoded->nb_samples);
	frame.timestamp = get_audio_timestamp_ns(r, decoded);

	int channels = 2;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 24, 100)
	channels = decoded->ch_layout.nb_channels;
#else
	channels = decoded->channels;
#endif
	frame.speakers = (channels == 1) ? SPEAKERS_MONO : SPEAKERS_STEREO;

	for (int i = 0; i < MAX_AV_PLANES; ++i) {
		frame.data[i] = decoded->data[i];
	}

	obs_source_t *audio_sources_buf[16];
	size_t num_sources = 0;
	{
		std::lock_guard<std::mutex> guard(s->lock);
		num_sources = std::min(s->audio_sources.size(), size_t(16));
		for (size_t i = 0; i < num_sources; ++i) {
			audio_sources_buf[i] = s->audio_sources[i];
		}
	}
	for (size_t i = 0; i < num_sources; ++i) {
		obs_source_output_audio(audio_sources_buf[i], &frame);
	}

	s->audio_output_count++;
	if (s->audio_output_count == 1 || s->audio_output_count % 120 == 0) {
		blog(LOG_INFO, "[obs-airplay] output audio frames=%llu samples=%d rate=%d",
		     static_cast<unsigned long long>(s->audio_output_count),
		     frame.frames, frame.samples_per_sec);
	}
	return true;
}

static void drain_audio_decoder(AirPlayReceiver *s, AudioDecoderResources &r)
{
	while (!s->stopping.load()) {
		av_frame_unref(r.frame);
		const int ret = avcodec_receive_frame(r.codec, r.frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			return;
		if (ret < 0) {
			blog(LOG_WARNING, "[obs-airplay] avcodec_receive_frame for audio failed: %s", av_err(ret).c_str());
			return;
		}

		s->audio_decoded_count++;
		output_decoded_audio(s, r, r.frame);
	}
}

static void audio_decoder_loop(AirPlayReceiver *s)
{
	AudioDecoderResources r;
	int audio_stream = -1;

	if (!open_audio_decoder(s, r, audio_stream)) {
		blog(LOG_ERROR, "[obs-airplay] audio decoder failed to start");
		return;
	}

	while (!s->stopping.load()) {
		av_packet_unref(r.packet);
		const int ret = av_read_frame(r.format, r.packet);
		if (ret == AVERROR_EXIT || s->stopping.load())
			break;
		if (ret < 0) {
			if (ret != AVERROR(EAGAIN) && ret != AVERROR(ETIMEDOUT) && ret != -ETIMEDOUT)
				blog(LOG_WARNING, "[obs-airplay] audio av_read_frame: %s", av_err(ret).c_str());
			continue;
		}

		if (r.packet->stream_index != audio_stream)
			continue;

		s->audio_packet_count++;
		if (s->audio_packet_count == 1 || s->audio_packet_count % 300 == 0) {
			blog(LOG_INFO, "[obs-airplay] Audio RTP packets=%llu latest_size=%d",
			     static_cast<unsigned long long>(s->audio_packet_count),
			     r.packet->size);
		}
		const int send_ret = avcodec_send_packet(r.codec, r.packet);
		if (send_ret < 0 && send_ret != AVERROR(EAGAIN)) {
			blog(LOG_WARNING, "[obs-airplay] audio avcodec_send_packet failed: %s", av_err(send_ret).c_str());
			continue;
		}

		drain_audio_decoder(s, r);
	}

	blog(LOG_INFO, "[obs-airplay] audio decoder stopped");
}

static void stop_pipeline(AirPlayReceiver *s)
{
	s->stopping.store(true);
	stop_child(s->uxplay);

	if (s->uxplay_log_thread.joinable())
		s->uxplay_log_thread.join();

	if (s->decoder_thread.joinable())
		s->decoder_thread.join();

	if (s->audio_decoder_thread.joinable())
		s->audio_decoder_thread.join();

	s->video_queue_cv.notify_all();
	if (s->video_output_thread.joinable())
		s->video_output_thread.join();

	{
		std::lock_guard<std::mutex> guard(s->video_queue_mutex);
		s->video_queue.clear();
	}

	if (!s->sdp_path.empty()) {
		unlink(s->sdp_path.c_str());
		s->sdp_path.clear();
	}

	if (!s->audio_sdp_path.empty()) {
		unlink(s->audio_sdp_path.c_str());
		s->audio_sdp_path.clear();
	}
}

static std::string get_plugin_binary_directory()
{
	Dl_info info;
	if (dladdr((void*)obs_module_load, &info) && info.dli_fname) {
		std::string s(info.dli_fname);
		size_t last_slash = s.find_last_of("/");
		if (last_slash != std::string::npos) {
			return s.substr(0, last_slash);
		}
	}
	return "";
}

static std::string find_uxplay_binary(const std::string &configured)
{
	// 1. Check if the configured path actually exists
	if (!configured.empty() && access(configured.c_str(), X_OK) == 0) {
		return configured;
	}

	// 2. Check in the plugin's own bundle folder
	std::string bundle_path = get_plugin_binary_directory() + "/uxplay";
	if (access(bundle_path.c_str(), X_OK) == 0) {
		return bundle_path;
	}

	// 3. Check in default opt/homebrew/bin
	std::string brew_path = "/opt/homebrew/bin/uxplay";
	if (access(brew_path.c_str(), X_OK) == 0) {
		return brew_path;
	}

	// 4. Check in usr/local/bin
	std::string local_path = "/usr/local/bin/uxplay";
	if (access(local_path.c_str(), X_OK) == 0) {
		return local_path;
	}

	// 5. Fallback to bundled path as ultimate fallback
	return bundle_path;
}

static bool start_pipeline(AirPlayReceiver *s)
{
	stop_pipeline(s);
	// Self-healing: Kill any orphaned or hung uxplay processes globally to release ports and Bonjour registrations
	system("killall -9 uxplay >/dev/null 2>&1 || true");
	s->stopping.store(false);
	s->packet_count = 0;
	s->decoded_count = 0;
	s->output_count = 0;
	s->audio_packet_count = 0;
	s->audio_decoded_count = 0;
	s->audio_output_count = 0;
	s->last_output_time_ns = 0;
	s->actual_width = static_cast<uint32_t>(s->width);
	s->actual_height = static_cast<uint32_t>(s->height);
	{
		std::lock_guard<std::mutex> guard(s->video_queue_mutex);
		s->video_queue.clear();
	}
	
	s->sdp_path = make_temp_sdp_path(s->rtp_port);
	s->audio_sdp_path = make_temp_audio_sdp_path(s->rtp_audio_port);

	if (!write_text_file(s->sdp_path, make_sdp(s->rtp_port, s->codec))) {
		blog(LOG_ERROR, "[obs-airplay] could not write SDP file: %s",
		     s->sdp_path.c_str());
		return false;
	}

	if (!write_text_file(s->audio_sdp_path, make_audio_sdp(s->rtp_audio_port))) {
		blog(LOG_ERROR, "[obs-airplay] could not write audio SDP file: %s",
		     s->audio_sdp_path.c_str());
		stop_pipeline(s);
		return false;
	}

	s->decoder_thread = std::thread(decoder_loop, s);
	s->audio_decoder_thread = std::thread(audio_decoder_loop, s);
	s->video_output_thread = std::thread(video_output_loop, s);
	usleep(200000);

	std::string final_uxplay_path = find_uxplay_binary(s->uxplay_path);
	
	// Configure child GStreamer environment to search our bundled gstreamer-1.0 plugin folder
	std::string plugin_path_env = get_plugin_binary_directory() + "/gstreamer-1.0";
	setenv("GST_PLUGIN_PATH", plugin_path_env.c_str(), 1);
	setenv("GST_PLUGIN_SYSTEM_PATH", plugin_path_env.c_str(), 1);

	if (!spawn_child(final_uxplay_path, build_uxplay_args(s), s->uxplay)) {
		stop_pipeline(s);
		return false;
	}
	if (s->uxplay.output_fd >= 0) {
		const int fd = s->uxplay.output_fd;
		s->uxplay_log_thread = std::thread(log_fd_loop, s, fd, "uxplay");
	}

	blog(LOG_INFO,
	     "[obs-airplay] receiver started: name='%s' codec=%s %dx%d@%d port=%d",
	     s->name.c_str(), codec_id(s->codec), s->width, s->height, s->fps,
	     s->rtp_port);
	return true;
}

static bool g_settings_loaded = false;

static void save_global_settings();
static void load_global_settings();

static void read_settings(AirPlayReceiver *s, obs_data_t *settings)
{
	s->name = get_string(settings, "name", "OBS AirPlay");
	s->uxplay_path = get_string(settings, "uxplay_path", kDefaultUxPlay);
	// Reset the path if it points to a developer folder that doesn't exist on this machine
	if (s->uxplay_path.find("/Users/eklavya/") == 0 && access(s->uxplay_path.c_str(), X_OK) != 0) {
		s->uxplay_path = "uxplay";
	}
	s->codec = get_codec(settings);
	s->width = static_cast<int>(obs_data_get_int(settings, "width"));
	s->height = static_cast<int>(obs_data_get_int(settings, "height"));
	s->fps = static_cast<int>(obs_data_get_int(settings, "fps"));
	s->rtp_port = static_cast<int>(obs_data_get_int(settings, "rtp_port"));
	s->rtp_audio_port = static_cast<int>(obs_data_get_int(settings, "rtp_audio_port"));
	s->auto_start = obs_data_get_bool(settings, "auto_start");
	s->hardware_decode = obs_data_get_bool(settings, "hardware_decode");

	if (s->width <= 0)
		s->width = 1920;
	if (s->height <= 0)
		s->height = 1080;
	if (s->fps <= 0)
		s->fps = 60;
	if (s->rtp_port <= 0)
		s->rtp_port = 5000;
	if (s->rtp_audio_port <= 0)
		s->rtp_audio_port = 5002;
}

static void save_global_settings()
{
	char *path_char = obs_module_config_path("settings.ini");
	if (!path_char)
		return;
	std::string path = path_char;
	bfree(path_char);

	// Extract the directory path and create it if it doesn't exist
	size_t last_slash = path.find_last_of("/\\");
	if (last_slash != std::string::npos) {
		std::string dir = path.substr(0, last_slash);
		os_mkdirs(dir.c_str());
	}

	FILE *file = fopen(path.c_str(), "w");
	if (!file) {
		blog(LOG_WARNING, "[obs-airplay] Could not open config file for writing: %s", path.c_str());
		return;
	}

	std::lock_guard<std::mutex> guard(g_receiver.lock);
	fprintf(file, "name=%s\n", g_receiver.name.c_str());
	fprintf(file, "codec=%s\n", codec_id(g_receiver.codec));
	fprintf(file, "width=%d\n", g_receiver.width);
	fprintf(file, "height=%d\n", g_receiver.height);
	fprintf(file, "fps=%d\n", g_receiver.fps);
	fprintf(file, "rtp_port=%d\n", g_receiver.rtp_port);
	fprintf(file, "rtp_audio_port=%d\n", g_receiver.rtp_audio_port);
	fprintf(file, "hardware_decode=%s\n", g_receiver.hardware_decode ? "true" : "false");
	fclose(file);
	blog(LOG_INFO, "[obs-airplay] Global settings saved to %s", path.c_str());
}

static void load_global_settings()
{
	char *path_char = obs_module_config_path("settings.ini");
	if (!path_char)
		return;
	std::string path = path_char;
	bfree(path_char);

	FILE *file = fopen(path.c_str(), "r");
	if (!file) {
		blog(LOG_INFO, "[obs-airplay] No global settings found at %s, using defaults", path.c_str());
		return;
	}

	std::lock_guard<std::mutex> guard(g_receiver.lock);
	char line[256];
	while (fgets(line, sizeof(line), file)) {
		std::string str(line);
		while (!str.empty() && (str.back() == '\n' || str.back() == '\r'))
			str.pop_back();

		size_t eq = str.find('=');
		if (eq == std::string::npos)
			continue;

		std::string key = str.substr(0, eq);
		std::string val = str.substr(eq + 1);

		try {
			if (key == "name") g_receiver.name = val;
			else if (key == "codec") g_receiver.codec = (val == "h265" ? Codec::H265 : Codec::H264);
			else if (key == "width") g_receiver.width = std::stoi(val);
			else if (key == "height") g_receiver.height = std::stoi(val);
			else if (key == "fps") g_receiver.fps = std::stoi(val);
			else if (key == "rtp_port") g_receiver.rtp_port = std::stoi(val);
			else if (key == "rtp_audio_port") g_receiver.rtp_audio_port = std::stoi(val);
			else if (key == "hardware_decode") g_receiver.hardware_decode = (val == "true");
		} catch (...) {
			blog(LOG_WARNING, "[obs-airplay] Error parsing config value: %s=%s", key.c_str(), val.c_str());
		}
	}
	fclose(file);
	g_settings_loaded = true;
	blog(LOG_INFO, "[obs-airplay] Global settings loaded from %s", path.c_str());
}

static std::string url_decode(const std::string &str)
{
	std::string res;
	res.reserve(str.size());
	for (size_t i = 0; i < str.size(); ++i) {
		if (str[i] == '+') {
			res += ' ';
		} else if (str[i] == '%' && i + 2 < str.size()) {
			char hex[3] = {str[i + 1], str[i + 2], '\0'};
			res += static_cast<char>(strtol(hex, nullptr, 16));
			i += 2;
		} else {
			res += str[i];
		}
	}
	return res;
}

static std::string get_query_param(const std::string &query, const std::string &key)
{
	size_t pos = query.find(key + "=");
	if (pos == std::string::npos)
		return "";
	pos += key.size() + 1;
	size_t end = query.find("&", pos);
	if (end == std::string::npos)
		return url_decode(query.substr(pos));
	return url_decode(query.substr(pos, end - pos));
}

static const char *source_get_name(void *)
{
	return "AirPlay Video";
}

static void *source_create(obs_data_t *settings, obs_source_t *source)
{
	AirPlaySource *s = new AirPlaySource();
	s->source = source;

	{
		std::lock_guard<std::mutex> guard(g_receiver.lock);
		g_receiver.sources.push_back(source);
		if (!g_settings_loaded) {
			read_settings(&g_receiver, settings);
			g_settings_loaded = true;
		}
	}

	if (g_receiver.auto_start && !g_receiver.uxplay.running())
		start_pipeline(&g_receiver);

	return s;
}

static void source_destroy(void *data)
{
	AirPlaySource *s = static_cast<AirPlaySource *>(data);
	{
		std::lock_guard<std::mutex> guard(g_receiver.lock);
		auto &sources = g_receiver.sources;
		sources.erase(std::remove(sources.begin(), sources.end(), s->source),
			      sources.end());
	}
	delete s;
}

static void source_update(void *data, obs_data_t *settings)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(settings);
}

static uint32_t source_get_width(void *data)
{
	UNUSED_PARAMETER(data);
	std::lock_guard<std::mutex> guard(g_receiver.lock);
	return g_receiver.actual_width;
}

static uint32_t source_get_height(void *data)
{
	UNUSED_PARAMETER(data);
	std::lock_guard<std::mutex> guard(g_receiver.lock);
	return g_receiver.actual_height;
}

static const char *audio_source_get_name(void *)
{
	return "AirPlay Audio";
}

static void *audio_source_create(obs_data_t *settings, obs_source_t *source)
{
	AirPlaySource *s = new AirPlaySource();
	s->source = source;

	{
		std::lock_guard<std::mutex> guard(g_receiver.lock);
		g_receiver.audio_sources.push_back(source);
		if (!g_settings_loaded) {
			read_settings(&g_receiver, settings);
			g_settings_loaded = true;
		}
	}

	if (g_receiver.auto_start && !g_receiver.uxplay.running())
		start_pipeline(&g_receiver);

	return s;
}

static void audio_source_destroy(void *data)
{
	AirPlaySource *s = static_cast<AirPlaySource *>(data);
	{
		std::lock_guard<std::mutex> guard(g_receiver.lock);
		auto &sources = g_receiver.audio_sources;
		sources.erase(std::remove(sources.begin(), sources.end(), s->source),
			      sources.end());
	}
	delete s;
}

static std::string receiver_status_json()
{
	std::lock_guard<std::mutex> guard(g_receiver.lock);
	char fps_buf[32];
	snprintf(fps_buf, sizeof(fps_buf), "%.1f", static_cast<double>(g_receiver.fps));

	std::ostringstream out;
	out << "{"
	    << "\"running\":" << (g_receiver.uxplay.running() ? "true" : "false")
	    << ",\"name\":\"" << g_receiver.name << "\""
	    << ",\"codec\":\"" << codec_id(g_receiver.codec) << "\""
	    << ",\"width\":" << g_receiver.width
	    << ",\"height\":" << g_receiver.height
	    << ",\"actual_width\":" << g_receiver.actual_width
	    << ",\"actual_height\":" << g_receiver.actual_height
	    << ",\"fps\":" << g_receiver.fps
	    << ",\"active_fps\":" << fps_buf
	    << ",\"port\":" << g_receiver.rtp_port
	    << ",\"audio_port\":" << g_receiver.rtp_audio_port
	    << ",\"hardware_decode\":" << (g_receiver.hardware_decode ? "true" : "false")
	    << ",\"sources\":" << g_receiver.sources.size()
	    << ",\"audio_sources\":" << g_receiver.audio_sources.size()
	    << ",\"packets\":" << g_receiver.packet_count
	    << ",\"decoded\":" << g_receiver.decoded_count
	    << ",\"output\":" << g_receiver.output_count
	    << ",\"audio_packets\":" << g_receiver.audio_packet_count
	    << ",\"audio_decoded\":" << g_receiver.audio_decoded_count
	    << ",\"audio_output\":" << g_receiver.audio_output_count
	    << "}";
	return out.str();
}

static std::string receiver_dock_html()
{
	return R"HTML(<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>AirPlay Receiver Control</title>
<link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap" rel="stylesheet">
<style>
  :root {
    --bg-color: #0c0d12;
    --panel-bg: rgba(20, 22, 33, 0.7);
    --card-bg: rgba(30, 33, 50, 0.4);
    --input-bg: #141621;
    --border-color: rgba(255, 255, 255, 0.08);
    --border-hover: rgba(255, 255, 255, 0.15);
    --text-color: #f3f4f8;
    --text-muted: #8e9aa8;
    --accent-blue: #3b82f6;
    --accent-blue-hover: #2563eb;
    --accent-green: #10b981;
    --accent-green-hover: #059669;
    --accent-red: #ef4444;
    --accent-red-hover: #dc2626;
    --font-family: 'Inter', -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
  }
  
  body {
    margin: 0;
    background: radial-gradient(circle at top, #1e1e2d 0%, var(--bg-color) 100%);
    color: var(--text-color);
    font-family: var(--font-family);
    padding: 10px;
    box-sizing: border-box;
    display: flex;
    flex-direction: column;
    min-height: 100vh;
  }
  
  main {
    display: flex;
    flex-direction: column;
    gap: 10px;
    width: 100%;
    margin: 0 auto;
    box-sizing: border-box;
  }

  .header-container {
    display: flex;
    justify-content: space-between;
    align-items: center;
    padding: 4px 2px;
  }

  h1 {
    font-size: 13px;
    font-weight: 700;
    margin: 0;
    display: flex;
    align-items: center;
    gap: 6px;
    text-transform: uppercase;
    letter-spacing: 1px;
    color: #e2e8f0;
  }

  .status-badge {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    padding: 4px 10px;
    border-radius: 9999px;
    font-size: 10px;
    font-weight: 700;
    text-transform: uppercase;
    letter-spacing: 0.5px;
    transition: all 0.3s cubic-bezier(0.4, 0, 0.2, 1);
  }

  .status-badge.running {
    background-color: rgba(16, 185, 129, 0.15);
    color: #34d399;
    border: 1px solid rgba(16, 185, 129, 0.3);
    box-shadow: 0 0 10px rgba(16, 185, 129, 0.1);
  }

  .status-badge.stopped {
    background-color: rgba(239, 68, 68, 0.15);
    color: #f87171;
    border: 1px solid rgba(239, 68, 68, 0.3);
  }

  .status-dot {
    width: 6px;
    height: 6px;
    border-radius: 50%;
    transition: all 0.3s;
  }

  .status-badge.running .status-dot {
    background-color: #34d399;
    box-shadow: 0 0 8px #34d399;
    animation: pulse 2s infinite;
  }

  .status-badge.stopped .status-dot {
    background-color: #f87171;
  }

  @keyframes pulse {
    0% { transform: scale(0.9); opacity: 0.6; }
    50% { transform: scale(1.2); opacity: 1; box-shadow: 0 0 10px rgba(52, 211, 153, 0.8); }
    100% { transform: scale(0.9); opacity: 0.6; }
  }

  .stream-card {
    background: var(--panel-bg);
    border: 1px solid var(--border-color);
    border-radius: 8px;
    padding: 8px 12px;
    backdrop-filter: blur(12px);
    box-shadow: 0 4px 20px rgba(0, 0, 0, 0.2);
    text-align: center;
  }

  .stream-label {
    font-size: 9px;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.5px;
    color: var(--text-muted);
  }

  .stream-value {
    font-size: 12px;
    font-weight: 600;
    color: #60a5fa;
    margin-top: 2px;
    word-break: break-all;
  }

  .metrics-panel {
    background: var(--panel-bg);
    border: 1px solid var(--border-color);
    border-radius: 8px;
    padding: 8px;
    display: flex;
    flex-direction: column;
    gap: 6px;
  }

  .metrics-grid {
    display: grid;
    grid-template-columns: repeat(3, 1fr);
    gap: 6px;
  }

  .metric-box {
    background: var(--card-bg);
    border: 1px solid var(--border-color);
    border-radius: 6px;
    padding: 6px;
    text-align: center;
    display: flex;
    flex-direction: column;
    justify-content: center;
    min-width: 0;
  }

  .metric-label {
    font-size: 8px;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.5px;
    color: var(--text-muted);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .metric-value {
    font-size: 11px;
    font-weight: 700;
    color: var(--text-color);
    margin-top: 2px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .config-panel {
    background: var(--panel-bg);
    border: 1px solid var(--border-color);
    border-radius: 8px;
    padding: 10px;
    display: flex;
    flex-direction: column;
    gap: 8px;
  }

  .form-group {
    display: flex;
    flex-direction: column;
    gap: 3px;
  }

  label {
    font-size: 9px;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.5px;
    color: var(--text-muted);
  }

  input, select {
    background: var(--input-bg);
    color: var(--text-color);
    border: 1px solid var(--border-color);
    border-radius: 6px;
    padding: 5px 8px;
    font-size: 12px;
    font-family: inherit;
    outline: none;
    transition: all 0.2s ease;
  }

  input:focus, select:focus {
    border-color: var(--accent-blue);
    box-shadow: 0 0 0 2px rgba(59, 130, 246, 0.2);
  }

  .adv-details {
    margin-top: 4px;
    border-top: 1px solid var(--border-color);
    padding-top: 6px;
  }

  .adv-summary {
    font-size: 10px;
    font-weight: 700;
    text-transform: uppercase;
    letter-spacing: 0.5px;
    color: var(--accent-blue);
    cursor: pointer;
    outline: none;
    user-select: none;
    padding: 2px 0;
    transition: color 0.2s;
  }

  .adv-summary:hover {
    color: #60a5fa;
  }

  .adv-content {
    display: flex;
    flex-direction: column;
    gap: 8px;
    margin-top: 8px;
    padding-bottom: 2px;
  }

  .row-grid {
    display: grid;
    grid-template-columns: repeat(2, 1fr);
    gap: 8px;
  }

  @media (max-width: 250px) {
    .row-grid {
      grid-template-columns: 1fr;
      gap: 6px;
    }
  }

  .checkbox-group {
    flex-direction: row;
    align-items: center;
    gap: 6px;
    padding: 2px 0;
  }

  .checkbox-group input[type="checkbox"] {
    width: 12px;
    height: 12px;
    cursor: pointer;
    margin: 0;
  }

  .checkbox-group label {
    text-transform: none;
    font-size: 11px;
    color: var(--text-color);
    cursor: pointer;
  }

  .actions {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 6px;
  }

  button {
    font-family: inherit;
    font-size: 11px;
    font-weight: 600;
    padding: 7px 10px;
    border: none;
    border-radius: 6px;
    cursor: pointer;
    transition: all 0.2s ease;
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 4px;
  }

  button:disabled {
    opacity: 0.4;
    cursor: not-allowed;
  }

  .btn-success {
    background-color: var(--accent-green);
    color: #0c0d12;
  }

  .btn-success:hover:not(:disabled) {
    background-color: var(--accent-green-hover);
    box-shadow: 0 0 10px rgba(16, 185, 129, 0.2);
  }

  .btn-danger {
    background-color: var(--accent-red);
    color: white;
  }

  .btn-danger:hover:not(:disabled) {
    background-color: var(--accent-red-hover);
    box-shadow: 0 0 10px rgba(239, 68, 68, 0.2);
  }

  .btn-primary {
    background-color: var(--accent-blue);
    color: white;
    grid-column: span 2;
    padding: 8px 12px;
    font-size: 12px;
  }

  .btn-primary:hover:not(:disabled) {
    background-color: var(--accent-blue-hover);
    box-shadow: 0 0 10px rgba(59, 130, 246, 0.2);
  }

  .footer-logs {
    margin-top: auto;
    padding-top: 8px;
    font-size: 9px;
    color: var(--text-muted);
    text-align: center;
    border-top: 1px solid var(--border-color);
  }
</style>
</head>
<body>
<main>
  <div class="header-container">
    <h1>AirPlay Receiver</h1>
    <div id="statusBadge" class="status-badge stopped">
      <span class="status-dot"></span>
      <span id="statusText">Stopped</span>
    </div>
  </div>

  <div class="stream-card">
    <div class="stream-label">Active Connection</div>
    <div id="streamVal" class="stream-value">-</div>
  </div>

  <div class="metrics-panel">
    <div class="metrics-grid">
      <div class="metric-box">
        <span class="metric-label">V-Sources</span>
        <span id="sourcesVal" class="metric-value">0</span>
      </div>
      <div class="metric-box">
        <span class="metric-label">V-Packets</span>
        <span id="packetsVal" class="metric-value">0</span>
      </div>
      <div class="metric-box">
        <span class="metric-label">V-Frames</span>
        <span id="framesVal" class="metric-value">0</span>
      </div>
      
      <div class="metric-box">
        <span class="metric-label">A-Sources</span>
        <span id="audioSourcesVal" class="metric-value">0</span>
      </div>
      <div class="metric-box">
        <span class="metric-label">A-Packets</span>
        <span id="audioPacketsVal" class="metric-value">0</span>
      </div>
      <div class="metric-box">
        <span class="metric-label">A-Frames</span>
        <span id="audioFramesVal" class="metric-value">0</span>
      </div>
    </div>
  </div>

  <div class="config-panel">
    <div class="form-group">
      <label for="nameInput">AirPlay Name</label>
      <input type="text" id="nameInput" placeholder="OBS AirPlay" />
    </div>
    
    <details class="adv-details">
      <summary class="adv-summary">Advanced Settings</summary>
      <div class="adv-content">
        <div class="row-grid">
          <div class="form-group">
            <label for="codecSelect">Codec</label>
            <select id="codecSelect">
              <option value="h264">H.264 / AVC</option>
              <option value="h265">H.265 / HEVC</option>
            </select>
          </div>
          <div class="form-group">
            <label for="fpsSelect">FPS Limit</label>
            <select id="fpsSelect">
              <option value="60">60 FPS</option>
              <option value="30">30 FPS</option>
            </select>
          </div>
        </div>

        <div class="row-grid">
          <div class="form-group">
            <label for="resSelect">Resolution</label>
            <select id="resSelect">
              <option value="1920x1080">1920x1080</option>
              <option value="1280x720">1280x720</option>
              <option value="3840x2160">3840x2160</option>
            </select>
          </div>
          <div class="form-group">
            <label for="portInput">Video Port</label>
            <input type="number" id="portInput" min="1024" max="65535" />
          </div>
        </div>

        <div class="row-grid">
          <div class="form-group" style="grid-column: span 2;">
            <label for="audioPortInput">Audio Port</label>
            <input type="number" id="audioPortInput" min="1024" max="65535" />
          </div>
        </div>

        <div class="form-group checkbox-group">
          <input type="checkbox" id="hwCheckbox" />
          <label for="hwCheckbox">VideoToolbox Hardware Decode</label>
        </div>
      </div>
    </details>
  </div>

  <div class="actions">
    <button id="startBtn" class="btn-success" onclick="startReceiver()">
      <svg width="10" height="10" viewBox="0 0 24 24" fill="currentColor"><path d="M8 5v14l11-7z"/></svg> Start
    </button>
    <button id="stopBtn" class="btn-danger" onclick="stopReceiver()">
      <svg width="10" height="10" viewBox="0 0 24 24" fill="currentColor"><path d="M6 19h4V5H6v14zm8-14v14h4V5h-4z"/></svg> Stop
    </button>
    <button id="applyBtn" class="btn-primary" onclick="applySettings()">
      <svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"><path d="M20 6L9 17l-5-5"/></svg> Apply & Restart
    </button>
  </div>

  <div class="footer-logs">
    Connected to OBS Studio Plugin • Port 8799
  </div>
</main>

<script>
  let isRunning = false;

  async function fetchStatus() {
    try {
      const response = await fetch('/status');
      const status = await response.json();
      updateUI(status);
    } catch (e) {
      console.error("Error fetching status:", e);
    }
  }

  function updateUI(status) {
    isRunning = status.running;
    
    // Update badge
    const badge = document.getElementById('statusBadge');
    const text = document.getElementById('statusText');
    if (isRunning) {
      badge.className = 'status-badge running';
      text.textContent = 'Running';
      document.getElementById('startBtn').disabled = true;
      document.getElementById('stopBtn').disabled = false;
    } else {
      badge.className = 'status-badge stopped';
      text.textContent = 'Stopped';
      document.getElementById('startBtn').disabled = false;
      document.getElementById('stopBtn').disabled = true;
    }

    // Update stats
    document.getElementById('sourcesVal').textContent = status.sources;
    document.getElementById('audioSourcesVal').textContent = status.audio_sources;
    document.getElementById('streamVal').textContent = `${status.codec.toUpperCase()} ${status.width}x${status.height}@${status.fps} FPS • Video Port ${status.port} • Audio Port ${status.audio_port}`;
    document.getElementById('packetsVal').textContent = status.packets.toLocaleString();
    document.getElementById('audioPacketsVal').textContent = status.audio_packets.toLocaleString();
    document.getElementById('framesVal').textContent = status.output.toLocaleString();
    document.getElementById('audioFramesVal').textContent = status.audio_output.toLocaleString();

    // Populate inputs ONLY if they are not currently focused by the user (to avoid overwriting user input)
    if (document.activeElement !== document.getElementById('nameInput')) {
      document.getElementById('nameInput').value = status.name;
    }
    if (document.activeElement !== document.getElementById('codecSelect')) {
      document.getElementById('codecSelect').value = status.codec;
    }
    if (document.activeElement !== document.getElementById('fpsSelect')) {
      document.getElementById('fpsSelect').value = status.fps;
    }
    if (document.activeElement !== document.getElementById('resSelect')) {
      const resVal = `${status.width}x${status.height}`;
      const select = document.getElementById('resSelect');
      let exists = false;
      for (let i = 0; i < select.options.length; i++) {
        if (select.options[i].value === resVal) { exists = true; break; }
      }
      if (!exists) {
        const opt = document.createElement('option');
        opt.value = resVal;
        opt.textContent = `${resVal} (Custom)`;
        select.appendChild(opt);
      }
      select.value = resVal;
    }
    if (document.activeElement !== document.getElementById('portInput')) {
      document.getElementById('portInput').value = status.port;
    }
    if (document.activeElement !== document.getElementById('audioPortInput')) {
      document.getElementById('audioPortInput').value = status.audio_port;
    }
    if (document.activeElement !== document.getElementById('hwCheckbox')) {
      document.getElementById('hwCheckbox').checked = status.hardware_decode;
    }
  }

  async function startReceiver() {
    await fetch('/start');
    await fetchStatus();
  }

  async function stopReceiver() {
    await fetch('/stop');
    await fetchStatus();
  }

  async function applySettings() {
    const name = encodeURIComponent(document.getElementById('nameInput').value);
    const codec = document.getElementById('codecSelect').value;
    const fps = document.getElementById('fpsSelect').value;
    const res = document.getElementById('resSelect').value.split('x');
    const width = res[0];
    const height = res[1];
    const port = document.getElementById('portInput').value;
    const audioPort = document.getElementById('audioPortInput').value;
    const hw = document.getElementById('hwCheckbox').checked ? 'true' : 'false';

    const url = `/update?name=${name}&codec=${codec}&fps=${fps}&width=${width}&height=${height}&rtp_port=${port}&rtp_audio_port=${audioPort}&hardware_decode=${hw}`;
    
    const applyBtn = document.getElementById('applyBtn');
    const originalContent = applyBtn.innerHTML;
    applyBtn.disabled = true;
    applyBtn.innerHTML = 'Applying...';
    
    try {
      await fetch(url);
      await new Promise(r => setTimeout(r, 600));
      await fetchStatus();
    } catch (e) {
      console.error("Error applying settings:", e);
    } finally {
      applyBtn.disabled = false;
      applyBtn.innerHTML = originalContent;
    }
  }

  setInterval(fetchStatus, 1000);
  fetchStatus();
</script>
</body>
</html>)HTML";
}

static void http_reply(int client, const std::string &body,
		       const char *type = "text/html")
{
	std::ostringstream response;
	response << "HTTP/1.1 200 OK\r\n"
		 << "Content-Type: " << type << "\r\n"
		 << "Access-Control-Allow-Origin: *\r\n"
		 << "Content-Length: " << body.size() << "\r\n"
		 << "Connection: close\r\n\r\n"
		 << body;
	const std::string data = response.str();
	send(client, data.data(), data.size(), 0);
}

static void handle_http_client(int client)
{
	char buffer[1024] = {};
	const ssize_t n = recv(client, buffer, sizeof(buffer) - 1, 0);
	if (n <= 0)
		return;

	std::string request(buffer, static_cast<size_t>(n));
	const bool start = request.find("GET /start ") == 0;
	const bool stop = request.find("GET /stop ") == 0;
	const bool restart = request.find("GET /restart ") == 0;
	const bool status = request.find("GET /status ") == 0;
	const bool update = request.find("GET /update?") == 0;

	if (start) {
		start_pipeline(&g_receiver);
	} else if (stop) {
		stop_pipeline(&g_receiver);
	} else if (restart) {
		start_pipeline(&g_receiver);
	} else if (update) {
		size_t space_pos = request.find(" ", 11);
		if (space_pos != std::string::npos) {
			std::string query = request.substr(12, space_pos - 12);
			std::string name_param = get_query_param(query, "name");
			std::string codec_param = get_query_param(query, "codec");
			std::string fps_param = get_query_param(query, "fps");
			std::string width_param = get_query_param(query, "width");
			std::string height_param = get_query_param(query, "height");
			std::string port_param = get_query_param(query, "rtp_port");
			std::string audio_port_param = get_query_param(query, "rtp_audio_port");
			std::string hw_param = get_query_param(query, "hardware_decode");

			bool running = g_receiver.uxplay.running();
			if (running) {
				stop_pipeline(&g_receiver);
			}

			{
				std::lock_guard<std::mutex> guard(g_receiver.lock);
				if (!name_param.empty())
					g_receiver.name = name_param;
				if (!codec_param.empty())
					g_receiver.codec = (codec_param == "h265" ? Codec::H265 : Codec::H264);
				if (!fps_param.empty())
					g_receiver.fps = std::stoi(fps_param);
				if (!width_param.empty())
					g_receiver.width = std::stoi(width_param);
				if (!height_param.empty())
					g_receiver.height = std::stoi(height_param);
				if (!port_param.empty())
					g_receiver.rtp_port = std::stoi(port_param);
				if (!audio_port_param.empty())
					g_receiver.rtp_audio_port = std::stoi(audio_port_param);
				if (!hw_param.empty())
					g_receiver.hardware_decode = (hw_param == "true");
			}

			save_global_settings();

			if (running) {
				start_pipeline(&g_receiver);
			}
		}
	}

	if (status || start || stop || restart || update)
		http_reply(client, receiver_status_json(), "application/json");
	else
		http_reply(client, receiver_dock_html());
}

static void http_loop()
{
	g_http_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (g_http_fd < 0) {
		blog(LOG_WARNING, "[obs-airplay] dock server socket failed");
		return;
	}

	int yes = 1;
	setsockopt(g_http_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(8799);

	if (bind(g_http_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) !=
	    0) {
		blog(LOG_WARNING, "[obs-airplay] dock server bind failed: %s",
		     strerror(errno));
		close(g_http_fd);
		g_http_fd = -1;
		return;
	}

	listen(g_http_fd, 8);
	blog(LOG_INFO, "[obs-airplay] dock server listening on http://127.0.0.1:8799/");

	while (!g_http_stopping.load()) {
		fd_set set;
		FD_ZERO(&set);
		FD_SET(g_http_fd, &set);
		timeval timeout = {0, 250000};
		const int ready = select(g_http_fd + 1, &set, nullptr, nullptr,
					 &timeout);
		if (ready <= 0)
			continue;
		int client = accept(g_http_fd, nullptr, nullptr);
		if (client >= 0) {
			handle_http_client(client);
			close(client);
		}
	}
}

static void start_http_server()
{
	g_http_stopping.store(false);
	g_http_thread = std::thread(http_loop);
}

static void stop_http_server()
{
	g_http_stopping.store(true);
	if (g_http_fd >= 0) {
		shutdown(g_http_fd, SHUT_RDWR);
		close(g_http_fd);
		g_http_fd = -1;
	}
	if (g_http_thread.joinable())
		g_http_thread.join();
}

static void source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "name", "OBS AirPlay");
	obs_data_set_default_string(settings, "uxplay_path", kDefaultUxPlay);
	obs_data_set_default_string(settings, "codec", "h264");
	obs_data_set_default_int(settings, "width", 1920);
	obs_data_set_default_int(settings, "height", 1080);
	obs_data_set_default_int(settings, "fps", 60);
	obs_data_set_default_int(settings, "rtp_port", 5000);
	obs_data_set_default_int(settings, "rtp_audio_port", 5002);
	obs_data_set_default_bool(settings, "hardware_decode", true);
	obs_data_set_default_bool(settings, "auto_start", true);
}

} // namespace

bool obs_module_load(void)
{
	load_global_settings();

	obs_source_info info = {};
	info.id = "obs_airplay_source";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO;
	info.get_name = source_get_name;
	info.create = source_create;
	info.destroy = source_destroy;
	info.update = source_update;
	info.get_width = source_get_width;
	info.get_height = source_get_height;
	info.get_defaults = source_get_defaults;

	obs_register_source(&info);

	obs_source_info audio_info = {};
	audio_info.id = "obs_airplay_audio_source";
	audio_info.type = OBS_SOURCE_TYPE_INPUT;
	audio_info.output_flags = OBS_SOURCE_AUDIO;
	audio_info.get_name = audio_source_get_name;
	audio_info.create = audio_source_create;
	audio_info.destroy = audio_source_destroy;
	audio_info.update = source_update;
	audio_info.get_defaults = source_get_defaults;

	obs_register_source(&audio_info);

	start_http_server();
	blog(LOG_INFO, "[obs-airplay] plugin loaded");
	return true;
}

void obs_module_unload(void)
{
	stop_http_server();
	stop_pipeline(&g_receiver);
}
