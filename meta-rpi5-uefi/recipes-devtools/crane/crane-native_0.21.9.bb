SUMMARY = "crane, the go-containerregistry CLI"
DESCRIPTION = "Pulls and inspects OCI images. talos-boot-dtbs uses it to lift \
the bcm2712 device trees out of the Talos kernel image, so that the DTB the \
board boots is the one the kernel it boots was built against. \
\
The upstream release binary rather than a Go build: this is a build-host tool \
that never ships in an image, and building it from source would drag a whole \
Go toolchain into a firmware build for one 'crane export' invocation."
HOMEPAGE = "https://github.com/google/go-containerregistry"

LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://LICENSE;md5=175792518e4ac015ab6696d16c4f607e"

# Prebuilt Go binary: nothing to compile, and stripping a Go binary that was
# already stripped upstream only risks breaking it.
INHIBIT_DEFAULT_DEPS = "1"
INHIBIT_PACKAGE_STRIP = "1"
INHIBIT_SYSROOT_STRIP = "1"
EXCLUDE_FROM_WORLD = "1"

# Build-host architecture, in the naming the release assets use.
CRANE_ARCH = "${@{'x86_64': 'x86_64', 'aarch64': 'arm64'}.get(d.getVar('BUILD_ARCH'), 'unsupported')}"

CRANE_SHA256 = "${@{ \
    'x86_64': '5c16d8ddb971cb1d5e6ed8b1e743da8224414eeba2c2762d8f1a61b2f095699e', \
    'arm64':  '1f4c647b7bb260ab5435661df5b526cf59950ebf95201790db7183ac189cbcbd', \
}.get(d.getVar('CRANE_ARCH'), '')}"

SRC_URI = "https://github.com/google/go-containerregistry/releases/download/v${PV}/go-containerregistry_Linux_${CRANE_ARCH}.tar.gz"
SRC_URI[sha256sum] = "${CRANE_SHA256}"

# The tarball is flat -- LICENSE, README.md, crane, gcrane, krane -- with no
# top-level directory, so the unpack dir is the source dir.
UNPACKDIR ?= "${WORKDIR}"
S = "${UNPACKDIR}"

inherit native

python () {
    if d.getVar('CRANE_ARCH') == 'unsupported':
        raise bb.parse.SkipRecipe(
            "no crane release binary for build host %s; add its checksum to "
            "CRANE_SHA256 or put crane on PATH" % d.getVar('BUILD_ARCH'))
}

do_configure[noexec] = "1"
do_compile[noexec] = "1"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${S}/crane ${D}${bindir}/crane
}
