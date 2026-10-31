// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Checks and manages account security capabilities.

#include "privileges.hpp"

#include "arch/common/Process/manager.hpp"
#include "arch/x86_64/cpu/cpuid.h"
#include "arch/x86_64/mm/spinlock.hpp"
#include "arch/x86_64/rng/virtio_rng.hpp"
#include "arch/x86_64/scheduler/scheduler.hpp"
#include "boot/boot_protocol.h"
#include "filesystem/vfs.h"
#include "password_hash.hpp"

#include <stddef.h>

namespace welovec::security {
namespace {

using welovec::x86_64::mm::IrqSpinLock;
constexpr uint32_t kMaximumUsers = WELOVEC_USER_MAX_ACCOUNTS;
constexpr uint32_t kPasswordIterations = 600000;
constexpr uint32_t kMinimumAcceptedIterations = 120000;
constexpr uint32_t kMaximumAcceptedIterations = 2000000;
constexpr uint32_t kSaltSize = 16;
constexpr uint32_t kHashSize = 32;
constexpr uint64_t kLoginLockTicks = 500;
constexpr uint32_t kLoginFailuresBeforeLock = 5;
constexpr uint64_t kDatabaseMagic = UINT64_C(0x574C435553455231);
constexpr uint32_t kDatabaseVersion = 2;
constexpr char kDatabasePath0[] = "/userspace/system/security0.db";
constexpr char kDatabasePath1[] = "/userspace/system/security1.db";

struct Account {
    welovec_security_user public_info;
    uint32_t iterations;
    uint8_t salt[kSaltSize];
    uint8_t password_hash[kHashSize];
    bool has_password;
    uint32_t failed_logins;
    uint64_t lockout_until;
};

struct __attribute__((packed)) DiskHeader {
    uint64_t magic;
    uint64_t sequence;
    uint32_t version;
    uint32_t account_count;
    uint32_t checksum;
};

struct __attribute__((packed)) DiskAccount {
    uint32_t uid;
    uint32_t gid;
    uint32_t role;
    uint32_t iterations;
    char name[WELOVEC_SECURITY_NAME_MAX];
    uint8_t salt[kSaltSize];
    uint8_t password_hash[kHashSize];
};

struct __attribute__((packed)) DiskImage {
    DiskHeader header;
    DiskAccount accounts[kMaximumUsers - 2];
};

struct SecurityManager {
    constexpr SecurityManager()
        : lock{}, accounts{}, pending_accounts{}, count(0), pending_count(0),
          next_uid(1001), initialized(false), setup_required(false),
          entropy_valid(false), mutation_pending(false), login_pending(false),
          sequence(0), active_slot(-1), entropy_counter(0), failed_logins(0),
          unknown_lockout_until(0), entropy{} {}

    IrqSpinLock lock;
    Account accounts[kMaximumUsers];
    Account pending_accounts[kMaximumUsers];
    uint32_t count;
    uint32_t pending_count;
    uint32_t next_uid;
    bool initialized;
    bool setup_required;
    bool entropy_valid;
    bool mutation_pending;
    bool login_pending;
    uint64_t sequence;
    int32_t active_slot;
    uint64_t entropy_counter;
    uint32_t failed_logins;
    uint64_t unknown_lockout_until;
    uint8_t entropy[32];
};

constinit SecurityManager gSecurity{};
DiskImage gDiskImage{};
DiskImage gDiskCandidates[2]{};

void zero_bytes(void *memory, uint64_t size) {
    auto *bytes = static_cast<uint8_t *>(memory);
    while (size--) *bytes++ = 0;
}

void copy_bytes(void *destination, const void *source, uint64_t size) {
    auto *out = static_cast<uint8_t *>(destination);
    const auto *in = static_cast<const uint8_t *>(source);
    for (uint64_t i = 0; i < size; ++i) out[i] = in[i];
}

bool fill_hardware_entropy(uint8_t *output, uint32_t size) {
    if (!output || !size) return false;
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    const uint32_t max_leaf = eax;
    if (max_leaf < 1)
        return welovec::x86_64::rng::fill_from_virtio(output, size);

    cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    const bool has_rdrand = (ecx & (1u << 30)) != 0;
    bool has_rdseed = false;
    if (max_leaf >= 7) {
        cpuid(7, 0, &eax, &ebx, &ecx, &edx);
        has_rdseed = (ebx & (1u << 18)) != 0;
    }
    if (!has_rdseed && !has_rdrand)
        return welovec::x86_64::rng::fill_from_virtio(output, size);

    uint32_t offset = 0;
    uint8_t any = 0;
    while (offset < size) {
        uint64_t sample = 0;
        bool generated = false;
        for (uint32_t attempt = 0; has_rdseed && attempt < 128; ++attempt) {
            uint8_t success = 0;
            __asm__ volatile("rdseed %0; setc %1"
                             : "=&r"(sample), "=qm"(success) : : "cc");
            if (success) { generated = true; break; }
        }
        for (uint32_t attempt = 0; !generated && has_rdrand && attempt < 128;
             ++attempt) {
            uint8_t success = 0;
            __asm__ volatile("rdrand %0; setc %1"
                             : "=&r"(sample), "=qm"(success) : : "cc");
            if (success) generated = true;
        }
        if (!generated) {
            zero_bytes(output, size);
            return welovec::x86_64::rng::fill_from_virtio(output, size);
        }
        const uint32_t remaining = size - offset;
        const uint32_t copy_size = remaining < sizeof(sample)
            ? remaining : sizeof(sample);
        const auto *bytes = reinterpret_cast<const uint8_t *>(&sample);
        for (uint32_t i = 0; i < copy_size; ++i) {
            output[offset + i] = bytes[i];
            any |= bytes[i];
        }
        offset += copy_size;
    }
    if (!any) {
        zero_bytes(output, size);
        return welovec::x86_64::rng::fill_from_virtio(output, size);
    }
    return true;
}

void copy_name(char *destination, const char *source) {
    uint32_t index = 0;
    while (index + 1 < WELOVEC_SECURITY_NAME_MAX && source[index]) {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

uint32_t text_length(const char *text, uint32_t maximum) {
    if (!text) return maximum;
    uint32_t length = 0;
    while (length < maximum && text[length]) ++length;
    return length;
}

bool valid_name(const char *name, uint32_t *out_length) {
    if (!name || !out_length) return false;
    uint32_t length = 0;
    while (length + 1 < WELOVEC_SECURITY_NAME_MAX && name[length]) {
        const char c = name[length];
        const bool allowed = (c >= 'a' && c <= 'z') ||
                             (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!allowed) return false;
        ++length;
    }
    if (!length || name[length]) return false;
    if ((length == 1 && name[0] == '.') ||
        (length == 2 && name[0] == '.' && name[1] == '.')) return false;
    *out_length = length;
    return true;
}

bool valid_password(const char *password, uint32_t *out_length) {
    if (!password || !out_length) return false;
    uint32_t length = text_length(password, WELOVEC_USER_PASSWORD_MAX + 1);
    if (length < WELOVEC_USER_PASSWORD_MIN ||
        length > WELOVEC_USER_PASSWORD_MAX) return false;
    *out_length = length;
    return true;
}

bool same_name(const char *left, const char *right) {
    uint32_t index = 0;
    while (left[index] && right[index]) {
        if (left[index] != right[index]) return false;
        ++index;
    }
    return left[index] == right[index];
}

int find_uid_locked(uint32_t uid) {
    for (uint32_t i = 0; i < gSecurity.count; ++i)
        if (gSecurity.accounts[i].public_info.uid == uid) return static_cast<int>(i);
    return -1;
}

int find_name_locked(const char *name) {
    for (uint32_t i = 0; i < gSecurity.count; ++i)
        if (same_name(gSecurity.accounts[i].public_info.name, name))
            return static_cast<int>(i);
    return -1;
}

bool insert_builtin_locked(const char *name, uint32_t uid, uint64_t capabilities) {
    if (gSecurity.count >= kMaximumUsers) return false;
    Account &account = gSecurity.accounts[gSecurity.count++];
    zero_bytes(&account, sizeof(account));
    account.public_info.uid = uid;
    account.public_info.gid = uid;
    account.public_info.capabilities = capabilities;
    copy_name(account.public_info.name, name);
    return true;
}

uint32_t crc32(const uint8_t *data, uint32_t size) {
    uint32_t crc = UINT32_C(0xffffffff);
    for (uint32_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (uint32_t bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) &
                                static_cast<uint32_t>(-(static_cast<int32_t>(crc & 1))));
    }
    return ~crc;
}

uint32_t disk_size(uint32_t count) {
    return sizeof(DiskHeader) + count * sizeof(DiskAccount);
}

bool validate_disk_image(DiskImage *image, uint32_t size) {
    if (!image || size < sizeof(DiskHeader) || image->header.magic != kDatabaseMagic ||
        image->header.version != kDatabaseVersion ||
        !image->header.account_count ||
        image->header.account_count > kMaximumUsers - 2 ||
        size != disk_size(image->header.account_count)) return false;
    const uint32_t expected = image->header.checksum;
    image->header.checksum = 0;
    const uint32_t actual = crc32(reinterpret_cast<const uint8_t *>(image), size);
    image->header.checksum = expected;
    if (expected != actual) return false;

    bool administrator_present = false;
    for (uint32_t i = 0; i < image->header.account_count; ++i) {
        const DiskAccount &entry = image->accounts[i];
        uint32_t name_length = 0;
        if (entry.uid < 1001 || entry.uid >= 1001 + kMaximumUsers - 2 ||
            entry.gid != entry.uid || entry.role > 1 ||
            entry.iterations < kMinimumAcceptedIterations ||
            entry.iterations > kMaximumAcceptedIterations ||
            !valid_name(entry.name, &name_length) ||
            same_name(entry.name, "root") || same_name(entry.name, "guest"))
            return false;
        (void)name_length;
        uint8_t hash_bits = 0;
        for (uint32_t byte = 0; byte < kHashSize; ++byte)
            hash_bits |= entry.password_hash[byte];
        if (!hash_bits) return false;
        administrator_present |= entry.role == 1;
        for (uint32_t prior = 0; prior < i; ++prior) {
            if (image->accounts[prior].uid == entry.uid ||
                same_name(image->accounts[prior].name, entry.name)) return false;
        }
    }
    return administrator_present;
}

bool read_database_slot(uint32_t slot, DiskImage *out, bool *exists) {
    if (!out || !exists) return false;
    const char *path = slot == 0 ? kDatabasePath0 : kDatabasePath1;
    *exists = file_exists(path);
    if (!*exists) return false;
    uint8_t *data = nullptr;
    uint32_t size = 0;
    if (!vfs_read_file(path, &data, &size) || !data) return false;
    bool valid = size <= sizeof(DiskImage);
    if (valid) {
        zero_bytes(out, sizeof(*out));
        copy_bytes(out, data, size);
        valid = validate_disk_image(out, size);
    }
    vfs_free_file(data);
    return valid;
}

void account_from_disk(Account *out, const DiskAccount &disk) {
    zero_bytes(out, sizeof(*out));
    out->public_info.uid = disk.uid;
    out->public_info.gid = disk.gid;
    out->public_info.capabilities = disk.role ? WELOVEC_CAP_ALL : 0;
    copy_name(out->public_info.name, disk.name);
    out->iterations = disk.iterations;
    copy_bytes(out->salt, disk.salt, sizeof(out->salt));
    copy_bytes(out->password_hash, disk.password_hash, sizeof(out->password_hash));
    out->has_password = true;
}

void database_from_pending(uint64_t sequence) {
    zero_bytes(&gDiskImage, sizeof(gDiskImage));
    gDiskImage.header.magic = kDatabaseMagic;
    gDiskImage.header.sequence = sequence;
    gDiskImage.header.version = kDatabaseVersion;
    gDiskImage.header.account_count = gSecurity.pending_count - 2;
    for (uint32_t i = 2; i < gSecurity.pending_count; ++i) {
        const Account &source = gSecurity.pending_accounts[i];
        DiskAccount &target = gDiskImage.accounts[i - 2];
        target.uid = source.public_info.uid;
        target.gid = source.public_info.gid;
        target.role = source.public_info.capabilities == WELOVEC_CAP_ALL ? 1 : 0;
        target.iterations = source.iterations;
        copy_name(target.name, source.public_info.name);
        copy_bytes(target.salt, source.salt, sizeof(target.salt));
        copy_bytes(target.password_hash, source.password_hash, sizeof(target.password_hash));
    }
    const uint32_t size = disk_size(gDiskImage.header.account_count);
    gDiskImage.header.checksum = crc32(reinterpret_cast<const uint8_t *>(&gDiskImage), size);
}

bool write_pending_database(uint64_t sequence, int32_t slot) {
    database_from_pending(sequence);
    const char *path = slot == 0 ? kDatabasePath0 : kDatabasePath1;
    return vfs_write_file(path, reinterpret_cast<const uint8_t *>(&gDiskImage),
                          disk_size(gDiskImage.header.account_count));
}

void derive_salt_locked(const char *name, uint32_t uid, uint64_t sequence,
                        uint8_t salt[kSaltSize]) {
    static constexpr uint8_t fallback_key[] =
        "WeLoveC account password salt fallback v1";
    uint8_t input[8 + 8 + 4 + 8 + WELOVEC_SECURITY_NAME_MAX]{};
    uint64_t counter = ++gSecurity.entropy_counter;
    uint32_t low_tsc = 0, high_tsc = 0;
    __asm__ volatile("rdtsc" : "=a"(low_tsc), "=d"(high_tsc));
    const uint64_t timestamp = (static_cast<uint64_t>(high_tsc) << 32) | low_tsc;
    uint32_t offset = 0;
    const uint64_t values[] = {counter, sequence};
    for (uint64_t value : values) {
        for (uint32_t i = 0; i < 8; ++i)
            input[offset++] = static_cast<uint8_t>(value >> (i * 8));
    }
    for (uint32_t i = 0; i < 4; ++i)
        input[offset++] = static_cast<uint8_t>(uid >> (i * 8));
    for (uint32_t i = 0; i < 8; ++i)
        input[offset++] = static_cast<uint8_t>(timestamp >> (i * 8));
    uint32_t i = 0;
    while (i + 1 < WELOVEC_SECURITY_NAME_MAX && name[i]) {
        input[offset + i] = static_cast<uint8_t>(name[i]);
        ++i;
    }
    uint8_t digest[32];
    const uint8_t *key = gSecurity.entropy_valid
        ? gSecurity.entropy : fallback_key;
    const uint64_t key_size = gSecurity.entropy_valid
        ? sizeof(gSecurity.entropy) : sizeof(fallback_key) - 1;
    crypto::hmac_sha256(key, key_size, input, sizeof(input), digest);
    copy_bytes(salt, digest, kSaltSize);
    crypto::secure_zero(digest, sizeof(digest));
    crypto::secure_zero(input, sizeof(input));
}

bool lookup_credentials(uint32_t uid, Credentials *out) {
    if (!out || !gSecurity.initialized) return false;
    const uint64_t irq = gSecurity.lock.lock_irqsave();
    const int index = find_uid_locked(uid);
    if (index < 0) {
        gSecurity.lock.unlock_irqrestore(irq);
        return false;
    }
    const welovec_security_user &user = gSecurity.accounts[index].public_info;
    out->uid = user.uid;
    out->gid = user.gid;
    out->capabilities = user.capabilities;
    gSecurity.lock.unlock_irqrestore(irq);
    return true;
}

int64_t add_account(const char *name, const char *password, bool administrator,
                    bool initial_setup) {
    uint32_t name_length = 0, password_length = 0;
    if (!valid_name(name, &name_length) || !valid_password(password, &password_length))
        return -22;
    (void)name_length;
    if (!gSecurity.initialized) return -5;

    uint64_t next_sequence = 0;
    int32_t target_slot = 0;
    uint32_t uid = 0;
    const uint64_t irq = gSecurity.lock.lock_irqsave();
    if (gSecurity.mutation_pending) {
        gSecurity.lock.unlock_irqrestore(irq);
        return -16;
    }
    if (initial_setup) {
        if (!gSecurity.setup_required || gSecurity.count != 2) {
            gSecurity.lock.unlock_irqrestore(irq);
            return -17;
        }
    } else if (gSecurity.setup_required) {
        gSecurity.lock.unlock_irqrestore(irq);
        return -13;
    }
    if (find_name_locked(name) >= 0) {
        gSecurity.lock.unlock_irqrestore(irq);
        return -17;
    }
    if (gSecurity.count >= kMaximumUsers || gSecurity.next_uid == UINT32_MAX) {
        gSecurity.lock.unlock_irqrestore(irq);
        return -28;
    }
    uid = gSecurity.next_uid++;
    gSecurity.pending_count = gSecurity.count + 1;
    for (uint32_t i = 0; i < gSecurity.count; ++i)
        gSecurity.pending_accounts[i] = gSecurity.accounts[i];
    Account &new_account = gSecurity.pending_accounts[gSecurity.count];
    zero_bytes(&new_account, sizeof(new_account));
    new_account.public_info.uid = uid;
    new_account.public_info.gid = uid;
    new_account.public_info.capabilities = administrator ? WELOVEC_CAP_ALL : 0;
    copy_name(new_account.public_info.name, name);
    new_account.iterations = kPasswordIterations;
    new_account.has_password = true;
    derive_salt_locked(name, uid, gSecurity.sequence + 1, new_account.salt);
    gSecurity.mutation_pending = true;
    next_sequence = gSecurity.sequence + 1;
    target_slot = gSecurity.active_slot == 0 ? 1 : 0;
    gSecurity.lock.unlock_irqrestore(irq);

    crypto::pbkdf2_sha256(reinterpret_cast<const uint8_t *>(password), password_length,
                          new_account.salt, sizeof(new_account.salt),
                          kPasswordIterations, new_account.password_hash);
    const bool written = write_pending_database(next_sequence, target_slot);

    const uint64_t finish_irq = gSecurity.lock.lock_irqsave();
    if (written) {
        for (uint32_t i = 0; i < gSecurity.pending_count; ++i)
            gSecurity.accounts[i] = gSecurity.pending_accounts[i];
        gSecurity.count = gSecurity.pending_count;
        gSecurity.sequence = next_sequence;
        gSecurity.active_slot = target_slot;
        gSecurity.setup_required = false;
    } else if (gSecurity.next_uid == uid + 1) {
        --gSecurity.next_uid;
    }
    gSecurity.mutation_pending = false;
    gSecurity.lock.unlock_irqrestore(finish_irq);
    crypto::secure_zero(&new_account, sizeof(new_account));
    return written ? static_cast<int64_t>(uid) : -5;
}

} // namespace

bool initialize(const WelovecBootInfo *boot_info) {
    if (gSecurity.initialized) return true;
    if (!boot_info || boot_info->magic != WELOVEC_BOOTINFO_MAGIC ||
        boot_info->size < sizeof(WelovecBootInfo)) return false;

    uint8_t initial_entropy[32]{};
    bool initial_entropy_valid = false;
    if ((boot_info->flags & WELOVEC_BOOT_FLAG_RNG_VALID) != 0) {
        uint8_t any = 0;
        for (uint32_t i = 0; i < sizeof(initial_entropy); ++i) {
            initial_entropy[i] = boot_info->entropy[i];
            any |= initial_entropy[i];
        }
        initial_entropy_valid = any != 0;
    }
    if (!initial_entropy_valid) {
        zero_bytes(initial_entropy, sizeof(initial_entropy));
        initial_entropy_valid = fill_hardware_entropy(
            initial_entropy, sizeof(initial_entropy));
    }

    const uint64_t irq = gSecurity.lock.lock_irqsave();
    zero_bytes(gSecurity.accounts, sizeof(gSecurity.accounts));
    gSecurity.count = 0;
    gSecurity.next_uid = 1001;
    gSecurity.sequence = 0;
    gSecurity.active_slot = -1;
    gSecurity.setup_required = false;
    gSecurity.entropy_valid = initial_entropy_valid;
    if (initial_entropy_valid)
        copy_bytes(gSecurity.entropy, initial_entropy, sizeof(gSecurity.entropy));
    crypto::secure_zero(initial_entropy, sizeof(initial_entropy));
    if (!insert_builtin_locked("root", WELOVEC_UID_ROOT, WELOVEC_CAP_ALL) ||
        !insert_builtin_locked("guest", WELOVEC_UID_GUEST, 0)) {
        gSecurity.lock.unlock_irqrestore(irq);
        return false;
    }
    gSecurity.lock.unlock_irqrestore(irq);

    if (!fs_is_directory("/userspace/system") &&
        !make_dir("/userspace/system")) return false;

    bool exists0 = false, exists1 = false;
    const bool valid0 = read_database_slot(0, &gDiskCandidates[0], &exists0);
    const bool valid1 = read_database_slot(1, &gDiskCandidates[1], &exists1);
    if ((exists0 && !valid0) && (exists1 && !valid1)) return false;
    if ((exists0 || exists1) && !valid0 && !valid1) return false;

    const int selected = valid0 && valid1
        ? (gDiskCandidates[1].header.sequence > gDiskCandidates[0].header.sequence ? 1 : 0)
        : (valid1 ? 1 : (valid0 ? 0 : -1));
    const uint64_t lock = gSecurity.lock.lock_irqsave();
    if (selected >= 0) {
        const DiskImage &image = gDiskCandidates[selected];
        for (uint32_t i = 0; i < image.header.account_count; ++i) {
            Account &account = gSecurity.accounts[gSecurity.count++];
            account_from_disk(&account, image.accounts[i]);
            if (account.public_info.uid >= gSecurity.next_uid)
                gSecurity.next_uid = account.public_info.uid + 1;
        }
        gSecurity.sequence = image.header.sequence;
        gSecurity.active_slot = selected;
        gSecurity.setup_required = false;
    } else {
        gSecurity.setup_required = true;
    }
    gSecurity.initialized = true;
    gSecurity.lock.unlock_irqrestore(lock);
    return true;
}

bool setup_required() {
    if (!gSecurity.initialized) return false;
    const uint64_t irq = gSecurity.lock.lock_irqsave();
    const bool result = gSecurity.setup_required;
    gSecurity.lock.unlock_irqrestore(irq);
    return result;
}

bool default_credentials(Credentials *out) { return lookup_credentials(WELOVEC_UID_GUEST, out); }
bool root_credentials(Credentials *out) { return lookup_credentials(WELOVEC_UID_ROOT, out); }
bool credentials_for_uid(uint32_t uid, Credentials *out) { return lookup_credentials(uid, out); }

bool credentials_are_valid(const Credentials &credentials) {
    Credentials registered{};
    return lookup_credentials(credentials.uid, &registered) &&
           registered.gid == credentials.gid &&
           registered.capabilities == credentials.capabilities;
}

bool has_capability(const Credentials &credentials, uint64_t capability) {
    return capability != 0 && (credentials.capabilities & capability) == capability;
}

bool current_has_capability(uint64_t capability) {
    const uint64_t process_id = welovec::process::current_id();
    if (process_id && !welovec::process::current_session_authenticated()) return false;
    Credentials credentials{};
    return welovec::process::current_credentials(&credentials) &&
           has_capability(credentials, capability);
}

bool current_user_status(welovec_user_status *out) {
    if (!out) return false;
    Credentials credentials{};
    if (!welovec::process::current_credentials(&credentials)) return false;
    const bool authenticated = welovec::process::current_session_authenticated();
    const uint64_t irq = gSecurity.lock.lock_irqsave();
    const int index = find_uid_locked(credentials.uid);
    if (index < 0) {
        gSecurity.lock.unlock_irqrestore(irq);
        return false;
    }
    zero_bytes(out, sizeof(*out));
    out->uid = credentials.uid;
    out->gid = credentials.gid;
    out->authenticated = authenticated ? 1u : 0u;
    out->capabilities = out->authenticated ? credentials.capabilities : 0;
    copy_name(out->name, gSecurity.accounts[index].public_info.name);
    gSecurity.lock.unlock_irqrestore(irq);
    return true;
}

int64_t login(const char *name, const char *password) {
    if (!gSecurity.initialized || !password) return -22;
    const uint32_t password_length = text_length(password, WELOVEC_USER_PASSWORD_MAX + 1);
    if (password_length > WELOVEC_USER_PASSWORD_MAX) return -22;
    uint32_t name_length = 0;
    const bool name_valid = valid_name(name, &name_length);
    (void)name_length;
    uint8_t salt[kSaltSize]{};
    uint8_t expected[kHashSize]{};
    uint32_t iterations = kPasswordIterations;
    Credentials credentials{};
    bool account_found = false;
    const uint64_t now = welovec::x86_64::scheduler::tick_count();

    const uint64_t irq = gSecurity.lock.lock_irqsave();
    if (gSecurity.login_pending) {
        gSecurity.lock.unlock_irqrestore(irq);
        return -13;
    }
    const int index = name_valid ? find_name_locked(name) : -1;
    if ((index >= 0 && gSecurity.accounts[index].lockout_until > now) ||
        (index < 0 && gSecurity.unknown_lockout_until > now)) {
        gSecurity.lock.unlock_irqrestore(irq);
        return -13;
    }
    gSecurity.login_pending = true;
    if (index >= 2 && gSecurity.accounts[index].has_password) {
        const Account &account = gSecurity.accounts[index];
        copy_bytes(salt, account.salt, sizeof(salt));
        copy_bytes(expected, account.password_hash, sizeof(expected));
        iterations = account.iterations;
        credentials.uid = account.public_info.uid;
        credentials.gid = account.public_info.gid;
        credentials.capabilities = account.public_info.capabilities;
        account_found = true;
    } else {
        static constexpr uint8_t dummy_salt[kSaltSize] = {
            0x31, 0x7a, 0x93, 0x0d, 0x48, 0x65, 0xa1, 0xfe,
            0x22, 0x19, 0xd4, 0x80, 0x6b, 0x53, 0xc7, 0x0e
        };
        copy_bytes(salt, dummy_salt, sizeof(salt));
    }
    gSecurity.lock.unlock_irqrestore(irq);

    uint8_t actual[kHashSize];
    /* SYSCALL masks IF on entry. No security lock is held during PBKDF2, so
       let timer/input interrupts continue while the deliberately expensive
       password check runs. Restore the syscall's masked-interrupt state
       before touching shared login state again. */
    __asm__ volatile("sti" ::: "memory");
    crypto::pbkdf2_sha256(reinterpret_cast<const uint8_t *>(password), password_length,
                          salt, sizeof(salt), iterations, actual);
    __asm__ volatile("cli" ::: "memory");
    const bool hash_matches = crypto::constant_time_equal(actual, expected, sizeof(actual));
    const bool authenticated = account_found & hash_matches;
    const uint64_t finish_irq = gSecurity.lock.lock_irqsave();
    gSecurity.login_pending = false;
    if (authenticated) {
        gSecurity.accounts[index].failed_logins = 0;
        gSecurity.accounts[index].lockout_until = 0;
    } else if (index >= 0) {
        Account &account = gSecurity.accounts[index];
        if (++account.failed_logins >= kLoginFailuresBeforeLock) {
            account.failed_logins = 0;
            account.lockout_until = now + kLoginLockTicks;
        }
    } else if (++gSecurity.failed_logins >= kLoginFailuresBeforeLock) {
        gSecurity.failed_logins = 0;
        gSecurity.unknown_lockout_until = now + kLoginLockTicks;
    }
    gSecurity.lock.unlock_irqrestore(finish_irq);
    crypto::secure_zero(actual, sizeof(actual));
    crypto::secure_zero(expected, sizeof(expected));
    crypto::secure_zero(salt, sizeof(salt));
    if (!authenticated) return -13;
    return welovec::process::set_current_credentials(credentials, true) ? 0 : -3;
}

int64_t logout() {
    Credentials guest{};
    if (!default_credentials(&guest)) return -5;
    return welovec::process::set_current_credentials(guest, false) ? 0 : -3;
}

int64_t create_user(const char *name, const char *password, uint32_t flags) {
    if (flags & ~WELOVEC_USER_CREATE_ADMIN) return -22;
    if (!welovec::process::current_session_authenticated() ||
        !current_has_capability(WELOVEC_CAP_MANAGE_USERS)) return -13;
    if ((flags & WELOVEC_USER_CREATE_ADMIN) &&
        !current_has_capability(WELOVEC_CAP_EXEC_AS_ADMIN)) return -13;
    return add_account(name, password,
                       (flags & WELOVEC_USER_CREATE_ADMIN) != 0, false);
}

int64_t create_initial_admin(const char *name, const char *password) {
    return add_account(name, password, true, true);
}

int64_t list_users(welovec_user_record *users, uint64_t capacity) {
    if (!gSecurity.initialized) return -5;
    const uint64_t irq = gSecurity.lock.lock_irqsave();
    const uint64_t count = gSecurity.count >= 2 ? gSecurity.count - 2 : 0;
    if (!users && capacity == 0) {
        gSecurity.lock.unlock_irqrestore(irq);
        return static_cast<int64_t>(count);
    }
    if (!users || capacity < count) {
        gSecurity.lock.unlock_irqrestore(irq);
        return -34;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const welovec_security_user &source = gSecurity.accounts[i + 2].public_info;
        users[i].uid = source.uid;
        users[i].gid = source.gid;
        users[i].capabilities = source.capabilities;
        copy_name(users[i].name, source.name);
    }
    gSecurity.lock.unlock_irqrestore(irq);
    return static_cast<int64_t>(count);
}

bool secure_random_bytes(uint8_t *output, uint64_t size) {
    if (!output && size) return false;
    if (!size) return true;

    uint8_t key[32];
    uint64_t first_counter = 0;
    const uint64_t blocks = (size + sizeof(key) - 1) / sizeof(key);
    for (;;) {
        const uint64_t irq = gSecurity.lock.lock_irqsave();
        if (!gSecurity.initialized) {
            gSecurity.lock.unlock_irqrestore(irq);
            return false;
        }
        if (gSecurity.entropy_valid &&
            blocks <= UINT64_MAX - gSecurity.entropy_counter) {
            copy_bytes(key, gSecurity.entropy, sizeof(key));
            first_counter = gSecurity.entropy_counter;
            gSecurity.entropy_counter += blocks;
            gSecurity.lock.unlock_irqrestore(irq);
            break;
        }
        gSecurity.lock.unlock_irqrestore(irq);

        /* Refill from a hardware or virtio RNG; never substitute clocks. */
        uint8_t reseed[32]{};
        if (!fill_hardware_entropy(reseed, sizeof(reseed))) {
            crypto::secure_zero(reseed, sizeof(reseed));
            return false;
        }
        const uint64_t reseed_irq = gSecurity.lock.lock_irqsave();
        if (!gSecurity.initialized) {
            gSecurity.lock.unlock_irqrestore(reseed_irq);
            crypto::secure_zero(reseed, sizeof(reseed));
            return false;
        }
        copy_bytes(gSecurity.entropy, reseed, sizeof(reseed));
        gSecurity.entropy_valid = true;
        gSecurity.entropy_counter = 0;
        gSecurity.lock.unlock_irqrestore(reseed_irq);
        crypto::secure_zero(reseed, sizeof(reseed));
    }

    uint64_t produced = 0;
    for (uint64_t block = 0; block < blocks; ++block) {
        uint8_t input[16] = {
            'W', 'L', 'C', '-', 'R', 'N', 'G', 1,
            0, 0, 0, 0, 0, 0, 0, 0
        };
        const uint64_t counter = first_counter + block;
        for (uint32_t byte = 0; byte < 8; ++byte)
            input[8 + byte] = static_cast<uint8_t>(counter >> (byte * 8));
        uint8_t digest[32];
        crypto::hmac_sha256(key, sizeof(key), input, sizeof(input), digest);
        const uint64_t remaining = size - produced;
        const uint64_t copied = remaining < sizeof(digest) ? remaining : sizeof(digest);
        copy_bytes(output + produced, digest, copied);
        produced += copied;
        crypto::secure_zero(digest, sizeof(digest));
        crypto::secure_zero(input, sizeof(input));
    }
    crypto::secure_zero(key, sizeof(key));
    return true;
}

} // namespace welovec::security

extern "C" int welovec_security_initialize(const WelovecBootInfo *boot_info) {
    return welovec::security::initialize(boot_info) ? 1 : 0;
}

extern "C" int welovec_security_setup_required(void) {
    return welovec::security::setup_required() ? 1 : 0;
}

extern "C" int64_t welovec_security_create_initial_admin(
    const char *name, const char *password) {
    return welovec::security::create_initial_admin(name, password);
}
