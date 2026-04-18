#!/usr/bin/env python3
"""Generate BE300.xcodeproj/project.pbxproj for the iOS port."""

import hashlib
import os

def uid(name):
    return hashlib.md5(name.encode()).hexdigest()[:24].upper()

# Source file definitions: (key_base, relative_path_from_project)
swift_files = [
    ('app_swift', 'BE300/App.swift'),
    ('emulator_engine', 'BE300/EmulatorEngine.swift'),
    ('metal_renderer', 'BE300/MetalRenderer.swift'),
    ('input_overlay', 'BE300/InputOverlay.swift'),
    ('emulator_view', 'BE300/EmulatorView.swift'),
]

be300_c_files = [
    ('machine_be300', '../../src/machine_be300.c'),
    ('be300_devices', '../../src/be300_devices.c'),
    ('host_io', '../../src/host_io.c'),
    ('loader', '../../src/loader.c'),
    ('ui', '../../src/ui.c'),
]

hw_c_files = [
    ('bcu', '../../src/hw/bcu.c'),
    ('cmu', '../../src/hw/cmu.c'),
    ('pmu', '../../src/hw/pmu.c'),
    ('icu', '../../src/hw/icu.c'),
    ('siu', '../../src/hw/siu.c'),
    ('rtc', '../../src/hw/rtc.c'),
    ('gpio', '../../src/hw/gpio.c'),
    ('nand', '../../src/hw/nand.c'),
]

gxemul_c_files = [
    ('gx_cpu', '../../gxemul/src/cpus/cpu.c'),
    ('gx_cpu_mips', '../../gxemul/src/cpus/cpu_mips.c'),
    ('gx_cpu_mips16', '../../gxemul/src/cpus/cpu_mips16.c'),
    ('gx_cpu_mips_coproc', '../../gxemul/src/cpus/cpu_mips_coproc.c'),
    ('gx_cpu_mips_unaligned', '../../gxemul/src/cpus/cpu_mips_instr_unaligned.c'),
    ('gx_memory', '../../gxemul/src/core/memory.c'),
    ('gx_interrupt', '../../gxemul/src/core/interrupt.c'),
    ('gx_timer', '../../gxemul/src/core/timer.c'),
    ('gx_settings', '../../gxemul/src/core/settings.c'),
    ('gx_misc', '../../gxemul/src/core/misc.c'),
    ('gx_debugmsg', '../../gxemul/src/core/debugmsg.c'),
    ('gx_float_emul', '../../gxemul/src/core/float_emul.c'),
    ('gx_console', '../../gxemul/src/console/console.c'),
    ('gx_device', '../../gxemul/src/devices/device.c'),
    ('gx_dev_vr41xx', '../../gxemul/src/devices/dev_vr41xx.c'),
    ('gx_dev_ram', '../../gxemul/src/devices/dev_ram.c'),
    ('gx_dev_ns16550', '../../gxemul/src/devices/dev_ns16550.c'),
    ('gx_dev_fb', '../../gxemul/src/devices/dev_fb.c'),
    ('gx_symbol', '../../gxemul/src/symbol/symbol.c'),
    ('gx_machine', '../../gxemul/src/machines/machine.c'),
    ('gx_machine_hpcmips', '../../gxemul/src/machines/machine_hpcmips.c'),
    ('gx_stubs', '../../gxemul/src/stubs.c'),
]

all_source_files = swift_files + be300_c_files + hw_c_files + gxemul_c_files

lines = []
def w(s=''):
    lines.append(s)

# Collect sections
file_ref_lines = []
build_file_lines = []
source_build_ids = []

for key, path in all_source_files:
    fid = uid('file_' + key)
    bid = uid('build_' + key)
    name = path.split('/')[-1]
    stype = 'sourcecode.swift' if name.endswith('.swift') else 'sourcecode.c.c'
    file_ref_lines.append(
        '\t\t%s /* %s */ = {isa = PBXFileReference; lastKnownFileType = %s; path = "%s"; sourceTree = "<group>"; };'
        % (fid, name, stype, path))
    build_file_lines.append(
        '\t\t%s /* %s in Sources */ = {isa = PBXBuildFile; fileRef = %s /* %s */; };'
        % (bid, name, fid, name))
    source_build_ids.append('\t\t\t\t%s /* %s in Sources */,' % (bid, name))

# Resource files
kernel_fid = uid('file_kernel')
kernel_bid = uid('build_kernel')
file_ref_lines.append(
    '\t\t%s /* vmlinux-pgui-demo */ = {isa = PBXFileReference; lastKnownFileType = file; path = "../../kernels/vmlinux-pgui-demo"; sourceTree = "<group>"; };'
    % kernel_fid)
build_file_lines.append(
    '\t\t%s /* vmlinux-pgui-demo in Resources */ = {isa = PBXBuildFile; fileRef = %s /* vmlinux-pgui-demo */; };'
    % (kernel_bid, kernel_fid))

assets_fid = uid('file_assets')
assets_bid = uid('build_assets')
file_ref_lines.append(
    '\t\t%s /* Assets.xcassets */ = {isa = PBXFileReference; lastKnownFileType = folder.assetcatalog; path = BE300/Assets.xcassets; sourceTree = "<group>"; };'
    % assets_fid)
build_file_lines.append(
    '\t\t%s /* Assets.xcassets in Resources */ = {isa = PBXBuildFile; fileRef = %s /* Assets.xcassets */; };'
    % (assets_bid, assets_fid))

bridging_fid = uid('file_bridging_header')
file_ref_lines.append(
    '\t\t%s /* BE300-Bridging-Header.h */ = {isa = PBXFileReference; lastKnownFileType = sourcecode.c.h; path = "BE300-Bridging-Header.h"; sourceTree = "<group>"; };'
    % bridging_fid)

infoplist_fid = uid('file_infoplist')
file_ref_lines.append(
    '\t\t%s /* Info.plist */ = {isa = PBXFileReference; lastKnownFileType = text.plist.xml; path = BE300/Info.plist; sourceTree = "<group>"; };'
    % infoplist_fid)

product_fid = uid('appProduct')
file_ref_lines.append(
    '\t\t%s /* BE300.app */ = {isa = PBXFileReference; explicitFileType = wrapper.application; includeInIndex = 0; path = BE300.app; sourceTree = BUILT_PRODUCTS_DIR; };'
    % product_fid)

def group_children(file_list):
    result = []
    for key, path in file_list:
        name = path.split('/')[-1]
        result.append('\t\t\t\t%s /* %s */,' % (uid('file_' + key), name))
    return result

# Write the pbxproj
w('// !$*UTF8*$!')
w('{')
w('\tarchiveVersion = 1;')
w('\tclasses = {')
w('\t};')
w('\tobjectVersion = 56;')
w('\tobjects = {')
w()
w('/* Begin PBXBuildFile section */')
for l in build_file_lines: w(l)
w('/* End PBXBuildFile section */')
w()
w('/* Begin PBXFileReference section */')
for l in file_ref_lines: w(l)
w('/* End PBXFileReference section */')
w()
w('/* Begin PBXGroup section */')

# Main group
w('\t\t%s = {' % uid('mainGroup'))
w('\t\t\tisa = PBXGroup;')
w('\t\t\tchildren = (')
for gname in ['swiftGroup', 'cGroup', 'hwGroup', 'gxemulGroup', 'resourcesGroup', 'productsGroup']:
    label = {'swiftGroup':'Swift','cGroup':'BE300 C','hwGroup':'HW','gxemulGroup':'GXemul','resourcesGroup':'Resources','productsGroup':'Products'}[gname]
    w('\t\t\t\t%s /* %s */,' % (uid(gname), label))
w('\t\t\t);')
w('\t\t\tsourceTree = "<group>";')
w('\t\t};')

# Sub groups
for gname, label, flist in [
    ('swiftGroup', 'Swift', swift_files),
    ('cGroup', 'BE300 C', be300_c_files),
    ('hwGroup', 'HW', hw_c_files),
    ('gxemulGroup', 'GXemul', gxemul_c_files),
]:
    w('\t\t%s /* %s */ = {' % (uid(gname), label))
    w('\t\t\tisa = PBXGroup;')
    w('\t\t\tchildren = (')
    for l in group_children(flist): w(l)
    w('\t\t\t);')
    w('\t\t\tname = "%s";' % label)
    w('\t\t\tsourceTree = "<group>";')
    w('\t\t};')

# Resources group
w('\t\t%s /* Resources */ = {' % uid('resourcesGroup'))
w('\t\t\tisa = PBXGroup;')
w('\t\t\tchildren = (')
w('\t\t\t\t%s /* Assets.xcassets */,' % assets_fid)
w('\t\t\t\t%s /* vmlinux-pgui-demo */,' % kernel_fid)
w('\t\t\t\t%s /* BE300-Bridging-Header.h */,' % bridging_fid)
w('\t\t\t\t%s /* Info.plist */,' % infoplist_fid)
w('\t\t\t);')
w('\t\t\tname = Resources;')
w('\t\t\tsourceTree = "<group>";')
w('\t\t};')

# Products group
w('\t\t%s /* Products */ = {' % uid('productsGroup'))
w('\t\t\tisa = PBXGroup;')
w('\t\t\tchildren = (')
w('\t\t\t\t%s /* BE300.app */,' % product_fid)
w('\t\t\t);')
w('\t\t\tname = Products;')
w('\t\t\tsourceTree = "<group>";')
w('\t\t};')

w('/* End PBXGroup section */')
w()

# Native target
w('/* Begin PBXNativeTarget section */')
w('\t\t%s /* BE300 */ = {' % uid('nativeTarget'))
w('\t\t\tisa = PBXNativeTarget;')
w('\t\t\tbuildConfigurationList = %s /* Build configuration list for PBXNativeTarget "BE300" */;' % uid('buildConfigList_target'))
w('\t\t\tbuildPhases = (')
w('\t\t\t\t%s /* Sources */,' % uid('buildPhase_sources'))
w('\t\t\t\t%s /* Frameworks */,' % uid('buildPhase_frameworks'))
w('\t\t\t\t%s /* Resources */,' % uid('buildPhase_resources'))
w('\t\t\t);')
w('\t\t\tbuildRules = (')
w('\t\t\t);')
w('\t\t\tdependencies = (')
w('\t\t\t);')
w('\t\t\tname = BE300;')
w('\t\t\tproductName = BE300;')
w('\t\t\tproductReference = %s /* BE300.app */;' % product_fid)
w('\t\t\tproductType = "com.apple.product-type.application";')
w('\t\t};')
w('/* End PBXNativeTarget section */')
w()

# Project
w('/* Begin PBXProject section */')
w('\t\t%s /* Project object */ = {' % uid('project'))
w('\t\t\tisa = PBXProject;')
w('\t\t\tbuildConfigurationList = %s /* Build configuration list for PBXProject "BE300" */;' % uid('buildConfigList_project'))
w('\t\t\tcompatibilityVersion = "Xcode 14.0";')
w('\t\t\thasScannedForEncodings = 0;')
w('\t\t\tknownRegions = (')
w('\t\t\t\ten,')
w('\t\t\t\tBase,')
w('\t\t\t);')
w('\t\t\tmainGroup = %s;' % uid('mainGroup'))
w('\t\t\tproductRefGroup = %s /* Products */;' % uid('productsGroup'))
w('\t\t\tprojectDirPath = "";')
w('\t\t\tprojectRoot = "";')
w('\t\t\ttargets = (')
w('\t\t\t\t%s /* BE300 */,' % uid('nativeTarget'))
w('\t\t\t);')
w('\t\t};')
w('/* End PBXProject section */')
w()

# Resources build phase
w('/* Begin PBXResourcesBuildPhase section */')
w('\t\t%s /* Resources */ = {' % uid('buildPhase_resources'))
w('\t\t\tisa = PBXResourcesBuildPhase;')
w('\t\t\tbuildActionMask = 2147483647;')
w('\t\t\tfiles = (')
w('\t\t\t\t%s /* vmlinux-pgui-demo in Resources */,' % kernel_bid)
w('\t\t\t\t%s /* Assets.xcassets in Resources */,' % assets_bid)
w('\t\t\t);')
w('\t\t\trunOnlyForDeploymentPostprocessing = 0;')
w('\t\t};')
w('/* End PBXResourcesBuildPhase section */')
w()

# Sources build phase
w('/* Begin PBXSourcesBuildPhase section */')
w('\t\t%s /* Sources */ = {' % uid('buildPhase_sources'))
w('\t\t\tisa = PBXSourcesBuildPhase;')
w('\t\t\tbuildActionMask = 2147483647;')
w('\t\t\tfiles = (')
for l in source_build_ids: w(l)
w('\t\t\t);')
w('\t\t\trunOnlyForDeploymentPostprocessing = 0;')
w('\t\t};')
w('/* End PBXSourcesBuildPhase section */')
w()

# Frameworks build phase
w('/* Begin PBXFrameworksBuildPhase section */')
w('\t\t%s /* Frameworks */ = {' % uid('buildPhase_frameworks'))
w('\t\t\tisa = PBXFrameworksBuildPhase;')
w('\t\t\tbuildActionMask = 2147483647;')
w('\t\t\tfiles = (')
w('\t\t\t);')
w('\t\t\trunOnlyForDeploymentPostprocessing = 0;')
w('\t\t};')
w('/* End PBXFrameworksBuildPhase section */')
w()

# Build configurations
def write_project_config(config_uid, name, is_debug):
    w('\t\t%s /* %s */ = {' % (config_uid, name))
    w('\t\t\tisa = XCBuildConfiguration;')
    w('\t\t\tbuildSettings = {')
    w('\t\t\t\tALWAYS_SEARCH_USER_PATHS = NO;')
    w('\t\t\t\tCLANG_CXX_LANGUAGE_STANDARD = "gnu++20";')
    w('\t\t\t\tCLANG_ENABLE_MODULES = YES;')
    w('\t\t\t\tCLANG_ENABLE_OBJC_ARC = YES;')
    w('\t\t\t\tCOPY_PHASE_STRIP = NO;')
    if is_debug:
        w('\t\t\t\tDEBUG_INFORMATION_FORMAT = dwarf;')
        w('\t\t\t\tENABLE_STRICT_OBJC_MSGSEND = YES;')
        w('\t\t\t\tENABLE_TESTABILITY = YES;')
    else:
        w('\t\t\t\tDEBUG_INFORMATION_FORMAT = "dwarf-with-dsym";')
        w('\t\t\t\tENABLE_NS_ASSERTIONS = NO;')
        w('\t\t\t\tENABLE_STRICT_OBJC_MSGSEND = YES;')
    w('\t\t\t\tGCC_C_LANGUAGE_STANDARD = c11;')
    if is_debug:
        w('\t\t\t\tGCC_DYNAMIC_NO_PIC = NO;')
        w('\t\t\t\tGCC_OPTIMIZATION_LEVEL = 0;')
        w('\t\t\t\tGCC_PREPROCESSOR_DEFINITIONS = (')
        w('\t\t\t\t\t"DEBUG=1",')
        w('\t\t\t\t\t"$(inherited)",')
        w('\t\t\t\t);')
    else:
        w('\t\t\t\tGCC_OPTIMIZATION_LEVEL = 2;')
    w('\t\t\t\tGCC_WARN_ABOUT_RETURN_TYPE = YES_ERROR;')
    w('\t\t\t\tGCC_WARN_UNINITIALIZED_AUTOS = YES_AGGRESSIVE;')
    w('\t\t\t\tGCC_WARN_UNUSED_FUNCTION = YES;')
    w('\t\t\t\tGCC_WARN_UNUSED_VARIABLE = NO;')
    w('\t\t\t\tIPHONEOS_DEPLOYMENT_TARGET = 16.0;')
    if is_debug:
        w('\t\t\t\tMTL_ENABLE_DEBUG_INFO = INCLUDE_SOURCE;')
        w('\t\t\t\tONLY_ACTIVE_ARCH = YES;')
    else:
        w('\t\t\t\tMTL_ENABLE_DEBUG_INFO = NO;')
    w('\t\t\t\tSDKROOT = iphoneos;')
    if is_debug:
        w('\t\t\t\tSWIFT_ACTIVE_COMPILATION_CONDITIONS = DEBUG;')
        w('\t\t\t\tSWIFT_OPTIMIZATION_LEVEL = "-Onone";')
    else:
        w('\t\t\t\tSWIFT_COMPILATION_MODE = wholemodule;')
        w('\t\t\t\tSWIFT_OPTIMIZATION_LEVEL = "-O";')
        w('\t\t\t\tVALIDATE_PRODUCT = YES;')
    w('\t\t\t\tOTHER_CFLAGS = (')
    w('\t\t\t\t\t"-Wno-unused-parameter",')
    w('\t\t\t\t\t"-Wno-unused-variable",')
    w('\t\t\t\t\t"-Wno-sign-compare",')
    w('\t\t\t\t\t"-Wno-missing-field-initializers",')
    w('\t\t\t\t);')
    w('\t\t\t};')
    w('\t\t\tname = %s;' % name)
    w('\t\t};')

def write_target_config(config_uid, name):
    w('\t\t%s /* %s */ = {' % (config_uid, name))
    w('\t\t\tisa = XCBuildConfiguration;')
    w('\t\t\tbuildSettings = {')
    w('\t\t\t\tASSETCATALOG_COMPILER_APPICON_NAME = AppIcon;')
    w('\t\t\t\tCODE_SIGN_STYLE = Automatic;')
    w('\t\t\t\tCURRENT_PROJECT_VERSION = 1;')
    w('\t\t\t\tGENERATE_INFOPLIST_FILE = NO;')
    w('\t\t\t\tINFOPLIST_FILE = BE300/Info.plist;')
    w('\t\t\t\tINFOPLIST_KEY_UIApplicationSceneManifest_Generation = YES;')
    w('\t\t\t\tINFOPLIST_KEY_UIApplicationSupportsIndirectInputEvents = YES;')
    w('\t\t\t\tINFOPLIST_KEY_UIStatusBarHidden = YES;')
    w('\t\t\t\tLD_RUNPATH_SEARCH_PATHS = (')
    w('\t\t\t\t\t"$(inherited)",')
    w('\t\t\t\t\t"@executable_path/Frameworks",')
    w('\t\t\t\t);')
    w('\t\t\t\tMARKETING_VERSION = 1.0;')
    w('\t\t\t\tPRODUCT_BUNDLE_IDENTIFIER = com.be300.emulator;')
    w('\t\t\t\tPRODUCT_NAME = "$(TARGET_NAME)";')
    w('\t\t\t\tSWIFT_EMIT_LOC_STRINGS = YES;')
    w('\t\t\t\tSWIFT_OBJC_BRIDGING_HEADER = "BE300-Bridging-Header.h";')
    w('\t\t\t\tSWIFT_VERSION = 5.0;')
    w('\t\t\t\tTARGETED_DEVICE_FAMILY = "1,2";')
    w('\t\t\t\tHEADER_SEARCH_PATHS = (')
    w('\t\t\t\t\t"$(SRCROOT)/../../src",')
    w('\t\t\t\t\t"$(SRCROOT)/../../gxemul/src/include",')
    w('\t\t\t\t);')
    w('\t\t\t};')
    w('\t\t\tname = %s;' % name)
    w('\t\t};')

w('/* Begin XCBuildConfiguration section */')
write_project_config(uid('debugConfig_project'), 'Debug', True)
write_project_config(uid('releaseConfig_project'), 'Release', False)
write_target_config(uid('debugConfig_target'), 'Debug')
write_target_config(uid('releaseConfig_target'), 'Release')
w('/* End XCBuildConfiguration section */')
w()

# Configuration lists
w('/* Begin XCConfigurationList section */')
w('\t\t%s /* Build configuration list for PBXProject "BE300" */ = {' % uid('buildConfigList_project'))
w('\t\t\tisa = XCConfigurationList;')
w('\t\t\tbuildConfigurations = (')
w('\t\t\t\t%s /* Debug */,' % uid('debugConfig_project'))
w('\t\t\t\t%s /* Release */,' % uid('releaseConfig_project'))
w('\t\t\t);')
w('\t\t\tdefaultConfigurationIsVisible = 0;')
w('\t\t\tdefaultConfigurationName = Release;')
w('\t\t};')
w('\t\t%s /* Build configuration list for PBXNativeTarget "BE300" */ = {' % uid('buildConfigList_target'))
w('\t\t\tisa = XCConfigurationList;')
w('\t\t\tbuildConfigurations = (')
w('\t\t\t\t%s /* Debug */,' % uid('debugConfig_target'))
w('\t\t\t\t%s /* Release */,' % uid('releaseConfig_target'))
w('\t\t\t);')
w('\t\t\tdefaultConfigurationIsVisible = 0;')
w('\t\t\tdefaultConfigurationName = Release;')
w('\t\t};')
w('/* End XCConfigurationList section */')
w()

w('\t};')
w('\trootObject = %s /* Project object */;' % uid('project'))
w('}')

output = '\n'.join(lines) + '\n'

outpath = os.path.join(os.path.dirname(__file__), 'BE300', 'BE300.xcodeproj', 'project.pbxproj')
with open(outpath, 'w') as f:
    f.write(output)

print('Generated %s (%d lines)' % (outpath, len(lines)))
