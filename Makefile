.PHONY: test test-protocol inspect build dist

VERSION ?= $(shell tr -d '[:space:]' < version.txt)
DIST := dist/DaysGoneCoop-$(VERSION)

test: test-protocol
	poetry run python -m unittest discover -s tools -p 'test_*.py' -v

test-protocol:
	@bin=$$(mktemp /tmp/dg-coop-protocol.XXXXXX); \
	trap 'rm -f "$$bin"' EXIT; \
	$(CXX) -std=c++20 -Wall -Wextra -Werror tools/test_protocol.cpp -o "$$bin" && "$$bin"

inspect:
	@poetry run python tools/engine_discovery.py --exe "$(EXE)" --manifest "$(MANIFEST)" --inventory config/steam-19221447.json

build:
	bash build.sh

# Release archive: the plugin, its default INI, licenses and the README.
dist: build
	rm -rf "$(DIST)" "$(DIST).zip"
	mkdir -p "$(DIST)"
	cp package/DaysGoneCoop.asi package/DaysGoneCoop.ini package/LICENSE-Dear-ImGui.txt LICENSE README.md "$(DIST)/"
	cd dist && zip -qr -X "DaysGoneCoop-$(VERSION).zip" "DaysGoneCoop-$(VERSION)"
	cd dist && shasum -a 256 "DaysGoneCoop-$(VERSION).zip" > "DaysGoneCoop-$(VERSION).zip.sha256"
	@echo "packaged dist/DaysGoneCoop-$(VERSION).zip"
