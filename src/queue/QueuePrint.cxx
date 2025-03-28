// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "QueuePrint.hxx"
#include "Queue.hxx"
#include "song/Filter.hxx"
#include "SongPrint.hxx"
#include "song/DetachedSong.hxx"
#include "song/LightSong.hxx"
#include "client/Response.hxx"

#include <fmt/format.h>

/**
 * Send detailed information about a range of songs in the queue to a
 * client.
 *
 * @param client the client which has requested information
 * @param start the index of the first song (including)
 * @param end the index of the last song (excluding)
 */
static void
queue_print_song_info(Response &r, const Queue &queue,
		      unsigned position)
{
	song_print_info(r, queue.Get(position));
	r.Fmt("Pos: {}\nId: {}\n",
	      position, queue.PositionToId(position));

	uint8_t priority = queue.GetPriorityAtPosition(position);
	if (priority != 0)
		r.Fmt("Prio: {}\n", priority);
}

void
queue_print_info(Response &r, const Queue &queue,
		 unsigned start, unsigned end)
{
	assert(start <= end);
	assert(end <= queue.GetLength());

	for (unsigned i = start; i < end; ++i)
		queue_print_song_info(r, queue, i);
}

void
queue_print_uris(Response &r, const Queue &queue,
		 unsigned start, unsigned end)
{
	assert(start <= end);
	assert(end <= queue.GetLength());

	for (unsigned i = start; i < end; ++i) {
		r.Fmt("{}:", i);
		song_print_uri(r, queue.Get(i));
	}
}

void
queue_print_changes_info(Response &r, const Queue &queue,
			 uint32_t version,
			 unsigned start, unsigned end)
{
	assert(start <= end);
	assert(end <= queue.GetLength());

	for (unsigned i = start; i < end; i++)
		if (queue.IsNewerAtPosition(i, version))
			queue_print_song_info(r, queue, i);
}

void
queue_print_changes_position(Response &r, const Queue &queue,
			     uint32_t version,
			     unsigned start, unsigned end)
{
	assert(start <= end);
	assert(end <= queue.GetLength());

	for (unsigned i = start; i < end; i++)
		if (queue.IsNewerAtPosition(i, version))
			r.Fmt("cpos: {}\nId: {}\n",
			      i, queue.PositionToId(i));
}

void
queue_find(Response &r, const Queue &queue,
	   const SongFilter &filter)
{
	for (unsigned i = 0; i < queue.GetLength(); i++) {
		const LightSong song{queue.Get(i)};

		if (filter.Match(song))
			queue_print_song_info(r, queue, i);
	}
}
