SUMMARY = "UEFI Secure Boot default key set (PK + Microsoft KEK/db CAs)"
DESCRIPTION = "The certificates rpi5-uefi-firmware embeds as PKDefault/KEKDefault/ \
               dbDefault when RPI5_SECURE_BOOT_DEFAULT_KEYS=1: this project's own \
               Platform Key, which ships in the layer, and Microsoft's six KEK and db \
               CA certificates, which are fetched and checksum-pinned rather than \
               vendored. Staged into ${datadir}/secureboot-keys for the firmware \
               recipe to point ArmPlatformPkg/SecureBootDefaultKeys.fdf.inc's \
               PK_DEFAULT_FILE / KEK_DEFAULT_FILE* / DB_DEFAULT_FILE* macros at. \
\
               Split out of the firmware recipe so the one network fetch that is not \
               a git tree, its User-Agent workaround and the DER validation of what \
               comes back all live in one place -- and so the firmware recipe depends \
               on it only when the default key set is wanted, leaving an \
               RPI5_SECURE_BOOT_DEFAULT_KEYS=0 build with nothing to download. \
\
               files/secureboot-keys/README.md is the reference for all of it: what \
               each certificate is, why both the 2011 and 2023 generations ship, why \
               there is no DBX, and where the PK private key is (not here)."

# CLOSED, and deliberately: none of the seven files is software and none comes
# with a licence text to check a sum against. The PK certificate is this
# project's own; Microsoft's CA certificates are published for exactly this
# purpose and carry no licence document. Nothing here is compiled, packaged or
# installed into any rootfs -- the certificates only ever reach the firmware
# volume as SECTION RAW blobs.
LICENSE = "CLOSED"

PV = "1.0"

# The DER validation in do_install.
DEPENDS = "openssl-native"

# Microsoft's Secure Boot CA certificates, fetched rather than vendored.
#
# These are the canonical URLs behind Microsoft's fwlink redirects (321185,
# 321192, 321194 for the 2011 generation; 2239775, 2239776, 2239872 for
# 2023). Use these direct URLs, NOT the fwlinks: bitbake's decodeurl() /
# encodeurl() round trip mangles a query string, turning "?linkid=2239775"
# into "%3Flinkid%3D2239775" and breaking the fetch. The %20 in the 2023
# paths does survive the round trip -- verified against poky's own
# bitbake/lib/bb/fetch2.
#
# The sha256sum of each file IS that certificate's SHA-256 fingerprint,
# because these are raw DER blobs. So the checksums below are not just
# download integrity -- they pin the exact certificate identity, and
# bitbake refuses to build if Microsoft ever serves different bytes at
# these paths. Cross-check any change against the fingerprint table in
# files/secureboot-keys/README.md before accepting it.
SECUREBOOT_MS_CERT_BASE = "https://www.microsoft.com/pkiops/certs"

SRC_URI = "file://secureboot-keys \
    ${SECUREBOOT_MS_CERT_BASE}/MicCorKEKCA2011_2011-06-24.crt;name=mskek2011;downloadfilename=MicCorKEKCA2011.crt;subdir=secureboot-certs \
    ${SECUREBOOT_MS_CERT_BASE}/microsoft%20corporation%20kek%202k%20ca%202023.crt;name=mskek2023;downloadfilename=MicCorKEK2KCA2023.crt;subdir=secureboot-certs \
    ${SECUREBOOT_MS_CERT_BASE}/MicWinProPCA2011_2011-10-19.crt;name=msdbwin2011;downloadfilename=MicWinProPCA2011.crt;subdir=secureboot-certs \
    ${SECUREBOOT_MS_CERT_BASE}/MicCorUEFCA2011_2011-06-27.crt;name=msdbuefi2011;downloadfilename=MicCorUEFCA2011.crt;subdir=secureboot-certs \
    ${SECUREBOOT_MS_CERT_BASE}/windows%20uefi%20ca%202023.crt;name=msdbwin2023;downloadfilename=WindowsUEFICA2023.crt;subdir=secureboot-certs \
    ${SECUREBOOT_MS_CERT_BASE}/microsoft%20uefi%20ca%202023.crt;name=msdbuefi2023;downloadfilename=MicrosoftUEFICA2023.crt;subdir=secureboot-certs \
    "

SRC_URI[mskek2011.sha256sum]    = "a1117f516a32cefcba3f2d1ace10a87972fd6bbe8fe0d0b996e09e65d802a503"
SRC_URI[mskek2023.sha256sum]    = "3cd3f0309edae228767a976dd40d9f4affc4fbd5218f2e8cc3c9dd97e8ac6f9d"
SRC_URI[msdbwin2011.sha256sum]  = "e8e95f0733a55e8bad7be0a1413ee23c51fcea64b3c8fa6a786935fddcc71961"
SRC_URI[msdbuefi2011.sha256sum] = "48e99b991f57fc52f76149599bff0a58c47154229b9f8d603ac40d3500248507"
SRC_URI[msdbwin2023.sha256sum]  = "076f1fea90ac29155ebf77c17682f75f1fdd1be196da302dc8461e350a9ae330"
SRC_URI[msdbuefi2023.sha256sum] = "f6124e34125bee3fe6d79a574eaa7b91c0e7bd9d929c1a321178efd611dad901"

# www.microsoft.com refuses the wget fetcher with a bare 403, which surfaces
# as "wget ... failed with exit code 8" on do_fetch. It is the User-Agent
# alone, not TLS fingerprinting or a dead URL: the same request succeeds from
# curl, `curl -A wget/1.21` is refused, and a browser UA on wget succeeds --
# all six certificate URLs verified 200 that way (2026-08-18).
#
# So give the fetcher a browser User-Agent. Overriding FETCHCMD_wget is
# recipe-scoped, and this recipe fetches nothing else -- which is half the
# reason the certificates are a recipe of their own rather than six entries in
# a SRC_URI full of git trees. The rest of the command line stays bitbake's own
# default (bitbake/lib/bb/fetch2/wget.py: "/usr/bin/env wget -t 2 -T 100"),
# since bitbake appends -O/-P/--progress itself.
#
# The pinned sha256sums above are what actually guarantee we got the right
# bytes, so relaxing how they are requested costs nothing in integrity.
FETCHCMD_wget = "/usr/bin/env wget -t 2 -T 100 --user-agent='Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36'"

# UNPACKDIR only exists from styhead (Yocto 5.1) on; scarthgap unpacks
# straight into WORKDIR.
UNPACKDIR ?= "${WORKDIR}"

S = "${UNPACKDIR}"

# Certificates only: nothing is compiled and nothing is packaged, but they
# still have to reach the firmware recipe's sysroot, so do_populate_sysroot
# runs.
inherit allarch nopackages

# Where the key set lands in the sysroot. rpi5-uefi-firmware reads exactly
# these paths under ${STAGING_DATADIR} -- keep the two in step. Two subdirs, so
# a glance says which half ships in the layer and which half came off the wire.
SECUREBOOT_KEYS_DIR = "${datadir}/secureboot-keys"

# The six filenames are the downloadfilename= values above, in the order
# SecureBootDefaultKeys.fdf.inc's macros want them: KEK first, then db.
SECUREBOOT_MS_CERT_FILES = "MicCorKEKCA2011.crt MicCorKEK2KCA2023.crt \
                            MicWinProPCA2011.crt MicCorUEFCA2011.crt \
                            WindowsUEFICA2023.crt MicrosoftUEFICA2023.crt"

do_configure[noexec] = "1"
do_compile[noexec] = "1"

do_install() {
    # Every one of these must be a DER X.509 certificate with an RSA key: the
    # FDF wires each one in as a SECTION RAW and
    # SecureBootVariableProvisionLib runs RsaGetPublicKeyFromX509() over it,
    # building the EFI_SIGNATURE_LISTs itself. No PEM, no .esl. Checked here,
    # where the files are owned, rather than in the firmware recipe: the
    # failure mode otherwise is silent -- firmware whose key defaults simply
    # never load, with nothing said about it at build time or on the board.
    install -d ${D}${SECUREBOOT_KEYS_DIR}/pk ${D}${SECUREBOOT_KEYS_DIR}/ms

    [ -f "${WORKDIR}/secureboot-keys/PkDefault.der" ] || \
        bbfatal "files/secureboot-keys/PkDefault.der is missing"
    openssl x509 -inform DER -in "${WORKDIR}/secureboot-keys/PkDefault.der" -noout >/dev/null 2>&1 || \
        bbfatal "files/secureboot-keys/PkDefault.der is not a DER X.509 certificate"
    install -m 0644 "${WORKDIR}/secureboot-keys/PkDefault.der" \
        ${D}${SECUREBOOT_KEYS_DIR}/pk/PkDefault.der

    for c in ${SECUREBOOT_MS_CERT_FILES}; do
        [ -f "${WORKDIR}/secureboot-certs/$c" ] || \
            bbfatal "$c was not fetched into ${WORKDIR}/secureboot-certs"
        openssl x509 -inform DER -in "${WORKDIR}/secureboot-certs/$c" -noout >/dev/null 2>&1 || \
            bbfatal "${WORKDIR}/secureboot-certs/$c is not a DER X.509 certificate"
        install -m 0644 "${WORKDIR}/secureboot-certs/$c" ${D}${SECUREBOOT_KEYS_DIR}/ms/$c
    done
}
