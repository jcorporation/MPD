// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

/* necessary because libavutil/common.h uses UINT64_C */
#define __STDC_CONSTANT_MACROS

#include "FfmpegInputPlugin.hxx"
#include "lib/ffmpeg/IOContext.hxx"
#include "lib/ffmpeg/Init.hxx"
#include "../InputStream.hxx"
#include "PluginUnavailable.hxx"
#include "../InputPlugin.hxx"
#include "util/StringAPI.hxx"

class FfmpegInputStream final : public InputStream {
	Ffmpeg::IOContext io;

public:
	FfmpegInputStream(const char *_uri, Mutex &_mutex)
		:InputStream(_uri, _mutex),
		 io(_uri, AVIO_FLAG_READ)
	{
		seekable = (io->seekable & AVIO_SEEKABLE_NORMAL) != 0;
		size = io.GetSize();

		/* hack to make MPD select the "ffmpeg" decoder plugin
		   - since avio.h doesn't tell us the MIME type of the
		   resource, we can't select a decoder plugin, but the
		   "ffmpeg" plugin is quite good at auto-detection */
		SetMimeType("audio/x-mpd-ffmpeg");
		SetReady();
	}

	/* virtual methods from InputStream */
	[[nodiscard]] bool IsEOF() const noexcept override;
	size_t Read(std::unique_lock<Mutex> &lock,
		    std::span<std::byte> dest) override;
	void Seek(std::unique_lock<Mutex> &lock,
		  offset_type offset) override;
};

[[gnu::const]]
static inline bool
input_ffmpeg_supported() noexcept
{
	void *opaque = nullptr;
	return avio_enum_protocols(&opaque, 0) != nullptr;
}

static void
input_ffmpeg_init(EventLoop &, const ConfigBlock &)
{
	FfmpegInit();

	/* disable this plugin if there's no registered protocol */
	if (!input_ffmpeg_supported())
		throw PluginUnavailable("No protocol");
}

static std::set<std::string, std::less<>>
input_ffmpeg_protocols() noexcept
{
	void *opaque = nullptr;
	const char* protocol;
	std::set<std::string, std::less<>> protocols;
	while ((protocol = avio_enum_protocols(&opaque, 0))) {
		if (StringIsEqual(protocol, "hls")) {
			/* just "hls://" doesn't work, but these do
			   work: */
			protocols.emplace("hls+http://");
			protocols.emplace("hls+https://");
			continue;
		}

		if (protocol_is_whitelisted(protocol)) {
			std::string schema(protocol);
			schema.append("://");
			protocols.emplace(schema);
		}
	}

	return protocols;
}

static InputStreamPtr
input_ffmpeg_open(const char *uri,
		  Mutex &mutex)
{
	return std::make_unique<FfmpegInputStream>(uri, mutex);
}

size_t
FfmpegInputStream::Read(std::unique_lock<Mutex> &,
			std::span<std::byte> dest)
{
	size_t result;

	{
		const ScopeUnlock unlock(mutex);
		result = io.Read(dest);
	}

	offset += result;
	return (size_t)result;
}

bool
FfmpegInputStream::IsEOF() const noexcept
{
	return io.IsEOF();
}

void
FfmpegInputStream::Seek(std::unique_lock<Mutex> &, offset_type new_offset)
{
	uint64_t result;

	{
		const ScopeUnlock unlock(mutex);
		result = io.Seek(new_offset);
	}

	offset = result;
}

const InputPlugin input_plugin_ffmpeg = {
	"ffmpeg",
	nullptr,
	input_ffmpeg_init,
	nullptr,
	input_ffmpeg_open,
	input_ffmpeg_protocols
};
