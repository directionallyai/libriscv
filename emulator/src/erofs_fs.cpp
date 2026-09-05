#include "erofs_fs.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
struct GlobalLiberofs {
	GlobalLiberofs() { if (liberofs_global_init()) throw std::runtime_error("liberofs initialization failed"); }
	~GlobalLiberofs() { liberofs_global_exit(); }
};
GlobalLiberofs global_liberofs;
}

ErofsFilesystem::ErofsFilesystem(const std::string& image)
{
	int err = erofs_dev_open(&m_sbi, image.c_str(), O_RDONLY);
	if (err) throw std::runtime_error("Could not open EROFS image " + image + ": " + std::strerror(-err));
	m_open = true;
	err = erofs_read_superblock(&m_sbi);
	if (err) {
		erofs_dev_close(&m_sbi);
		m_open = false;
		throw std::runtime_error("Could not read EROFS superblock: " + std::string(std::strerror(-err)));
	}
}

ErofsFilesystem::~ErofsFilesystem()
{
	if (m_open) {
		erofs_put_super(&m_sbi);
		erofs_dev_close(&m_sbi);
	}
}

std::string ErofsFilesystem::normalize(std::string_view path)
{
	std::string out(path);
	if (out.empty()) out = "/";
	if (out.front() != '/') out.insert(out.begin(), '/');
	return out;
}

erofs_inode ErofsFilesystem::inode(std::string_view path) const
{
	erofs_inode ino {};
	ino.sbi = &m_sbi;
	const auto p = normalize(path);
	const int err = erofs_ilookup(p.c_str(), &ino);
	if (err) throw std::system_error(-err, std::generic_category(), p);
	return ino;
}

ErofsFilesystem::Node ErofsFilesystem::lookup(std::string_view path) const
{
	auto ino = inode(path);
	return {ino.nid, ino.i_mode, ino.i_size, ino.i_ino[0], ino.i_uid, ino.i_gid,
		ino.i_nlink, ino.i_mtime, ino.i_mtime_nsec};
}

std::vector<uint8_t> ErofsFilesystem::read_file(std::string_view path) const
{
	auto ino = inode(path);
	if (S_ISLNK(ino.i_mode)) {
		const auto current = normalize(path);
		auto target = read_link(path);
		if (target.empty() || target.front() != '/') {
			const auto slash = current.rfind('/');
			target = current.substr(0, slash + 1) + target;
		}
		return read_file(target);
	}
	if (!S_ISREG(ino.i_mode))
		throw std::system_error(EISDIR, std::generic_category(), normalize(path));
	std::vector<uint8_t> data(ino.i_size);
	erofs_vfile vf {};
	int err = erofs_iopen(&vf, &ino);
	if (err) throw std::system_error(-err, std::generic_category(), normalize(path));
	const ssize_t len = erofs_pread(&vf, data.data(), data.size(), 0);
	if (len < 0) throw std::system_error(-len, std::generic_category(), normalize(path));
	return data;
}

std::vector<ErofsFilesystem::DirEntry> ErofsFilesystem::read_dir(std::string_view path) const
{
	auto ino = inode(path);
	if (!S_ISDIR(ino.i_mode)) throw std::system_error(ENOTDIR, std::generic_category(), normalize(path));
	struct Context {
		erofs_dir_context ctx;
		std::vector<DirEntry> entries;
	};
	Context context {};
	context.ctx.dir = &ino;
	context.ctx.cb = [] (erofs_dir_context* raw) -> int {
		auto* self = reinterpret_cast<Context*>(raw);
		self->entries.push_back({std::string(raw->dname, raw->de_namelen), raw->de_nid, raw->de_ftype});
		return 0;
	};
	const int err = erofs_iterate_dir(&context.ctx, false);
	if (err) throw std::system_error(-err, std::generic_category(), normalize(path));
	return std::move(context.entries);
}

std::string ErofsFilesystem::read_link(std::string_view path) const
{
	auto ino = inode(path);
	if (!S_ISLNK(ino.i_mode)) throw std::system_error(EINVAL, std::generic_category(), normalize(path));
	std::vector<uint8_t> data(ino.i_size);
	erofs_vfile vf {};
	int err = erofs_iopen(&vf, &ino);
	if (err) throw std::system_error(-err, std::generic_category(), normalize(path));
	const ssize_t len = erofs_pread(&vf, data.data(), data.size(), 0);
	if (len < 0) throw std::system_error(-len, std::generic_category(), normalize(path));
	return std::string(reinterpret_cast<const char*>(data.data()), data.size());
}

int ErofsFilesystem::open_sealed(std::string_view path) const
{
	auto data = read_file(path);
	int fd = memfd_create("rvlinux-erofs", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (fd < 0) return -errno;
	if (!data.empty() && ::write(fd, data.data(), data.size()) != ssize_t(data.size())) {
		const int err = errno; ::close(fd); return -err;
	}
	if (lseek(fd, 0, SEEK_SET) < 0 || fcntl(fd, F_ADD_SEALS,
		F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE) < 0) {
		const int err = errno; ::close(fd); return -err;
	}
	return fd;
}

namespace {
static std::string join_path(const std::string& base, const std::string& path) {
	if (!path.empty() && path.front() == '/') return path;
	return base.empty() || base == "/" ? "/" + path : base + "/" + path;
}
struct GuestStat {
	uint64_t dev, ino; uint32_t mode, nlink, uid, gid; uint64_t rdev, pad1;
	int64_t size; int32_t blksize, pad2; int64_t blocks, atime; uint64_t atime_nsec;
	int64_t mtime; uint64_t mtime_nsec; int64_t ctime; uint64_t ctime_nsec; uint32_t unused4, unused5;
};
static GuestStat guest_stat(const ErofsFilesystem::Node& n) {
	GuestStat st {}; st.dev=1; st.ino=n.ino?n.ino:n.nid; st.mode=n.mode; st.nlink=n.nlink;
	st.uid=n.uid; st.gid=n.gid; st.size=n.size; st.blksize=4096; st.blocks=(n.size+511)/512;
	st.atime=st.mtime=st.ctime=n.mtime; st.atime_nsec=st.mtime_nsec=st.ctime_nsec=n.mtime_nsec; return st;
}
static int error_code(const std::exception& e) {
	if (auto* se=dynamic_cast<const std::system_error*>(&e)) return -se->code().value(); return -EIO;
}
}

template <int W> void setup_erofs_syscalls(riscv::Machine<W>& machine, ErofsRuntime<W>& runtime) {
	using Machine=riscv::Machine<W>; machine.fds().permit_filesystem=false; machine.fds().permit_sockets=false;
	machine.fds().cwd="/"; machine.set_userdata(&runtime);
	Machine::install_syscall_handler(56, [](Machine& m) {
		auto& rt=*m.template get_userdata<ErofsRuntime<W>>(); int dirfd=m.template sysarg<int>(0);
		std::string path=m.memory.memstring(m.sysarg(1)); int flags=m.template sysarg<int>(2);
		if (flags&(O_WRONLY|O_RDWR|O_CREAT|O_TRUNC|O_APPEND)) { m.set_result(-EROFS); return; }
		if (path.empty()) { m.set_result(-ENOENT); return; }
		if (path.front()!='/') { if (dirfd==AT_FDCWD) path=join_path(m.fds().cwd,path);
			else if (auto i=rt.paths.find(dirfd);i!=rt.paths.end()) path=join_path(i->second,path);
			else { m.set_result(-EBADF); return; } }
		try { auto node=rt.fs->lookup(path); int realfd;
			if (S_ISDIR(node.mode)) realfd=::open("/dev/null",O_RDONLY|O_CLOEXEC);
			else if (S_ISREG(node.mode) || S_ISLNK(node.mode)) realfd=rt.fs->open_sealed(path); else { m.set_result(-ENXIO); return; }
			if (realfd<0) { m.set_result(realfd==-1?-errno:realfd); return; }
			int vfd=m.fds().assign_file(realfd); rt.paths.emplace(vfd,path);
			if (S_ISDIR(node.mode)) rt.directories.emplace(vfd,typename ErofsRuntime<W>::OpenDirectory{path,rt.fs->read_dir(path),0});
			m.set_result(vfd); } catch(const std::exception& e) { m.set_result(error_code(e)); }
	});
	Machine::install_syscall_handler(57, [](Machine& m) { auto& rt=*m.template get_userdata<ErofsRuntime<W>>();
		int vfd=m.template sysarg<int>(0); if(vfd>=0&&vfd<=2){m.set_result(0);return;} int fd=m.fds().erase(vfd);
		if(fd<0){m.set_result(-EBADF);return;} ::close(fd);rt.paths.erase(vfd);rt.directories.erase(vfd);m.set_result(0); });
	Machine::install_syscall_handler(61, [](Machine& m) { auto& rt=*m.template get_userdata<ErofsRuntime<W>>();
		int fd=m.template sysarg<int>(0);auto it=rt.directories.find(fd);if(it==rt.directories.end()){m.set_result(-ENOTDIR);return;}
		auto addr=m.sysarg(1);size_t count=m.sysarg(2);std::vector<uint8_t> out;
		while(it->second.index<it->second.entries.size()){auto& e=it->second.entries[it->second.index];size_t len=(20+e.name.size()+7)&~size_t(7);
			if(out.size()+len>count)break;size_t at=out.size();out.resize(at+len);uint64_t ino=e.nid,off=it->second.index+1;uint16_t rec=len;
			std::memcpy(out.data()+at,&ino,8);std::memcpy(out.data()+at+8,&off,8);std::memcpy(out.data()+at+16,&rec,2);out[at+18]=e.type;
			std::memcpy(out.data()+at+19,e.name.c_str(),e.name.size()+1);++it->second.index;}
		if(!out.empty())m.copy_to_guest(addr,out.data(),out.size());m.set_result(out.size()); });
	Machine::install_syscall_handler(79, [](Machine& m) { auto& rt=*m.template get_userdata<ErofsRuntime<W>>();int d=m.template sysarg<int>(0);
		std::string p=m.memory.memstring(m.sysarg(1));if(p.empty()){auto i=rt.paths.find(d);if(i==rt.paths.end()){
			if (int vfd=m.template sysarg<int>(0); vfd>=0 && vfd<=2) { struct stat hs {}; if (::fstat(vfd, &hs)<0) {m.set_result(-errno);return;} GuestStat st {}; st.dev=hs.st_dev;st.ino=hs.st_ino;st.mode=hs.st_mode;st.nlink=hs.st_nlink;st.uid=hs.st_uid;st.gid=hs.st_gid;st.rdev=hs.st_rdev;st.size=hs.st_size;st.blksize=hs.st_blksize;st.blocks=hs.st_blocks;m.copy_to_guest(m.sysarg(2),&st,sizeof(st));m.set_result(0);return;}
			m.set_result(-EBADF);return;}p=i->second;}
		else if(p.front()!='/'){if(d==AT_FDCWD)p=join_path(m.fds().cwd,p);else if(auto i=rt.paths.find(d);i!=rt.paths.end())p=join_path(i->second,p);else{m.set_result(-EBADF);return;}}
		try{auto st=guest_stat(rt.fs->lookup(p));m.copy_to_guest(m.sysarg(2),&st,sizeof(st));m.set_result(0);}catch(const std::exception&e){m.set_result(error_code(e));} });
	Machine::install_syscall_handler(80, [](Machine& m) {auto& rt=*m.template get_userdata<ErofsRuntime<W>>();auto i=rt.paths.find(m.template sysarg<int>(0));
		if(i==rt.paths.end()){m.set_result(-EBADF);return;}try{auto st=guest_stat(rt.fs->lookup(i->second));m.copy_to_guest(m.sysarg(1),&st,sizeof(st));m.set_result(0);}catch(const std::exception&e){m.set_result(error_code(e));}});
	Machine::install_syscall_handler(78, [](Machine& m){auto&rt=*m.template get_userdata<ErofsRuntime<W>>();int d=m.template sysarg<int>(0);std::string p=m.memory.memstring(m.sysarg(1));
		if(p.empty()){m.set_result(-ENOENT);return;}if(p.front()!='/'){if(d==AT_FDCWD)p=join_path(m.fds().cwd,p);else if(auto i=rt.paths.find(d);i!=rt.paths.end())p=join_path(i->second,p);else{m.set_result(-EBADF);return;}}
		try{auto s=rt.fs->read_link(p);s.resize(std::min<size_t>(s.size(),m.sysarg(3)));m.copy_to_guest(m.sysarg(2),s.data(),s.size());m.set_result(s.size());}catch(const std::exception&e){m.set_result(error_code(e));}});
	Machine::install_syscall_handler(29, [](Machine& m) { int v=m.template sysarg<int>(0); uint64_t req=m.template sysarg<uint64_t>(1); int fd=m.fds().translate(v); if(fd<0){m.set_result(-EBADF);return;} if(req==0x5451 || req==0x5450){int f=fcntl(fd,F_GETFD);if(f<0){m.set_result(-errno);return;} int r=fcntl(fd,F_SETFD,req==0x5451?f|FD_CLOEXEC:f&~FD_CLOEXEC);m.set_result(r<0?-errno:r);return;} m.set_result(-ENOSYS);});
	for(int n:{34,35,36,37,38,53,54,88,276}) Machine::install_syscall_handler(n,[](Machine&m){m.set_result(-EROFS);});
}
#ifdef RISCV_32I
template void setup_erofs_syscalls<4>(riscv::Machine<4>&,ErofsRuntime<4>&);
#endif
#ifdef RISCV_64I
template void setup_erofs_syscalls<8>(riscv::Machine<8>&,ErofsRuntime<8>&);
#endif
