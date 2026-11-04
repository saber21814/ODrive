# GNU make prefers this project entrypoint over the unchanged upstream Makefile.
# Original-board operations remain explicit: make -f Makefile <target>.
.DEFAULT_GOAL := all
.NOTPARALLEL:
PYTHON ?= python
PROJECT := $(PYTHON) -B Rehab/scripts/project.py

.PHONY: all build flash flash-stlink2 flash-check check help build-bench flash-bench flash-check-bench check-bench
all build:
	$(PROJECT) build

flash flash-stlink2:
	$(PROJECT) flash --serial "$(PROGRAMMER)"

# Build and validate, print the exact programmer arguments, never contact hardware.
flash-check:
	$(PROJECT) flash --dry-run --serial "$(PROGRAMMER)"

check:
	$(PROJECT) check

build-bench:
	$(PROJECT) build --profile bench
flash-bench:
	$(PROJECT) flash --profile bench --serial "$(PROGRAMMER)"
flash-check-bench:
	$(PROJECT) flash --profile bench --dry-run --serial "$(PROGRAMMER)"
check-bench:
	$(PROJECT) check --profile bench

help:
	@echo make: build and validate V1.3 full, motor locked
	@echo make flash: build, validate, program and verify V1.3 full using ST-Link
	@echo make flash-check: same preflight without accessing hardware
	@echo make flash-bench: explicit bounded native motor bench, boot idle
	@echo make -f Makefile: explicit original-board entrypoint, not for V1.3
