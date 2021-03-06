#!/usr/bin/env python

# This script is used to generate Makefile.qemu2-sources.mk from
# the build output of build-qemu-android.sh. To use it:
#
#  rm -rf /tmp/qemu2-build
#  external/qemu/android/scripts/build-qemu-android.sh \
#      --darwin-ssh=<hostname> \
#      --install-dir=/tmp/qemu2-build
#
#  external/qemu/android-qemu2-glue/scripts/gen-qemu2-sources-mk.py \
#      /tmp/qemu2-build \
#      > external/qemu/android-qemu2-glue/build/Makefile.qemu2-sources.mk
#
import os
import sys

TARGET_SUFFIX="-softmmu"

EXPECTED_HOSTS = set([
    'linux-x86_64',
    'windows-x86',
    'windows-x86_64',
    'darwin-x86_64'])

LINK_QEMU_PREFIX = 'LINK-qemu-system-'
IGNORED_OBJECTS = [
    '../audio/sdlaudio.o',
    'gdbstub-xml.o',
    'hw/i386/acpi-build.o',
    'hw/i386/pc_piix.o',

    # these aren't used in the emulator but add 1MB+ to binary size
    '../hw/net/e1000.o',
    '../hw/net/e1000e.o',
    '../hw/net/e1000e_core.o',
    '../hw/net/e1000x_common.o',

    '../qmp-introspect.o',
    '../qmp-marshal.o',
    'trace/generated-helpers.o',
    '../ui/sdl_zoom.o',
    '../ui/sdl.o',
    '../ui/sdl2.o',
    '../ui/sdl2-2d.o',
    '../ui/sdl2-input.o',
    '../vl.o',
    '/version.o',   # something from the Windows build
    ]

CC_OBJECTS = [
    'disas/arm-a64.o',
    'disas/libvixl/vixl/a64/decoder-a64.o',
    'disas/libvixl/vixl/a64/disasm-a64.o',
    'disas/libvixl/vixl/a64/instructions-a64.o',
    'disas/libvixl/vixl/compiler-intrinsics.o',
    'disas/libvixl/vixl/utils.o',
    ]

# objects which have to be moved to *TARGET files,
# even if they could've been common (e.g. to fix some linking issues)
FORCE_TARGET_OBJECTS = [
    '../vl.o'
    ]

def find_target_lists(build_path, hosts):
    """Return a set of QEMU cpu architectures targetted by the binaries
       found under |build_path|. |hosts| is a set of hosts to probe for"""
    result = set()
    for host in hosts:
        for subdir, dirs, files in os.walk(os.path.join(build_path,host)):
            for efile in files:
                if efile.startswith(LINK_QEMU_PREFIX):
                    arch = efile[len(LINK_QEMU_PREFIX):]
                    if arch[-5:] == 'w.exe':
                        arch = arch[:-5]
                    result.add(arch)
            dirs = []
    return sorted(result)

def find_link_map(build_path, host):
    """Return a map of target architecture -> set of object file names
       as they appear in LINK-qemu-system-* files. |build_path| is the
       top-level build path, and |host| is the host to probe."""
    result = {}
    build_path = os.path.join(build_path, host)
    link_prefix = 'LINK-qemu-system-'
    for subdir, dirs, files in os.walk(build_path):
        for efile in files:
            if efile.startswith(link_prefix):
                target = efile[len(link_prefix):]
                # Remove w.exe suffix for Windows binaries.
                if target[-5:] == 'w.exe':
                    target = target[:-5]
                result[target] = set()
                with open(os.path.join(subdir,efile)) as lfile:
                    for line in lfile:
                        line = line.strip()
                        if line[-2:] in [ '.o', '.a' ]:
                            result[target].add(line)
        dirs = []
    return result

def list_files(name, files):
    print "%s := \\" % name

    for f in sorted(source_list_from_objects(files)):
        print "    %s \\" % f
    print ""


def source_list_from_objects(objects):
    result = set()
    for obj in objects:
        if obj in IGNORED_OBJECTS:
            continue
        if obj.startswith('@BUILD_DIR@/'):
            continue
        if obj.endswith('.a'):
            continue
        if obj.startswith('../'):
            obj = obj[3:]
        if obj in CC_OBJECTS:
            obj = obj[:-2] + '.cc'
        else:
            obj = obj[:-2] + '.c'
        result.add(obj)
    return sorted(result)

def main(args):
    """\
A small script used to generate the sub-Makefiles
describing the common and target-specific sources
for the QEMU2 build performed with the emulator's
build system. This is done by looking at the output
of a regular QEMU2 build, and finding which files
were built and where.

Usage: <program-name> <path-to-build-dir>

"""
    if len(args) != 2:
        print "ERROR: This script takes a single argument."
        sys.exit(1)
    build_dir = args[1]

    print "# Auto-generated by %s - DO NOT EDIT !!" % os.path.basename(args[0])
    print ""

    target_list = find_target_lists(build_dir, EXPECTED_HOSTS)
    #print "# Found targets: %s" % repr(target_list)

    host_link_map = {}
    for host in EXPECTED_HOSTS:
        host_link_map[host] = find_link_map(build_dir, host)

    host_list = sorted(host_link_map.keys())
    # The set of all objects
    all_objects = set()
    for host in host_list:
        for target in host_link_map[host]:
            all_objects |= host_link_map[host][target]

    # The set of all objects whose path begins with ../
    # this corresponds to objects that will end up in the qemu2_common
    # static library.
    common_all_objects = set([x for x in all_objects if x.startswith('../')])

    # move the forced target object out of common
    common_all_objects -= set(FORCE_TARGET_OBJECTS)

    # The set of common objects that are shared by all hosts.
    common_objects = common_all_objects.copy()
    for host in host_list:
        for target in host_link_map[host]:
            common_objects &= host_link_map[host][target]

    list_files('QEMU2_COMMON_SOURCES', common_objects)

    # For each host, the specific common objects that are not shared with
    # all other hosts, but still shared by all targets.
    host_common_map = {}
    for host in host_list:
        host_common_map[host] = common_all_objects - common_objects
        for target in host_link_map[host]:
            host_common_map[host] &= host_link_map[host][target]
        list_files('QEMU2_COMMON_SOURCES_%s' % host, host_common_map[host])

    # The set of all target-specifc objects.
    all_target_objects = all_objects - common_objects
    for host in host_list:
        all_target_objects -= host_common_map[host]

    # Find the list of target files that are shared by all targets and all
    # hosts at the same time.
    target_common_map = {}
    for target in target_list:
        target_common_map[target] = all_target_objects.copy()
        for host in host_list:
            target_common_map[target] &= host_link_map[host][target]

    target_common_objects = all_target_objects.copy()
    for target in target_common_map:
        target_common_objects &= target_common_map[target]

    list_files('QEMU2_TARGET_SOURCES', target_common_objects)

    # For each target, find the files shared by all hosts, that only
    # belong to this target.
    for target in target_list:
        target_objects = target_common_map[target] - target_common_objects
        list_files('QEMU2_TARGET_%s_SOURCES' % target, target_objects)

    # Finally, the target- and host- specific objects.
    for target in target_list:
        for host in host_list:
            objects = host_link_map[host][target] - target_common_map[target] - \
                      target_common_objects - common_all_objects
            list_files('QEMU2_TARGET_%s_SOURCES_%s' % (target, host), objects)

if __name__ == "__main__":
    main(sys.argv)
