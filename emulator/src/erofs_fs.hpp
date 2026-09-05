#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <libriscv/machine.hpp>

extern "C" {
#include <erofs/internal.h>
#include <erofs/dir.h>
}

class ErofsFilesystem {
public:
	struct Node {
		erofs_nid_t nid = 0;
		uint16_t mode = 0;
		uint64_t size = 0;
		uint64_t ino = 0;
		uint32_t uid = 0, gid = 0, nlink = 0;
		uint64_t mtime = 0;
		uint32_t mtime_nsec = 0;
	};
	struct DirEntry {
		std::string name;
		erofs_nid_t nid;
		uint8_t type;
	};

	explicit ErofsFilesystem(const std::string& image);
	~ErofsFilesystem();
	ErofsFilesystem(const ErofsFilesystem&) = delete;
	ErofsFilesystem& operator=(const ErofsFilesystem&) = delete;

	Node lookup(std::string_view path) const;
	std::vector<uint8_t> read_file(std::string_view path) const;
	std::vector<DirEntry> read_dir(std::string_view path) const;
	std::string read_link(std::string_view path) const;
	int open_sealed(std::string_view path) const;

private:
	static std::string normalize(std::string_view path);
	erofs_inode inode(std::string_view path) const;
	mutable erofs_sb_info m_sbi {};
	bool m_open = false;
};

template <int W> struct ErofsRuntime {
	std::shared_ptr<ErofsFilesystem> fs;
	riscv::address_type<W> symbol_function = 0;
	struct OpenDirectory {
		std::string path;
		std::vector<ErofsFilesystem::DirEntry> entries;
		size_t index = 0;
	};
	std::unordered_map<int, OpenDirectory> directories;
	std::unordered_map<int, std::string> paths;
};

template <int W>
void setup_erofs_syscalls(riscv::Machine<W>& machine, ErofsRuntime<W>& runtime);
