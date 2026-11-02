/**************************************************************************/
/*  file_access_windows.h                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#ifdef WINDOWS_ENABLED

#include "core/io/file_access.h"

#include <cstdio>
#include <cstring>

class FileAccessWindows : public FileAccess {
	GDSOFTCLASS(FileAccessWindows, FileAccess);
	FILE *f = nullptr;
	int flags = 0;
	void check_errors(bool p_write = false) const;
	mutable int prev_op = 0;
	mutable Error last_error = OK;
	String path;
	String path_src;
	String save_path;

	// Internal read cache. Lets get_8/16/32/64 and any small get_buffer call
	// avoid the per-call CRT fread overhead (function call + _lock_file). All
	// OS reads go through _fread_nolock in cache-sized chunks. Invalidated on
	// seek / write / resize / close.
	static constexpr uint64_t READ_CACHE_SIZE = 32768;
	mutable uint8_t read_cache[READ_CACHE_SIZE];
	mutable uint64_t read_cache_pos = 0; // File offset of read_cache[0].
	mutable uint32_t read_cache_filled = 0; // Valid bytes in read_cache.
	mutable uint32_t read_cache_consumed = 0; // Bytes already served to callers.
	mutable bool read_eof_seen = false;

	void _invalidate_read_cache() const;
	uint64_t _fill_read_cache() const;
	void _sync_for_write() const;
	_FORCE_INLINE_ void _ensure_read_mode() const {
		if (flags == READ_WRITE || flags == WRITE_READ) {
			if (prev_op == WRITE) {
				fflush(f);
				_invalidate_read_cache();
			}
			prev_op = READ;
		}
	}

	// Shared fast path for get_16/32/64: little-endian read from the cache,
	// falls through to get_buffer when the value straddles a refill boundary.
	template <typename T>
	_FORCE_INLINE_ T _get_cached_le() const {
		T data = 0;
		if ((uint64_t)(read_cache_filled - read_cache_consumed) >= sizeof(T)) {
			memcpy(&data, &read_cache[read_cache_consumed], sizeof(T));
			read_cache_consumed += sizeof(T);
			return data;
		}
		get_buffer(reinterpret_cast<uint8_t *>(&data), sizeof(T));
		return data;
	}

	void _close();

	static HashSet<String> invalid_files;

public:
	static bool is_path_invalid(const String &p_path);

	virtual String fix_path(const String &p_path) const override;
	virtual Error open_internal(const String &p_path, int p_mode_flags) override; ///< open a file
	virtual bool is_open() const override; ///< true when file is open

	virtual String get_path() const override; /// returns the path for the current open file
	virtual String get_path_absolute() const override; /// returns the absolute path for the current open file

	virtual void seek(uint64_t p_position) override; ///< seek to a given position
	virtual void seek_end(int64_t p_position = 0) override; ///< seek from the end of file
	virtual uint64_t get_position() const override; ///< get position in the file
	virtual uint64_t get_length() const override; ///< get size of the file

	virtual bool eof_reached() const override; ///< reading passed EOF

	// Hot-path override: serve a single byte directly from the read cache,
	// bypassing the get_buffer virtual dispatch and its mode/length checks.
	virtual uint8_t get_8() const override;
	virtual uint16_t get_16() const override;
	virtual uint32_t get_32() const override;
	virtual uint64_t get_64() const override;

	virtual uint64_t get_buffer(uint8_t *p_dst, uint64_t p_length) const override;

	virtual Error get_error() const override; ///< get last error

	virtual Error resize(int64_t p_length) override;
	virtual void flush() override;
	virtual bool store_buffer(const uint8_t *p_src, uint64_t p_length) override; ///< store an array of bytes

	virtual bool file_exists(const String &p_name) override; ///< return true if a file exists

	uint64_t _get_modified_time(const String &p_file) override;
	uint64_t _get_access_time(const String &p_file) override;
	int64_t _get_size(const String &p_file) override;
	virtual BitField<FileAccess::UnixPermissionFlags> _get_unix_permissions(const String &p_file) override;
	virtual Error _set_unix_permissions(const String &p_file, BitField<FileAccess::UnixPermissionFlags> p_permissions) override;

	virtual bool _get_hidden_attribute(const String &p_file) override;
	virtual Error _set_hidden_attribute(const String &p_file, bool p_hidden) override;
	virtual bool _get_read_only_attribute(const String &p_file) override;
	virtual Error _set_read_only_attribute(const String &p_file, bool p_ro) override;

	virtual PackedByteArray _get_extended_attribute(const String &p_file, const String &p_attribute_name) override;
	virtual Error _set_extended_attribute(const String &p_file, const String &p_attribute_name, const PackedByteArray &p_data) override;
	virtual Error _remove_extended_attribute(const String &p_file, const String &p_attribute_name) override;
	virtual PackedStringArray _get_extended_attributes_list(const String &p_file) override;

	virtual void close() override;

	static void initialize();
	static void finalize();

	FileAccessWindows() {}
	virtual ~FileAccessWindows();
};

#endif // WINDOWS_ENABLED
