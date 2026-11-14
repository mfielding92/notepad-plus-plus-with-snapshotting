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


#include "SessionSnapshot.h"

#include <climits>
#include <cwchar>
#include <vector>

namespace
{
	constexpr wchar_t SNAPSHOT_SUFFIX[] = L"_SNAPSHOT.xml";
	constexpr size_t SNAPSHOT_SUFFIX_LEN = 13;
	constexpr size_t SNAPSHOT_NAME_LEN = 15 + SNAPSHOT_SUFFIX_LEN; // YYYYMMDD-HHMMSS + suffix
	constexpr wchar_t TMP_EXT[] = L".tmp";
	constexpr size_t TMP_EXT_LEN = 4;
	constexpr unsigned long long FILETIME_TICKS_PER_HOUR = 36000000000ULL; // 100 ns ticks
	constexpr DWORD MAX_WRITE_CHUNK = 1024 * 1024 * 1024;

	const char base64Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	void setError(DWORD* lastError, DWORD err)
	{
		if (lastError)
			*lastError = err;
	}

	bool isSeparator(wchar_t c)
	{
		return c == L'\\' || c == L'/';
	}

	bool isAsciiLetter(wchar_t c)
	{
		return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
	}

	bool readDigits(const std::wstring& s, size_t pos, size_t count, unsigned& value)
	{
		value = 0;
		for (size_t i = pos; i < pos + count; ++i)
		{
			if (s[i] < L'0' || s[i] > L'9')
				return false;
			value = value * 10 + static_cast<unsigned>(s[i] - L'0');
		}
		return true;
	}

	unsigned daysInMonth(unsigned year, unsigned month)
	{
		static constexpr unsigned days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
		if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
			return 29;
		return days[month - 1];
	}

	bool toTicks(const SYSTEMTIME& st, unsigned long long& ticks)
	{
		FILETIME ft{};
		if (!::SystemTimeToFileTime(&st, &ft))
			return false;
		ticks = (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
		return true;
	}

	std::wstring joinPath(const std::wstring& dir, const std::wstring& name)
	{
		std::wstring path = dir;
		if (!path.empty() && !isSeparator(path.back()))
			path += L'\\';
		path += name;
		return path;
	}

	int base64Value(char c)
	{
		if (c >= 'A' && c <= 'Z')
			return c - 'A';
		if (c >= 'a' && c <= 'z')
			return c - 'a' + 26;
		if (c >= '0' && c <= '9')
			return c - '0' + 52;
		if (c == '+')
			return 62;
		if (c == '/')
			return 63;
		return -1;
	}
}

namespace SessionSnapshot
{
	std::wstring makeFileName(const SYSTEMTIME& localTime)
	{
		wchar_t name[64]{};
		swprintf(name, _countof(name), L"%04u%02u%02u-%02u%02u%02u%ls",
			static_cast<unsigned>(localTime.wYear), static_cast<unsigned>(localTime.wMonth), static_cast<unsigned>(localTime.wDay),
			static_cast<unsigned>(localTime.wHour), static_cast<unsigned>(localTime.wMinute), static_cast<unsigned>(localTime.wSecond),
			SNAPSHOT_SUFFIX);
		return name;
	}

	bool parseFileName(const std::wstring& fileName, SYSTEMTIME& outLocalTime)
	{
		if (fileName.length() != SNAPSHOT_NAME_LEN)
			return false;

		if (fileName[8] != L'-' || fileName.compare(15, SNAPSHOT_SUFFIX_LEN, SNAPSHOT_SUFFIX) != 0)
			return false;

		unsigned year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
		if (!readDigits(fileName, 0, 4, year) || !readDigits(fileName, 4, 2, month) || !readDigits(fileName, 6, 2, day) ||
			!readDigits(fileName, 9, 2, hour) || !readDigits(fileName, 11, 2, minute) || !readDigits(fileName, 13, 2, second))
			return false;

		if (year < 1601 || month < 1 || month > 12 || day < 1 || day > daysInMonth(year, month) || hour > 23 || minute > 59 || second > 59)
			return false;

		outLocalTime.wYear = static_cast<WORD>(year);
		outLocalTime.wMonth = static_cast<WORD>(month);
		outLocalTime.wDayOfWeek = 0;
		outLocalTime.wDay = static_cast<WORD>(day);
		outLocalTime.wHour = static_cast<WORD>(hour);
		outLocalTime.wMinute = static_cast<WORD>(minute);
		outLocalTime.wSecond = static_cast<WORD>(second);
		outLocalTime.wMilliseconds = 0;
		return true;
	}

	std::wstring resolveDir(bool useCustomDir, const std::wstring& customDir, const std::wstring& userPath)
	{
		std::wstring dir;
		if (useCustomDir)
		{
			const size_t first = customDir.find_first_not_of(L" \t");
			if (first != std::wstring::npos)
			{
				const size_t last = customDir.find_last_not_of(L" \t");
				dir = customDir.substr(first, last - first + 1);
			}
		}

		if (dir.empty())
			dir = userPath + L"\\backup";

		while (!dir.empty() && isSeparator(dir.back()))
			dir.pop_back();

		if (dir.length() == 2 && isAsciiLetter(dir[0]) && dir[1] == L':')
			dir += L'\\';

		return dir;
	}

	bool ensureDir(const std::wstring& dir, DWORD* lastError)
	{
		std::wstring path = dir;
		while (!path.empty() && isSeparator(path.back()))
			path.pop_back();

		if (path.empty())
		{
			setError(lastError, ERROR_PATH_NOT_FOUND);
			return false;
		}

		DWORD attributes = ::GetFileAttributesW(path.c_str());
		if (attributes != INVALID_FILE_ATTRIBUTES)
		{
			if (attributes & FILE_ATTRIBUTE_DIRECTORY)
				return true;

			setError(lastError, ERROR_ALREADY_EXISTS);
			return false;
		}

		const size_t sepPos = path.find_last_of(L"\\/");
		if (sepPos != std::wstring::npos && sepPos > 0)
		{
			if (!ensureDir(path.substr(0, sepPos), lastError))
				return false;
		}

		if (!::CreateDirectoryW(path.c_str(), nullptr))
		{
			const DWORD err = ::GetLastError();
			attributes = ::GetFileAttributesW(path.c_str());
			if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY))
				return true; // created by someone else in the meantime

			setError(lastError, err);
			return false;
		}
		return true;
	}

	bool writeFileAtomic(const std::wstring& path, const char* data, size_t len, DWORD* lastError)
	{
		const std::wstring tmpPath = path + TMP_EXT;

		HANDLE hFile = ::CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (hFile == INVALID_HANDLE_VALUE)
		{
			setError(lastError, ::GetLastError());
			::DeleteFileW(tmpPath.c_str());
			return false;
		}

		bool isOK = true;
		DWORD err = ERROR_SUCCESS;
		size_t written = 0;
		while (written < len)
		{
			const DWORD toWrite = (len - written > MAX_WRITE_CHUNK) ? MAX_WRITE_CHUNK : static_cast<DWORD>(len - written);
			DWORD nbWritten = 0;
			if (!::WriteFile(hFile, data + written, toWrite, &nbWritten, nullptr))
			{
				err = ::GetLastError();
				isOK = false;
				break;
			}
			if (nbWritten == 0)
			{
				err = ERROR_WRITE_FAULT;
				isOK = false;
				break;
			}
			written += nbWritten;
		}

		if (isOK && !::FlushFileBuffers(hFile))
		{
			err = ::GetLastError();
			isOK = false;
		}

		if (!::CloseHandle(hFile) && isOK)
		{
			err = ::GetLastError();
			isOK = false;
		}

		if (isOK && !::MoveFileExW(tmpPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			err = ::GetLastError();
			isOK = false;
		}

		if (!isOK)
		{
			setError(lastError, err);
			::DeleteFileW(tmpPath.c_str());
		}
		return isOK;
	}

	PruneResult prune(const std::wstring& dir, const SYSTEMTIME& nowLocal, unsigned retentionHours)
	{
		PruneResult result;

		struct Candidate
		{
			std::wstring _name;
			unsigned long long _ticks = 0;
			unsigned long long _lastWriteUtcTicks = 0;
			bool _isTmp = false;
		};
		std::vector<Candidate> candidates;

		WIN32_FIND_DATAW findData{};
		HANDLE hFind = ::FindFirstFileExW(joinPath(dir, L"*").c_str(), FindExInfoBasic, &findData, FindExSearchNameMatch, nullptr, 0);
		if (hFind == INVALID_HANDLE_VALUE)
			return result;

		do
		{
			if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;

			std::wstring name = findData.cFileName;
			bool isTmp = false;
			if (name.length() == SNAPSHOT_NAME_LEN + TMP_EXT_LEN && name.compare(SNAPSHOT_NAME_LEN, TMP_EXT_LEN, TMP_EXT) == 0)
				isTmp = true;

			SYSTEMTIME nameTime{};
			if (!parseFileName(isTmp ? name.substr(0, SNAPSHOT_NAME_LEN) : name, nameTime))
				continue;

			Candidate candidate;
			if (!toTicks(nameTime, candidate._ticks))
				continue;
			candidate._lastWriteUtcTicks = (static_cast<unsigned long long>(findData.ftLastWriteTime.dwHighDateTime) << 32) | findData.ftLastWriteTime.dwLowDateTime;
			candidate._name = std::move(name);
			candidate._isTmp = isTmp;
			candidates.push_back(std::move(candidate));
		}
		while (::FindNextFileW(hFind, &findData));
		::FindClose(hFind);

		// the newest snapshot (by the time in its name) is always kept
		size_t newestIndex = candidates.size();
		for (size_t i = 0; i < candidates.size(); ++i)
		{
			if (!candidates[i]._isTmp && (newestIndex == candidates.size() || candidates[i]._ticks > candidates[newestIndex]._ticks))
				newestIndex = i;
		}

		// a file is old only if both the time in its name and its last write time (UTC) are older than the retention
		unsigned long long nowTicks = 0;
		unsigned long long nowUtcTicks = 0;
		SYSTEMTIME nowUtc{};
		const bool canDelete = retentionHours != 0 && retentionHours <= ULLONG_MAX / FILETIME_TICKS_PER_HOUR && toTicks(nowLocal, nowTicks)
			&& ::TzSpecificLocalTimeToSystemTime(nullptr, &nowLocal, &nowUtc) && toTicks(nowUtc, nowUtcTicks);
		const unsigned long long retentionTicks = canDelete ? retentionHours * FILETIME_TICKS_PER_HOUR : 0;

		for (size_t i = 0; i < candidates.size(); ++i)
		{
			const Candidate& candidate = candidates[i];
			const bool isDue = canDelete && i != newestIndex
				&& candidate._ticks < nowTicks && (nowTicks - candidate._ticks) > retentionTicks
				&& candidate._lastWriteUtcTicks < nowUtcTicks && (nowUtcTicks - candidate._lastWriteUtcTicks) > retentionTicks;
			if (isDue)
			{
				if (::DeleteFileW(joinPath(dir, candidate._name).c_str()))
					++result.deleted;
				else
					++result.failed;
			}
			else if (!candidate._isTmp)
			{
				++result.kept;
			}
		}
		return result;
	}

	std::string base64Encode(const char* data, size_t len)
	{
		std::string out;
		if (!data || len == 0)
			return out;

		out.reserve(((len + 2) / 3) * 4);
		const unsigned char* bytes = reinterpret_cast<const unsigned char*>(data);
		size_t i = 0;
		for (; i + 2 < len; i += 3)
		{
			const unsigned triple = (static_cast<unsigned>(bytes[i]) << 16) | (static_cast<unsigned>(bytes[i + 1]) << 8) | bytes[i + 2];
			out.push_back(base64Alphabet[(triple >> 18) & 0x3F]);
			out.push_back(base64Alphabet[(triple >> 12) & 0x3F]);
			out.push_back(base64Alphabet[(triple >> 6) & 0x3F]);
			out.push_back(base64Alphabet[triple & 0x3F]);
		}

		if (i + 1 == len)
		{
			const unsigned triple = static_cast<unsigned>(bytes[i]) << 16;
			out.push_back(base64Alphabet[(triple >> 18) & 0x3F]);
			out.push_back(base64Alphabet[(triple >> 12) & 0x3F]);
			out += "==";
		}
		else if (i + 2 == len)
		{
			const unsigned triple = (static_cast<unsigned>(bytes[i]) << 16) | (static_cast<unsigned>(bytes[i + 1]) << 8);
			out.push_back(base64Alphabet[(triple >> 18) & 0x3F]);
			out.push_back(base64Alphabet[(triple >> 12) & 0x3F]);
			out.push_back(base64Alphabet[(triple >> 6) & 0x3F]);
			out.push_back('=');
		}
		return out;
	}

	bool base64Decode(const std::string& text, std::string& out)
	{
		std::string compact;
		compact.reserve(text.size());
		for (char c : text)
		{
			if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
				continue;
			if (c != '=' && base64Value(c) < 0)
				return false;
			compact.push_back(c);
		}

		if (compact.size() % 4 != 0)
			return false;

		out.clear();
		out.reserve((compact.size() / 4) * 3);
		for (size_t i = 0; i < compact.size(); i += 4)
		{
			const bool isLastQuad = (i + 4 == compact.size());
			size_t nbPad = 0;
			if (compact[i + 3] == '=')
			{
				nbPad = (compact[i + 2] == '=') ? 2 : 1;
			}
			if (nbPad > 0 && !isLastQuad)
				return false;

			unsigned quad = 0;
			for (size_t j = 0; j < 4 - nbPad; ++j)
			{
				const int value = base64Value(compact[i + j]);
				if (value < 0) // '=' at a place where only data can be
					return false;
				quad = (quad << 6) | static_cast<unsigned>(value);
			}
			quad <<= 6 * nbPad;

			out.push_back(static_cast<char>((quad >> 16) & 0xFF));
			if (nbPad < 2)
				out.push_back(static_cast<char>((quad >> 8) & 0xFF));
			if (nbPad < 1)
				out.push_back(static_cast<char>(quad & 0xFF));
		}
		return true;
	}

	void Scheduler::markDirty()
	{
		_isPending = true;
	}

	bool Scheduler::shouldWrite() const
	{
		return _isPending;
	}

	void Scheduler::onWriteResult(WriteOutcome outcome)
	{
		switch (outcome)
		{
			case WriteOutcome::Ok:
			{
				_isPending = false;
				_level = Level::Ok;
				_isNoticeOwed = false;
				break;
			}

			case WriteOutcome::OkFallback:
			{
				_isPending = false;
				if (_level < Level::OkFallback)
					_isNoticeOwed = true;
				_level = Level::OkFallback;
				break;
			}

			case WriteOutcome::Failed:
			{
				// pending is left unchanged: the next tick tries again
				if (_level < Level::Failed)
					_isNoticeOwed = true;
				_level = Level::Failed;
				break;
			}
		}
	}

	bool Scheduler::consumeFailureNotice()
	{
		const bool isNoticeOwed = _isNoticeOwed;
		_isNoticeOwed = false;
		return isNoticeOwed;
	}

	bool Scheduler::shouldPrune(unsigned long long nowTickMs) const
	{
		return !_hasPruned || nowTickMs - _lastPruneTick >= PRUNE_MIN_INTERVAL_MS;
	}

	void Scheduler::onPruned(unsigned long long nowTickMs)
	{
		_hasPruned = true;
		_lastPruneTick = nowTickMs;
	}
}
