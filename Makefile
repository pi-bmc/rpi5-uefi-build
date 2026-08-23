.PHONY: format build
.DEFAULT: build

build:
	kas build

format:
	@for f in ./meta-rpi5-uefi/**/*.bb; do oelint-adv --fix "$f"; done
