.PHONY: build format format-recipes format-edk2 check-format
.DEFAULT_GOAL := build

build:
	kas build

format: format-recipes format-edk2

# oelint-adv autofixes for the BitBake recipes. (make runs /bin/sh: no **
# globstar, and $$f -- not $f -- reaches the shell.)
format-recipes:
	@command -v oelint-adv >/dev/null || { echo "oelint-adv not found (pip install oelint-adv)"; exit 1; }
	@find meta-rpi5-uefi -type f \( -name '*.bb' -o -name '*.bbappend' \) | \
	while read -r f; do oelint-adv --fix "$$f"; done

# TianoCore Uncrustify (edk2 UncrustifyCheck config + pinned fork binary)
# over the EDK2 sources carried by the layer.
format-edk2:
	hack/format-edk2.sh

check-format:
	hack/format-edk2.sh --check
