// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "AlsaOutputPlugin.hxx"
#include "lib/alsa/AllowedFormat.hxx"
#include "lib/alsa/ChannelMap.hxx"
#include "lib/alsa/Error.hxx"
#include "lib/alsa/HwSetup.hxx"
#include "lib/alsa/NonBlock.hxx"
#include "lib/alsa/PeriodBuffer.hxx"
#include "lib/fmt/RuntimeError.hxx"
#include "lib/fmt/ToBuffer.hxx"
#include "../OutputAPI.hxx"
#include "../Error.hxx"
#include "mixer/plugins/AlsaMixerPlugin.hxx"
#include "pcm/Export.hxx"
#include "pcm/Features.h" // for ENABLE_DSD
#include "time/PeriodClock.hxx"
#include "thread/Mutex.hxx"
#include "thread/Cond.hxx"
#include "util/Manual.hxx"
#include "util/Domain.hxx"
#include "event/MultiSocketMonitor.hxx"
#include "event/InjectEvent.hxx"
#include "event/FineTimerEvent.hxx"
#include "event/Call.hxx"
#include "util/RingBuffer.hxx"
#include "Log.hxx"

#ifdef ENABLE_DSD
#include "util/AllocatedArray.hxx"
#endif

#include <alsa/asoundlib.h>

#include <atomic>
#include <string>
#include <forward_list>

static const char default_device[] = "default";

static constexpr unsigned MPD_ALSA_BUFFER_TIME_US = 500000;

class AlsaOutput final
	: AudioOutput, MultiSocketMonitor
{
	InjectEvent defer_invalidate_sockets;

	/**
	 * This timer is used to re-schedule the #MultiSocketMonitor
	 * after it had been disabled to wait for the next Play() call
	 * to deliver more data.  This timer is necessary to start
	 * generating silence if Play() doesn't get called soon enough
	 * to avoid the xrun.
	 */
	FineTimerEvent silence_timer;

	PeriodClock throttle_silence_log;

	Manual<PcmExport> pcm_export;

	/**
	 * The configured name of the ALSA device; empty for the
	 * default device
	 */
	const std::string device;

	std::forward_list<Alsa::AllowedFormat> allowed_formats;

	/**
	 * Protects #dop_setting and #allowed_formats.
	 */
	mutable Mutex attributes_mutex;

	/** the libasound PCM device handle */
	snd_pcm_t *pcm;

	/**
	 * The size of one audio frame passed to method play().
	 */
	size_t in_frame_size;

	/**
	 * The size of one audio frame passed to libasound.
	 */
	size_t out_frame_size;

	Event::Duration effective_period_duration;

	/**
	 * This buffer gets allocated after opening the ALSA device.
	 * It contains silence samples, enough to fill one period (see
	 * #period_frames).
	 */
	std::byte *silence;

	Alsa::NonBlockPcm non_block;

	/**
	 * For copying data from OutputThread to IOThread.
	 */
	using RingBuffer = ::RingBuffer<std::byte>;
	RingBuffer ring_buffer;

	Alsa::PeriodBuffer period_buffer;

	/**
	 * Protects #cond, #error, #active, #waiting, #drain.
	 */
	mutable Mutex mutex;

	/**
	 * Used to wait when #ring_buffer is full.  It will be
	 * signalled each time data is popped from the #ring_buffer,
	 * making space for more data.
	 */
	Cond cond;

	std::exception_ptr error;

	/**
	 * The size of one period, in number of frames.
	 */
	snd_pcm_uframes_t period_frames;

	/**
	 * If snd_pcm_avail() goes above this value and no more data
	 * is available in the #ring_buffer, we need to play some
	 * silence.
	 */
	snd_pcm_sframes_t max_avail_frames;

	/** libasound's buffer_time setting (in microseconds) */
	const unsigned buffer_time;

	/** libasound's period_time setting (in microseconds) */
	const unsigned period_time;

	/** the mode flags passed to snd_pcm_open */
	const int mode;

#ifdef ENABLE_DSD
	/**
	 * Enable DSD over PCM according to the DoP standard?
	 *
	 * @see http://dsd-guide.com/dop-open-standard
	 */
	bool dop_setting;

	/**
	 * Are we currently playing DSD?  (Native DSD or DoP)
	 */
	bool use_dsd;

	/**
	 * Play some silence before closing the output in DSD mode?
	 * This is a workaround for some DACs which emit noise when
	 * stopping DSD playback.
	 */
	const bool stop_dsd_silence;

	/**
	 * Are we currently draining with #stop_dsd_silence?
	 */
	bool in_stop_dsd_silence;

	/**
	 * Enable the DSD sync workaround for Thesycon USB audio
	 * receivers?  On this device, playing DSD512 or PCM causes
	 * all subsequent attempts to play other DSD rates to fail,
	 * which can be fixed by briefly playing PCM at 44.1 kHz.
	 */
	const bool thesycon_dsd_workaround;

	bool need_thesycon_dsd_workaround = thesycon_dsd_workaround;
#endif

	/**
	 * After Open() or Cancel(), has this output been activated by
	 * a Play() command?
	 *
	 * Protected by #mutex.
	 */
	bool active;

	/**
	 * Is this output waiting for more data?
	 *
	 * Protected by #mutex.
	 */
	bool waiting;

	/**
	 * Do we need to call snd_pcm_prepare() before the next write?
	 * It means that we put the device to SND_PCM_STATE_SETUP by
	 * calling snd_pcm_drop().
	 *
	 * Without this flag, we could easily recover after a failed
	 * optimistic write (returning -EBADFD), but the Raspberry Pi
	 * audio driver is infamous for generating ugly artefacts from
	 * this.
	 */
	bool must_prepare;

	/**
	 * Has snd_pcm_writei() been called successfully at least once
	 * since the PCM was prepared?
	 *
	 * This is necessary to work around a kernel bug which causes
	 * snd_pcm_drain() to return -EAGAIN forever in non-blocking
	 * mode if snd_pcm_writei() was never called.
	 */
	bool written;

	bool drain;

	/**
	 * Was Interrupt() called?  This will unblock
	 * LockWaitWriteAvailable().  It will be reset by Cancel() and
	 * Pause(), as documented by the #AudioOutput interface.
	 *
	 * Only initialized while the output is open.
	 */
	bool interrupted;

	/**
	 * Close the ALSA PCM while playback is paused?  This defaults
         * to true because this allows other applications to use the
         * PCM while MPD is paused.
	 */
	const bool close_on_pause;

	std::atomic_bool paused;

public:
	AlsaOutput(EventLoop &loop, const ConfigBlock &block);

	~AlsaOutput() noexcept override {
		/* free libasound's config cache */
		snd_config_update_free_global();
	}

	AlsaOutput(const AlsaOutput &) = delete;
	AlsaOutput &operator=(const AlsaOutput &) = delete;

	using MultiSocketMonitor::GetEventLoop;

	[[gnu::pure]]
	const char *GetDevice() const noexcept {
		return device.empty() ? default_device : device.c_str();
	}

	static AudioOutput *Create(EventLoop &event_loop,
				   const ConfigBlock &block) {
		return new AlsaOutput(event_loop, block);
	}

private:
	void UnregisterSockets() noexcept {
		MultiSocketMonitor::Reset();
		defer_invalidate_sockets.Cancel();
	}

	std::map<std::string, std::string, std::less<>> GetAttributes() const noexcept override;
	void SetAttribute(std::string &&name, std::string &&value) override;

	void Enable() override;
	void Disable() noexcept override;

	void Open(AudioFormat &audio_format) override;
	void Close() noexcept override;

	void Interrupt() noexcept override;
	std::chrono::steady_clock::duration Delay() const noexcept override;

	std::size_t Play(std::span<const std::byte> src) override;
	void Drain() override;
	void Cancel() noexcept override;
	bool Pause() noexcept override;

	/**
	 * Set up the snd_pcm_t object which was opened by the caller.
	 * Set up the configured settings and the audio format.
	 *
	 * Throws on error.
	 */
	void Setup(AudioFormat &audio_format, PcmExport::Params &params);

#ifdef ENABLE_DSD
	void SetupDop(AudioFormat audio_format,
		      PcmExport::Params &params);
#endif

	void SetupOrDop(AudioFormat &audio_format, PcmExport::Params &params
#ifdef ENABLE_DSD
			, bool dop
#endif
			);

	[[gnu::pure]]
	bool LockIsActive() const noexcept {
		const std::scoped_lock lock{mutex};
		return active;
	}

	[[gnu::pure]]
	bool LockIsActiveAndNotWaiting() const noexcept {
		const std::scoped_lock lock{mutex};
		return active && !waiting;
	}

	/**
	 * Activate the output by registering the sockets in the
	 * #EventLoop.  Before calling this, filling the ring buffer
	 * has no effect; nothing will be played, and no code will be
	 * run on #EventLoop's thread.
	 *
	 * Caller must hold the mutex.
	 *
	 * @return true if Activate() was called, false if the mutex
	 * was never unlocked
	 */
	bool Activate() noexcept {
		if (active && !waiting)
			return false;

		active = true;
		waiting = false;

		const ScopeUnlock unlock(mutex);
		defer_invalidate_sockets.Schedule();
		return true;
	}

	/**
	 * Wait until there is some space available in the ring buffer.
	 *
	 * Caller must not lock the mutex.
	 *
	 * Throws on error.
	 *
	 * @return the number of frames available for writing
	 */
	size_t LockWaitWriteAvailable();

	int Recover(int err) noexcept;

	/**
	 * Drain all buffers.  To be run in #EventLoop's thread.
	 *
	 * Throws on error.
	 *
	 * @return true if draining is complete, false if this method
	 * needs to be called again later
	 */
	bool DrainInternal();

	/**
	 * Stop playback immediately, dropping all buffers.  To be run
	 * in #EventLoop's thread.
	 */
	void CancelInternal() noexcept;

	/**
	 * @return false if no data was moved
	 */
	bool CopyRingToPeriodBuffer() noexcept;

	snd_pcm_sframes_t WriteFromPeriodBuffer() noexcept;

	void LockCaughtError() noexcept {
		period_buffer.Clear();

		const std::scoped_lock lock{mutex};
		error = std::current_exception();
		active = false;
		waiting = false;
#ifdef ENABLE_DSD
		in_stop_dsd_silence = false;
#endif
		cond.notify_one();
	}

	/**
	 * Callback for @silence_timer
	 */
	void OnSilenceTimer() noexcept {
		{
			const std::scoped_lock lock{mutex};
			assert(active);
			waiting = false;
		}

		MultiSocketMonitor::InvalidateSockets();
	}

	/* virtual methods from class MultiSocketMonitor */
	Event::Duration PrepareSockets() noexcept override;
	void DispatchSockets() noexcept override;
};

static constexpr Domain alsa_output_domain("alsa_output");

static int
GetAlsaOpenMode(const ConfigBlock &block)
{
	int mode = 0;

#ifdef SND_PCM_NO_AUTO_RESAMPLE
	if (!block.GetBlockValue("auto_resample", true))
		mode |= SND_PCM_NO_AUTO_RESAMPLE;
#endif

#ifdef SND_PCM_NO_AUTO_CHANNELS
	if (!block.GetBlockValue("auto_channels", true))
		mode |= SND_PCM_NO_AUTO_CHANNELS;
#endif

#ifdef SND_PCM_NO_AUTO_FORMAT
	if (!block.GetBlockValue("auto_format", true))
		mode |= SND_PCM_NO_AUTO_FORMAT;
#endif

	return mode;
}

AlsaOutput::AlsaOutput(EventLoop &_loop, const ConfigBlock &block)
	:AudioOutput(FLAG_ENABLE_DISABLE|FLAG_PAUSE),
	 MultiSocketMonitor(_loop),
	 defer_invalidate_sockets(_loop, BIND_THIS_METHOD(InvalidateSockets)),
	 silence_timer(_loop, BIND_THIS_METHOD(OnSilenceTimer)),
	 device(block.GetBlockValue("device", "")),
	 buffer_time(block.GetPositiveValue("buffer_time",
					    MPD_ALSA_BUFFER_TIME_US)),
	 period_time(block.GetPositiveValue("period_time", 0U)),
	 mode(GetAlsaOpenMode(block)),
#ifdef ENABLE_DSD
	 dop_setting(block.GetBlockValue("dop", false) ||
		     /* legacy name from MPD 0.18 and older: */
		     block.GetBlockValue("dsd_usb", false)),
	 stop_dsd_silence(block.GetBlockValue("stop_dsd_silence", false)),
	 thesycon_dsd_workaround(block.GetBlockValue("thesycon_dsd_workaround",
						     false)),
#endif
	close_on_pause(block.GetBlockValue("close_on_pause", true))
{
	const char *allowed_formats_string =
		block.GetBlockValue("allowed_formats", nullptr);
	if (allowed_formats_string != nullptr)
		allowed_formats = Alsa::AllowedFormat::ParseList(allowed_formats_string);
}

std::map<std::string, std::string, std::less<>>
AlsaOutput::GetAttributes() const noexcept
{
	const std::scoped_lock lock{attributes_mutex};

	return {
		{"allowed_formats", Alsa::ToString(allowed_formats)},
#ifdef ENABLE_DSD
		{"dop", dop_setting ? "1" : "0"},
#endif
	};
}

void
AlsaOutput::SetAttribute(std::string &&name, std::string &&value)
{
	if (name == "allowed_formats") {
		const std::scoped_lock lock{attributes_mutex};
		allowed_formats = Alsa::AllowedFormat::ParseList(value);
#ifdef ENABLE_DSD
	} else if (name == "dop") {
		const std::scoped_lock lock{attributes_mutex};
		if (value == "0")
			dop_setting = false;
		else if (value == "1")
			dop_setting = true;
		else
			throw std::invalid_argument("Bad 'dop' value");
#endif
	} else
		AudioOutput::SetAttribute(std::move(name), std::move(value));
}

void
AlsaOutput::Enable()
{
	pcm_export.Construct();
}

void
AlsaOutput::Disable() noexcept
{
	pcm_export.Destruct();
}

static bool
alsa_test_default_device()
{
	snd_pcm_t *handle;

	int ret = snd_pcm_open(&handle, default_device,
			       SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
	if (ret) {
		FmtError(alsa_output_domain,
			 "Error opening default ALSA device: {}",
			 snd_strerror(-ret));
		return false;
	} else
		snd_pcm_close(handle);

	return true;
}

/**
 * Wrapper for snd_pcm_sw_params().
 */
static void
AlsaSetupSw(snd_pcm_t *pcm, snd_pcm_uframes_t start_threshold,
	    snd_pcm_uframes_t avail_min)
{
	snd_pcm_sw_params_t *swparams;
	snd_pcm_sw_params_alloca(&swparams);

	int err = snd_pcm_sw_params_current(pcm, swparams);
	if (err < 0)
		throw Alsa::MakeError(err, "snd_pcm_sw_params_current() failed");

	err = snd_pcm_sw_params_set_start_threshold(pcm, swparams,
						    start_threshold);
	if (err < 0)
		throw Alsa::MakeError(err, "snd_pcm_sw_params_set_start_threshold() failed");

	err = snd_pcm_sw_params_set_avail_min(pcm, swparams, avail_min);
	if (err < 0)
		throw Alsa::MakeError(err, "snd_pcm_sw_params_set_avail_min() failed");

	err = snd_pcm_sw_params(pcm, swparams);
	if (err < 0)
		throw Alsa::MakeError(err, "snd_pcm_sw_params() failed");
}

inline void
AlsaOutput::Setup(AudioFormat &audio_format,
		  PcmExport::Params &params)
{
	const auto hw_result = Alsa::SetupHw(pcm,
					     buffer_time, period_time,
					     audio_format, params);

	FmtDebug(alsa_output_domain, "format={} ({})",
		 snd_pcm_format_name(hw_result.format),
		 snd_pcm_format_description(hw_result.format));

	FmtDebug(alsa_output_domain, "buffer_size={} period_size={}",
		 hw_result.buffer_size,
		 hw_result.period_size);

	Alsa::SetupChannelMap(pcm, audio_format.channels, params);

	AlsaSetupSw(pcm, hw_result.buffer_size - hw_result.period_size,
		    hw_result.period_size);

	auto alsa_period_size = hw_result.period_size;
	if (alsa_period_size == 0)
		/* this works around a SIGFPE bug that occurred when
		   an ALSA driver indicated period_size==0; this
		   caused a division by zero in alsa_play().  By using
		   the fallback "1", we make sure that this won't
		   happen again. */
		alsa_period_size = 1;

	period_frames = alsa_period_size;
	effective_period_duration = audio_format.FramesToTime<decltype(effective_period_duration)>(period_frames);

	/* generate silence if there's less than one period of data
	   in the ALSA-PCM buffer */
	max_avail_frames = hw_result.buffer_size - hw_result.period_size;

	silence = new std::byte[snd_pcm_frames_to_bytes(pcm, alsa_period_size)];
	snd_pcm_format_set_silence(hw_result.format, silence,
				   alsa_period_size * audio_format.channels);

}

#ifdef ENABLE_DSD

inline void
AlsaOutput::SetupDop(const AudioFormat audio_format,
		     PcmExport::Params &params)
{
	assert(audio_format.format == SampleFormat::DSD);

	/* pass 24 bit to AlsaSetup() */

	AudioFormat dop_format = audio_format;
	dop_format.format = SampleFormat::S24_P32;

	const AudioFormat check = dop_format;

	Setup(dop_format, params);

	/* if the device allows only 32 bit, shift all DoP
	   samples left by 8 bit and leave the lower 8 bit cleared;
	   the DSD-over-USB documentation does not specify whether
	   this is legal, but there is anecdotical evidence that this
	   is possible (and the only option for some devices) */
	params.shift8 = dop_format.format == SampleFormat::S32;
	if (dop_format.format == SampleFormat::S32)
		dop_format.format = SampleFormat::S24_P32;

	if (dop_format != check) {
		/* no bit-perfect playback, which is required
		   for DSD over USB */
		delete[] silence;
		throw std::runtime_error("Failed to configure DSD-over-PCM");
	}
}

#endif

inline void
AlsaOutput::SetupOrDop(AudioFormat &audio_format, PcmExport::Params &params
#ifdef ENABLE_DSD
		       , bool dop
#endif
		       )
{
#ifdef ENABLE_DSD
	std::exception_ptr dop_error;
	if (dop && audio_format.format == SampleFormat::DSD) {
		try {
			params.dsd_mode = PcmExport::DsdMode::DOP;
			SetupDop(audio_format, params);
			return;
		} catch (...) {
			dop_error = std::current_exception();
			params.dsd_mode = PcmExport::DsdMode::NONE;
		}
	}

	try {
#endif
		Setup(audio_format, params);
#ifdef ENABLE_DSD
	} catch (...) {
		if (dop_error)
			/* if DoP was attempted, prefer returning the
			   original DoP error instead of the fallback
			   error */
			std::rethrow_exception(dop_error);
		else
			throw;
	}
#endif
}

static const Alsa::AllowedFormat &
BestMatch(const std::forward_list<Alsa::AllowedFormat> &haystack,
	  const AudioFormat &needle)
{
	assert(!haystack.empty());

	for (const auto &i : haystack)
		if (needle.MatchMask(i.format))
			return i;

	return haystack.front();
}

#ifdef ENABLE_DSD

static void
Play_44_1_Silence(snd_pcm_t *pcm)
{
	snd_pcm_hw_params_t *hw;
	snd_pcm_hw_params_alloca(&hw);

	int err;

	err = snd_pcm_hw_params_any(pcm, hw);
	if (err < 0)
		throw Alsa::MakeError(err, "snd_pcm_hw_params_any() failed");

	err = snd_pcm_hw_params_set_access(pcm, hw,
					   SND_PCM_ACCESS_RW_INTERLEAVED);
	if (err < 0)
		throw Alsa::MakeError(err, "snd_pcm_hw_params_set_access() failed");

	err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16);
	if (err < 0)
		throw Alsa::MakeError(err, "snd_pcm_hw_params_set_format() failed");

	unsigned channels = 1;
	err = snd_pcm_hw_params_set_channels_near(pcm, hw, &channels);
	if (err < 0)
		throw Alsa::MakeError(err, "snd_pcm_hw_params_set_channels_near() failed");

	constexpr snd_pcm_uframes_t rate = 44100;
	err = snd_pcm_hw_params_set_rate(pcm, hw, rate, 0);
	if (err < 0)
		throw Alsa::MakeError(err, "snd_pcm_hw_params_set_rate() failed");

	snd_pcm_uframes_t buffer_size = 1;
	err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer_size);
	if (err < 0)
		throw Alsa::MakeError(err, "snd_pcm_hw_params_set_buffer_size_near() failed");

	snd_pcm_uframes_t period_size = 1;
	int dir = 0;
	err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period_size,
						     &dir);
	if (err < 0)
		throw Alsa::MakeError(err, "snd_pcm_hw_params_set_period_size_near() failed");

	err = snd_pcm_hw_params(pcm, hw);
	if (err < 0)
		throw Alsa::MakeError(err, "snd_pcm_hw_params() failed");

	snd_pcm_sw_params_t *sw;
	snd_pcm_sw_params_alloca(&sw);

	err = snd_pcm_sw_params_current(pcm, sw);
	if (err < 0)
		throw Alsa::MakeError(err, "snd_pcm_sw_params_current() failed");

	err = snd_pcm_sw_params_set_start_threshold(pcm, sw, period_size);
	if (err < 0)
		throw Alsa::MakeError(err, "snd_pcm_sw_params_set_start_threshold() failed");

	err = snd_pcm_sw_params(pcm, sw);
	if (err < 0)
		throw Alsa::MakeError(err, "snd_pcm_sw_params() failed");

	err = snd_pcm_prepare(pcm);
	if (err < 0)
		throw Alsa::MakeError(err, "snd_pcm_prepare() failed");

	AllocatedArray<int16_t> buffer{channels * period_size};
	buffer = std::span<const int16_t>{};

	/* play at least 250ms of silence */
	for (snd_pcm_uframes_t remaining_frames = rate / 4;;) {
		auto n = snd_pcm_writei(pcm, buffer.data(),
					period_size);
		if (n < 0)
			throw Alsa::MakeError(err, "snd_pcm_writei() failed");

		if (snd_pcm_uframes_t(n) >= remaining_frames)
			break;

		remaining_frames -= snd_pcm_uframes_t(n);
	}

	err = snd_pcm_drain(pcm);
	if (err < 0)
		throw Alsa::MakeError(err, "snd_pcm_drain() failed");
}

#endif

void
AlsaOutput::Open(AudioFormat &audio_format)
{
	paused = false;

#ifdef ENABLE_DSD
	bool dop;
#endif

	{
		const std::scoped_lock lock{attributes_mutex};
#ifdef ENABLE_DSD
		dop = dop_setting;
#endif

		if (!allowed_formats.empty()) {
			const auto &a = BestMatch(allowed_formats,
						  audio_format);
			audio_format.ApplyMask(a.format);
#ifdef ENABLE_DSD
			dop = a.dop;
#endif
		}
	}

	int err = snd_pcm_open(&pcm, GetDevice(),
			       SND_PCM_STREAM_PLAYBACK, mode);
	if (err < 0)
		throw Alsa::MakeError(err,
				      FmtBuffer<256>("Failed to open ALSA device {:?}",
						     GetDevice()));

	const char *pcm_name = snd_pcm_name(pcm);
	if (pcm_name == nullptr)
		pcm_name = "?";

	FmtDebug(alsa_output_domain, "opened {:?} type={}",
		 pcm_name,
		 snd_pcm_type_name(snd_pcm_type(pcm)));

#ifdef ENABLE_DSD
	if (need_thesycon_dsd_workaround &&
	    audio_format.format == SampleFormat::DSD &&
	    audio_format.sample_rate <= 256 * 44100 / 8) {
		LogDebug(alsa_output_domain, "Playing some 44.1 kHz silence");

		try {
			Play_44_1_Silence(pcm);
		} catch (...) {
			LogError(std::current_exception());
		}

		need_thesycon_dsd_workaround = false;
	}
#endif

	PcmExport::Params params;

	try {
		SetupOrDop(audio_format, params
#ifdef ENABLE_DSD
			   , dop
#endif
			   );
	} catch (...) {
		snd_pcm_close(pcm);
		std::throw_with_nested(FmtRuntimeError("Error opening ALSA device {:?}",
						       GetDevice()));
	}

	snd_pcm_nonblock(pcm, 1);

#ifdef ENABLE_DSD
	use_dsd = audio_format.format == SampleFormat::DSD;
	in_stop_dsd_silence = false;

	if (thesycon_dsd_workaround &&
	    (!use_dsd ||
	     audio_format.sample_rate > 256 * 44100 / 8))
		need_thesycon_dsd_workaround = true;

	if (params.dsd_mode == PcmExport::DsdMode::DOP)
		LogDebug(alsa_output_domain, "DoP (DSD over PCM) enabled");
#endif

	pcm_export->Open(audio_format.format,
			 audio_format.channels,
			 params);

	in_frame_size = audio_format.GetFrameSize();
	out_frame_size = pcm_export->GetOutputFrameSize();

	drain = false;
	interrupted = false;

	size_t period_size = period_frames * out_frame_size;
	ring_buffer = RingBuffer{period_size * 4};

	period_buffer.Allocate(period_frames, out_frame_size);

	active = false;
	waiting = false;
	must_prepare = false;
	written = false;
	error = {};
}

void
AlsaOutput::Interrupt() noexcept
{
	std::scoped_lock lock{mutex};

	/* the "interrupted" flag will prevent
	   LockWaitWriteAvailable() from actually waiting, and will
	   instead throw AudioOutputInterrupted */
	interrupted = true;
	cond.notify_one();
}

std::chrono::steady_clock::duration
AlsaOutput::Delay() const noexcept
{
	if (paused)
		return std::chrono::steady_clock::duration::max();

	return AudioOutput::Delay();
}

inline int
AlsaOutput::Recover(int err) noexcept
{
	if (err == -EPIPE) {
		FmtDebug(alsa_output_domain,
			 "Underrun on ALSA device {:?}",
			 GetDevice());
	} else if (err == -ESTRPIPE) {
		FmtDebug(alsa_output_domain,
			 "ALSA device {:?} was suspended",
			 GetDevice());
	}

	switch (snd_pcm_state(pcm)) {
	case SND_PCM_STATE_PAUSED:
		err = snd_pcm_pause(pcm, /* disable */ 0);
		break;
	case SND_PCM_STATE_SUSPENDED:
		err = snd_pcm_resume(pcm);
		if (err == -EAGAIN)
			return 0;
		/* fall-through to snd_pcm_prepare: */
		[[fallthrough]];

	case SND_PCM_STATE_SETUP:
	case SND_PCM_STATE_XRUN:
		period_buffer.Rewind();
		written = false;
		err = snd_pcm_prepare(pcm);
		break;

	case SND_PCM_STATE_OPEN:
	case SND_PCM_STATE_DISCONNECTED:
	case SND_PCM_STATE_DRAINING:
		/* can't play in this state; throw the error */
		break;

	case SND_PCM_STATE_PREPARED:
	case SND_PCM_STATE_RUNNING:
		/* the state is ok, but the error was unexpected;
		   throw it */
		break;

	default:
		/* this default case is just here to work around
		   -Wswitch due to SND_PCM_STATE_PRIVATE1 (libasound
		   1.1.6) */
		break;
	}

	return err;
}

bool
AlsaOutput::CopyRingToPeriodBuffer() noexcept
{
	if (period_buffer.IsFull())
		return false;

	size_t nbytes = ring_buffer.ReadTo({period_buffer.GetTail(), period_buffer.GetSpaceBytes()});
	if (nbytes == 0)
		return false;

	period_buffer.AppendBytes(nbytes);

	const std::scoped_lock lock{mutex};
	/* notify the OutputThread that there is now
	   room in ring_buffer */
	cond.notify_one();

	return true;
}

snd_pcm_sframes_t
AlsaOutput::WriteFromPeriodBuffer() noexcept
{
	assert(period_buffer.IsFull());
	assert(period_buffer.GetFrames(out_frame_size) > 0);

	auto frames_written = snd_pcm_writei(pcm, period_buffer.GetHead(),
					     period_buffer.GetFrames(out_frame_size));
	if (frames_written > 0) {
		written = true;
		period_buffer.ConsumeFrames(frames_written,
					    out_frame_size);
	}

	return frames_written;
}

inline bool
AlsaOutput::DrainInternal()
{
#ifdef ENABLE_DSD
	if (in_stop_dsd_silence) {
		/* "stop_dsd_silence" is in progress: clear internal
		   buffers and instead, fill the period buffer with
		   silence */
		in_stop_dsd_silence = false;
		ring_buffer.Clear();
		period_buffer.Clear();
		period_buffer.FillWithSilence(silence, out_frame_size);
	}
#endif

	/* drain ring_buffer */
	CopyRingToPeriodBuffer();

	/* drain period_buffer */
	if (!period_buffer.IsCleared()) {
		if (!period_buffer.IsFull())
			/* generate some silence to finish the partial
			   period */
			period_buffer.FillWithSilence(silence, out_frame_size);

		/* drain period_buffer */
		unsigned int retry_count = 0;
		while (!period_buffer.IsDrained() && retry_count <= 1) {
			auto frames_written = WriteFromPeriodBuffer();
			if (frames_written < 0) {
				if (frames_written == -EAGAIN || frames_written == -EINTR)
					return false;

				if (Recover(frames_written) < 0)
					throw Alsa::MakeError(frames_written,
							      "snd_pcm_writei() failed");

				retry_count++;
				continue;
			}

			/* need to call CopyRingToPeriodBuffer() and
			   WriteFromPeriodBuffer() again in the next
			   iteration, so don't finish the drain just
			   yet */
			return false;
		}
	}

	if (!written)
		/* if nothing has ever been written to the PCM, we
		   don't need to drain it */
		return true;

	switch (snd_pcm_state(pcm)) {
	case SND_PCM_STATE_PREPARED:
	case SND_PCM_STATE_RUNNING:
		/* these states require a call to snd_pcm_drain() */
		break;

	case SND_PCM_STATE_DRAINING:
		/* already draining, but not yet finished; this is
		   probably a spurious epoll event, and we should wait
		   for the next one */
		return false;

	default:
		/* all other states cannot be drained, and we're
		   done */
		return true;
	}

	/* .. and finally drain the ALSA hardware buffer */

	const int result = snd_pcm_drain(pcm);
	if (result == 0)
		return true;
	else if (result == -EAGAIN)
		return false;
	else
		throw Alsa::MakeError(result, "snd_pcm_drain() failed");
}

void
AlsaOutput::Drain()
{
	std::unique_lock lock{mutex};

	if (error)
		std::rethrow_exception(error);

	drain = true;

	Activate();

	cond.wait(lock, [this]{ return !drain || !active; });

	if (error)
		std::rethrow_exception(error);
}

inline void
AlsaOutput::CancelInternal() noexcept
{
	/* this method doesn't need to lock the mutex because while it
	   runs, the calling thread is blocked inside Cancel() */

	must_prepare = true;

	snd_pcm_drop(pcm);

	pcm_export->Reset();
	period_buffer.Clear();
	ring_buffer.Clear();

	active = false;
	waiting = false;

	UnregisterSockets();
	silence_timer.Cancel();
}

void
AlsaOutput::Cancel() noexcept
{
	{
		std::lock_guard lock{mutex};
		interrupted = false;
	}

	if (!LockIsActive()) {
		/* early cancel, quick code path without thread
		   synchronization */

		pcm_export->Reset();
		assert(period_buffer.IsCleared());
		ring_buffer.Clear();

		return;
	}

#ifdef ENABLE_DSD
	if (stop_dsd_silence && use_dsd) {
		/* play some DSD silence instead of snd_pcm_drop() */
		std::unique_lock lock{mutex};
		in_stop_dsd_silence = true;
		drain = true;
		cond.wait(lock, [this]{ return !drain || !active; });
		return;
	}
#endif

	BlockingCall(GetEventLoop(), [this](){
		CancelInternal();
	});
}

bool
AlsaOutput::Pause() noexcept
{
	std::lock_guard lock{mutex};
	interrupted = false;

	if (close_on_pause)
		return false;

	// TODO use snd_pcm_pause()?

	paused = true;
	return true;
}

void
AlsaOutput::Close() noexcept
{
	/* make sure the I/O thread isn't inside DispatchSockets() */
	BlockingCall(GetEventLoop(), [this](){
		UnregisterSockets();
		silence_timer.Cancel();
	});

	period_buffer.Free();
	ring_buffer = {};
	snd_pcm_close(pcm);
	delete[] silence;
}

size_t
AlsaOutput::LockWaitWriteAvailable()
{
	const size_t out_block_size = pcm_export->GetOutputBlockSize();
	const size_t min_available = 2 * out_block_size;

	std::unique_lock lock{mutex};

	while (true) {
		if (error)
			std::rethrow_exception(error);

		if (interrupted)
			/* a CANCEL command is in flight - don't block
			   here */
			throw AudioOutputInterrupted{};

		size_t write_available = ring_buffer.WriteAvailable();
		if (write_available >= min_available) {
			/* reserve room for one extra block, just in
			   case PcmExport::Export() has some partial
			   block data in its internal buffer */
			write_available -= out_block_size;

			return write_available / out_frame_size;
		}

		/* now that the ring_buffer is full, we can activate
		   the socket handlers to trigger the first
		   snd_pcm_writei() */
		if (Activate())
			/* since everything may have changed while the
			   mutex was unlocked, we need to skip the
			   cond.wait() call below and check the new
			   status */
			continue;

		/* wait for the DispatchSockets() to make room in the
		   ring_buffer */
		cond.wait(lock);
	}
}

std::size_t
AlsaOutput::Play(std::span<const std::byte> src)
{
	assert(!src.empty());
	assert(src.size() % in_frame_size == 0);

	paused = false;

	const size_t max_frames = LockWaitWriteAvailable();
	const size_t max_size = max_frames * in_frame_size;
	if (src.size() > max_size)
		src = src.first(max_size);

	const auto e = pcm_export->Export(src);
	if (e.empty())
		return src.size();

	size_t bytes_written = ring_buffer.WriteFrom(e);
	assert(bytes_written == e.size());
	(void)bytes_written;

	return src.size();
}

Event::Duration
AlsaOutput::PrepareSockets() noexcept
{
	if (!LockIsActiveAndNotWaiting()) {
		ClearSocketList();
		return Event::Duration(-1);
	}

	try {
		return non_block.PrepareSockets(*this, pcm);
	} catch (...) {
		ClearSocketList();
		LockCaughtError();
		return Event::Duration(-1);
	}
}

void
AlsaOutput::DispatchSockets() noexcept
try {
	non_block.DispatchSockets(*this, pcm);

	if (must_prepare) {
		must_prepare = false;
		written = false;

		int err = snd_pcm_prepare(pcm);
		if (err < 0)
			throw Alsa::MakeError(err, "snd_pcm_prepare() failed");
	}

	{
		const std::scoped_lock lock{mutex};

		assert(active);

		if (drain) {
			{
				ScopeUnlock unlock(mutex);
				if (!DrainInternal())
					return;

				MultiSocketMonitor::InvalidateSockets();
			}

			drain = false;
			cond.notify_one();
			return;
		}
	}

	CopyRingToPeriodBuffer();

	if (!period_buffer.IsFull()) {
		if (snd_pcm_state(pcm) == SND_PCM_STATE_PREPARED ||
		    snd_pcm_avail(pcm) <= max_avail_frames) {
			/* at SND_PCM_STATE_PREPARED (not yet switched
			   to SND_PCM_STATE_RUNNING), we have no
			   pressure to fill the ALSA buffer, because
			   no xrun can possibly occur; and if no data
			   is available right now, we can easily wait
			   until some is available; so we just stop
			   monitoring the ALSA file descriptor, and
			   let it be reactivated by Play()/Activate()
			   whenever more data arrives */
			/* the same applies when there is still enough
			   data in the ALSA-PCM buffer (determined by
			   snd_pcm_avail()); this can happen at the
			   start of playback, when our ring_buffer is
			   smaller than the ALSA-PCM buffer */

			{
				const std::scoped_lock lock{mutex};
				waiting = true;
				cond.notify_one();
			}

			/* avoid race condition: see if data has
			   arrived meanwhile before disabling the
			   event (but after setting the "waiting"
			   flag) */
			if (!CopyRingToPeriodBuffer()) {
				UnregisterSockets();

				/* just in case Play() doesn't get
				   called soon enough, schedule a
				   timer which generates silence
				   before the xrun occurs */
				/* the timer fires in half of a
				   period; this short duration may
				   produce a few more wakeups than
				   necessary, but should be small
				   enough to avoid the xrun */
				silence_timer.Schedule(effective_period_duration / 2);
			}

			return;
		}

		if (throttle_silence_log.CheckUpdate(std::chrono::seconds(5)))
			LogWarning(alsa_output_domain, "Decoder is too slow; playing silence to avoid xrun");

		/* insert some silence if the buffer has not enough
		   data yet, to avoid ALSA xrun */
		period_buffer.FillWithSilence(silence, out_frame_size);
	}

	auto frames_written = WriteFromPeriodBuffer();
	if (frames_written < 0) {
		if (frames_written == -EAGAIN || frames_written == -EINTR)
			/* try again in the next DispatchSockets()
			   call which is still scheduled */
			return;

		if (Recover(frames_written) < 0)
			throw Alsa::MakeError(frames_written,
					      "snd_pcm_writei() failed");

		/* recovered; try again in the next DispatchSockets()
		   call */
		return;
	}
} catch (...) {
	MultiSocketMonitor::Reset();
	LockCaughtError();
}

constexpr struct AudioOutputPlugin alsa_output_plugin = {
	"alsa",
	alsa_test_default_device,
	&AlsaOutput::Create,
	&alsa_mixer_plugin,
};
