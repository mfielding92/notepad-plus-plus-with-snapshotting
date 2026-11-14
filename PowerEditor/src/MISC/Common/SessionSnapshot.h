// This file is part of Notepad++ project
// Copyright (C)2026 Notepad++ team

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// at your option any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.


#pragma once

#include <windows.h>

#include <cstddef>
#include <string>

// Periodic whole-session snapshots (YYYYMMDD-HHMMSS_SNAPSHOT.xml) written into the backup folder.
// This module has no dependency on the rest of Notepad++, so it can be built and tested alone.
namespace SessionSnapshot
{
	inline constexpr unsigned DEFAULT_RETENTION_HOURS = 24;
	inline constexpr unsigned MAX_RETENTION_HOURS = 8760;      // 365 days
	inline constexpr unsigned long long PRUNE_MIN_INTERVAL_MS = 60000;

	std::wstring makeFileName(const SYSTEMTIME& localTime);
	bool parseFileName(const std::wstring& fileName, SYSTEMTIME& outLocalTime);
	std::wstring resolveDir(bool useCustomDir, const std::wstring& customDir, const std::wstring& userPath);
	bool ensureDir(const std::wstring& dir, DWORD* lastError = nullptr);
	bool writeFileAtomic(const std::wstring& path, const char* data, size_t len, DWORD* lastError = nullptr);

	struct PruneResult { size_t deleted = 0; size_t kept = 0; size_t failed = 0; };
	PruneResult prune(const std::wstring& dir, const SYSTEMTIME& nowLocal, unsigned retentionHours);

	std::string base64Encode(const char* data, size_t len);
	bool base64Decode(const std::string& text, std::string& out);

	enum class WriteOutcome { Ok, OkFallback, Failed };

	// UI thread only: decides when a snapshot is due, and when the user has to be told about a failure (each time it gets worse, until an Ok)
	class Scheduler
	{
	public:
		void markDirty();
		bool shouldWrite() const;
		void onWriteResult(WriteOutcome outcome);
		bool consumeFailureNotice();
		bool shouldPrune(unsigned long long nowTickMs) const;
		void onPruned(unsigned long long nowTickMs);

	private:
		enum class Level { Ok, OkFallback, Failed }; // in order of seriousness

		bool _isPending = false;
		bool _isNoticeOwed = false;
		Level _level = Level::Ok;
		bool _hasPruned = false;
		unsigned long long _lastPruneTick = 0;
	};
}
