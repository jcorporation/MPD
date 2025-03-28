// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "SmbclientInputPlugin.hxx"
#include "lib/smbclient/Init.hxx"
#include "lib/smbclient/Context.hxx"
#include "../InputStream.hxx"
#include "../InputPlugin.hxx"
#include "../MaybeBufferedInputStream.hxx"
#include "PluginUnavailable.hxx"
#include "system/Error.hxx"

#include <libsmbclient.h>

class SmbclientInputStream final : public InputStream {
	SmbclientContext ctx;
	SMBCFILE *const handle;

public:
	SmbclientInputStream(std::string &&_uri,
			     Mutex &_mutex,
			     SmbclientContext &&_ctx,
			     SMBCFILE *_handle, const struct stat &st)
		:InputStream(_uri, _mutex),
		 ctx(std::move(_ctx)), handle(_handle)
	{
		seekable = true;
		size = st.st_size;
		SetReady();
	}

	~SmbclientInputStream() override {
		ctx.Close(handle);
	}

	/* virtual methods from InputStream */

	[[nodiscard]] bool IsEOF() const noexcept override {
		return offset >= size;
	}

	size_t Read(std::unique_lock<Mutex> &lock,
		    std::span<std::byte> dest) override;
	void Seek(std::unique_lock<Mutex> &lock, offset_type offset) override;
};

/*
 * InputPlugin methods
 *
 */

static void
input_smbclient_init(EventLoop &, const ConfigBlock &)
{
	try {
		SmbclientInit();
	} catch (...) {
		std::throw_with_nested(PluginUnavailable("libsmbclient initialization failed"));
	}

	// TODO: create one global SMBCCTX here?

	// TODO: evaluate ConfigBlock, call smbc_setOption*()
}

static InputStreamPtr
input_smbclient_open(std::string_view _uri,
		     Mutex &mutex)
{
	auto ctx = SmbclientContext::New();

	std::string uri{_uri};
	SMBCFILE *handle = ctx.OpenReadOnly(uri.c_str());
	if (handle == nullptr)
		throw MakeErrno("smbc_open() failed");

	struct stat st;
	if (ctx.Stat(handle, st) < 0) {
		const int e = errno;
		ctx.Close(handle);
		throw MakeErrno(e, "smbc_fstat() failed");
	}

	return std::make_unique<MaybeBufferedInputStream>
		(std::make_unique<SmbclientInputStream>(std::move(uri), mutex,
							std::move(ctx),
							handle, st));
}

size_t
SmbclientInputStream::Read(std::unique_lock<Mutex> &,
			   std::span<std::byte> dest)
{
	ssize_t nbytes;

	{
		const ScopeUnlock unlock(mutex);
		nbytes = ctx.Read(handle, dest.data(), dest.size());
	}

	if (nbytes < 0)
		throw MakeErrno("smbc_read() failed");

	offset += nbytes;
	return nbytes;
}

void
SmbclientInputStream::Seek(std::unique_lock<Mutex> &,
			   offset_type new_offset)
{
	off_t result;

	{
		const ScopeUnlock unlock(mutex);
		result = ctx.Seek(handle, new_offset);
	}

	if (result < 0)
		throw MakeErrno("smbc_lseek() failed");

	offset = result;
}

static constexpr const char *smbclient_prefixes[] = {
	"smb://",
	nullptr
};

const InputPlugin input_plugin_smbclient = {
	"smbclient",
	smbclient_prefixes,
	input_smbclient_init,
	nullptr,
	input_smbclient_open,
	nullptr
};
