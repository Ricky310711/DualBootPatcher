/*
 * Copyright (C) 2015  Andrew Gunnerson <andrewgunnerson@gmail.com>
 *
 * This file is part of MultiBootPatcher
 *
 * MultiBootPatcher is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * MultiBootPatcher is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with MultiBootPatcher.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "switcher.h"

#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>

#include <openssl/sha.h>

#include "roms.h"
#include "util/chmod.h"
#include "util/chown.h"
#include "util/copy.h"
#include "util/directory.h"
#include "util/file.h"
#include "util/finally.h"
#include "util/logging.h"
#include "util/path.h"
#include "util/properties.h"
#include "util/selinux.h"
#include "util/string.h"

#define MULTIBOOT_DIR "/data/media/0/MultiBoot"
#define CHECKSUMS_PATH "/data/multiboot/checksums.prop"

namespace mb
{

/*!
 * \brief Check a checksum property
 *
 * \param props Pointer to properties map
 * \param rom_id ROM ID
 * \param image Image filename (without directory)
 * \param sha512_out SHA512 hex digest output
 *
 * \return ChecksumsGetResult::FOUND if the hash was successfully retrieved,
 *         ChecksumsGetResult::NOT_FOUND if the hash does not exist in the map,
 *         ChecksumsGetResult::MALFORMED if the property has an invalid format
 */
ChecksumsGetResult checksums_get(std::unordered_map<std::string, std::string> *props,
                                 const std::string &rom_id,
                                 const std::string &image,
                                 std::string *sha512_out)
{
    std::string checksums_path = get_raw_path(CHECKSUMS_PATH);

    std::string key(rom_id);
    key += "/";
    key += image;

    if (props->find(key) == props->end()) {
        return ChecksumsGetResult::NOT_FOUND;
    }

    const std::string &value = (*props)[key];

    std::size_t pos = value.find(":");
    if (pos != std::string::npos) {
        std::string algo = value.substr(0, pos);
        std::string hash = value.substr(pos + 1);
        if (algo != "sha512") {
            LOGE("%s: Invalid hash algorithm: %s",
                 checksums_path.c_str(), algo.c_str());
            return ChecksumsGetResult::MALFORMED;
        }

        *sha512_out = hash;
        return ChecksumsGetResult::FOUND;
    } else {
        LOGE("%s: Invalid checksum property: %s=%s",
             checksums_path.c_str(), key.c_str(), value.c_str());
        return ChecksumsGetResult::MALFORMED;
    }
}

/*!
 * \brief Update a checksum property
 *
 * \param props Pointer to properties map
 * \param rom_id ROM ID
 * \param image Image filename (without directory)
 * \param sha512 SHA512 hex digest
 */
void checksums_update(std::unordered_map<std::string, std::string> *props,
                      const std::string &rom_id,
                      const std::string &image,
                      const std::string &sha512)
{
    std::string key(rom_id);
    key += "/";
    key += image;

    (*props)[key] = "sha512:";
    (*props)[key] += sha512;
}

/*!
 * \brief Read checksums properties from \a /data/multiboot/checksums.prop
 *
 * \param props Pointer to properties map
 *
 * \return True if the file was successfully read. Otherwise, false.
 */
bool checksums_read(std::unordered_map<std::string, std::string> *props)
{
    std::string checksums_path = get_raw_path(CHECKSUMS_PATH);

    if (!util::file_get_all_properties(checksums_path, props)) {
        LOGE("%s: Failed to load properties", checksums_path.c_str());
        return false;
    }
    return true;
}

/*!
 * \brief Write checksums properties to \a /data/multiboot/checksums.prop
 *
 * \param props Properties map
 *
 * \return True if successfully written. Otherwise, false.
 */
bool checksums_write(const std::unordered_map<std::string, std::string> &props)
{
    std::string checksums_path = get_raw_path(CHECKSUMS_PATH);

    if (remove(checksums_path.c_str()) < 0 && errno != ENOENT) {
        LOGW("%s: Failed to remove file: %s",
             checksums_path.c_str(), strerror(errno));
    }

    util::mkdir_parent(checksums_path, 0755);
    util::create_empty_file(checksums_path);

    if (!util::chown(checksums_path, 0, 0, 0)) {
        LOGW("%s: Failed to chown file: %s",
             checksums_path.c_str(), strerror(errno));
    }
    if (chmod(checksums_path.c_str(), 0700) < 0) {
        LOGW("%s: Failed to chmod file: %s",
             checksums_path.c_str(), strerror(errno));
    }

    if (!util::file_write_properties(checksums_path, props)) {
        LOGW("%s: Failed to write new properties: %s",
             checksums_path.c_str(), strerror(errno));
        return false;
    }

    return true;
}

struct Flashable
{
    std::string image;
    std::string block_dev;
    std::string expected_hash;
    std::string hash;
    unsigned char *data = nullptr;
    std::size_t size = 0;
};

/*!
 * \brief Perform non-recursive search for a block device
 *
 * This function will non-recursively search \a search_dirs for a block device
 * named \a partition. \a /dev/block/ is implicitly added to the search paths.
 *
 * \param search_dirs Search paths
 * \param partition Block device name
 *
 * \return Block device path if found. Otherwise, an empty string.
 */
static std::string find_block_dev(const std::vector<std::string> &search_dirs,
                                  const std::string &partition)
{
    struct stat sb;

    if (util::starts_with(partition, "mmcblk")) {
        std::string path("/dev/block/");
        path += partition;

        if (stat(path.c_str(), &sb) == 0) {
            return path;
        }
    }

    for (const std::string &base_dir : search_dirs) {
        std::string block_dev(base_dir);
        block_dev += "/";
        block_dev += partition;

        if (stat(block_dev.c_str(), &sb) == 0) {
            return block_dev;
        }
    }

    return std::string();
}

/*!
 * \brief Fix permissions and label on /data/media/0/MultiBoot/
 *
 * This function will do the following on /data/media/0/MultiBoot/:
 * 1. Recursively change ownership to media_rw:media_rw
 * 2. Recursively change mode to 0775
 * 3. Recursively change the SELinux label to the same label as /data/media/0/
 *
 * \return True if all operations succeeded. False, if any failed.
 */
static bool fix_permissions()
{
    util::create_empty_file(MULTIBOOT_DIR "/.nomedia");

    if (!util::chown(MULTIBOOT_DIR, "media_rw", "media_rw",
                     util::CHOWN_RECURSIVE)) {
        LOGE("Failed to chown %s", MULTIBOOT_DIR);
        return false;
    }

    if (!util::chmod_recursive(MULTIBOOT_DIR, 0775)) {
        LOGE("Failed to chmod %s", MULTIBOOT_DIR);
        return false;
    }

    std::string context;
    if (util::selinux_lget_context("/data/media/0", &context)
            && !util::selinux_lset_context_recursive(MULTIBOOT_DIR, context)) {
        LOGE("%s: Failed to set context to %s: %s",
             MULTIBOOT_DIR, context.c_str(), strerror(errno));
        return false;
    }

    return true;
}

static bool add_extra_images(const std::string &multiboot_dir,
                             const std::vector<std::string> &block_dev_dirs,
                             std::vector<Flashable> *flashables)
{
    DIR *dir;
    dirent *ent;

    dir = opendir(multiboot_dir.c_str());
    if (!dir) {
        return false;
    }

    auto close_directory = util::finally([&]{
        closedir(dir);
    });

    while ((ent = readdir(dir))) {
        std::string name(ent->d_name);
        if (name == ".img" || !util::ends_with(name, ".img")) {
            // Skip non-images
            continue;
        }
        if (util::starts_with(name, "boot.img")) {
            // Skip boot images, which are handled separately
            continue;
        }

        std::string partition = name.substr(0, name.size() - 4);
        std::string path(multiboot_dir);
        path += "/";
        path += name;

        // Blacklist non-modem partition
        if (partition != "mdm" && partition != "modem") {
            LOGW("Partition %s is not whitelisted for flashing",
                 partition.c_str());
            continue;
        }

        std::string block_dev = find_block_dev(block_dev_dirs, partition);
        if (block_dev.empty()) {
            LOGW("Couldn't find block device for partition %s",
                 partition.c_str());
            continue;
        }

        LOGD("Found extra image:");
        LOGD("- Source: %s", path.c_str());
        LOGD("- Target: %s", block_dev.c_str());

        flashables->emplace_back();
        flashables->back().image = std::move(path);
        flashables->back().block_dev = std::move(block_dev);
    }

    return true;
}

/*!
 * \brief Switch to another ROM
 *
 * \note If the checksum is missing for some images to be flashed and invalid
 *       for some other images to be flashed, this function will always return
 *       SwitchRomResult::CHECKSUM_INVALID.
 *
 * \param id ROM ID to switch to
 * \param boot_blockdev Block device path of the boot partition
 * \param blockdev_base_dirs Search paths (non-recursive) for block devices
 *                           corresponding to extra flashable images in
 *                           /sdcard/MultiBoot/[ROM ID]/ *.img
 *
 * \return SwitchRomResult::SUCCEEDED if the switching succeeded,
 *         SwitchRomResult::FAILED if the switching failed,
 *         SwitchRomResult::CHECKSUM_NOT_FOUND if the checksum for some image is missing,
 *         SwitchRomResult::CHECKSUM_INVALID if the checksum for some image is invalid
 *
 */
SwitchRomResult switch_rom(const std::string &id, const std::string &boot_blockdev,
                           const std::vector<std::string> &blockdev_base_dirs,
                           bool force_update_checksums)
{
    LOGD("Attempting to switch to %s", id.c_str());
    LOGD("Force update checksums: %d", force_update_checksums);

    // Path for all of the images
    std::string multiboot_path(MULTIBOOT_DIR);
    multiboot_path += "/";
    multiboot_path += id;

    std::string bootimg_path(multiboot_path);
    bootimg_path += "/boot.img";

    // Verify ROM ID
    Roms roms;
    roms.add_installed();

    auto r = roms.find_by_id(id);
    if (!r) {
        LOGE("Invalid ROM ID: %s", id.c_str());
        return SwitchRomResult::FAILED;
    }

    if (!util::mkdir_recursive(multiboot_path, 0775)) {
        LOGE("%s: Failed to create directory: %s",
             multiboot_path.c_str(), strerror(errno));
        return SwitchRomResult::FAILED;
    }

    // We'll read the files we want to flash into memory so a malicious app
    // can't change the file between the hash verification step and flashing
    // step.

    std::vector<Flashable> flashables;
    auto free_flashables = util::finally([&]{
        for (Flashable &f : flashables) {
            free(f.data);
        }
    });

    flashables.emplace_back();
    flashables.back().image = bootimg_path;
    flashables.back().block_dev = boot_blockdev;

    if (!add_extra_images(multiboot_path, blockdev_base_dirs, &flashables)) {
        LOGW("Failed to find extra images");
    }

    std::unordered_map<std::string, std::string> props;
    checksums_read(&props);

    for (Flashable &f : flashables) {
        // If memory becomes an issue, an alternative method is to create a
        // temporary directory in /data/multiboot/ that's only writable by root
        // and copy the images there.
        if (!util::file_read_all(f.image, &f.data, &f.size)) {
            LOGE("%s: Failed to read image: %s",
                 f.image.c_str(), strerror(errno));
            return SwitchRomResult::FAILED;
        }

        // Get actual sha512sum
        unsigned char digest[SHA512_DIGEST_LENGTH];
        SHA512(f.data, f.size, digest);
        f.hash = util::hex_string(digest, SHA512_DIGEST_LENGTH);

        if (force_update_checksums) {
            checksums_update(&props, id, util::base_name(f.image), f.hash);
        }

        // Get expected sha512sum
        ChecksumsGetResult ret = checksums_get(
                &props, id, util::base_name(f.image), &f.expected_hash);
        if (ret == ChecksumsGetResult::MALFORMED) {
            return SwitchRomResult::CHECKSUM_INVALID;
        }

        // Verify hashes if we have an expected hash
        if (ret == ChecksumsGetResult::FOUND && f.expected_hash != f.hash) {
            LOGE("%s: Checksum (%s) does not match expected (%s)",
                 f.image.c_str(), f.hash.c_str(), f.expected_hash.c_str());
            return SwitchRomResult::CHECKSUM_INVALID;
        }
    }

    // Fail if we're missing expected hashes. We do this last to make sure
    // CHECKSUM_INVALID is returned if some checksums don't match (for the ones
    // that aren't missing).
    for (Flashable &f : flashables) {
        if (f.expected_hash.empty()) {
            LOGE("%s: Checksum does not exist", f.image.c_str());
            return SwitchRomResult::CHECKSUM_NOT_FOUND;
        }
    }

    // Now we can flash the images
    for (Flashable &f : flashables) {
        // Cast is okay. The data is just passed to fwrite (ie. no signed
        // extension issues)
        if (!util::file_write_data(f.block_dev, (char *) f.data, f.size)) {
            LOGE("%s: Failed to write image: %s",
                 f.block_dev.c_str(), strerror(errno));
            return SwitchRomResult::FAILED;
        }
    }

    if (force_update_checksums) {
        LOGD("Updating checksums file");
        checksums_write(props);
    }

    if (!fix_permissions()) {
        return SwitchRomResult::FAILED;
    }

    return SwitchRomResult::SUCCEEDED;
}

/*!
 * \brief Set the kernel for a ROM
 *
 * \note This will update the checksum for the image in
 *       \a /data/multiboot/checksums.prop.
 *
 * \param id ROM ID to set the kernel for
 * \param boot_blockdev Block device path of the boot partition
 *
 * \return True if the kernel was successfully set. Otherwise, false.
 */
bool set_kernel(const std::string &id, const std::string &boot_blockdev)
{
    LOGD("Attempting to set the kernel for %s", id.c_str());

    // Path for all of the images
    std::string multiboot_path(MULTIBOOT_DIR);
    multiboot_path += "/";
    multiboot_path += id;

    std::string bootimg_path(multiboot_path);
    bootimg_path += "/boot.img";

    // Verify ROM ID
    Roms roms;
    roms.add_installed();

    auto r = roms.find_by_id(id);
    if (!r) {
        LOGE("Invalid ROM ID: %s", id.c_str());
        return false;
    }

    if (!util::mkdir_recursive(multiboot_path, 0775)) {
        LOGE("%s: Failed to create directory: %s",
             multiboot_path.c_str(), strerror(errno));
        return false;
    }

    unsigned char *data;
    std::size_t size;

    if (!util::file_read_all(boot_blockdev, &data, &size)) {
        LOGE("%s: Failed to read block device: %s",
             boot_blockdev.c_str(), strerror(errno));
        return false;
    }

    auto free_data = util::finally([&]{
        free(data);
    });

    // Get actual sha512sum
    unsigned char digest[SHA512_DIGEST_LENGTH];
    SHA512(data, size, digest);
    std::string hash = util::hex_string(digest, SHA512_DIGEST_LENGTH);

    // Add to checksums.prop
    std::unordered_map<std::string, std::string> props;
    checksums_read(&props);
    checksums_update(&props, id, "boot.img", hash);

    // NOTE: This function isn't responsible for updating the checksums for
    //       any extra images. We don't want to mask any malicious changes.

    // Cast is okay. The data is just passed to fwrite (ie. no signed
    // extension issues)
    if (!util::file_write_data(bootimg_path, (char *) data, size)) {
        LOGE("%s: Failed to write image: %s",
             bootimg_path.c_str(), strerror(errno));
        return false;
    }

    LOGD("Updating checksums file");
    checksums_write(props);

    if (!fix_permissions()) {
        return false;
    }

    return true;
}

}
