// SPDX-License-Identifier: BSD-3-Clause

#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <unwind.h>
#include <fcntl.h>
#include <linux/dm-ioctl.h>
#include <linux/fs.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <vector>

#include <android/log.h>

#include <aidl/android/hardware/security/keymint/ErrorCode.h>
#include <aidl/android/hardware/security/keymint/IKeyMintDevice.h>
#include <openssl/sha.h>

using namespace aidl::android::hardware::security::keymint;
using Bytes = std::vector<uint8_t>;

static const char kCreateKeyMintDevice[] =
	"_ZN4aidl7android8hardware8security7keymint19CreateKeyMintDeviceENS3_13SecurityLevelE";

/* vold KeyStorage.cpp */
static const char kHashPrefixSecdiscardable[] = "Android secdiscardable SHA512";
static const size_t kGcmNonceBytes = 12;
static const size_t kGcmMacBytes = 16;

static bool read_file(const std::string &path, Bytes *out)
{
	int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
	struct stat st;

	if (fd < 0 || fstat(fd, &st)) {
		fprintf(stderr, "%s: %s\n", path.c_str(), strerror(errno));
		if (fd >= 0)
			close(fd);
		return false;
	}

	out->resize(st.st_size);
	ssize_t n = read(fd, out->data(), out->size());
	close(fd);
	if (n != (ssize_t)out->size()) {
		fprintf(stderr, "%s: short read\n", path.c_str());
		return false;
	}
	return true;
}

static Bytes hash_with_prefix(const char *prefix, const Bytes &data)
{
	std::string p = prefix;
	Bytes out(SHA512_DIGEST_LENGTH);
	SHA512_CTX c;

	p.resize(SHA512_CBLOCK);
	SHA512_Init(&c);
	SHA512_Update(&c, p.data(), p.size());
	SHA512_Update(&c, data.data(), data.size());
	SHA512_Final(out.data(), &c);
	return out;
}

static int km_error(const ndk::ScopedAStatus &st)
{
	if (st.isOk())
		return 0;
	if (st.getExceptionCode() == EX_SERVICE_SPECIFIC)
		return st.getServiceSpecificError();
	return -1000 - st.getExceptionCode();
}

static void report(const char *what, const ndk::ScopedAStatus &st)
{
	int err = km_error(st);

	fprintf(stderr, "%s failed: %d (%s)\n", what, err,
		err > -1000 ? toString(static_cast<ErrorCode>(err)).c_str() : st.getDescription().c_str());
}

/*
 * libqtikeymint's begin() reads the auth token without checking whether it
 * is engaged, so it gets an empty token; a MAC shorter than 32 bytes counts
 * as no token there. update() and finish() check, and forward an engaged
 * token to the TA, so they get std::nullopt.
 */
static const std::optional<HardwareAuthToken> kBeginNoAuthToken = HardwareAuthToken{};

static KeyParameter param(Tag tag, KeyParameterValue value)
{
	return KeyParameter{ .tag = tag, .value = std::move(value) };
}

static bool decrypt_key_storage(IKeyMintDevice &km, const Bytes &blob, const Bytes &app_id,
				const Bytes &encrypted, Bytes *plain)
{
	if (encrypted.size() < kGcmNonceBytes + kGcmMacBytes) {
		fprintf(stderr, "encrypted_key too short\n");
		return false;
	}

	Bytes nonce(encrypted.begin(), encrypted.begin() + kGcmNonceBytes);
	Bytes body(encrypted.begin() + kGcmNonceBytes, encrypted.end());
	std::vector<KeyParameter> params = {
		param(Tag::BLOCK_MODE, KeyParameterValue::make<KeyParameterValue::blockMode>(BlockMode::GCM)),
		param(Tag::PADDING, KeyParameterValue::make<KeyParameterValue::paddingMode>(PaddingMode::NONE)),
		param(Tag::MAC_LENGTH, KeyParameterValue::make<KeyParameterValue::integer>(kGcmMacBytes * 8)),
		param(Tag::APPLICATION_ID, KeyParameterValue::make<KeyParameterValue::blob>(app_id)),
		param(Tag::NONCE, KeyParameterValue::make<KeyParameterValue::blob>(nonce)),
		param(Tag::PURPOSE, KeyParameterValue::make<KeyParameterValue::keyPurpose>(KeyPurpose::DECRYPT)),
	};
	BeginResult begin;

	auto st = km.begin(KeyPurpose::DECRYPT, blob, params, kBeginNoAuthToken, &begin);
	if (!st.isOk()) {
		report("begin", st);
		if (km_error(st) == (int)ErrorCode::KEY_REQUIRES_UPGRADE)
			fprintf(stderr, "refusing to upgrade the vold key outside Android\n");
		return false;
	}

	Bytes out;
	st = begin.operation->update(body, std::nullopt, std::nullopt, &out);
	if (!st.isOk()) {
		report("update", st);
		return false;
	}

	Bytes tail;
	st = begin.operation->finish(std::nullopt, std::nullopt, std::nullopt, std::nullopt,
				     std::nullopt, &tail);
	if (!st.isOk()) {
		report("finish", st);
		return false;
	}

	out.insert(out.end(), tail.begin(), tail.end());
	*plain = std::move(out);
	return true;
}

static bool ephemeral_key(IKeyMintDevice &km, const Bytes &storage_key, Bytes *eph)
{
	auto st = km.convertStorageKeyToEphemeral(storage_key, eph);
	if (st.isOk())
		return true;
	if (km_error(st) != (int)ErrorCode::KEY_REQUIRES_UPGRADE) {
		report("convertStorageKeyToEphemeral", st);
		return false;
	}

	/* As keystore2 does; vold keeps the original storage key blob. */
	Bytes upgraded;
	st = km.upgradeKey(storage_key, {}, &upgraded);
	if (!st.isOk()) {
		report("upgradeKey (storage key, in memory)", st);
		return false;
	}
	st = km.convertStorageKeyToEphemeral(upgraded, eph);
	if (!st.isOk()) {
		report("convertStorageKeyToEphemeral", st);
		return false;
	}
	return true;
}

static void dm_init(struct dm_ioctl *io, size_t size, const char *name)
{
	memset(io, 0, size);
	io->version[0] = DM_VERSION_MAJOR;
	io->version[1] = DM_VERSION_MINOR;
	io->version[2] = DM_VERSION_PATCHLEVEL;
	io->data_size = size;
	io->data_start = sizeof(*io);
	strncpy(io->name, name, sizeof(io->name) - 1);
}

static bool dm_create(const char *name, const char *blkdev, uint64_t sectors,
		      const std::string &hex_key, dev_t *dev)
{
	std::string params = "aes-xts-plain64 " + hex_key + " 0 " + blkdev +
			     " 0 4 allow_discards sector_size:4096 iv_large_sectors wrappedkey_v0";
	size_t size = sizeof(struct dm_ioctl) + sizeof(struct dm_target_spec) + params.size() + 8;
	std::vector<uint8_t> buf(size);
	auto *io = reinterpret_cast<struct dm_ioctl *>(buf.data());
	auto *spec = reinterpret_cast<struct dm_target_spec *>(io + 1);
	int fd = open("/dev/device-mapper", O_RDWR | O_CLOEXEC);
	bool ok = false;

	if (fd < 0) {
		perror("/dev/device-mapper");
		return false;
	}

	dm_init(io, sizeof(*io), name);
	if (ioctl(fd, DM_DEV_CREATE, io)) {
		perror("DM_DEV_CREATE");
		goto out;
	}

	dm_init(io, size, name);
	io->target_count = 1;
	spec->sector_start = 0;
	spec->length = sectors;
	strncpy(spec->target_type, "default-key", sizeof(spec->target_type) - 1);
	memcpy(spec + 1, params.c_str(), params.size() + 1);
	if (ioctl(fd, DM_TABLE_LOAD, io)) {
		perror("DM_TABLE_LOAD");
		goto remove;
	}

	dm_init(io, sizeof(*io), name);
	if (ioctl(fd, DM_DEV_SUSPEND, io)) {
		perror("DM_DEV_SUSPEND (resume)");
		goto remove;
	}

	*dev = io->dev;
	ok = true;
	goto out;

remove:
	dm_init(io, sizeof(*io), name);
	ioctl(fd, DM_DEV_REMOVE, io);
out:
	close(fd);
	return ok;
}

static _Unwind_Reason_Code print_frame(struct _Unwind_Context *ctx, void *)
{
	uintptr_t pc = _Unwind_GetIP(ctx);
	Dl_info info;

	if (pc && dladdr(reinterpret_cast<void *>(pc), &info) && info.dli_fname)
		fprintf(stderr, "  %s+0x%zx\n", info.dli_fname,
			(size_t)(pc - reinterpret_cast<uintptr_t>(info.dli_fbase)));
	else
		fprintf(stderr, "  0x%zx\n", (size_t)pc);
	return _URC_NO_REASON;
}

static void crash_handler(int sig, siginfo_t *si, void *)
{
	fprintf(stderr, "signal %d at %p\n", sig, si->si_addr);
	_Unwind_Backtrace(print_frame, nullptr);
	_exit(128 + sig);
}

static int usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s <metadata key dir> <userdata block device> <dm name>\n"
		"       %s --info <metadata key dir>\n"
		"  e.g. %s /metadata/vold/metadata_encryption/key /dev/block/by-name/userdata userdata-ro\n",
		argv0, argv0, argv0);
	return 2;
}

int main(int argc, char **argv)
{
	bool info_only = argc == 3 && !strcmp(argv[1], "--info");

	if (argc != 4 && !info_only)
		return usage(argv[0]);

	setvbuf(stdout, nullptr, _IONBF, 0);
	__android_log_set_logger(__android_log_stderr_logger);
	__android_log_set_minimum_priority(ANDROID_LOG_VERBOSE);

	struct sigaction sa = {};
	sa.sa_sigaction = crash_handler;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &sa, nullptr);
	sigaction(SIGBUS, &sa, nullptr);
	sigaction(SIGABRT, &sa, nullptr);

	std::string dir = argv[info_only ? 2 : 1];
	const char *blkdev = info_only ? nullptr : argv[2];
	const char *name = info_only ? nullptr : argv[3];
	Bytes version, secdiscardable, blob, encrypted, storage_key, eph;

	if (!read_file(dir + "/version", &version) || !read_file(dir + "/keymaster_key_blob", &blob) ||
	    !read_file(dir + "/encrypted_key", &encrypted))
		return 1;
	if (std::string(version.begin(), version.end()) != "1") {
		fprintf(stderr, "unexpected key version\n");
		return 1;
	}
	if (access((dir + "/keymaster_key_blob_upgraded").c_str(), F_OK) == 0)
		fprintf(stderr, "note: keymaster_key_blob_upgraded exists, using keymaster_key_blob\n");

	Bytes app_id;
	if (access((dir + "/secdiscardable").c_str(), F_OK) == 0) {
		if (!read_file(dir + "/secdiscardable", &secdiscardable))
			return 1;
		app_id = hash_with_prefix(kHashPrefixSecdiscardable, secdiscardable);
	}

	void *lib = dlopen("libqtikeymint.so", RTLD_NOW);
	if (!lib) {
		fprintf(stderr, "dlopen: %s\n", dlerror());
		return 1;
	}
	/* The QTI build returns a raw pointer to a new AndroidKeyMintDevice. */
	auto create = reinterpret_cast<IKeyMintDevice *(*)(SecurityLevel)>(
		dlsym(lib, kCreateKeyMintDevice));
	if (!create) {
		fprintf(stderr, "dlsym: %s\n", dlerror());
		return 1;
	}

	IKeyMintDevice *km = create(SecurityLevel::TRUSTED_ENVIRONMENT);
	if (!km) {
		fprintf(stderr, "CreateKeyMintDevice returned null\n");
		return 1;
	}

	KeyMintHardwareInfo info;
	auto st = km->getHardwareInfo(&info);
	if (!st.isOk()) {
		report("getHardwareInfo", st);
		return 1;
	}
	printf("KeyMint: %s by %s, version %d, security level %d\n", info.keyMintName.c_str(),
	       info.keyMintAuthorName.c_str(), info.versionNumber, (int)info.securityLevel);

	std::vector<KeyCharacteristics> chars;
	st = km->getKeyCharacteristics(blob, app_id, {}, &chars);
	if (!st.isOk()) {
		report("getKeyCharacteristics", st);
	} else {
		for (const auto &c : chars)
			for (const auto &p : c.authorizations)
				printf("  [%d] %s\n", (int)c.securityLevel, p.toString().c_str());
	}
	if (info_only)
		return 0;

	printf("begin: app_id %zu bytes, blob %zu bytes\n", app_id.size(), blob.size());
	if (!decrypt_key_storage(*km, blob, app_id, encrypted, &storage_key))
		return 1;
	printf("metadata storage key: %zu bytes\n", storage_key.size());

	if (!ephemeral_key(*km, storage_key, &eph))
		return 1;
	printf("ephemeral key: %zu bytes\n", eph.size());

	int fd = open(blkdev, O_RDONLY | O_CLOEXEC);
	uint64_t bytes = 0;
	if (fd < 0 || ioctl(fd, BLKGETSIZE64, &bytes)) {
		perror(blkdev);
		return 1;
	}
	close(fd);

	static const char hex[] = "0123456789abcdef";
	std::string hex_key;
	for (uint8_t b : eph) {
		hex_key += hex[b >> 4];
		hex_key += hex[b & 0xf];
	}

	dev_t dev;
	if (!dm_create(name, blkdev, (bytes / 512) & ~7ULL, hex_key, &dev))
		return 1;

	std::string node = "/dev/block/dm-" + std::to_string(minor(dev));
	if (access(node.c_str(), F_OK) && mknod(node.c_str(), S_IFBLK | 0600, dev))
		perror(node.c_str());

	printf("created %s as %s; mount it read-only, e.g.\n"
	       "  mkdir -p /mnt/data && mount -t f2fs -o ro,norecovery %s /mnt/data\n",
	       name, node.c_str(), node.c_str());
	return 0;
}
