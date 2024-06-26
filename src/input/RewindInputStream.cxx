// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "RewindInputStream.hxx"
#include "ProxyInputStream.hxx"

#include <cassert>

#include <string.h>

class RewindInputStream final : public ProxyInputStream {
	/**
	 * The read position within the buffer.  Undefined as long as
	 * ReadingFromBuffer() returns false.
	 */
	size_t head;

	/**
	 * The write/append position within the buffer.
	 */
	size_t tail = 0;

	/**
	 * The size of this buffer is the maximum number of bytes
	 * which can be rewinded cheaply without passing the "seek"
	 * call to CURL.
	 *
	 * The origin of this buffer is always the beginning of the
	 * stream (offset 0).
	 */
	std::byte buffer[64 * 1024];

public:
	explicit RewindInputStream(InputStreamPtr _input)
		:ProxyInputStream(std::move(_input)) {}

	/* virtual methods from InputStream */

	void Update() noexcept override {
		if (!ReadingFromBuffer())
			ProxyInputStream::Update();
	}

	[[nodiscard]] bool IsEOF() const noexcept override {
		return !ReadingFromBuffer() && ProxyInputStream::IsEOF();
	}

	size_t Read(std::unique_lock<Mutex> &lock,
		    std::span<std::byte> dest) override;
	void Seek(std::unique_lock<Mutex> &lock, offset_type offset) override;

private:
	/**
	 * Are we currently reading from the buffer, and does the
	 * buffer contain more data for the next read operation?
	 */
	[[nodiscard]] bool ReadingFromBuffer() const noexcept {
		return tail > 0 && offset < input->GetOffset();
	}
};

size_t
RewindInputStream::Read(std::unique_lock<Mutex> &lock,
			std::span<std::byte> dest)
{
	if (ReadingFromBuffer()) {
		/* buffered read */

		assert(head == (size_t)offset);
		assert(tail == (size_t)input->GetOffset());

		if (dest.size() > tail - head)
			dest = dest.first(tail - head);

		memcpy(dest.data(), buffer + head, dest.size());
		head += dest.size();
		offset += dest.size();

		return dest.size();
	} else {
		/* pass method call to underlying stream */

		size_t nbytes = input->Read(lock, dest);

		if (std::cmp_greater(input->GetOffset(), sizeof(buffer)))
			/* disable buffering */
			tail = 0;
		else if (tail == (size_t)offset) {
			/* append to buffer */

			memcpy(buffer + tail, dest.data(), nbytes);
			tail += nbytes;

			assert(tail == (size_t)input->GetOffset());
		}

		CopyAttributes();

		return nbytes;
	}
}

void
RewindInputStream::Seek(std::unique_lock<Mutex> &lock, offset_type new_offset)
{
	assert(IsReady());

	if (tail > 0 && std::cmp_less_equal(new_offset, tail)) {
		/* buffered seek */

		assert(!ReadingFromBuffer() ||
		       head == (size_t)offset);
		assert(tail == (size_t)input->GetOffset());

		head = (size_t)new_offset;
		offset = new_offset;
	} else {
		/* disable the buffer, because input has left the
		   buffered range now */
		tail = 0;

		ProxyInputStream::Seek(lock, new_offset);
	}
}

InputStreamPtr
input_rewind_open(InputStreamPtr is)
{
	assert(is != nullptr);
	assert(!is->IsReady() || is->GetOffset() == 0);

	if (is->IsReady() && is->IsSeekable())
		/* seekable resources don't need this plugin */
		return is;

	return std::make_unique<RewindInputStream>(std::move(is));
}
