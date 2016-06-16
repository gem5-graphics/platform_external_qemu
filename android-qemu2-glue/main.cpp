// Copyright 2015 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include "android/base/containers/StringVector.h"
#include "android/base/files/PathUtils.h"
#include "android/base/Log.h"
#include "android/base/memory/ScopedPtr.h"
#include "android/base/StringFormat.h"
#include "android/base/system/System.h"

#include "android/android.h"
#include "android/avd/hw-config.h"
#include "android/cmdline-option.h"
#include "android/constants.h"
#include "android/crashreport/crash-handler.h"
#include "android/error-messages.h"
#include "android/filesystems/ext4_resize.h"
#include "android/filesystems/ext4_utils.h"
#include "android/globals.h"
#include "android/help.h"
#include "android/kernel/kernel_utils.h"
#include "android/main-common.h"
#include "android/main-common-ui.h"
#include "android/main-kernel-parameters.h"
#include "android/opengl/emugl_config.h"
#include "android/process_setup.h"
#include "android/utils/bufprint.h"
#include "android/utils/debug.h"
#include "android/utils/path.h"
#include "android/utils/lineinput.h"
#include "android/utils/property_file.h"
#include "android/utils/filelock.h"
#include "android/utils/tempfile.h"
#include "android/utils/stralloc.h"
#include "android/utils/win32_cmdline_quote.h"

#include "android/skin/winsys.h"

#include "config-target.h"

extern "C" {
#include "android/skin/charmap.h"
}

#include "android/ui-emu-agent.h"
#include "android-qemu2-glue/emulation/serial_line.h"
#include "android-qemu2-glue/qemu-control-impl.h"

#ifdef TARGET_AARCH64
#define TARGET_ARM64
#endif
#ifdef TARGET_I386
#define TARGET_X86
#endif

#include <algorithm>
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>

#include "android/version.h"
#define  D(...)  do {  if (VERBOSE_CHECK(init)) dprint(__VA_ARGS__); } while (0)

int android_base_port;

extern bool android_op_wipe_data;
extern bool android_op_writable_system;

using namespace android::base;
using android::base::System;

namespace {

enum ImageType {
    IMAGE_TYPE_SYSTEM = 0,
    IMAGE_TYPE_CACHE,
    IMAGE_TYPE_USER_DATA,
    IMAGE_TYPE_SD_CARD,
};

const int kMaxPartitions = 4;
const int kMaxTargetQemuParams = 16;

/*
 * A structure used to model information about a given target CPU architecture.
 * |androidArch| is the architecture name, following Android conventions.
 * |qemuArch| is the same name, following QEMU conventions, used to locate
 * the final qemu-system-<qemuArch> binary.
 * |qemuCpu| is the QEMU -cpu parameter value.
 * |ttyPrefix| is the prefix to use for TTY devices.
 * |storageDeviceType| is the QEMU storage device type.
 * |networkDeviceType| is the QEMU network device type.
 * |imagePartitionTypes| defines the order of how the image partitions are
 * listed in the command line, because the command line order determines which
 * mount point the partition is attached to.  For x86, the first partition
 * listed in command line is mounted first, i.e. to /dev/block/vda,
 * the next one to /dev/block/vdb, etc. However, for arm/mips, it's reversed;
 * the last one is mounted to /dev/block/vda. the 2nd last to /dev/block/vdb.
 * So far, we have 4(kMaxPartitions) types defined for system, cache, userdata
 * and sdcard images.
 * |qemuExtraArgs| are the qemu parameters specific to the target platform.
 * this is a NULL-terminated list of string pointers of at most
 * kMaxTargetQemuParams(16).
 */
struct TargetInfo {
    const char* androidArch;
    const char* qemuArch;
    const char* qemuCpu;
    const char* ttyPrefix;
    const char* storageDeviceType;
    const char* networkDeviceType;
    const ImageType imagePartitionTypes[kMaxPartitions];
    const char* qemuExtraArgs[kMaxTargetQemuParams];
};

// The current target architecture information!
const TargetInfo kTarget = {
#ifdef TARGET_ARM64
    "arm64",
    "aarch64",
    "cortex-a57",
    "ttyAMA",
    "virtio-blk-device",
    "virtio-net-device",
    {IMAGE_TYPE_SD_CARD, IMAGE_TYPE_USER_DATA, IMAGE_TYPE_CACHE, IMAGE_TYPE_SYSTEM},
    {NULL},
#elif defined(TARGET_ARM)
    "arm",
    "arm",
    "cortex-a15",
    "ttyAMA",
    "virtio-blk-device",
    "virtio-net-device",
    {IMAGE_TYPE_SD_CARD, IMAGE_TYPE_USER_DATA, IMAGE_TYPE_CACHE, IMAGE_TYPE_SYSTEM},
    {NULL},
#elif defined(TARGET_MIPS64)
    "mips64",
    "mips64el",
    "MIPS64R6-generic",
    "ttyGF",
    "virtio-blk-device",
    "virtio-net-device",
    {IMAGE_TYPE_SD_CARD, IMAGE_TYPE_USER_DATA, IMAGE_TYPE_CACHE, IMAGE_TYPE_SYSTEM},
    {NULL},
#elif defined(TARGET_MIPS)
    "mips",
    "mipsel",
    "74Kf",
    "ttyGF",
    "virtio-blk-device",
    "virtio-net-device",
    {IMAGE_TYPE_SD_CARD, IMAGE_TYPE_USER_DATA, IMAGE_TYPE_CACHE, IMAGE_TYPE_SYSTEM},
    {NULL},
#elif defined(TARGET_I386)
    "x86",
    "i386",
    "qemu32",
    "ttyS",
    "virtio-blk-pci",
    "virtio-net-pci",
    {IMAGE_TYPE_SYSTEM, IMAGE_TYPE_CACHE, IMAGE_TYPE_USER_DATA, IMAGE_TYPE_SD_CARD},
    {"-vga", "none", NULL},
#elif defined(TARGET_X86_64)
    "x86_64",
    "x86_64",
    "qemu64",
    "ttyS",
    "virtio-blk-pci",
    "virtio-net-pci",
    {IMAGE_TYPE_SYSTEM, IMAGE_TYPE_CACHE, IMAGE_TYPE_USER_DATA, IMAGE_TYPE_SD_CARD},
    {"-vga", "none", NULL},
#else
    #error No target platform is defined
#endif
};

static std::string getNthParentDir(const char* path, size_t n) {
    StringVector dir = PathUtils::decompose(path);
    PathUtils::simplifyComponents(&dir);
    if (dir.size() < n + 1U) {
        return std::string("");
    }
    dir.resize(dir.size() - n);
    return PathUtils::recompose(dir);
}

/* generate parameters for each partition by type.
 * Param:
 *  args - array to hold parameters for qemu
 *  argsPosition - current index in the parameter array
 *  driveIndex - a sequence number for the drive parameter
 *  hw - the hardware configuration that conatains image info.
 *  type - what type of partition parameter to generate
*/

static void makePartitionCmd(const char** args, int* argsPosition, int* driveIndex,
                             AndroidHwConfig* hw, ImageType type, bool writable,
                             int apiLevel) {
    int n   = *argsPosition;
    int idx = *driveIndex;

#if defined(TARGET_X86_64) || defined(TARGET_I386)
    /* for x86, 'if=none' is necessary for virtio blk*/
    std::string driveParam("if=none,");
#else
    std::string driveParam;
#endif
    std::string deviceParam;

    switch (type) {
        case IMAGE_TYPE_SYSTEM:
            driveParam += StringFormat("index=%d,id=system,file=%s",
                                        idx++,
                                        hw->disk_systemPartition_initPath);
            // API 15 and under images need a read+write
            // system image.
            if (apiLevel > 15) {
                // API > 15 uses read-only system partition.
                // You can override this explicitly
                // by passing -writable-system to emulator.
                if (!writable)
                    driveParam += ",read-only";
            }
            deviceParam = StringFormat("%s,drive=system",
                                       kTarget.storageDeviceType);
            break;
        case IMAGE_TYPE_CACHE:
            driveParam += StringFormat("index=%d,id=cache,file=%s",
                                      idx++,
                                      hw->disk_cachePartition_path);
            deviceParam = StringFormat("%s,drive=cache",
                                       kTarget.storageDeviceType);
            break;
        case IMAGE_TYPE_USER_DATA:
            driveParam += StringFormat("index=%d,id=userdata,file=%s",
                                      idx++,
                                      hw->disk_dataPartition_path);
            deviceParam = StringFormat("%s,drive=userdata",
                                       kTarget.storageDeviceType);
            break;
        case IMAGE_TYPE_SD_CARD:
            if (hw->hw_sdCard_path != NULL && strcmp(hw->hw_sdCard_path, "")) {
               driveParam += StringFormat("index=%d,id=sdcard,file=%s",
                                         idx++, hw->hw_sdCard_path);
               deviceParam = StringFormat("%s,drive=sdcard",
                                          kTarget.storageDeviceType);
            } else {
                /* no sdcard is defined */
                return;
            }
            break;
        default:
            dwarning("Unknown Image type %d\n", type);
            return;
    }
    args[n++] = "-drive";
    args[n++] = ASTRDUP(driveParam.c_str());
    args[n++] = "-device";
    args[n++] = ASTRDUP(deviceParam.c_str());
    /* update the index */
    *argsPosition = n;
    *driveIndex = idx;
}


}  // namespace


extern "C" int run_qemu_main(int argc, const char **argv);

static void enter_qemu_main_loop(int argc, char **argv) {
#ifndef _WIN32
    sigset_t set;
    sigemptyset(&set);
    pthread_sigmask(SIG_SETMASK, &set, NULL);
#endif

    D("Starting QEMU main loop");
    run_qemu_main(argc, (const char**)argv);
    D("Done with QEMU main loop");

    if (android_init_error_occurred()) {
        skin_winsys_error_dialog(android_init_error_get_message(), "Error");
    }
}

#if defined(_WIN32)
// On Windows, link against qtmain.lib which provides a WinMain()
// implementation, that later calls qMain().
#define main qt_main
#endif

extern "C" int main(int argc, char **argv) {
    process_early_setup(argc, argv);

    if (argc < 1) {
        fprintf(stderr, "Invalid invocation (no program path)\n");
        return 1;
    }

    /* The emulator always uses the first serial port for kernel messages
     * and the second one for qemud. So start at the third if we need one
     * for logcat or 'shell'
     */
    const char* args[128];
    args[0] = argv[0];
    int n = 1;  // next parameter index

    AndroidHwConfig* hw = android_hw;
    AvdInfo* avd;
    AndroidOptions opts[1];
    int exitStatus = 0;

    if (!emulator_parseCommonCommandLineOptions(&argc,
                                                &argv,
                                                kTarget.androidArch,
                                                true,  // is_qemu2
                                                opts,
                                                hw,
                                                &android_avdInfo,
                                                &exitStatus)) {
        // Special case for QEMU positional parameters.
        if (exitStatus == EMULATOR_EXIT_STATUS_POSITIONAL_QEMU_PARAMETER) {
            // Copy all QEMU options to |args|, and set |n| to the number
            // of options in |args| (|argc| must be positive here).
            for (n = 1; n < argc; ++n) {
                args[n] = argv[n - 1];
            }

            // Skip the translation of command-line options and jump
            // straight to qemu_main().
            enter_qemu_main_loop(n, (char**)args);
            return 0;
        }

        // Normal exit.
        return exitStatus;
    }
    // just because we know that we're in the new emulator as we got here
    opts->ranchu = 1;

    avd = android_avdInfo;

    if (!emulator_parseUiCommandLineOptions(opts, avd, hw)) {
        return 1;
    }

    char boot_prop_ip[128] = {};
    if (opts->shared_net_id) {
        char*  end;
        long   shared_net_id = strtol(opts->shared_net_id, &end, 0);
        if (end == NULL || *end || shared_net_id < 1 || shared_net_id > 255) {
            fprintf(stderr, "option -shared-net-id must be an integer between 1 and 255\n");
            return 1;
        }
        snprintf(boot_prop_ip, sizeof(boot_prop_ip),
                 "net.shared_net_ip=10.1.2.%ld", shared_net_id);
    }
    if (boot_prop_ip[0]) {
        args[n++] = "-boot-property";
        args[n++] = boot_prop_ip;
    }

    if (opts->tcpdump) {
        args[n++] = "-tcpdump";
        args[n++] = opts->tcpdump;
    }

#ifdef CONFIG_NAND_LIMITS
    if (opts->nand_limits) {
        args[n++] = "-nand-limits";
        args[n++] = opts->nand_limits;
    }
#endif

    if (opts->timezone) {
        args[n++] = "-timezone";
        args[n++] = opts->timezone;
    }

    if (opts->netspeed) {
        args[n++] = "-netspeed";
        args[n++] = opts->netspeed;
    }
    if (opts->netdelay) {
        args[n++] = "-netdelay";
        args[n++] = opts->netdelay;
    }
    if (opts->netfast) {
        args[n++] = "-netfast";
    }

    if (opts->audio) {
        args[n++] = "-audio";
        args[n++] = opts->audio;
    }

    if (opts->cpu_delay) {
        args[n++] = "-cpu-delay";
        args[n++] = opts->cpu_delay;
    }

    if (opts->dns_server) {
        args[n++] = "-dns-server";
        args[n++] = opts->dns_server;
    }

    /** SNAPSHOT STORAGE HANDLING */

    /* If we have a valid snapshot storage path */

    if (opts->snapstorage) {
        // NOTE: If QEMU2_SNAPSHOT_SUPPORT is not defined, a warning has been
        //       already printed by emulator_parseCommonCommandLineOptions().
#ifdef QEMU2_SNAPSHOT_SUPPORT
        /* We still use QEMU command-line options for the following since
        * they can change from one invokation to the next and don't really
        * correspond to the hardware configuration itself.
        */
        if (!opts->no_snapshot_load) {
            args[n++] = "-loadvm";
            args[n++] = ASTRDUP(opts->snapshot);
        }

        if (!opts->no_snapshot_save) {
            args[n++] = "-savevm-on-exit";
            args[n++] = ASTRDUP(opts->snapshot);
        }

        if (opts->no_snapshot_update_time) {
            args[n++] = "-snapshot-no-time-update";
        }
#endif  // QEMU2_SNAPSHOT_SUPPORT
    }

    {
        // Always setup a single serial port, that can be connected
        // either to the 'null' chardev, or the -shell-serial one,
        // which by default will be either 'stdout' (Posix) or 'con:'
        // (Windows).
        const char* serial =
                (opts->shell || opts->logcat || opts->show_kernel)
                ? opts->shell_serial : "null";
        args[n++] = "-serial";
        args[n++] = serial;
    }

    if (opts->radio) {
        args[n++] = "-radio";
        args[n++] = opts->radio;
    }

    if (opts->gps) {
        args[n++] = "-gps";
        args[n++] = opts->gps;
    }

    if (opts->code_profile) {
        args[n++] = "-code-profile";
        args[n++] = opts->code_profile;
    }

    /* Pass boot properties to the core. First, those from boot.prop,
     * then those from the command-line */
    const FileData* bootProperties = avdInfo_getBootProperties(avd);
    if (!fileData_isEmpty(bootProperties)) {
        PropertyFileIterator iter[1];
        propertyFileIterator_init(iter,
                                  bootProperties->data,
                                  bootProperties->size);
        while (propertyFileIterator_next(iter)) {
            char temp[MAX_PROPERTY_NAME_LEN + MAX_PROPERTY_VALUE_LEN + 2];
            snprintf(temp, sizeof temp, "%s=%s", iter->name, iter->value);
            args[n++] = "-boot-property";
            args[n++] = ASTRDUP(temp);
        }
    }

    if (opts->prop != NULL) {
        ParamList*  pl = opts->prop;
        for ( ; pl != NULL; pl = pl->next ) {
            args[n++] = "-boot-property";
            args[n++] = pl->param;
        }
    }

    if (opts->ports) {
        args[n++] = "-android-ports";
        args[n++] = opts->ports;
    }

    if (opts->port) {
        int port = -1;
        if (!android_parse_port_option(opts->port, &port)) {
            return 1;
        }
        // Reuse the -android-ports parameter since -ports does the same
        // thing but with the second port just being the console port + 1
        std::string portsOption = StringFormat("%d,%d", port, port + 1);
        args[n++] = "-android-ports";
        args[n++] = strdup(portsOption.c_str());
    }

    if (opts->report_console) {
        args[n++] = "-android-report-console";
        args[n++] = opts->report_console;
    }

    if (opts->http_proxy) {
        args[n++] = "-http-proxy";
        args[n++] = opts->http_proxy;
    }

    if (!opts->charmap) {
        /* Try to find a valid charmap name */
        char* charmap = avdInfo_getCharmapFile(avd, hw->hw_keyboard_charmap);
        if (charmap != NULL) {
            D("autoconfig: -charmap %s", charmap);
            opts->charmap = charmap;
        }
    }

    if (opts->charmap) {
        char charmap_name[SKIN_CHARMAP_NAME_SIZE];

        if (!path_exists(opts->charmap)) {
            derror("Charmap file does not exist: %s", opts->charmap);
            return 1;
        }
        /* We need to store the charmap name in the hardware configuration.
         * However, the charmap file itself is only used by the UI component
         * and doesn't need to be set to the emulation engine.
         */
        kcm_extract_charmap_name(opts->charmap, charmap_name,
                                 sizeof(charmap_name));
        reassign_string(&hw->hw_keyboard_charmap, charmap_name);
    }

    /* Deal with camera emulation */
    if (opts->webcam_list) {
        /* List connected webcameras */
        args[n++] = "-list-webcam";
    }

// TODO: imement network
#if 0
    /* Set up the interfaces for inter-emulator networking */
    if (opts->shared_net_id) {
        unsigned int shared_net_id = atoi(opts->shared_net_id);
        char nic[37];

        args[n++] = "-net";
        args[n++] = "nic,vlan=0";
        args[n++] = "-net";
        args[n++] = "user,vlan=0";

        args[n++] = "-net";
        snprintf(nic, sizeof nic, "nic,vlan=1,macaddr=52:54:00:12:34:%02x", shared_net_id);
        args[n++] = strdup(nic);
        args[n++] = "-net";
        args[n++] = "socket,vlan=1,mcast=230.0.0.10:1234";
    }
#endif

    // Create userdata file from init version if needed.
    if (android_op_wipe_data || !path_exists(hw->disk_dataPartition_path)) {
        if (!path_exists(hw->disk_dataPartition_initPath)) {
            derror("Missing initial data partition file: %s",
                   hw->disk_dataPartition_initPath);
            return 1;
        }
        D("Creating: %s\n", hw->disk_dataPartition_path);

        if (path_copy_file(hw->disk_dataPartition_path,
                           hw->disk_dataPartition_initPath) < 0) {
            derror("Could not create %s: %s", hw->disk_dataPartition_path,
                   strerror(errno));
            return 1;
        }

        resizeExt4Partition(android_hw->disk_dataPartition_path,
                            android_hw->disk_dataPartition_size);
    }
    else {
        // Resize userdata-qemu.img if the size is smaller than what config.ini
        // says.
        // This can happen as user wants a larger data partition without wiping
        // it.
        // b.android.com/196926
        System::FileSize current_data_size;
        if (System::get()->pathFileSize(hw->disk_dataPartition_path,
                                        &current_data_size)) {
            System::FileSize partition_size = static_cast<System::FileSize>(
                    android_hw->disk_dataPartition_size);
            if (android_hw->disk_dataPartition_size > 0 &&
                    current_data_size < partition_size) {
                dwarning("userdata partition is resized from %d M to %d M\n",
                         (int)(current_data_size / (1024 * 1024)),
                         (int)(partition_size / (1024 * 1024)));
                resizeExt4Partition(android_hw->disk_dataPartition_path,
                                    android_hw->disk_dataPartition_size);
            }
        }
    }


    // Create cache partition image if it doesn't exist already.
    if (!path_exists(hw->disk_cachePartition_path)) {
        D("Creating empty ext4 cache partition: %s",
          hw->disk_cachePartition_path);
        int ret = android_createEmptyExt4Image(
                hw->disk_cachePartition_path,
                hw->disk_cachePartition_size,
                "cache");
        if (ret < 0) {
            derror("Could not create %s: %s", hw->disk_cachePartition_path,
                   strerror(-ret));
            return 1;
        }
    }

#if defined(TARGET_X86_64) || defined(TARGET_I386)
    char* accel_status = NULL;
    CpuAccelMode accel_mode = ACCEL_AUTO;
    const bool accel_ok = handleCpuAcceleration(opts, avd, &accel_mode, &accel_status);

    if (accel_mode == ACCEL_OFF) {  // 'accel off' is specified'
        args[n++] = "-cpu";
        args[n++] = kTarget.qemuCpu;
    } else if (accel_mode == ACCEL_ON) {  // 'accel on' is specified'
        if (!accel_ok) {
            derror("CPU acceleration is not supported on this machine!");
            derror("Reason: %s", accel_status);
            return 1;
        }
        args[n++] = ASTRDUP(kEnableAccelerator);
    } else {  // ACCEL_AUTO
        if (accel_ok) {
            args[n++] = ASTRDUP(kEnableAccelerator);
        } else {
            args[n++] = "-cpu";
            args[n++] = kTarget.qemuCpu;
        }
    }

    AFREE(accel_status);
#else   // !TARGET_X86_64 && !TARGET_I386
    args[n++] = "-cpu";
    args[n++] = kTarget.qemuCpu;
    args[n++] = "-machine";
    args[n++] = "type=ranchu";
#endif  // !TARGET_X86_64 && !TARGET_I386

#if defined(TARGET_X86_64) || defined(TARGET_I386)
    // SMP Support.
    std::string ncores;
    if (hw->hw_cpu_ncore > 1) {
        args[n++] = "-smp";

#ifdef _WIN32
        if (hw->hw_cpu_ncore > 16) {
            dwarning("HAXM does not support more than 16 cores. Number of cores set to 16");
            hw->hw_cpu_ncore = 16;
        }
#endif
        ncores = StringFormat("cores=%ld", hw->hw_cpu_ncore);
        args[n++] = ncores.c_str();
    }
#endif  // !TARGET_X86_64 && !TARGET_I386

    // Memory size
    args[n++] = "-m";
    std::string memorySize = StringFormat("%ld", hw->hw_ramSize);
    args[n++] = memorySize.c_str();

    // Kernel command-line parameters.
    AndroidGlesEmulationMode glesMode = kAndroidGlesEmulationOff;
    if (hw->hw_gpu_enabled) {
        if (!strcmp(hw->hw_gpu_mode, "guest")) {
            glesMode = kAndroidGlesEmulationGuest;
        } else {
            glesMode = kAndroidGlesEmulationHost;
        }
    }

    uint64_t glesCMA = 0ULL;
    if ((glesMode == kAndroidGlesEmulationGuest) ||
        (opts->gpu && !strcmp(opts->gpu, "guest")) ||
        !hw->hw_gpu_enabled) {
        // Set CMA (continguous memory allocation) to values that depend on
        // the desired resolution.
        // We will assume a double buffered 32-bit framebuffer in the calculation.
        int framebuffer_width = hw->hw_lcd_width;
        int framebuffer_height = hw->hw_lcd_height;
        uint64_t bytes = framebuffer_width * framebuffer_height * 4;
        const uint64_t one_MB = 1024ULL * 1024;
        glesCMA = (2 * bytes + one_MB - 1) / one_MB;
        VERBOSE_PRINT(init, "Adjusting Contiguous Memory Allocation"
                      "of %dx%d framebuffer for software renderer to %"
                      PRIu64 "MB.", framebuffer_width, framebuffer_height,
                      glesCMA);
    }

    int apiLevel = avd ? avdInfo_getApiLevel(avd) : 1000;

    char* kernel_parameters = emulator_getKernelParameters(
            opts, kTarget.androidArch, apiLevel, kTarget.ttyPrefix,
            hw->kernel_parameters, glesMode, glesCMA,
            true  // isQemu2
            );
    if (!kernel_parameters) {
        return 1;
    }

    args[n++] = "-append";
    args[n++] = kernel_parameters;

    // Support for changing default lcd-density
    std::string lcd_density;
    if (hw->hw_lcd_density) {
        args[n++] = "-lcd-density";
        lcd_density = StringFormat("%d", hw->hw_lcd_density);
        args[n++] = lcd_density.c_str();
    }

    // Kernel image
    args[n++] = "-kernel";
    args[n++] = hw->kernel_path;

    // Ramdisk
    args[n++] = "-initrd";
    args[n++] = hw->disk_ramdisk_path;

    /*
     * add partition parameters with the sequence
     * pre-defined in targetInfo.imagePartitionTypes
     */
    int s;
    int drvIndex = 0;
    for (s = 0; s < kMaxPartitions; s++) {
        bool writable = (kTarget.imagePartitionTypes[s] == IMAGE_TYPE_SYSTEM) ?
                    android_op_writable_system : true;
        makePartitionCmd(args, &n, &drvIndex, hw,
                         kTarget.imagePartitionTypes[s], writable, apiLevel);
    }

    // Network
    args[n++] = "-netdev";
    args[n++] = "user,id=mynet";
    args[n++] = "-device";
    std::string netDevice =
            StringFormat("%s,netdev=mynet", kTarget.networkDeviceType);
    args[n++] = netDevice.c_str();
    args[n++] = "-show-cursor";

    // Graphics
    if (opts->no_window) {
        args[n++] = "-nographic";
        // also disable the qemu monitor which will otherwise grab stdio
        args[n++] = "-monitor";
        args[n++] = "none";
    }

    // Data directory (for keymaps and PC Bios).
    args[n++] = "-L";
    std::string dataDir = getNthParentDir(args[0], 3U);
    if (dataDir.empty()) {
        dataDir = "lib/pc-bios";
    } else {
        dataDir += "/lib/pc-bios";
    }
    args[n++] = dataDir.c_str();

    /* append extra qemu parameters if any */
    for (int idx = 0; kTarget.qemuExtraArgs[idx] != NULL; idx++) {
        args[n++] = kTarget.qemuExtraArgs[idx];
    }

    /* append the options after -qemu */
    for (int i = 0; i < argc; ++i) {
        args[n++] = argv[i];
    }

    /* Generate a hardware-qemu.ini for this AVD. The real hardware
     * configuration is ususally stored in several files, e.g. the AVD's
     * config.ini plus the skin-specific hardware.ini.
     *
     * The new file will group all definitions and will be used to
     * launch the core with the -android-hw <file> option.
     */
    {
        const char* coreHwIniPath = avdInfo_getCoreHwIniPath(avd);
        const auto hwIni = android::base::makeCustomScopedPtr(
                         iniFile_newEmpty(NULL), iniFile_free);
        androidHwConfig_write(hw, hwIni.get());

        if (filelock_create(coreHwIniPath) == NULL) {
            /* The AVD is already in use, we still support this as an
             * experimental feature. Use a temporary hardware-qemu.ini
             * file though to avoid overwriting the existing one. */
             TempFile*  tempIni = tempfile_create();
             coreHwIniPath = tempfile_path(tempIni);
        }

        /* While saving HW config, ignore valueless entries. This will not break
         * anything, but will significantly simplify comparing the current HW
         * config with the one that has been associated with a snapshot (in case
         * VM starts from a snapshot for this instance of emulator). */
        if (iniFile_saveToFileClean(hwIni.get(), coreHwIniPath) < 0) {
            derror("Could not write hardware.ini to %s: %s", coreHwIniPath, strerror(errno));
            exit(2);
        }
        args[n++] = "-android-hw";
        args[n++] = strdup(coreHwIniPath);

        crashhandler_copy_attachment(CRASH_AVD_HARDWARE_INFO, coreHwIniPath);

        /* In verbose mode, dump the file's content */
        if (VERBOSE_CHECK(init)) {
            FILE* file = fopen(coreHwIniPath, "rt");
            if (file == NULL) {
                derror("Could not open hardware configuration file: %s\n",
                       coreHwIniPath);
            } else {
                LineInput* input = lineInput_newFromStdFile(file);
                const char* line;
                printf("Content of hardware configuration file:\n");
                while ((line = lineInput_getLine(input)) !=  NULL) {
                    printf("  %s\n", line);
                }
                printf(".\n");
                lineInput_free(input);
                fclose(file);
            }
        }
    }

    args[n] = NULL;
    // Check if we had enough slots in |args|.
    assert(n < (int)(sizeof(args)/sizeof(args[0])));

    if(VERBOSE_CHECK(init)) {
        int i;
        printf("QEMU options list:\n");
        for(i = 0; i < n; i++) {
            printf("emulator: argv[%02d] = \"%s\"\n", i, args[i]);
        }
        /* Dump final command-line option to make debugging the core easier */
        printf("Concatenated QEMU options:\n");
        for (i = 0; i < n; i++) {
            /* To make it easier to copy-paste the output to a command-line,
             * quote anything that contains spaces.
             */
            if (strchr(args[i], ' ') != NULL) {
                printf(" '%s'", args[i]);
            } else {
                printf(" %s", args[i]);
            }
        }
        printf("\n");
    }

    qemu2_android_serialline_init();

    static UiEmuAgent uiEmuAgent;
    uiEmuAgent.battery = gQAndroidBatteryAgent;
    uiEmuAgent.cellular = gQAndroidCellularAgent;
    uiEmuAgent.finger = gQAndroidFingerAgent;
    uiEmuAgent.location = gQAndroidLocationAgent;
    uiEmuAgent.sensors = gQAndroidSensorsAgent;
    uiEmuAgent.telephony = gQAndroidTelephonyAgent;
    uiEmuAgent.userEvents = gQAndroidUserEventAgent;
    uiEmuAgent.window = gQAndroidEmulatorWindowAgent;

    // for now there's no uses of SettingsAgent, so we don't set it
    uiEmuAgent.settings = NULL;

    /* Setup SDL UI just before calling the code */
#ifndef _WIN32
    sigset_t set;
    sigfillset(&set);
    pthread_sigmask(SIG_SETMASK, &set, NULL);
#endif  // !_WIN32

    if (!emulator_initUserInterface(opts, &uiEmuAgent)) {
        return 1;
    }

    skin_winsys_spawn_thread(opts->no_window, enter_qemu_main_loop, n, (char**)args);
    skin_winsys_enter_main_loop(opts->no_window, argc, argv);

    emulator_finiUserInterface();

    process_late_teardown();
    return 0;
}