// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "db/plugins/simple/Directory.hxx"
#include "decoder/DecoderList.hxx"
#include "playlist/PlaylistRegistry.hxx"
#include "fs/Traits.hxx"

#include "archive/Features.h" // for ENABLE_ARCHIVE
#ifdef ENABLE_ARCHIVE
#include "archive/ArchiveList.hxx"
#endif

[[gnu::pure]]
static bool
HaveArchivePluginForFilename(std::string_view filename) noexcept
{
#ifdef ENABLE_ARCHIVE
	const auto suffix = PathTraitsUTF8::GetFilenameSuffix(filename);
	return !suffix.empty() &&
		archive_plugin_from_suffix(suffix) != nullptr;
#else
	(void)filename;
	return false;
#endif
}

[[gnu::pure]]
static bool
HaveContainerPluginForFilename(std::string_view filename) noexcept
{
	const auto suffix = PathTraitsUTF8::GetFilenameSuffix(filename);
	return !suffix.empty() &&
		// TODO: check if this plugin really supports containers
		decoder_plugins_supports_suffix(suffix);
}

[[gnu::pure]]
static bool
HavePlaylistPluginForFilename(std::string_view filename) noexcept
{
	const auto suffix = PathTraitsUTF8::GetFilenameSuffix(filename);
	if (suffix.empty())
		return false;

	const auto plugin = FindPlaylistPluginBySuffix(suffix);
	if (plugin == nullptr)
		return false;

	/* discard the special directory if the user disables the
	   plugin's "as_directory" setting */
	return GetPlaylistPluginAsFolder(*plugin);
}

bool
Directory::IsPluginAvailable() const noexcept
{
	switch (device) {
	case DEVICE_INARCHIVE:
		return HaveArchivePluginForFilename(GetName());

	case DEVICE_CONTAINER:
		return HaveContainerPluginForFilename(GetName());

	case DEVICE_PLAYLIST:
		return HavePlaylistPluginForFilename(GetName());

	default:
		return true;
	}
}
